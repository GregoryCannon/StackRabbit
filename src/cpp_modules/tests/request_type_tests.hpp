// SECTION B: per request type tests.
//
// Header-only: included by stackrabbit_test.cpp. One TEST_CASE per request
// type, asserting the structure of each output format, plus a matrix test
// covering all types across representative boards.

#ifndef SR_REQUEST_TYPE_TESTS
#define SR_REQUEST_TYPE_TESTS

TEST_CASE("GET_MOVE returns a sane [rotation, xOffset, yOffset] triple") {
  auto input =
      InputBuilder().withBoard1(makeFlatBoard(4)).withLevel(19).withCurPiece(IDX_I).build();
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
  auto input = InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O).build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP, input);

  REQUIRE(output.size() >= 2);
  CHECK(output[0] == '{');
  CHECK(output[output.size() - 1] == '}');
  // Keys are lock positions encoded as "rot|x|y" (encodeLockPosition)
  CHECK(output.find('|') != std::string::npos);
}

TEST_CASE("GET_LOCK_VALUE_LOOKUP supports tournament mode via pruningBreadth == -1") {
  // In tournament mode (pruningBreadth == -1), playoutCount is the TOTAL playout budget
  // and every first placement gets a candidate with a playout-backed estimate.
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withCurPiece(IDX_O)
                   .withPruningBreadth(-1)
                   .withPlayoutCount(2000)
                   .build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP, input);

  REQUIRE(output.size() >= 2);
  CHECK(output[0] == '{');
  CHECK(output[output.size() - 1] == '}');
  CHECK(output.find('|') != std::string::npos);
}

TEST_CASE("GET_LOCK_VALUE_LOOKUP_DAS supports tournament mode via pruningBreadth == -1") {
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(4))
                   .withCurPiece(IDX_O)
                   .withPruningBreadth(-1)
                   .withPlayoutCount(2000)
                   .build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, input);

  REQUIRE(output.size() >= 2);
  CHECK(output[0] == '[');
  CHECK(output[output.size() - 1] == ']');
  // Three sub-maps, separated by ",\n"
  CHECK(countOccurrences(output, ",\n") == 2);
  CHECK(output.find('|') != std::string::npos);
}

TEST_CASE("GET_LOCK_VALUE_LOOKUP_DAS returns an array of three maps") {
  // Per getLockValueLookupDas: [partialChargePenalties, minChargePenalties, deepValues]
  auto input = InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O).build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, input);

  REQUIRE(output.size() >= 2);
  CHECK(output[0] == '[');
  CHECK(output[output.size() - 1] == ']');
  // Three sub-maps, separated by ",\n"
  CHECK(countOccurrences(output, ",\n") == 2);
  // Keys are lock positions encoded as "rot|x|y" (encodeLockPosition)
  CHECK(output.find('|') != std::string::npos);
}

TEST_CASE("GET_LOCK_VALUE_LOOKUP_DAS keeps its 3-map shape in DAS mode (dasCharge >= 0)") {
  // The bucket structure is charge-agnostic (the buckets exist precisely because
  // the player's charge is unknown); dasCharge only tells the C++ side that DAS
  // mode is active so the searches run through dasMoveSearch.
  for (int dasCharge : {0, 5, 10, 16}) {
    auto input = InputBuilder()
                     .withBoard1(makeFlatBoard(4))
                     .withCurPiece(IDX_O)
                     .withDasCharge(dasCharge)
                     .build();
    std::string output = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, input);

    REQUIRE(output.size() >= 2);
    CHECK(output[0] == '[');
    CHECK(output[output.size() - 1] == ']');
    CHECK(countOccurrences(output, ",\n") == 2);
    CHECK(output.find('|') != std::string::npos);
  }
}

/** Extracts the numeric value stored for `key` in a "{...}" penalty/value map. */
static float lookupMapValue(const std::string &mapJson, const std::string &key) {
  size_t keyPos = mapJson.find("\"" + key + "\":");
  REQUIRE(keyPos != std::string::npos);
  size_t valueStart = keyPos + key.size() + 3; // skip "key":
  size_t valueEnd = mapJson.find_first_of(",}", valueStart);
  REQUIRE(valueEnd != std::string::npos);
  return std::stof(mapJson.substr(valueStart, valueEnd - valueStart));
}

