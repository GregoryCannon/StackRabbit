//
//  run_agent.cpp
//  Benchmark harness: runs an agent (the engine's mainProcess with configurable request
//  type and search parameters) on each benchmark position and scores the answers against
//  the oracle. See oracle_format.md for the oracle file spec.
//
//  Usage (from repo root):
//    g++ -O2 src/benchmark/run_agent.cpp -o build/run_agent && ./build/run_agent [requestType] [playoutCount] [playoutLength] [pruningBreadth] [startPos] [endPos] [outputPath]
//
//  - requestType: GET_MOVE, GET_LOCK_VALUE_LOOKUP (implemented); GET_TOP_MOVES (planned)
//  - Position range is inclusive and 1-based (benchmark position indices)
//  - Results are written to a new timestamped file in src/benchmark/agent_results/ per
//    run (or to outputPath if provided), so runs with different configs can be compared
//

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <map>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>
#include "../cpp_modules/src/main.cpp"
#include "benchmark_common.hpp"

const char *BENCHMARK_PATH = "src/benchmark/benchmark_positions.jsonl";
const char *ORACLE_PATH = "src/benchmark/oracle_positions.jsonl";
const char *RESULTS_DIR = "src/benchmark/agent_results";

std::string timeline = "X.....";

// --- Input parsing ---

struct HarnessConfig {
  RequestType requestType;
  const char *requestTypeName;
  int playoutCount;
  int playoutLength;
  int pruningBreadth;
  int startPos;
  int endPos;
  std::string outputPath;
};

bool parseRequestType(const char *name, OUT RequestType &outType) {
  if (strcmp(name, "GET_MOVE") == 0) { outType = GET_MOVE; return true; }
  if (strcmp(name, "GET_LOCK_VALUE_LOOKUP") == 0) { outType = GET_LOCK_VALUE_LOOKUP; return true; }
  if (strcmp(name, "GET_TOP_MOVES") == 0) { outType = GET_TOP_MOVES; return true; }
  return false;
}

// --- Oracle data ---

struct OraclePosition {
  std::string bestFirstKey; // First placement of the best (first, second) pair
  float bestRating;
  float bestStandardError;
  float margin;
  float secondStandardError;
  // Oracle ratings collapsed by first placement (max over second placements), sorted best-first.
  // Mirrors how getLockValueLookup aggregates pairs, so agent first placements can be ranked.
  struct CollapsedRating {
    std::string key;
    float rating;
    float standardError; // SE of the pair that achieved this collapsed rating
  };
  std::vector<CollapsedRating> collapsedRatings;
};

void buildCollapsedRatings(const std::vector<std::vector<std::string>> &ratingEntries, OUT OraclePosition &oracle) {
  std::map<std::string, OraclePosition::CollapsedRating> bestByFirstKey;
  for (const std::vector<std::string> &entry : ratingEntries) {
    // entry = [firstKey, secondKey, rating, standardError, n] as strings
    float rating = (float) atof(entry[2].c_str());
    float standardError = (float) atof(entry[3].c_str());
    auto it = bestByFirstKey.find(entry[0]);
    if (it == bestByFirstKey.end() || rating > it->second.rating) {
      bestByFirstKey[entry[0]] = {entry[0], rating, standardError};
    }
  }
  for (const auto &pair : bestByFirstKey) {
    oracle.collapsedRatings.push_back(pair.second);
  }
  std::stable_sort(oracle.collapsedRatings.begin(), oracle.collapsedRatings.end(),
                   [](const OraclePosition::CollapsedRating &a, const OraclePosition::CollapsedRating &b) {
                     return a.rating > b.rating;
                   });
}

