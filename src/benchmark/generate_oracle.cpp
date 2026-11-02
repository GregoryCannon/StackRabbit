//
//  generate_oracle.cpp
//  Generates the benchmark oracle: for each position in benchmark_positions.jsonl, runs
//  a depth-2 search over all (first, second) placement pairs, then rates every pair with
//  a two-round playout budget (1000 playouts for all pairs, 7000 more for the top 10% by
//  round-1 rating), and writes the pooled mean / standard error / n per pair to
//  src/benchmark/oracle_positions.jsonl. See oracle_format.md for the file spec.
//
//  Build and run (from repo root):
//    g++ -O2 src/benchmark/generate_oracle.cpp -o build/generate_oracle && ./build/generate_oracle [startPos] [endPos]
//
//  Position range is inclusive and 1-based (line numbers in benchmark_positions.jsonl).
//  Results are appended to the oracle file, so ranges can be generated incrementally;
//  the header is written only when the file doesn't exist yet.
//

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numeric>
#include "../cpp_modules/src/main.cpp"
#include "benchmark_common.hpp"

const char *ORACLE_PATH = "src/benchmark/oracle_positions.jsonl";
const char *BENCHMARK_PATH = "src/benchmark/benchmark_positions.jsonl";

const int ROUND1_PLAYOUTS = 1000;
const int ROUND1_LENGTH = 10;
const int ROUND3_PLAYOUTS = 7000;
const float ROUND3_FRACTION = 0.1f;

std::string timeline = "X.....";

// --- Oracle computation ---

struct OraclePair {
  std::string firstKey;
  std::string secondKey;
  float rating;
  float standardError;
  int n;
};

struct OracleResult {
  std::vector<OraclePair> pairs; // Sorted best-first
};

/** Pools two rounds of playout scores into a mean and standard error of the mean. */
void pooledStats(const std::vector<float> &round1Scores, const std::vector<float> &round3Scores, OUT float &mean, OUT float &standardError) {
  int n = (int) (round1Scores.size() + round3Scores.size());
  double sum = 0;
  for (float s : round1Scores) sum += s;
  for (float s : round3Scores) sum += s;
  mean = (float) (sum / n);
  double squaredDiffSum = 0;
  for (float s : round1Scores) squaredDiffSum += ((double) s - mean) * ((double) s - mean);
  for (float s : round3Scores) squaredDiffSum += ((double) s - mean) * ((double) s - mean);
  // All playout scores are included, including deathCoef scores for dead playouts
  double variance = squaredDiffSum / (n - 1);
  standardError = (float) sqrt(variance / n);
}

bool ratingLess(const OraclePair &a, const OraclePair &b) {
  return a.rating > b.rating; // Best-first (descending)
}

