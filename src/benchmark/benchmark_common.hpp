#ifndef BENCHMARK_COMMON
#define BENCHMARK_COMMON

// Shared helpers for the benchmark tools (generate_oracle.cpp, run_agent.cpp):
// minimal JSONL field extraction, compressed-board decoding/encoding, and provenance.

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "../cpp_modules/src/formatting.hpp" // BOARD_ENCODING
#include "../cpp_modules/src/utils.hpp" // FULL_ROW

/** Extracts a field value from a flat JSON object line (no nesting/escaping). */
inline std::string extractStringField(const std::string &line, const char *key) {
  std::string needle = std::string("\"") + key + "\":";
  size_t pos = line.find(needle);
  if (pos == std::string::npos) {
    return "";
  }
  pos += needle.length();
  if (line[pos] == '"') {
    size_t end = line.find('"', pos + 1);
    return line.substr(pos + 1, end - pos - 1);
  }
  size_t end = line.find_first_of(",}", pos);
  return line.substr(pos, end - pos);
}

inline int extractIntField(const std::string &line, const char *key) {
  return atoi(extractStringField(line, key).c_str());
}

inline float extractFloatField(const std::string &line, const char *key) {
  return (float) atof(extractStringField(line, key).c_str());
}

/** Decodes a compressed formatBoard string back into 20 rows of 10-bit occupancy. */
inline void decodeCompressedBoard(const std::string &compressedBoard, OUT unsigned int outBoard[20]) {
  for (int i = 0; i < 20; i++) {
    outBoard[i] = 0;
  }
  size_t firstComma = compressedBoard.find(',');
  size_t secondComma = compressedBoard.find(',', firstComma + 1);
  if (firstComma == std::string::npos || secondComma == std::string::npos) {
    printf("Error: malformed compressed board: %s\n", compressedBoard.c_str());
    return;
  }
  int numEmptyRows = atoi(compressedBoard.substr(0, firstComma).c_str());
  std::string middleRows = compressedBoard.substr(firstComma + 1, secondComma - firstComma - 1);
  int numRightWellRows = atoi(compressedBoard.substr(secondComma + 1).c_str());

  int row = numEmptyRows;
  for (size_t i = 0; i < middleRows.length(); i += 2) {
    size_t leftIndex = BOARD_ENCODING.find(middleRows.at(i));
    size_t rightIndex = BOARD_ENCODING.find(middleRows.at(i + 1));
    if (leftIndex == std::string::npos || rightIndex == std::string::npos) {
      printf("Error: invalid character in compressed board: %s\n", compressedBoard.c_str());
      return;
    }
    outBoard[row] = (unsigned int) ((leftIndex << 5) | rightIndex);
    row++;
  }
  for (; row < 20; row++) {
    outBoard[row] = FULL_ROW - 1;
  }
}

/** Encodes a board as the 200-character 0/1 string that mainProcess expects. */
inline std::string encodeBoardToString(const unsigned int board[20]) {
  std::string output;
  output.reserve(200);
  for (int r = 0; r < 20; r++) {
    for (int c = 0; c < 10; c++) {
      output += (board[r] & (1 << (9 - c))) ? '1' : '0';
    }
  }
  return output;
}

/** Gets the current git commit hash, for recording provenance in output files. */
inline std::string getEngineCommit() {
  FILE *pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
  if (!pipe) return "unknown";
  char buffer[16];
  char *ok = fgets(buffer, sizeof(buffer), pipe);
  pclose(pipe);
  if (!ok) return "unknown";
  std::string commit(buffer);
  while (!commit.empty() && (commit.back() == '\n' || commit.back() == '\r')) commit.pop_back();
  return commit.empty() ? "unknown" : commit;
}

/** One benchmark position, as stored in benchmark_positions.jsonl. */
struct BenchmarkPosition {
  int index; // Slot-stable position index (1..200)
  std::string board;
  int level;
  int lines;
  int curPiece;
  int nextPiece;
  int aiMode;
  float shallowEval;
};

#endif