// Since the ratings entries are stored as JSON arrays of mixed strings/numbers, extract
// them with a small tolerant scanner rather than full JSON parsing.
void extractRatingEntries(const std::string &oracleLine, OUT std::vector<std::vector<std::string>> &entries) {
  size_t pos = oracleLine.find("\"ratings\":[");
  if (pos == std::string::npos) {
    return;
  }
  pos += strlen("\"ratings\":[");
  while (true) {
    size_t entryStart = oracleLine.find('[', pos);
    size_t arrayEnd = oracleLine.find(']', pos);
    if (entryStart == std::string::npos || arrayEnd == std::string::npos || entryStart > arrayEnd) {
      break;
    }
    std::string entry = oracleLine.substr(entryStart + 1, arrayEnd - entryStart - 1);
    std::vector<std::string> fields;
    size_t fieldStart = 0;
    while (true) {
      size_t comma = entry.find(',', fieldStart);
      std::string field = entry.substr(fieldStart, comma == std::string::npos ? std::string::npos : comma - fieldStart);
      // Strip whitespace and quotes
      size_t first = field.find_first_not_of(" \"");
      size_t last = field.find_last_not_of(" \"");
      fields.push_back(first == std::string::npos ? "" : field.substr(first, last - first + 1));
      if (comma == std::string::npos) break;
      fieldStart = comma + 1;
    }
    if (fields.size() >= 3) {
      entries.push_back(fields);
    }
    pos = arrayEnd + 1;
    if (oracleLine[pos] == ']') {
      break; // End of the ratings array
    }
  }
}

// --- Agent evaluation ---

struct AgentResult {
  bool valid;
  bool hasMove;
  std::string agentKey; // rot|x|y of the chosen first placement (GET_MOVE)
  bool match; // rank 1, or the move's rating is within the margin of error of the best
  bool withinMargin; // the move's rating is within 2x combined SE of the best
  int rank; // 1-based rank of agentKey in the collapsed oracle ratings (0 = unranked)
  float delta; // bestCollapsedRating - agentCollapsedRating
  float agentSe; // Standard error of the agent move's collapsed rating (0 if unranked)
  long timeMs;
};

std::string buildMainProcessInput(const BenchmarkPosition &pos, const HarnessConfig &config) {
  unsigned int board[20];
  decodeCompressedBoard(pos.board, board);
  // Input format: board|level|lines|curPiece|nextPiece|timeline|playoutCount|playoutLength|pruningBreadth|aggression
  std::ostringstream input;
  input << encodeBoardToString(board) << '|' << pos.level << '|' << pos.lines << '|' << pos.curPiece << '|'
        << pos.nextPiece << '|' << timeline << '|' << config.playoutCount << '|' << config.playoutLength << '|'
        << config.pruningBreadth << "|safe|";
  return input.str();
}

void resetAgentResult(OUT AgentResult &result) {
  result.hasMove = false;
  result.match = false;
  result.withinMargin = false;
  result.rank = 0;
  result.delta = 0;
  result.agentSe = 0;
}

/** Shared scoring: ranks the agent's chosen first placement within the oracle's
 * collapsed ratings (max over second placements per first placement). */
void scoreAgentKey(const std::string &agentKey, const OraclePosition &oracle, OUT AgentResult &result) {
  result.agentKey = agentKey;
  result.hasMove = true;
  for (size_t i = 0; i < oracle.collapsedRatings.size(); i++) {
    if (oracle.collapsedRatings[i].key == agentKey) {
      result.rank = (int) i + 1;
      result.delta = oracle.collapsedRatings[0].rating - oracle.collapsedRatings[i].rating;
      result.agentSe = oracle.collapsedRatings[i].standardError;
      float combinedSe = sqrtf(oracle.bestStandardError * oracle.bestStandardError + result.agentSe * result.agentSe);
      result.withinMargin = result.delta <= 2.0f * combinedSe;
      result.match = result.rank == 1 || result.withinMargin;
      return;
    }
  }
  // Unranked: the placement leaves the next piece with no legal continuation
}

void runGetMove(const std::string &mainProcessResult, const OraclePosition &oracle, int curPieceIndex, OUT AgentResult &result) {
  resetAgentResult(result);
  int rot, xOffset, yOffset;
  if (sscanf(mainProcessResult.c_str(), "[%d, %d, %d]", &rot, &xOffset, &yOffset) != 3) {
    return; // Error string or unexpected format
  }
  // Convert from the GET_MOVE offsets back to the raw rot|x|y key space
  int x = xOffset + INITIAL_X;
  int y = yOffset + PIECE_LIST[curPieceIndex].initialY;
  char keyBuffer[32];
  snprintf(keyBuffer, sizeof(keyBuffer), "%d|%d|%d", rot, x, y);
  scoreAgentKey(keyBuffer, oracle, result);
}

