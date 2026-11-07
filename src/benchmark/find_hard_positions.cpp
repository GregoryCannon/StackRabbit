//
//  find_hard_positions.cpp
//  Fork tool for the agent accuracy benchmark: generates "hard" benchmark positions.
//
//  For each benchmark position, re-randomizes the current + next piece, then gauges the
//  engine's move choice through GET_LOCK_VALUE_LOOKUP at two depths:
//    - shallow: playoutCount=0, playoutLength=0 (eval-only, static eval ranking)
//    - deep:    playoutCount=2000, playoutLength=10, pruningBreadth=20 (playout search)
//  A position is "hard" when the two gauges disagree; the randomized position is written to
//  the hard set with provenance, ready for a future oracle regeneration.
//
//  Build and run (from repo root):
//    g++ -O2 src/benchmark/find_hard_positions.cpp -o build/find_hard_positions &&
//    ./build/find_hard_positions [startPos] [endPos] [outputPath]
//
//  Position range is inclusive and 1-based (benchmark position indices).

#include "../cpp_modules/src/main.cpp"
#include "benchmark_common.hpp"
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

const char *BENCHMARK_PATH = "src/benchmark/data/benchmark_positions.jsonl";
const char *HARD_PATH = "src/benchmark/data/hard_benchmark_positions.jsonl";

const int SHALLOW_PLAYOUT_COUNT = 0;
const int SHALLOW_PLAYOUT_LENGTH = 0;
const int DEEP_PLAYOUT_COUNT = 2000;
const int DEEP_PLAYOUT_LENGTH = 10;
const int DEEP_PRUNING_BREADTH = 20;
const int MAX_ATTEMPTS = 30;

std::string getAgentKey(const std::string &input, int playoutCount, int playoutLength,
                        int pruningBreadth) {
  std::string result = mainProcess(input.c_str(), GET_LOCK_VALUE_LOOKUP);
  size_t pos = result.find('{');
  if (pos == std::string::npos) {
    return "";
  }
  std::string bestKey;
  float bestValue = -1000000.0f;
  bool anyEntry = false;
  size_t p = pos;
  while (true) {
    size_t keyStart = result.find('"', p);
    if (keyStart == std::string::npos) {
      break;
    }
    size_t keyEnd = result.find('"', keyStart + 1);
    if (keyEnd == std::string::npos) {
      break;
    }
    size_t colon = result.find(':', keyEnd);
    if (colon == std::string::npos) {
      break;
    }
    size_t comma = result.find(',', colon);
    size_t closing = result.find('}', colon);
    size_t valueEnd = (comma != std::string::npos && comma < closing) ? comma : closing;
    if (valueEnd == std::string::npos) {
      break;
    }
    float value = (float)atof(result.substr(colon + 1, valueEnd - colon - 1).c_str());
    if (!anyEntry || value > bestValue) {
      anyEntry = true;
      bestValue = value;
      bestKey = result.substr(keyStart + 1, keyEnd - keyStart - 1);
    }
    if (valueEnd >= result.size() || result[valueEnd] == '}') {
      break;
    }
    p = valueEnd + 1;
  }
  return bestKey;
}

std::string buildInput(const BenchmarkPosition &pos, int curPiece, int nextPiece,
                       const std::string &timeline, int playoutCount, int playoutLength,
                       int pruningBreadth) {
  unsigned int board[20];
  decodeCompressedBoard(pos.board, board);
  std::ostringstream input;
  input
      << encodeBoardToString(board)
      << '|'
      << pos.level
      << '|'
      << pos.lines
      << '|'
      << curPiece
      << '|'
      << nextPiece
      << '|'
      << timeline
      << '|'
      << playoutCount
      << '|'
      << playoutLength
      << '|'
      << pruningBreadth
      << "|safe|-1|";
  return input.str();
}

