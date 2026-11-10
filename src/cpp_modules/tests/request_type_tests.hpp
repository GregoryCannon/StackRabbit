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
  // mode is active so the searches run through fullMoveSearch.
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
  // low-charge buckets, producing positive penalties (+32.05 / +150.05). Fixed at
  // the source by clamping the surface in getNewSurfaceAndNumNewHoles for every
  // placement (a piece can never lower a column's surface), making the eval
  // path-independent for the same board. The penalty maps also clamp deltas at
  // zero (out-of-scope maneuvers must not be rewarded), which is why the
  // endpoint-level assertion below is <= 0 either way.
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

TEST_CASE("GET_LOCK_VALUE_LOOKUP_DAS clamps out-of-scope maneuver deltas to zero") {
  // The min-charge (5) bucket can reach maneuvers the full-charge search cannot,
  // via the DAS-reset stair-step (the first shift happens at spawn but resets the
  // charge to 0, so the piece descends deep between column crossings). On this
  // board that finds a Z tuck under the first piece's overhang (second cell
  // (1,-1,12)) that fills ~3.2 partial holes -- an eval ~146 points better than
  // anything full charge can do for first lock cell "0|-1|9", i.e. a POSITIVE
  // penalty of +146.62. These are valid-but-out-of-scope maneuvers (double-tuck
  // chains the engine deliberately omits), so the maps clamp the delta at zero
  // instead of rewarding them -- and the request must complete rather than throw
  // (an earlier fail-fast guard threw here).
  auto input = InputBuilder()
                   .withBoard1("00000000000000000000000000000000000000000000000000"
                       "00000000000000000000000000000000000000000000000000"
                       "00000000000000000000100000000010000000001010010000"
                       "11111110001111111110111111111011111111101111111110")
                   .withLevel(19)
                   .withLines(6)
                   .withCurPiece(IDX_S)
                   .withNextPiece(IDX_Z)
                   .withTimeline("X..")
                   .withPlayoutCount(0) // penalty maps are eval-only regardless of budget
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
  // The stair-step tuck inflated the min-charge delta to +146.62; it must be clamped.
  CHECK(lookupMapValue(minPenalties, "0|-1|9") == 0.0f);
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
    // TESTING.md -- without it the fallback path reads out of bounds).
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

// --- PRECOMPUTE_BACKUP ----------------------------------------------------
//
// The backup response: the best-eval placement plus keep-playing-the-default
// markers for every next piece; served when the full precompute has not
// finished by poll time. The pipes here follow the precompute format: the
// standard 10 fields plus reactionTime (nextPiece is unused, -1), so they are
// built straight from InputBuilder's 10 fields + "<reactionTime>|".

/** The backup-relevant request state, rebuilt the way mainProcess builds it. */
struct BackupProbe {
  GameState gameState;
  EvalContext evalContext;
};

// Mirrors main.cpp's PRECOMPUTE/BACKUP setup: board parse, surface/holes, the
// piece-range and eval contexts. In tap mode the timeline is the request's
// own; in DAS mode the caller passes the derived DAS-equivalent timeline
// (main.cpp keeps the real one for the searches only).
static BackupProbe probeStateFrom(const std::string &boardStr, int level, int lines,
                                  const std::string &timeline, std::string aggression) {
  BackupProbe probe;
  probe.gameState = GameState{};
  encodeBoard(boardStr.c_str(), probe.gameState.board);
  probe.gameState.level = level;
  probe.gameState.lines = lines;
  getSurfaceArray(probe.gameState.board, probe.gameState.surfaceArray);
  std::pair<int, float> holes = updateSurfaceAndHoles(probe.gameState.surfaceArray,
                                                      probe.gameState.board,
                                                      /* wellColumn= */ 9, /* isDigMode= */ false);
  probe.gameState.numTrueHoles = holes.first;
  probe.gameState.numPartialHoles = holes.second;
  PieceRangeContext contexts[4] = {
      getPieceRangeContext(timeline.c_str(), 1, /* gravityDoubled= */ true),
      getPieceRangeContext(timeline.c_str(), 1, /* gravityDoubled= */ false),
      getPieceRangeContext(timeline.c_str(), 2, /* gravityDoubled= */ false),
      getPieceRangeContext(timeline.c_str(), 3, /* gravityDoubled= */ false),
  };
  AggressionLevel aggressionLevel = SAFE;
  if (aggression == "aggro") {
    aggressionLevel = AGGRO;
  } else if (aggression == "hyper") {
    aggressionLevel = HYPER;
  }
  probe.evalContext = getEvalContext(probe.gameState, contexts, /* useTestWeights= */ false,
                                     aggressionLevel);
  std::pair<int, float> holes2 =
      updateSurfaceAndHoles(probe.gameState.surfaceArray, probe.gameState.board,
                            probe.evalContext.countWellHoles ? -1 : probe.evalContext.wellColumn,
                            probe.evalContext.aiMode == DIG);
  probe.gameState.numTrueHoles = holes2.first;
  probe.gameState.numPartialHoles = holes2.second;
  return probe;
}

/**
 * The documented backup selection procedure, applied independently: score
 * every fullMoveSearch result with the depth-1 move choice's per-candidate
 * evaluation (post-lock state, then fastEval) and return the argmax index.
 * Ties keep the first result in search order (strict >).
 */
static int backupSelectionIndex(const BackupProbe &probe, const Piece *curPiece,
                                const std::vector<FullMovePossibility> &results) {
  int winner = -1;
  float bestScore = FLOAT_MIN;
  for (size_t i = 0; i < results.size(); i++) {
    const FullMovePossibility &result = results[i];
    LockPlacement placement = {result.x, result.y, result.rotationIndex, -1, result.tuckInput,
                               curPiece};
    GameState resultingState = advanceGameState(probe.gameState, placement, &probe.evalContext);
    float score = fastEval(probe.gameState, resultingState, placement, &probe.evalContext, nullptr);
    if (score > bestScore) {
      bestScore = score;
      winner = (int)i;
    }
  }
  REQUIRE(winner >= 0);
  return winner;
}

/** The "rot,x,y|" head of the Default line. */
static std::string defaultLineHead(const std::string &response) {
  size_t lineEnd = response.find('\n');
  REQUIRE(lineEnd != std::string::npos);
  return response.substr(0, lineEnd);
}

TEST_CASE("PRECOMPUTE_BACKUP returns the exact backup response for the obvious well-fill placement") {
  // A 4-high stack with a 4-deep well at column 9, I piece, tap mode: the only
  // placement that clears lines is the vertical I into the well, which clears
  // all four rows and leaves an empty board -- unambiguously the best placement.
  auto input = InputBuilder()
                   .withBoard1(makeWellBoard(4, 4, 9))
                   .withLevel(18)
                   .withLines(0)
                   .withCurPiece(IDX_I)
                   .withNextPiece(-1)
                   .withTimeline("X.X.X.X.X.X.X.X.")
                   .withPlayoutCount(1)
                   .withPlayoutLength(1)
                   .withPruningBreadth(4)
                   .withAggression("safe")
                   .withDasCharge(-1)
                   .build()
               + "10|";

  std::string expected = "Default:1,4,18|"
                         // The full input sequence (999 = no slicing): the rotate/shift
                         // inputs, then the search's fall/lock/wait notation.
                         "I.R.R.R" + std::string(50, '.') + "*****" + std::string(17, '^') + "*****"
                         + "~~|" + std::string(200, '0') // the well fill clears the whole board
                         + "|18|4|-1"
                         "\nI:continue"
                         "\nO:continue"
                         "\nL:continue"
                         "\nJ:continue"
                         "\nT:continue"
                         "\nS:continue"
                         "\nZ:continue";
  std::string output = runRequest(PRECOMPUTE_BACKUP, input);
  CHECK(output == expected);
  // Deterministic: the same request answers the same bytes.
  CHECK(runRequest(PRECOMPUTE_BACKUP, input) == expected);
}

TEST_CASE("PRECOMPUTE_BACKUP picks the best-eval placement, not the search's first result") {
  auto input = InputBuilder()
                   .withBoard1(makeWellBoard(4, 4, 9))
                   .withLevel(18)
                   .withLines(0)
                   .withCurPiece(IDX_I)
                   .withNextPiece(-1)
                   .withTimeline("X.X.X.X.X.X.X.X.")
                   .withPlayoutCount(1)
                   .withPlayoutLength(1)
                   .withPruningBreadth(4)
                   .withAggression("safe")
                   .withDasCharge(-1)
                   .build()
               + "10|";

  BackupProbe probe = probeStateFrom(makeWellBoard(4, 4, 9), 18, 0, "X.X.X.X.X.X.X.X.", "safe");
  const Piece *curPiece = &PIECE_LIST[IDX_I];
  std::vector<FullMovePossibility> results;
  fullMoveSearch(probe.gameState, curPiece, "X.X.X.X.X.X.X.X.", /* dasCharge= */ -1,
                 /* trackInputSequences= */ true, results, FullMoveSearchContext(),
                 /* reactionTime= */ 10, /* adjTimeCaptures= */ NULL);
  REQUIRE(results.size() > 1);

  int winner = backupSelectionIndex(probe, curPiece, results);
  // The premise of this pin: on this board the search's first result is a
  // naive left-edge placement that is NOT the best-eval placement.
  REQUIRE(winner != 0);

  std::string output = runRequest(PRECOMPUTE_BACKUP, input);
  std::string head = defaultLineHead(output);
  REQUIRE(head.rfind("Default:", 0) == 0);
  size_t firstComma = head.find(',');
  size_t secondComma = head.find(',', firstComma + 1);
  REQUIRE(firstComma != std::string::npos);
  REQUIRE(secondComma != std::string::npos);
  int relRot = std::stoi(head.substr(8, firstComma - 8));
  int relX = std::stoi(head.substr(firstComma + 1, secondComma - firstComma - 1));
  int relY = std::stoi(head.substr(secondComma + 1));

  // The chosen placement is the argmax under the documented procedure.
  const int numOrientations = 2; // the I piece
  const FullMovePossibility &best = results[winner];
  CHECK(relRot == (best.rotationIndex - 0 + numOrientations) % numOrientations);
  CHECK(relX == best.x - INITIAL_X);
  CHECK(relY == best.y - curPiece->initialY);
  // Hand-verifiable anchor: the well fill (vertical I at column 9, four lines
  // cleared) is what the procedure must find.
  CHECK(best.rotationIndex == 1);
  CHECK(best.x == 7);
  CHECK(best.numLinesCleared == 4);
}

TEST_CASE("PRECOMPUTE_BACKUP breaks exact eval ties by keeping the first result in search order") {
  // On a 1-high flat stack the I piece's two middle horizontal placements land
  // on rows that score identically down to the bit (verified by the recompute
  // below); they are the top two scores, so the tie decides the winner.
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(1))
                   .withLevel(18)
                   .withLines(0)
                   .withCurPiece(IDX_I)
                   .withNextPiece(-1)
                   .withTimeline("X.X.X.X.X.X.X.X.")
                   .withPlayoutCount(1)
                   .withPlayoutLength(1)
                   .withPruningBreadth(4)
                   .withAggression("safe")
                   .withDasCharge(-1)
                   .build()
               + "10|";

  BackupProbe probe = probeStateFrom(makeFlatBoard(1), 18, 0, "X.X.X.X.X.X.X.X.", "safe");
  const Piece *curPiece = &PIECE_LIST[IDX_I];
  std::vector<FullMovePossibility> results;
  fullMoveSearch(probe.gameState, curPiece, "X.X.X.X.X.X.X.X.", /* dasCharge= */ -1,
                 /* trackInputSequences= */ true, results, FullMoveSearchContext(),
                 /* reactionTime= */ 10, /* adjTimeCaptures= */ NULL);
  REQUIRE(results.size() >= 4);

  int winner = backupSelectionIndex(probe, curPiece, results);
  // The premise: the winner's score is exactly tied (bit-for-bit) with a LATER
  // result's score, so first-in-search-order is what decides.
  REQUIRE(winner >= 0);
  REQUIRE((size_t)winner + 1 < results.size());
  {
    const FullMovePossibility &a = results[winner];
    LockPlacement pa = {a.x, a.y, a.rotationIndex, -1, a.tuckInput, curPiece};
    GameState sa = advanceGameState(probe.gameState, pa, &probe.evalContext);
    float scoreA = fastEval(probe.gameState, sa, pa, &probe.evalContext, nullptr);
    const FullMovePossibility &b = results[winner + 1];
    LockPlacement pb = {b.x, b.y, b.rotationIndex, -1, b.tuckInput, curPiece};
    GameState sb = advanceGameState(probe.gameState, pb, &probe.evalContext);
    float scoreB = fastEval(probe.gameState, sb, pb, &probe.evalContext, nullptr);
    unsigned int bitsA, bitsB;
    memcpy(&bitsA, &scoreA, sizeof(bitsA));
    memcpy(&bitsB, &scoreB, sizeof(bitsB));
    REQUIRE(bitsA == bitsB);
  }

  // x=2 (the earlier result) wins over its bit-identical successor x=3. The
  // response's post-lock board rendering clears every full row, so the
  // pre-existing full bottom row burns: the piece's row shifts down into row
  // 19 and the line count rises to 1.
  std::string head = defaultLineHead(runRequest(PRECOMPUTE_BACKUP, input));
  CHECK(head == "Default:0,-1,18|" + std::string("L") + std::string(56, '.')
                     + std::string(5, '*') + std::string(17, '^') + std::string(5, '*') + "~~|"
                     + std::string(190, '0') + "0011110000|18|1|-1");
}

