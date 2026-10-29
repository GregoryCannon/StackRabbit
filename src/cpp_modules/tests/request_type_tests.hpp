// SECTION B: per request type tests.
//
// Header-only: included by stackrabbit_test.cpp. One TEST_CASE per request
// type, asserting the structure of each output format, plus a matrix test
// covering all types across representative boards.

#ifndef SR_REQUEST_TYPE_TESTS
#define SR_REQUEST_TYPE_TESTS

TEST_CASE("GET_MOVE returns a sane [rotation, xOffset, yOffset] triple") {
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withLevel(19)
                   .withCurPiece(IDX_I)
                   .build();
  std::string output = runRequest(GET_MOVE, input);
  int rot = 0, xOffset = 0, yOffset = 0;
  parseMoveTriple(output, rot, xOffset, yOffset);
  CHECK(rot >= 0);
  CHECK(rot <= 3);
  CHECK(xOffset >= -8);
  CHECK(xOffset <= 8);
  CHECK(yOffset >= 0);
  CHECK(yOffset <= 19);
}

TEST_CASE("GET_MOVE is deterministic across repeated calls") {
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withLevel(18)
                   .withCurPiece(IDX_T)
                   .withNextPiece(IDX_S)
                   .build();
  std::string first = runRequest(GET_MOVE, input);
  std::string second = runRequest(GET_MOVE, input);
  CHECK(first == second);
}

TEST_CASE("GET_TOP_MOVES returns a full JSON move list with the right shape") {
  auto input = InputBuilder().build(); // empty board, I piece, O next, level 18
  std::string output = runRequest(GET_TOP_MOVES, input);

  CHECK(output[0] == '[');
  CHECK(output[output.size() - 1] == ']');
  // One entry per engine move, up to NUM_ENGINE_MOVES (8)
  int numMoves = countOccurrences(output, "{\"firstPlacement\"");
  CHECK(numMoves == 8);
  // Every entry carries both scores, an explanation, and 7 playout objects
  CHECK(countOccurrences(output, "\"playoutScore\"") == 8);
  CHECK(countOccurrences(output, "\"shallowEvalScore\"") == 8);
  CHECK(countOccurrences(output, "\"evalExplanation\"") == 8);
  CHECK(countOccurrences(output, "\"playout7\"") == 8);
}

TEST_CASE("GET_TOP_MOVES includes the player's move when a second board is given") {
  // Player placed the I piece vertically on top of the leftmost column.
  auto playerBoard = makeBoardWithStackedColumn(4, 4, 0);
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withBoard2(playerBoard)
                   .withTimeline("X.X.X.X.X.X.X.X.") // generous taps so the stack is reachable
                   .build();
  std::string output = runRequest(GET_TOP_MOVES, input);
  CHECK(output.find("\"isPlayerMove\":true") != std::string::npos);
}

TEST_CASE("GET_TOP_MOVES_HYBRID returns both noNextBox and nextBox lists") {
  auto input = InputBuilder().build();
  std::string output = runRequest(GET_TOP_MOVES_HYBRID, input);

  CHECK(output.find("\"noNextBox\":") == 1); // immediately after the opening brace
  CHECK(output.find("\"nextBox\":") != std::string::npos);
  // NUM_ENGINE_MOVES entries without next box + NUM_ENGINE_MOVES_ADJUSTMENTS with
  int noNextCount = countOccurrences(output, "{\"firstPlacement\"");
  CHECK(noNextCount == 8 + 3);
}

TEST_CASE("GET_LOCK_VALUE_LOOKUP returns a JSON map of lock positions to scores") {
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withCurPiece(IDX_O)
                   .build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP, input);

  REQUIRE(output.size() >= 2);
  CHECK(output[0] == '{');
  CHECK(output[output.size() - 1] == '}');
  // Keys are lock positions encoded as "rot|x|y" (encodeLockPosition)
  CHECK(output.find('|') != std::string::npos);
}

TEST_CASE("GET_LOCK_VALUE_LOOKUP_DAS returns an array of three maps") {
  // Per getLockValueLookupDas: [partialChargePenalties, minChargePenalties, deepValues]
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withCurPiece(IDX_O)
                   .build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, input);

  REQUIRE(output.size() >= 2);
  CHECK(output[0] == '[');
  CHECK(output[output.size() - 1] == ']');
  // Three sub-maps, separated by ",\n"
  CHECK(countOccurrences(output, ",\n") == 2);
  // Keys are lock positions encoded as "rot|x|y" (encodeLockPosition)
  CHECK(output.find('|') != std::string::npos);
}

TEST_CASE("GET_MOVE_AB_TEST always reports agree/disagree with an explanation") {
  auto input = InputBuilder().build();
  std::string output = runRequest(GET_MOVE_AB_TEST, input);

  bool isAgree = output.rfind("[\"agree\"", 0) == 0;
  bool isDisagree = output.rfind("[\"disagree\"", 0) == 0;
  CHECK((isAgree || isDisagree));
  CHECK(output.find("\"explanationMain\"") != std::string::npos);
}

TEST_CASE("All request types complete on empty, tall, and notched boards") {
  std::string boards[3] = {
      makeEmptyBoard(),
      makeFlatBoard(16),
      makeWellBoard(4, 4, 9),
  };
  RequestType types[4] = {GET_MOVE, GET_TOP_MOVES, GET_LOCK_VALUE_LOOKUP, RATE_MOVE};
  for (const auto &board : boards) {
    // Every type gets a player board: RATE_MOVE requires one (see findings in
    // TESTING.md -- without it the legacy fallback path reads out of bounds).
    // The player board is a vertical I dropped onto col 0 of the *current*
    // board, so the move is always reachable by the current piece (I).
    auto input = InputBuilder()
                     .withBoard1(board)
                     .withBoard2(withVerticalIOnColumn(board, 0))
                     .withTimeline("X.X.X.X.X.X.X.X.")
                     .withCurPiece(IDX_I)
                     .build();
    for (RequestType type : types) {
      std::string output = runRequest(type, input);
      CHECK(output.size() > 0);
      CHECK(output.find("Error") == std::string::npos);
    }
  }
}

#endif // SR_REQUEST_TYPE_TESTS