/** GET_LOCK_VALUE_LOOKUP: the engine returns a flat JSON map of firstPlacement -> rating
 * (already collapsed to the best second placement per first placement), so the agent's
 * move is simply the argmax. */
void runLockValueLookup(const std::string &mainProcessResult, const OraclePosition &oracle, OUT AgentResult &result) {
  resetAgentResult(result);
  size_t pos = mainProcessResult.find('{');
  if (pos == std::string::npos) {
    return; // Error string or unexpected format
  }
  std::string bestKey;
  float bestValue = FLOAT_MIN;
  bool anyEntry = false;
  while (true) {
    size_t keyStart = mainProcessResult.find('"', pos);
    if (keyStart == std::string::npos) break;
    size_t keyEnd = mainProcessResult.find('"', keyStart + 1);
    if (keyEnd == std::string::npos) break;
    size_t colon = mainProcessResult.find(':', keyEnd);
    if (colon == std::string::npos) break;
    size_t comma = mainProcessResult.find(',', colon);
    size_t closing = mainProcessResult.find('}', colon);
    size_t valueEnd = (comma != std::string::npos && comma < closing) ? comma : closing;
    if (valueEnd == std::string::npos) break;
    float value = (float) atof(mainProcessResult.substr(colon + 1, valueEnd - colon - 1).c_str());
    if (!anyEntry || value > bestValue) {
      anyEntry = true;
      bestValue = value;
      bestKey = mainProcessResult.substr(keyStart + 1, keyEnd - keyStart - 1);
    }
    pos = valueEnd;
  }
  if (!anyEntry) {
    return; // Empty map (no legal moves)
  }
  scoreAgentKey(bestKey, oracle, result);
}

void runAgentOnPosition(const BenchmarkPosition &pos, const OraclePosition &oracle, const HarnessConfig &config, OUT AgentResult &result) {
  result.valid = true;
  auto start = std::chrono::steady_clock::now();
  std::string mainProcessResult = mainProcess(buildMainProcessInput(pos, config).c_str(), config.requestType);
  result.timeMs = (long) std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

  switch (config.requestType) {
    case GET_MOVE:
      runGetMove(mainProcessResult, oracle, pos.curPiece, result);
      break;
    case GET_LOCK_VALUE_LOOKUP:
      runLockValueLookup(mainProcessResult, oracle, result);
      break;
    default:
      // Structured for future request types (GET_TOP_MOVES)
      printf("Error: request type %s is not implemented in the harness yet\n", config.requestTypeName);
      result.valid = false;
      break;
  }
}

// --- Output ---

std::string formatRate(double rate) {
  if (std::isnan(rate)) return "null";
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%.3f", rate);
  return buffer;
}

std::string formatDelta(float delta) {
  if (std::isnan(delta)) return "null";
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%.2f", delta);
  return buffer;
}