TEST_CASE("PRECOMPUTE_BACKUP returns exactly none when the piece has no legal placement") {
  // A fully topped-out board leaves no spawn cell free.
  auto input = InputBuilder()
                   .withBoard1(makeFlatBoard(20))
                   .withLevel(18)
                   .withLines(0)
                   .withCurPiece(IDX_I)
                   .withNextPiece(-1)
                   .withTimeline("X...")
                   .withPlayoutCount(1)
                   .withPlayoutLength(1)
                   .withPruningBreadth(4)
                   .withAggression("safe")
                   .withDasCharge(-1)
                   .build()
               + "10|";
  CHECK(runRequest(PRECOMPUTE_BACKUP, input) == "none");
}

TEST_CASE("PRECOMPUTE_BACKUP's 999 reaction time puts the full sequence in the Default line") {
  // Tap mode: the whole sequence (lock/wait markers included) sits before the
  // ~~ separator, nothing after it.
  auto tapInput = InputBuilder()
                      .withBoard1(makeWellBoard(4, 4, 9))
                      .withLevel(18)
                      .withLines(0)
                      .withCurPiece(IDX_I)
                      .withNextPiece(-1)
                      .withTimeline("X.X.X.X.X.X.X.X.")
                      .withPlayoutCount(1)
                      .withPlayoutLength(1)
                      .withPruningBreadth(4)
                      .withAggression("safe")
                      .withDasCharge(-1)
                      .build()
                  + "10|";
  {
    BackupProbe probe = probeStateFrom(makeWellBoard(4, 4, 9), 18, 0, "X.X.X.X.X.X.X.X.", "safe");
    const Piece *curPiece = &PIECE_LIST[IDX_I];
    std::vector<FullMovePossibility> results;
    fullMoveSearch(probe.gameState, curPiece, "X.X.X.X.X.X.X.X.", -1, true, results,
                   FullMoveSearchContext(), 10, NULL);
    const FullMovePossibility &best = results[backupSelectionIndex(probe, curPiece, results)];

    std::string defaultLine = defaultLineHead(runRequest(PRECOMPUTE_BACKUP, tapInput));
    std::string expectedLine =
        "Default:1,4,18|" + best.inputSequence + "~~|" + std::string(200, '0') + "|18|4|-1";
    CHECK(defaultLine == expectedLine);
    CHECK(countOccurrences(defaultLine, "~~") == 1);
  }

  // DAS mode: the sequence carries the '_' hold-through-ARE marker (the branch
  // whose slice index would be reactionTime + 1) and lowercase held inputs;
  // the same no-slicing outcome applies.
  auto dasInput = InputBuilder()
                      .withBoard1(makeWellBoard(4, 4, 9))
                      .withLevel(18)
                      .withLines(0)
                      .withCurPiece(IDX_I)
                      .withNextPiece(-1)
                      .withTimeline("X.....")
                      .withPlayoutCount(1)
                      .withPlayoutLength(1)
                      .withPruningBreadth(4)
                      .withAggression("safe")
                      .withDasCharge(16)
                      .build()
                  + "10|";
  {
    std::string dasEquivalent = getDasEquivalentInputTimeline("X.....");
    BackupProbe probe = probeStateFrom(makeWellBoard(4, 4, 9), 18, 0, dasEquivalent, "safe");
    const Piece *curPiece = &PIECE_LIST[IDX_I];
    std::vector<FullMovePossibility> results;
    fullMoveSearch(probe.gameState, curPiece, "X.....", /* dasCharge= */ 16, true, results,
                   FullMoveSearchContext(), 10, NULL);
    const FullMovePossibility &best = results[backupSelectionIndex(probe, curPiece, results)];

    std::string defaultLine = defaultLineHead(runRequest(PRECOMPUTE_BACKUP, dasInput));
    std::string expectedLine = "Default:1,4,18|" + best.inputSequence + "~~|" + std::string(200, '0')
                               + "|18|4|16";
    CHECK(defaultLine == expectedLine);
    CHECK(countOccurrences(defaultLine, "~~") == 1);
    // The hold branch is genuinely exercised: the sequence holds through ARE.
    CHECK(best.inputSequence.find('_') != std::string::npos);
  }
}