TEST_CASE("GET_LOCK_VALUE_LOOKUP_DAS penalties never reward losing DAS (regression)") {
  // Reproduces a positive-penalty report: for the first lock cell "3|-2|9" the
  // full-charge bucket reached the second cell (3,-1,12) as a TUCK while the
  // partial/min-charge buckets reached the identical cell as a plain DAS drop
  // that slid under the first piece's overhang mid-fall. advanceGameState only
  // applied its under-overhang surface clamp to tuck placements, so the plain
  // drop's surface was pushed below the overhang and the cavity it leaves was
  // never counted -- the identical board scored ~1.3 partial holes BETTER in the
  // low-charge buckets, producing positive penalties (+32.05 / +150.05).
  // Penalties are added to lock values by the TS consumer (getDasPenalty in
  // precompute.ts), so a positive penalty rewards losing DAS. The clamp now
  // applies to every placement (a piece can never lower a column's surface),
  // making the eval path-independent for the same board.
  auto input = InputBuilder()
                   .withBoard1("00000000000000000000000000000000000000000000000000"
                       "00000000000000000000000000000000000000000000000000"
                       "00000000000000001000100001110010000111001000011100"
                       "10111111001111111110111111111011111111101111111110")
                   .withLevel(19)
                   .withLines(22)
                   .withCurPiece(IDX_J)
                   .withNextPiece(IDX_L)
                   .withTimeline("X..")
                   .withPlayoutCount(0) // penalty maps are built from 0-playout shallow
                                        // lookups regardless of the budget, so this
                                        // still exercises the bucket math cheaply
                   .withPruningBreadth(102)
                   .withAggression("aggro")
                   .withDasCharge(16)
                   .build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, input);

  REQUIRE(output.size() >= 2);
  REQUIRE(output[0] == '[');
  REQUIRE(output[output.size() - 1] == ']');
  size_t sep1 = output.find(",\n");
  size_t sep2 = output.find(",\n", sep1 + 1);
  REQUIRE(sep1 != std::string::npos);
  REQUIRE(sep2 != std::string::npos);
  std::string partialPenalties = output.substr(1, sep1 - 1);
  std::string minPenalties = output.substr(sep1 + 2, sep2 - sep1 - 2);
  // The reported regression key must not carry a positive penalty in either map.
  CHECK(lookupMapValue(partialPenalties, "3|-2|9") <= 0.0f);
  CHECK(lookupMapValue(minPenalties, "3|-2|9") <= 0.0f);
}

TEST_CASE("GET_MOVE_AB_TEST always reports agree/disagree with an explanation") {
  auto input = InputBuilder().build();
  std::string output = runRequest(GET_MOVE_AB_TEST, input);

  bool isAgree = output.rfind("[\"agree\"", 0) == 0;
  bool isDisagree = output.rfind("[\"disagree\"", 0) == 0;
  CHECK((isAgree || isDisagree));
  CHECK(output.find("\"explanationMain\"") != std::string::npos);
}

TEST_CASE("searchDepth2 rejects dasCharge + secondPieceDasCharge passed together") {
  // The two DAS modes are mutually exclusive (programmer-error guard in searchDepth2):
  // dasCharge = generic DAS search (second piece inherits dasChargeAfter);
  // secondPieceDasCharge = DAS bucket hypothesis (second piece's charge prescribed).
  GameState g = {};
  g.level = 18;
  g.lines = 0;
  std::string board = makeFlatBoard(4);
  encodeBoard(board.c_str(), g.board);
  getSurfaceArray(g.board, g.surfaceArray);
  updateSurfaceAndHoles(g.surfaceArray, g.board, 9, false);
  const PieceRangeContext contexts[4] = {
      getPieceRangeContext("X..", 1, /* gravityDoubled= */ true),
      getPieceRangeContext("X..", 1, /* gravityDoubled= */ false),
      getPieceRangeContext("X..", 2, /* gravityDoubled= */ false),
      getPieceRangeContext("X..", 3, /* gravityDoubled= */ false),
  };
  const EvalContext ctx = getEvalContext(g, contexts, /* useTestWeights= */ false, SAFE);
  const Piece *J = &PIECE_LIST[IDX_J];
  const Piece *I = &PIECE_LIST[IDX_I];

  list<Possibility> rejected;
  CHECK(searchDepth2(g, J, I, /* dasCharge= */ 16, /* secondPieceDasCharge= */ 10, "X..", &ctx,
                     rejected)
        == 0);
  CHECK(rejected.size() == 0);

  list<Possibility> genericMode;
  CHECK(searchDepth2(g, J, I, /* dasCharge= */ 16, /* secondPieceDasCharge= */ -1, "X..", &ctx,
                     genericMode)
        > 0);
  list<Possibility> bucketMode;
  CHECK(searchDepth2(g, J, I, /* dasCharge= */ -1, /* secondPieceDasCharge= */ 10, "X..", &ctx,
                     bucketMode)
        > 0);
  list<Possibility> tapMode;
  CHECK(searchDepth2(g, J, I, /* dasCharge= */ -1, /* secondPieceDasCharge= */ -1, "X..", &ctx,
                     tapMode)
        > 0);
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