int main(int argc, const char *argv[]) {
  int startPos = argc > 1 ? atoi(argv[1]) : 1;
  int endPos = argc > 2 ? atoi(argv[2]) : 999999;
  std::string outputPath = argc > 3 ? argv[3] : HARD_PATH;

  std::ifstream in(BENCHMARK_PATH);
  if (!in.is_open()) {
    printf("Error: could not open %s\n", BENCHMARK_PATH);
    return 1;
  }
  std::string headerLine;
  std::getline(in, headerLine);
  std::string timeline = extractStringField(headerLine, "timeline");
  if (timeline.empty()) {
    timeline = "X.....";
  }

  std::vector<BenchmarkPosition> positions;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    BenchmarkPosition pos = {};
    pos.index = extractIntField(line, "index");
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

  std::ofstream out(outputPath, std::ios::out | std::ios::trunc);
  char buffer[600];
  int written = 0;
  for (const auto &pos : positions) {
    int curPieceOut = pos.curPiece;
    int nextPieceOut = pos.nextPiece;
    int sourceEasy = pos.index;
    bool hard = false;
    bool wasRandomized = false;
    std::string origShallowInput = buildInput(pos, pos.curPiece, pos.nextPiece, timeline,
                                              SHALLOW_PLAYOUT_COUNT, SHALLOW_PLAYOUT_LENGTH, 10);
    std::string origDeepInput =
        buildInput(pos, pos.curPiece, pos.nextPiece, timeline, DEEP_PLAYOUT_COUNT,
                   DEEP_PLAYOUT_LENGTH, DEEP_PRUNING_BREADTH);
    std::string keyShallowOrig =
        getAgentKey(origShallowInput, SHALLOW_PLAYOUT_COUNT, SHALLOW_PLAYOUT_LENGTH, 10);
    std::string keyDeepOrig =
        getAgentKey(origDeepInput, DEEP_PLAYOUT_COUNT, DEEP_PLAYOUT_LENGTH, DEEP_PRUNING_BREADTH);
    if (keyShallowOrig != keyDeepOrig) {
      hard = true;
    }
    int attempts = 0;
    for (int attempt = 0; attempt < MAX_ATTEMPTS && !hard; attempt++) {
      attempts = attempt + 1;
      unsigned seed = 1000u * (unsigned)pos.index + 7919u * (unsigned)(attempt + 1);
      std::mt19937 rng(seed);
      std::uniform_int_distribution<int> dist(0, 6);
      int curPiece = dist(rng);
      int nextPiece = dist(rng);
      while (nextPiece == curPiece) {
        nextPiece = dist(rng);
      }
      std::string shallowInput = buildInput(pos, curPiece, nextPiece, timeline,
                                            SHALLOW_PLAYOUT_COUNT, SHALLOW_PLAYOUT_LENGTH, 10);
      std::string deepInput = buildInput(pos, curPiece, nextPiece, timeline, DEEP_PLAYOUT_COUNT,
                                         DEEP_PLAYOUT_LENGTH, DEEP_PRUNING_BREADTH);
      std::string keyShallow =
          getAgentKey(shallowInput, SHALLOW_PLAYOUT_COUNT, SHALLOW_PLAYOUT_LENGTH, 10);
      std::string keyDeep =
          getAgentKey(deepInput, DEEP_PLAYOUT_COUNT, DEEP_PLAYOUT_LENGTH, DEEP_PRUNING_BREADTH);
      if (keyShallow != keyDeep) {
        hard = true;
        wasRandomized = true;
        curPieceOut = curPiece;
        nextPieceOut = nextPiece;
      }
    }
    if (hard) {
      if (wasRandomized) {
        printf("[pos %d] hard (attempt %d, randomized cur=%d next=%d)\n", pos.index, attempts,
               curPieceOut, nextPieceOut);
        fflush(stdout);
      } else {
        printf("[pos %d] hard (already hard, copied as-is)\n", pos.index);
        fflush(stdout);
      }
      snprintf(buffer, sizeof(buffer),
               "{\"index\":%d,\"board\":\"%s\",\"level\":%d,\"lines\":%d,\"curPiece\":%d,"
               "\"nextPiece\":%d,\"aiMode\":%d,\"shallowEval\":%.4f,\"sourceEasyIndex\":%d,"
               "\"randCurPiece\":%d,\"randNextPiece\":%d}",
               written + 1, pos.board.c_str(), pos.level, pos.lines, curPieceOut, nextPieceOut,
               pos.aiMode, pos.shallowEval, sourceEasy, curPieceOut, nextPieceOut);
      out << buffer << "\n";
      written++;
    } else {
      printf("[pos %d] no hard position found in %d attempts\n", pos.index, attempts);
      fflush(stdout);
    }
  }
  printf("Wrote %d hard position(s) to %s\n", written, outputPath.c_str());
  return 0;
}
