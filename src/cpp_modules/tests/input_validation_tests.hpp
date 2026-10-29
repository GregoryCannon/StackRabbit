// SECTION A: input parsing & validation tests.
//
// Header-only: included by stackrabbit_test.cpp. Everything about the input
// string parsing that mainProcess does before dispatching to a request type.

#ifndef SR_INPUT_VALIDATION_TESTS
#define SR_INPUT_VALIDATION_TESTS

TEST_CASE("A missing current piece returns an error message") {
  auto input = InputBuilder().withCurPiece(-1).build();
  CHECK(runRequest(GET_MOVE, input) ==
        "Error: please provide a value for currentPiece.");
  CHECK(runRequest(GET_TOP_MOVES, input) ==
        "Error: please provide a value for currentPiece.");
}

TEST_CASE("A missing next piece (-1) is valid and runs in no-next-box mode") {
  auto input = InputBuilder().withNextPiece(-1).build();
  std::string output = runRequest(GET_MOVE, input);
  int rot = 0, xOffset = 0, yOffset = 0;
  parseMoveTriple(output, rot, xOffset, yOffset);
  CHECK(rot >= 0);
  CHECK(rot <= 3);
}

TEST_CASE("Second board is accepted for RATE_MOVE and enables adjustment keys") {
  // Player placed the I piece vertically on top of the leftmost column.
  auto playerBoard = makeBoardWithStackedColumn(4, 4, 0);
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withBoard2(playerBoard)
                   .withTimeline("X.X.X.X.X.X.X.X.") // generous taps so the stack is reachable
                   .build();
  std::string output = runRequest(RATE_MOVE, input);
  // formatRateMove includes the "*AfterAdjustment" keys only when a next box is available
  CHECK(output.find("\"playerMoveNoAdjustment\"") != std::string::npos);
  CHECK(output.find("\"bestMoveNoAdjustment\"") != std::string::npos);
  CHECK(output.find("\"playerMoveAfterAdjustment\"") != std::string::npos);
  CHECK(output.find("\"bestMoveAfterAdjustment\"") != std::string::npos);
}

TEST_CASE("RATE_MOVE with an unreachable player board reports that the move was not found") {
  // A board differing from the surface by a single cell cannot result from
  // any I placement, so rateMove bails out with its documented error string.
  auto playerBoard = makeBoardWithStackedColumn(4, 1, 9);
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withBoard2(playerBoard)
                   .build();
  CHECK(runRequest(RATE_MOVE, input) == "Error: player move not found");
}

TEST_CASE("RATE_MOVE without a second board is rejected with an error") {
  // Regression: this used to slice past the end of the input buffer (segfault).
  auto input = InputBuilder().build();
  CHECK(runRequest(RATE_MOVE, input) == "Error: RATE_MOVE requests require a value for secondBoard.");
}

TEST_CASE("A board string shorter than 200 characters is rejected with an error") {
  // Regression: short inputs used to cause the arg slicing to read out of bounds.
  CHECK(runRequest(GET_MOVE, "123") == "Error: invalid board string (expected 200 characters of 0/1).");
}

TEST_CASE("Non-numeric numeric fields are silently parsed as 0 (characterization)") {
  // Replace the level/lines fields with junk; atoi() yields 0 for both.
  auto input = InputBuilder()
                   .withLevel(0)
                   .withLines(0)
                   .build();
  size_t firstPipe = input.find('|');
  size_t secondPipe = input.find('|', firstPipe + 1);
  size_t thirdPipe = input.find('|', secondPipe + 1);
  std::string junkInput =
      input.substr(0, firstPipe + 1) + "abc|xyz|" + input.substr(thirdPipe + 1);

  std::string output = runRequest(GET_MOVE, junkInput);
  int rot = 0, xOffset = 0, yOffset = 0;
  parseMoveTriple(output, rot, xOffset, yOffset);
  CHECK(xOffset >= -8);
  CHECK(xOffset <= 8);
}

TEST_CASE("Unknown aggression levels default to SAFE (characterization)") {
  // Use a board one cell short of a line clear: the burn coefficient makes
  // the aggression level observable there (on an empty board it is not).
  auto burnBoard = makeBoardOneCellShort(4, 0);
  auto safeInput = InputBuilder().withBoard1(burnBoard).withAggression("safe").build();
  auto garbageInput = InputBuilder().withBoard1(burnBoard).withAggression("banana").build();
  auto aggroInput = InputBuilder().withBoard1(burnBoard).withAggression("aggro").build();

  std::string safeOutput = runRequest(GET_LOCK_VALUE_LOOKUP, safeInput);
  std::string garbageOutput = runRequest(GET_LOCK_VALUE_LOOKUP, garbageInput);
  std::string aggroOutput = runRequest(GET_LOCK_VALUE_LOOKUP, aggroInput);

  // Unknown strings behave exactly like "safe", and "aggro" (different
  // burnCoef) actually changes the output.
  CHECK(garbageOutput == safeOutput);
  CHECK(aggroOutput != safeOutput);
}

#endif // SR_INPUT_VALIDATION_TESTS
