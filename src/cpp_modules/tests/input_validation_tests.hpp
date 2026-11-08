// SECTION A: input parsing & validation tests.
//
// Header-only: included by stackrabbit_test.cpp. Everything about the input
// string parsing that mainProcess does before dispatching to a request type.

#ifndef SR_INPUT_VALIDATION_TESTS
#define SR_INPUT_VALIDATION_TESTS

TEST_CASE("A missing current piece returns an error message") {
  auto input = InputBuilder().withCurPiece(-1).build();
  CHECK(runRequest(GET_MOVE, input) == "Error: please provide a value for currentPiece.");
  CHECK(runRequest(GET_TOP_MOVES, input) == "Error: please provide a value for currentPiece.");
}

TEST_CASE("Out-of-range piece indices are rejected for every request type") {
  // Before the shared range check existed, PIECE_LIST[argAsInt] read out of
  // bounds for any index above 6 on the non-PRECOMPUTE paths.
  auto input = InputBuilder().withCurPiece(9).build();
  CHECK(runRequest(GET_MOVE, input) == "Error: unknown current piece: 9");
  CHECK(runRequest(GET_TOP_MOVES, input) == "Error: unknown current piece: 9");
  CHECK(runRequest(GET_LOCK_VALUE_LOOKUP, input) == "Error: unknown current piece: 9");
  auto nextInput = InputBuilder().withNextPiece(7).build();
  CHECK(runRequest(GET_MOVE, nextInput) == "Error: unknown next piece: 7");
  CHECK(runRequest(GET_LOCK_VALUE_LOOKUP, nextInput) == "Error: unknown next piece: 7");
}

TEST_CASE("Engine-level range limits apply to every request type") {
  auto lowLevel = InputBuilder().withLevel(17).build();
  CHECK(runRequest(GET_MOVE, lowLevel) ==
        "Error: currently only 18, 19, and 29 starts are supported by StackRabbit. Requested: 17");
  auto badLength = InputBuilder().withPlayoutLength(21).build();
  CHECK(runRequest(GET_MOVE, badLength) ==
        "Error: invalid playout length (max is 20): 21");
  auto badCharge = InputBuilder().withDasCharge(17).build();
  CHECK(runRequest(GET_MOVE, badCharge) ==
        "Error: illegal DAS charge (expected -1 for tap mode, or 0-16): 17");
  auto badBreadth = InputBuilder().withPruningBreadth(1157).build();
  CHECK(runRequest(GET_MOVE, badBreadth) ==
        "Error: invalid pruning breadth (max is 1156, or 34x34): 1157");
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
  auto input = InputBuilder().withBoard1(makeFlatBoard(4)).withBoard2(playerBoard).build();
  CHECK(runRequest(RATE_MOVE, input) == "Error: player move not found");
}

TEST_CASE("RATE_MOVE without a second board is rejected with an error") {
  // Regression: this used to slice past the end of the input buffer (segfault).
  auto input = InputBuilder().build();
  CHECK(runRequest(RATE_MOVE, input)
        == "Error: RATE_MOVE requests require a value for secondBoard.");
}

TEST_CASE("A board string shorter than 200 characters is rejected with an error") {
  // Regression: short inputs used to cause the arg slicing to read out of bounds.
  CHECK(runRequest(GET_MOVE, "123")
        == "Error: invalid board string (expected 200 characters of 0/1).");
}

TEST_CASE("Non-numeric numeric fields are rejected (was: silently parsed as 0)") {
  // Replace the level/lines fields with junk; this used to be characterized as
  // atoi()-to-0, but every field is now mandatory and strictly validated (a
  // silent 0 can mean a degenerate game state or a different piece).
  auto input = InputBuilder().withLevel(0).withLines(0).build();
  size_t firstPipe = input.find('|');
  size_t secondPipe = input.find('|', firstPipe + 1);
  size_t thirdPipe = input.find('|', secondPipe + 1);
  std::string junkInput = input.substr(0, firstPipe + 1) + "abc|xyz|" + input.substr(thirdPipe + 1);

  CHECK(runRequest(GET_MOVE, junkInput) == "Error: invalid level value (expected an integer): abc");
}

TEST_CASE("Unknown aggression levels are rejected (was: silently SAFE)") {
  // Use a board one cell short of a line clear: the burn coefficient makes
  // the aggression level observable there (on an empty board it is not).
  auto burnBoard = makeBoardOneCellShort(4, 0);
  auto safeInput = InputBuilder().withBoard1(burnBoard).withAggression("safe").build();
  auto garbageInput = InputBuilder().withBoard1(burnBoard).withAggression("banana").build();
  auto aggroInput = InputBuilder().withBoard1(burnBoard).withAggression("aggro").build();

  std::string safeOutput = runRequest(GET_LOCK_VALUE_LOOKUP, safeInput);
  std::string aggroOutput = runRequest(GET_LOCK_VALUE_LOOKUP, aggroInput);

  // Unknown strings are rejected (matching the TS boundary), and "aggro"
  // (different burnCoef) actually changes the output.
  CHECK(runRequest(GET_LOCK_VALUE_LOOKUP, garbageInput)
        == "Error: invalid aggressionLevel value (expected safe, aggro, or hyper): banana");
  CHECK(aggroOutput != safeOutput);
}

