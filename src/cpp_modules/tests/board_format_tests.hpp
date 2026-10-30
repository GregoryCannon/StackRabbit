// Tests for the compressed board format: formatBoard (compress) and
// parseCompressedBoard (decompress).
//
// Header-only: included by stackrabbit_test.cpp.

#ifndef SR_BOARD_FORMAT_TESTS
#define SR_BOARD_FORMAT_TESTS

// 15 empty rows, then a 6-wide and an 8-wide left stack row, then 3 right-well rows.
static std::string makeExampleBoard() {
  std::string board = makeEmptyBoard();
  board.replace(150, 10, "1111110000");
  board.replace(160, 10, "1111111100");
  board.replace(170, 30, "111111111011111111101111111110");
  return board;
}

TEST_CASE("formatBoard produces the expected compressed string") {
  unsigned int board[20];
  encodeBoard(makeExampleBoard().c_str(), board);
  CHECK(formatBoard(board) == "15,FqFC,3");
}

TEST_CASE("formatBoard handles fully empty and all-right-well boards") {
  unsigned int board[20];
  encodeBoard(makeEmptyBoard().c_str(), board);
  CHECK(formatBoard(board) == "20,,0");

  std::string allWellBoard;
  for (int row = 0; row < 20; row++) {
    allWellBoard += "1111111110";
  }
  encodeBoard(allWellBoard.c_str(), board);
  CHECK(formatBoard(board) == "0,,20");
}

TEST_CASE("parseCompressedBoard decodes the documented example") {
  unsigned int board[20];
  parseCompressedBoard("15,CaEa,3", board);
  for (int i = 0; i < 15; i++) {
    CHECK(board[i] == 0);
  }
  CHECK(board[15] == 0b1110000000); // C = 11100 (left half), a = 00000 (right half)
  CHECK(board[16] == 0b1111000000); // E = 11110 (left half), a = 00000 (right half)
  for (int i = 17; i < 20; i++) {
    CHECK(board[i] == FULL_ROW - 1);
  }
}

TEST_CASE("formatBoard and parseCompressedBoard round trip both ways") {
  std::string wellsOnlyBoard = std::string(150, '0');
  for (int row = 15; row < 20; row++) {
    wellsOnlyBoard += "1111111110";
  }

  std::vector<std::string> boardStrings = {
      makeEmptyBoard(),
      makeFlatBoard(4),
      makeWellBoard(10, 4, 8),
      makeBoardOneCellShort(7, 3),
      makeExampleBoard(),
      wellsOnlyBoard,
  };
  for (const std::string &boardString : boardStrings) {
    unsigned int board[20];
    encodeBoard(boardString.c_str(), board);
    std::string compressed = formatBoard(board);

    unsigned int reparsedBoard[20];
    parseCompressedBoard(compressed, reparsedBoard);
    for (int i = 0; i < 20; i++) {
      CHECK(reparsedBoard[i] == board[i]);
    }
    CHECK(formatBoard(reparsedBoard) == compressed);
  }
}

TEST_CASE("parseCompressedBoard rejects malformed strings without touching the board") {
  unsigned int board[20];
  for (int i = 0; i < 20; i++) {
    board[i] = 7;
  }

  parseCompressedBoard("15,FqF,3", board);   // Odd number of letters
  parseCompressedBoard("15,FqFG,3", board);  // Letter outside the encoding
  parseCompressedBoard("15FqFC3", board);    // Missing commas
  parseCompressedBoard("14,FqFC,3", board);  // Only describes 19 rows
  parseCompressedBoard("16,FqFC,3", board);  // Describes 21 rows
  for (int i = 0; i < 20; i++) {
    CHECK(board[i] == 7);
  }
}

#endif // SR_BOARD_FORMAT_TESTS