void computeOracleForPosition(const BenchmarkPosition &pos, OUT OracleResult &result) {
  // Reconstruct the game state, mirroring mainProcess
  GameState gameState = {};
  decodeCompressedBoard(pos.board, gameState.board);
  gameState.level = pos.level;
  gameState.lines = pos.lines;
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  std::pair<int, float> initialHoles = updateSurfaceAndHoles(gameState.surfaceArray, gameState.board, /* excludeHolesColumn= */ WELL_COLUMN, /* isDigMode= */ false);
  gameState.numTrueHoles = initialHoles.first;
  gameState.numPartialHoles = initialHoles.second;

  const PieceRangeContext pieceRangeContextLookup[4] = {
    getPieceRangeContext(timeline.c_str(), 1, /* gravityDoubled= */ true),
    getPieceRangeContext(timeline.c_str(), 1, /* gravityDoubled= */ false),
    getPieceRangeContext(timeline.c_str(), 2, /* gravityDoubled= */ false),
    getPieceRangeContext(timeline.c_str(), 3, /* gravityDoubled= */ false),
  };
  const EvalContext evalContextRaw = getEvalContext(gameState, pieceRangeContextLookup, /* useTestWeights= */ false, SAFE);
  const EvalContext *evalContext = &evalContextRaw;

  // Recompute holes under the actual AI mode, as mainProcess does
  std::pair<int, float> actualHoles = updateSurfaceAndHoles(gameState.surfaceArray, gameState.board, evalContext->countWellHoles ? -1 : evalContext->wellColumn, evalContext->aiMode == DIG);
  gameState.numTrueHoles = actualHoles.first;
  gameState.numPartialHoles = actualHoles.second;

  const Piece *curPiece = &PIECE_LIST[pos.curPiece];
  const Piece *nextPiece = &PIECE_LIST[pos.nextPiece];

  // Full depth-2 search: all (first, second) placement pairs
  list<Possibility> possibilities;
  searchDepth2(gameState, curPiece, nextPiece, /* secondPieceDelay= */ 0, evalContext, possibilities, /* generateExplanation= */ false);

  struct PairWork {
    const Possibility *possibility;
    float round1Rating;
    std::vector<float> round1Scores;
    std::vector<float> round3Scores;
  };
  std::vector<PairWork> work;
  work.reserve(possibilities.size());
  for (const Possibility &p : possibilities) {
    work.push_back({&p, 0, {}, {}});
  }

  // Round 1: playout every pair
  for (PairWork &w : work) {
    getPlayoutScore(w.possibility->resultingState, ROUND1_PLAYOUTS, ROUND1_LENGTH, pieceRangeContextLookup, nextPiece->index, SAFE, /* playoutDataList= */ NULL, &w.round1Scores, /* sequenceOffset= */ 0);
    float round1Mean = 0;
    float unusedSe;
    pooledStats(w.round1Scores, {}, round1Mean, unusedSe);
    w.round1Rating = w.possibility->immediateReward + round1Mean;
  }

  // Round 3: extra playouts (disjoint sequence rows) for the top 10% by round-1 rating
  std::vector<int> order(work.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&work](int a, int b) { return work[a].round1Rating > work[b].round1Rating; });
  int numRound3 = (int) ceil(work.size() * ROUND3_FRACTION);
  for (int i = 0; i < numRound3; i++) {
    PairWork &w = work[order[i]];
    getPlayoutScore(w.possibility->resultingState, ROUND3_PLAYOUTS, ROUND1_LENGTH, pieceRangeContextLookup, nextPiece->index, SAFE, /* playoutDataList= */ NULL, &w.round3Scores, /* sequenceOffset= */ ROUND1_PLAYOUTS);
  }

  // Pool the rounds and build the sorted pair list
  for (PairWork &w : work) {
    OraclePair pair;
    pair.firstKey = encodeLockPosition({w.possibility->firstPlacement.x, w.possibility->firstPlacement.y, w.possibility->firstPlacement.rotationIndex});
    pair.secondKey = encodeLockPosition({w.possibility->secondPlacement.x, w.possibility->secondPlacement.y, w.possibility->secondPlacement.rotationIndex});
    pooledStats(w.round1Scores, w.round3Scores, pair.rating, pair.standardError);
    pair.rating += w.possibility->immediateReward;
    pair.n = (int) (w.round1Scores.size() + w.round3Scores.size());
    result.pairs.push_back(pair);
  }
  std::stable_sort(result.pairs.begin(), result.pairs.end(), ratingLess);
}

// --- Output ---

void writeOracleFile(const std::vector<BenchmarkPosition> &positions, const std::vector<OracleResult> &results) {
  // Append to the existing oracle file (incremental generation); the header is only
  // written when the file doesn't exist or is empty yet
  bool fileHasContent = false;
  {
    std::ifstream existing(ORACLE_PATH);
    fileHasContent = existing.is_open() && existing.peek() != std::ifstream::traits_type::eof();
  }
  std::ofstream out(ORACLE_PATH, std::ios::out | std::ios::app);
  char buffer[600];

  if (!fileHasContent) {
    // Header line
    out << "{\"type\":\"oracle\",\"source\":\"" << BENCHMARK_PATH << "\",\"timeline\":\"" << timeline
        << "\",\"engineCommit\":\"" << getEngineCommit() << "\",\"weights\":\"MAIN/SAFE\""
        << ",\"round1\":{\"playouts\":" << ROUND1_PLAYOUTS << ",\"playoutLength\":" << ROUND1_LENGTH << ",\"sequenceOffset\":0"
        << "},\"round3\":{\"playouts\":" << ROUND3_PLAYOUTS << ",\"playoutLength\":" << ROUND1_LENGTH
        << ",\"sequenceOffset\":" << ROUND1_PLAYOUTS << ",\"selection\":\"top 10% of pairs by round-1 rating\"}}\n";
  }

  // One line per position
  for (size_t i = 0; i < positions.size(); i++) {
    const OracleResult &result = results[i];
    const BenchmarkPosition &pos = positions[i];
    std::string line;
    snprintf(buffer, sizeof(buffer),
             "{\"index\":%d,\"board\":\"%s\",\"level\":%d,\"lines\":%d,\"curPiece\":%d,\"nextPiece\":%d,\"aiMode\":%d,\"shallowEval\":%.4f",
             pos.index, pos.board.c_str(), pos.level, pos.lines, pos.curPiece, pos.nextPiece, pos.aiMode, pos.shallowEval);
    line += buffer;

    if (result.pairs.empty()) {
      line += ",\"best\":null,\"bestRating\":null,\"bestStandardError\":null,\"margin\":null,\"ratings\":[]}";
      out << line << "\n";
      continue;
    }

    const OraclePair &best = result.pairs[0];
    // Round the ratings to the stored precision, then compute the margin from the rounded
    // values so consumers get exact arithmetic
    float bestRatingRounded = (float) (round(best.rating * 100.0) / 100.0);
    float secondRatingRounded = result.pairs.size() > 1 ? (float) (round(result.pairs[1].rating * 100.0) / 100.0) : bestRatingRounded;
    float margin = bestRatingRounded - secondRatingRounded;
    snprintf(buffer, sizeof(buffer),
             ",\"best\":[\"%s\",\"%s\"],\"bestRating\":%.2f,\"bestStandardError\":%.4f,\"margin\":%.2f,\"ratings\":[",
             best.firstKey.c_str(), best.secondKey.c_str(), bestRatingRounded, best.standardError, margin);
    line += buffer;

    for (const OraclePair &pair : result.pairs) {
      snprintf(buffer, sizeof(buffer),
               "[\"%s\",\"%s\",%.2f,%.4f,%d],",
               pair.firstKey.c_str(), pair.secondKey.c_str(), pair.rating, pair.standardError, pair.n);
      line += buffer;
    }
    line.pop_back(); // Remove the last comma
    line += "]}"; // Close the ratings array and the object

    out << line << "\n";
  }
}