TEST_CASE("PRECOMPUTE_BACKUP shares PRECOMPUTE's reaction-time field validation") {
  auto base = InputBuilder()
                  .withBoard1(makeFlatBoard(4))
                  .withLevel(18)
                  .withLines(0)
                  .withCurPiece(IDX_O)
                  .withNextPiece(-1)
                  .withTimeline("X...")
                  .withPlayoutCount(1)
                  .withPlayoutLength(1)
                  .withPruningBreadth(4)
                  .withAggression("safe")
                  .withDasCharge(-1)
                  .build()
              + "5|";
  // A 10-field pipe (no reactionTime) is rejected identically by both types.
  std::string tenField = base.substr(0, base.size() - 2);
  REQUIRE(tenField.back() == '|');
  std::string missingError = "Error: please provide a value for reactionTime.";
  CHECK(runRequest(PRECOMPUTE_BACKUP, tenField) == missingError);
  CHECK(runRequest(PRECOMPUTE, tenField) == missingError);

  SUBCASE("junk reaction time") {
    std::string junk = base.substr(0, base.size() - 2) + "junk|";
    std::string junkError =
        "Error: invalid reactionTime value (expected an integer between 0 and 60): junk";
    CHECK(runRequest(PRECOMPUTE_BACKUP, junk) == junkError);
    CHECK(runRequest(PRECOMPUTE, junk) == junkError);
  }
  SUBCASE("reaction time above 60") {
    std::string over = base.substr(0, base.size() - 2) + "61|";
    std::string overError =
        "Error: reaction time exceeds the maximum possible of 60 frames. Did you accidentally send "
        "it in milliseconds? 61";
    CHECK(runRequest(PRECOMPUTE_BACKUP, over) == overError);
    CHECK(runRequest(PRECOMPUTE, over) == overError);
  }
}

#endif // SR_REQUEST_TYPE_TESTS