void writeResultsFile(const HarnessConfig &config, const std::vector<BenchmarkPosition> &positions,
                      const std::vector<OraclePosition> &oracles, const std::vector<AgentResult> &results) {
  std::ofstream out(config.outputPath, std::ios::out | std::ios::trunc);
  char buffer[600];

  time_t now = time(nullptr);
  char dateBuffer[20];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", localtime(&now));
  out << "{\"type\":\"agentResults\",\"requestType\":\"" << config.requestTypeName
      << "\",\"playoutCount\":" << config.playoutCount << ",\"playoutLength\":" << config.playoutLength
      << ",\"pruningBreadth\":" << config.pruningBreadth << ",\"aggression\":\"safe\",\"timeline\":\"" << timeline
      << "\",\"engineCommit\":\"" << getEngineCommit() << "\",\"date\":\"" << dateBuffer << "\",\"source\":\"" << ORACLE_PATH << "\"}\n";

  for (size_t i = 0; i < positions.size(); i++) {
    const AgentResult &result = results[i];
    const OraclePosition &oracle = oracles[i];
    snprintf(buffer, sizeof(buffer),
             "{\"index\":%d,\"agentKey\":\"%s\",\"oracleBest\":\"%s\",\"match\":%s,\"rank\":%d,\"delta\":%s,\"margin\":%.2f,\"bestSe\":%.4f,\"timeMs\":%ld}",
             positions[i].index,
             result.agentKey.c_str(),
             oracle.bestFirstKey.c_str(),
             result.match ? "true" : "false",
             result.rank,
             formatDelta(result.delta).c_str(),
             oracle.margin, oracle.bestStandardError, result.timeMs);
    out << buffer << "\n";
  }

  // Summary line.
  // Match = the agent's move is the oracle's best (rank 1), or within the margin of
  // error of the best (2x combined SE). Severity buckets only apply to genuine errors
  // (deltas outside the margin of error), and partition them: inaccuracies < 7.5,
  // mistakes < 15, blunders >= 15.
  int total = 0, matches = 0, unranked = 0, noMove = 0;
  int inaccuracies = 0, mistakes = 0, blunders = 0;
  double rankSum = 0, deltaSum = 0;
  int rankedCount = 0;
  std::vector<float> nonzeroDeltas;
  std::vector<long> times;
  // Breakdowns by stratum and aiMode
  std::map<int, int> matchesByMode, totalByMode;
  std::map<int, int> matchesByStratum, totalByStratum;
  for (size_t i = 0; i < positions.size(); i++) {
    const AgentResult &result = results[i];
    const OraclePosition &oracle = oracles[i];
    if (!result.valid || !result.hasMove) {
      noMove++;
      continue;
    }
    times.push_back(result.timeMs);
    total++;
    totalByMode[positions[i].aiMode]++;
    totalByStratum[positions[i].level == 18 ? 18 : 19]++;
    if (result.rank > 0) {
      rankedCount++;
      rankSum += result.rank;
      deltaSum += result.delta;
      if (result.delta > 0) {
        nonzeroDeltas.push_back(result.delta);
        // Moves within the margin of error of the best aren't genuine errors; exclude
        // them from the severity buckets entirely
        if (!result.withinMargin) {
          if (result.delta < 7.5f) inaccuracies++;
          else if (result.delta < 15.0f) mistakes++;
          else blunders++;
        }
      }
    } else {
      // A placement with no rated continuation leaves the next piece unplaceable;
      // count it as a blunder (it's at least as bad as any rated alternative)
      unranked++;
      blunders++;
    }
    if (result.match) {
      matches++;
      matchesByMode[positions[i].aiMode]++;
      matchesByStratum[positions[i].level == 18 ? 18 : 19]++;
    }
  }
  std::sort(nonzeroDeltas.begin(), nonzeroDeltas.end());
  float medianNonzeroDelta = 0;
  if (!nonzeroDeltas.empty()) {
    medianNonzeroDelta = nonzeroDeltas.size() % 2 == 1
        ? nonzeroDeltas[nonzeroDeltas.size() / 2]
        : (nonzeroDeltas[nonzeroDeltas.size() / 2 - 1] + nonzeroDeltas[nonzeroDeltas.size() / 2]) / 2;
  }
  std::sort(times.begin(), times.end());
  long timeMedianMs = times.empty() ? 0 : (times.size() % 2 == 1 ? times[times.size() / 2] : (times[times.size() / 2 - 1] + times[times.size() / 2]) / 2);
  long timeP90Ms = times.empty() ? 0 : times[std::min(times.size() - 1, (size_t) (ceil(0.9 * times.size()) - 1))];
  long timeMaxMs = times.empty() ? 0 : times.back();

  std::string modeBreakdown, stratumBreakdown;
  for (const auto &pair : totalByMode) {
    char entryBuffer[40];
    snprintf(entryBuffer, sizeof(entryBuffer), "%s\"%d\":%.3f", modeBreakdown.empty() ? "" : ",", pair.first, (double) matchesByMode[pair.first] / pair.second);
    modeBreakdown += entryBuffer;
  }
  for (const auto &pair : totalByStratum) {
    char entryBuffer[40];
    snprintf(entryBuffer, sizeof(entryBuffer), "%s\"level%d\":%.3f", stratumBreakdown.empty() ? "" : ",", pair.first, (double) matchesByStratum[pair.first] / pair.second);
    stratumBreakdown += entryBuffer;
  }

  snprintf(buffer, sizeof(buffer),
           "{\"type\":\"summary\",\"positions\":%d,\"withMove\":%d,\"matchRate\":%s,\"inaccuracies\":%d,\"mistakes\":%d,\"blunders\":%d,\"medianNonzeroDelta\":%s,\"meanDelta\":%s,\"meanRank\":%s,\"timeMedianMs\":%ld,\"timeP90Ms\":%ld,\"timeMaxMs\":%ld,\"unranked\":%d,\"noMove\":%d,\"matchRateByAiMode\":{%s},\"matchRateByStratum\":{%s}}",
           total + noMove, total,
           formatRate(total ? (double) matches / total : NAN).c_str(),
           inaccuracies, mistakes, blunders,
           formatDelta(medianNonzeroDelta).c_str(),
           formatRate(rankedCount ? deltaSum / rankedCount : NAN).c_str(),
           formatRate(rankedCount ? rankSum / rankedCount : NAN).c_str(),
           timeMedianMs, timeP90Ms, timeMaxMs,
           unranked, noMove, modeBreakdown.c_str(), stratumBreakdown.c_str());
  out << buffer << "\n";

  // Also print the summary to the terminal
  printf("%s\n", buffer);
}