// --- Driver ---

void worker(const std::vector<BenchmarkPosition> &positions, int startIdx, int endIdx, OUT std::vector<OracleResult> &results) {
  for (int i = startIdx; i < endIdx; i++) {
    auto positionStart = std::chrono::steady_clock::now();
    computeOracleForPosition(positions[i], results[i]);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - positionStart).count();
    const OracleResult &result = results[i];
    if (result.pairs.empty()) {
      printf("[pos %d] done in %.1fs: no legal pairs\n", positions[i].index, elapsed);
    } else {
      const OraclePair &best = result.pairs[0];
      printf("[pos %d] done in %.1fs: %d pairs, best %s/%s = %.2f\n",
             positions[i].index, elapsed, (int) result.pairs.size(), best.firstKey.c_str(), best.secondKey.c_str(), best.rating);
    }
  }
}

int main(int argc, const char *argv[]) {
  auto timeStart = std::chrono::steady_clock::now();

  // Read the benchmark positions, filtered by the requested inclusive 1-based range
  int startPos = argc > 1 ? atoi(argv[1]) : 1;
  int endPos = argc > 2 ? atoi(argv[2]) : 999999;

  std::ifstream in(BENCHMARK_PATH);
  if (!in.is_open()) {
    printf("Error: could not open %s\n", BENCHMARK_PATH);
    return 1;
  }
  std::string headerLine;
  std::getline(in, headerLine);
  timeline = extractStringField(headerLine, "timeline");
  if (timeline.empty()) timeline = "X.....";

  std::vector<BenchmarkPosition> positions;
  std::string line;
  int lineIndex = 0;
  while (std::getline(in, line)) {
    lineIndex++;
    if (line.empty()) {
      continue;
    }
    BenchmarkPosition pos = {};
    // Prefer the explicit index field, so the benchmark file can be reordered without desync
    pos.index = extractIntField(line, "index");
    if (pos.index == 0) {
      pos.index = lineIndex; // Fallback for files without explicit indices
    }
    if (pos.index < startPos || pos.index > endPos) {
      continue;
    }
    pos.board = extractStringField(line, "board");
    pos.level = extractIntField(line, "level");
    pos.lines = extractIntField(line, "lines");
    pos.curPiece = extractIntField(line, "curPiece");
    pos.nextPiece = extractIntField(line, "nextPiece");
    pos.aiMode = extractIntField(line, "aiMode");
    pos.shallowEval = extractFloatField(line, "shallowEval");
    positions.push_back(pos);
  }
  printf("Running oracle on positions %d-%d (%d positions, timeline %s)\n", startPos, endPos, (int) positions.size(), timeline.c_str());

  // Threaded execution, results collected per position
  std::vector<OracleResult> results(positions.size());
  unsigned int numThreads = std::thread::hardware_concurrency();
  if (numThreads == 0) numThreads = 4;
  std::vector<std::thread> threads;
  int chunkSize = (int) ceil((double) positions.size() / numThreads);
  for (unsigned int i = 0; i < numThreads; i++) {
    int startIdx = i * chunkSize;
    int endIdx = std::min(startIdx + chunkSize, (int) positions.size());
    if (startIdx < endIdx) {
      threads.push_back(std::thread(worker, std::cref(positions), startIdx, endIdx, std::ref(results)));
    }
  }

  for (auto &t : threads) t.join();

  writeOracleFile(positions, results);

  auto timeEnd = std::chrono::steady_clock::now();
  long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
  printf("Wrote %d position(s) to %s\nTime elapsed: %.1f seconds\n", (int) positions.size(), ORACLE_PATH, ms / 1000.0);
  return 0;
}