TEST_CASE("A missing dasCharge field is rejected (the field is mandatory)") {
  // Callers must state the DAS state explicitly: -1 for tap mode, 0..16 for DAS
  // mode. Inputs from before the dasCharge field existed are rejected.
  auto input = InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O).build();
  std::string legacyInput = input.substr(0, input.size() - 3); // strip trailing "-1|"
  REQUIRE(legacyInput.size() > 0);
  CHECK(legacyInput.back() == '|');

  std::string expectedError = "Error: please provide a value for dasCharge.";
  CHECK(runRequest(GET_LOCK_VALUE_LOOKUP, legacyInput) == expectedError);
  CHECK(runRequest(GET_MOVE, legacyInput) == expectedError);
  CHECK(runRequest(GET_LOCK_VALUE_LOOKUP_DAS, legacyInput) == expectedError);
}

TEST_CASE("A non-numeric dasCharge is rejected instead of silently becoming 0 (DAS mode)") {
  // atoi("junk") -> 0 would flip the engine into DAS mode, so this field is
  // validated strictly (the other numeric fields keep their characterized behavior).
  auto input = InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O).build();
  std::string junkInput = input.substr(0, input.size() - 3) + "junk|";
  CHECK(runRequest(GET_MOVE, junkInput)
        == "Error: invalid dasCharge value (expected -1 or 0-16): junk");
}

TEST_CASE("An empty currentPiece field is rejected (was: silently the I piece)") {
  // atoi("") -> 0 used to select PIECE_LIST[0] (the I piece) for free.
  auto input = InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O).build();
  size_t restStart = input.find('|') + 1;     // after board1 (no second board in this input)
  std::string rest = input.substr(restStart); // "level|lines|currentPiece|nextPiece|..."
  size_t firstPipe = rest.find('|');
  size_t secondPipe = rest.find('|', firstPipe + 1);
  size_t thirdPipe = rest.find('|', secondPipe + 1);
  // Blank out the currentPiece token (between the 2nd and 3rd pipes of the rest).
  std::string emptyPieceInput =
      input.substr(0, restStart) + rest.substr(0, secondPipe + 1) + rest.substr(thirdPipe);
  CHECK(runRequest(GET_MOVE, emptyPieceInput)
        == "Error: invalid currentPiece value (expected a piece index or -1): ");
}

TEST_CASE("Timeline fields must be non-empty, X/.-only, and contain at least one input frame") {
  auto base = InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O);
  // Empty timeline: the engine used to never tap (x % 0 == 0 on some platforms).
  CHECK(
      runRequest(GET_MOVE, base.withTimeline("").build())
      == "Error: invalid inputFrameTimeline (expected X/. frames with at least one input frame): ");
  // No input frames at all: the move search used to hang.
  CHECK(runRequest(GET_MOVE, base.withTimeline(".....").build())
        == "Error: invalid inputFrameTimeline (expected X/. frames with at least one input frame): "
           ".....");
  // Invalid characters (TS normalizes '-' to '.'; C++ receives the normalized form).
  CHECK(runRequest(GET_MOVE, base.withTimeline("X.-X").build())
        == "Error: invalid inputFrameTimeline (expected X/. frames with at least one input frame): "
           "X.-X");
}

TEST_CASE("Missing trailing fields report the first missing field") {
  auto input = InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O).build();
  // Drop the last two fields (aggressionLevel, dasCharge).
  std::string truncated = input.substr(0, input.size() - 8); // strip "safe|-1|"
  CHECK(truncated.back() == '|');
  CHECK(runRequest(GET_MOVE, truncated) == "Error: please provide a value for aggressionLevel.");
}

TEST_CASE("dasCharge >= 0 keeps the legacy tapper approximation for non-DAS request paths") {
  // A DAS-holding player's non-DAS requests must behave exactly as they did when
  // TS encoded the DAS-equivalent timeline directly: the C++ side now derives
  // that timeline itself from dasCharge (docs/engine_boundaries.md).
  auto dasInput = InputBuilder()
                      .withBoard1(makeFlatBoard(4))
                      .withCurPiece(IDX_O)
                      .withTimeline("X...") // quickTapDelay 2 -> "X.....X.....X..X....."
                      .withDasCharge(16)
                      .build();
  auto legacyEncodedInput = InputBuilder()
                                .withBoard1(makeFlatBoard(4))
                                .withCurPiece(IDX_O)
                                .withTimeline("X.....X.....X..X.....")
                                .withDasCharge(-1)
                                .build();
  CHECK(runRequest(GET_LOCK_VALUE_LOOKUP, dasInput)
        == runRequest(GET_LOCK_VALUE_LOOKUP, legacyEncodedInput));
  CHECK(runRequest(GET_MOVE, dasInput) == runRequest(GET_MOVE, legacyEncodedInput));
}

#endif // SR_INPUT_VALIDATION_TESTS