// --- Driver ---

void worker(const std::vector<BenchmarkPosition> &positions, const std::vector<OraclePosition> &oracles,
            int startIdx, int endIdx, const HarnessConfig &config, OUT std::vector<AgentResult> &results) {
  for (int i = startIdx; i < endIdx; i++) {
    runAgentOnPosition(positions[i], oracles[i], config, results[i]);
  }
}

int main(int argc, const char *argv[]) {
  auto timeStart = std::chrono::steady_clock::now();

  HarnessConfig config = {};
  const char *requestTypeName = argc > 1 ? argv[1] : "GET_MOVE";
  if (!parseRequestType(requestTypeName, config.requestType)) {
    printf("Error: unknown request type %s (supported: GET_MOVE, GET_LOCK_VALUE_LOOKUP, GET_TOP_MOVES)\n", requestTypeName);
    return 1;
  }
  config.requestTypeName = requestTypeName;
  config.playoutCount = argc > 2 ? atoi(argv[2]) : DEFAULT_PLAYOUT_COUNT;
  config.playoutLength = argc > 3 ? atoi(argv[3]) : DEFAULT_PLAYOUT_LENGTH;
  config.pruningBreadth = argc > 4 ? atoi(argv[4]) : DEFAULT_PRUNING_BREADTH;
  config.startPos = argc > 5 ? atoi(argv[5]) : 1;
  config.endPos = argc > 6 ? atoi(argv[6]) : 999999;
  if (argc > 7) {
    config.outputPath = argv[7];
  } else {
    // Default output: a new timestamped file per run in the results folder
    std::string typeLower = requestTypeName;
    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), [](unsigned char c) { return std::tolower(c); });
    time_t now = time(nullptr);
    char timestampBuffer[20];
    strftime(timestampBuffer, sizeof(timestampBuffer), "%Y%m%d_%H%M%S", localtime(&now));
    mkdir(RESULTS_DIR, 0755); // No-op if it already exists
    char nameBuffer[80];
    snprintf(nameBuffer, sizeof(nameBuffer), "%s_%dx%d_b%d_%s.jsonl", typeLower.c_str(), config.playoutCount, config.playoutLength, config.pruningBreadth, timestampBuffer);
    config.outputPath = std::string(RESULTS_DIR) + "/" + nameBuffer;
  }

  // Load the benchmark positions and join with the oracle lines on index
  std::ifstream benchIn(BENCHMARK_PATH);
  std::ifstream oracleIn(ORACLE_PATH);
  if (!benchIn.is_open() || !oracleIn.is_open()) {
    printf("Error: could not open %s and/or %s\n", BENCHMARK_PATH, ORACLE_PATH);
    return 1;
  }
  std::string headerLine;
  std::getline(benchIn, headerLine);
  timeline = extractStringField(headerLine, "timeline");
  if (timeline.empty()) timeline = "X.....";
  std::getline(oracleIn, headerLine); // Skip the oracle header

  std::map<int, std::string> oracleLinesByIndex;
  std::string line;
  while (std::getline(oracleIn, line)) {
    if (!line.empty()) {
      oracleLinesByIndex[extractIntField(line, "index")] = line;
    }
  }

  std::vector<BenchmarkPosition> positions;
  std::vector<OraclePosition> oracles;
  while (std::getline(benchIn, line)) {
    if (line.empty()) continue;
    BenchmarkPosition pos = {};
    pos.index = extractIntField(line, "index");
    if (pos.index < config.startPos || pos.index > config.endPos) continue;
    pos.board = extractStringField(line, "board");
    pos.level = extractIntField(line, "level");
    pos.lines = extractIntField(line, "lines");
    pos.curPiece = extractIntField(line, "curPiece");
    pos.nextPiece = extractIntField(line, "nextPiece");
    pos.aiMode = extractIntField(line, "aiMode");
    pos.shallowEval = extractFloatField(line, "shallowEval");

    auto it = oracleLinesByIndex.find(pos.index);
    if (it == oracleLinesByIndex.end()) {
      printf("Error: no oracle line for position %d\n", pos.index);
      return 1;
    }
    OraclePosition oracle;
    oracle.bestFirstKey = extractStringField(it->second, "best").empty() ? "" : [&]() {
      // best is stored as an array ["rot|x|y","rot|x|y"]; extract the first element
      std::string bestField = extractStringField(it->second, "best");
      size_t start = bestField.find('"');
      size_t end = bestField.find('"', start + 1);
      return (start == std::string::npos || end == std::string::npos) ? "" : bestField.substr(start + 1, end - start - 1);
    }();
    oracle.bestRating = extractFloatField(it->second, "bestRating");
    oracle.bestStandardError = extractFloatField(it->second, "bestStandardError");
    oracle.margin = extractFloatField(it->second, "margin");
    std::vector<std::vector<std::string>> ratingEntries;
    extractRatingEntries(it->second, ratingEntries);
    if (ratingEntries.size() > 1) {
      oracle.secondStandardError = (float) atof(ratingEntries[1][3].c_str());
    }
    buildCollapsedRatings(ratingEntries, oracle);

    positions.push_back(pos);
    oracles.push_back(oracle);
  }
  printf("Running %s (playouts %d x %d, breadth %d) on %d positions -> %s\n",
         config.requestTypeName, config.playoutCount, config.playoutLength, config.pruningBreadth,
         (int) positions.size(), config.outputPath.c_str());

  std::vector<AgentResult> results(positions.size());
  unsigned int numThreads = std::thread::hardware_concurrency();
  if (numThreads == 0) numThreads = 4;
  std::vector<std::thread> threads;
  int chunkSize = (int) ceil((double) positions.size() / numThreads);
  for (unsigned int i = 0; i < numThreads; i++) {
    int startIdx = i * chunkSize;
    int endIdx = std::min(startIdx + chunkSize, (int) positions.size());
    if (startIdx < endIdx) {
      threads.push_back(std::thread(worker, std::cref(positions), std::cref(oracles), startIdx, endIdx, std::cref(config), std::ref(results)));
    }
  }
  for (auto &t : threads) t.join();

  writeResultsFile(config, positions, oracles, results);

  auto timeEnd = std::chrono::steady_clock::now();
  printf("Time elapsed: %.1f seconds\n", std::chrono::duration<double>(timeEnd - timeStart).count());
  return 0;
}
