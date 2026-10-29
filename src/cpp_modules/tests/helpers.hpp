// Shared test helpers for the StackRabbit C++ engine tests.
//
// Header-only: included by stackrabbit_test.cpp (and usable by other section
// headers). The engine is a unity build, so everything compiles as a single
// translation unit -- see TESTING.md.

#ifndef SR_TEST_HELPERS
#define SR_TEST_HELPERS

#include <string>

// PIECE_LIST index order (see data/tetrominoes.cpp and canonical_sequences.hpp)
const int IDX_I = 0;
const int IDX_O = 1;
const int IDX_L = 2;
const int IDX_J = 3;
const int IDX_T = 4;
const int IDX_S = 5;
const int IDX_Z = 6;

// 200-char board string. Row 0 is the TOP row of the board; each row is 10
// chars of '0'/'1' (leftmost column first), per encodeBoard in utils.hpp.
static std::string makeEmptyBoard() {
  return std::string(200, '0');
}

// Flat stack: the bottom `height` rows fully filled.
static std::string makeFlatBoard(int height) {
  std::string board(200, '0');
  for (int row = 20 - height; row < 20; row++) {
    for (int col = 0; col < 10; col++) {
      board[row * 10 + col] = '1';
    }
  }
  return board;
}

// Flat stack with a one-column-wide well of the given depth carved out.
static std::string makeWellBoard(int height, int wellDepth, int wellCol) {
  std::string board = makeFlatBoard(height);
  for (int row = 20 - wellDepth; row < 20; row++) {
    board[row * 10 + wellCol] = '0';
  }
  return board;
}

// Flat stack with an extra column of cells stacked on top of column `col`.
// A legal result of placing an I piece vertically on flat ground.
static std::string makeBoardWithStackedColumn(int height, int stackHeight, int col) {
  std::string board = makeFlatBoard(height);
  for (int row = 20 - height - stackHeight; row < 20 - height; row++) {
    board[row * 10 + col] = '1';
  }
  return board;
}

// Flat stack with a single cell carved out of the top row of the stack, i.e.
// one cell short of a full line clear (a "burn" setup).
static std::string makeBoardOneCellShort(int height, int missingCol) {
  std::string board = makeFlatBoard(height);
  board[(20 - height) * 10 + missingCol] = '0';
  return board;
}

// Result of dropping a vertical I piece (4 cells) onto the surface of
// `board1` at column `col` -- i.e. a board reachable by one legal placement.
static std::string withVerticalIOnColumn(const std::string &board1, int col) {
  int surface = 0;
  for (int row = 19; row >= 0 && board1[row * 10 + col] == '1'; row--) {
    surface++;
  }
  std::string board = board1;
  for (int row = 20 - surface - 4; row < 20 - surface; row++) {
    board[row * 10 + col] = '1';
  }
  return board;
}

// Builds the pipe-delimited input string that mainProcess expects:
//   [board1 (200)] | [optional board2 (200)] | level | lines | currentPieceIdx |
//   nextPieceIdx | inputFrameTimeline | playoutCount | playoutLength |
//   pruningBreadth | aggressionLevel |
// See src/server/request_parser.ts getCppEncodedInputString for the TS side.
struct InputBuilder {
  std::string board1 = makeEmptyBoard();
  bool hasBoard2 = false;
  std::string board2 = makeEmptyBoard();
  int level = 18;
  int lines = 0;
  int curPiece = IDX_I;
  int nextPiece = IDX_O;
  std::string timeline = "X...";
  int playoutCount = 12;
  int playoutLength = 2;
  int pruningBreadth = 4;
  std::string aggression = "safe";

  InputBuilder &withBoard1(std::string b) { board1 = b; return *this; }
  InputBuilder &withBoard2(std::string b) { hasBoard2 = true; board2 = b; return *this; }
  InputBuilder &withLevel(int l) { level = l; return *this; }
  InputBuilder &withLines(int l) { lines = l; return *this; }
  InputBuilder &withCurPiece(int p) { curPiece = p; return *this; }
  InputBuilder &withNextPiece(int p) { nextPiece = p; return *this; }
  InputBuilder &withTimeline(std::string t) { timeline = t; return *this; }
  InputBuilder &withPlayoutCount(int c) { playoutCount = c; return *this; }
  InputBuilder &withPlayoutLength(int l) { playoutLength = l; return *this; }
  InputBuilder &withPruningBreadth(int b) { pruningBreadth = b; return *this; }
  InputBuilder &withAggression(std::string a) { aggression = a; return *this; }

  std::string build() const {
    std::string rest = std::to_string(level) + "|" + std::to_string(lines) + "|" +
                       std::to_string(curPiece) + "|" + std::to_string(nextPiece) + "|" +
                       timeline + "|" + std::to_string(playoutCount) + "|" +
                       std::to_string(playoutLength) + "|" +
                       std::to_string(pruningBreadth) + "|" + aggression + "|";
    if (hasBoard2) {
      return board1 + "|" + board2 + "|" + rest;
    }
    return board1 + "|" + rest;
  }
};

static std::string runRequest(RequestType type, const std::string &input) {
  return mainProcess(input.c_str(), type);
}

/** Parses a "[a, b, c]" move triple like the GET_MOVE output. */
static void parseMoveTriple(const std::string &output, int &rot, int &xOffset, int &yOffset) {
  REQUIRE(output.size() >= 2);
  REQUIRE(output[0] == '[');
  REQUIRE(output[output.size() - 1] == ']');
  std::string body = output.substr(1, output.size() - 2);
  size_t firstComma = body.find(',');
  size_t secondComma = body.find(',', firstComma + 1);
  REQUIRE(firstComma != std::string::npos);
  REQUIRE(secondComma != std::string::npos);
  rot = std::stoi(body.substr(0, firstComma));
  xOffset = std::stoi(body.substr(firstComma + 1, secondComma - firstComma - 1));
  yOffset = std::stoi(body.substr(secondComma + 1));
}

/** Counts non-overlapping occurrences of a substring. */
static int countOccurrences(const std::string &haystack, const std::string &needle) {
  int count = 0;
  size_t pos = haystack.find(needle);
  while (pos != std::string::npos) {
    count++;
    pos = haystack.find(needle, pos + needle.size());
  }
  return count;
}

#endif // SR_TEST_HELPERS
