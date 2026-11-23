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

TEST_CASE("GET_LOCK_VALUE_LOOKUP_DAS filters first-piece placements by dasCharge") {
  // When dasCharge >= 0, first-piece reachability starts from that charge; when -1,
  // it defaults to DAS_CHARGE_MAX. The three-map bucket shape always holds.
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

  // On a tall level-19 board with low incoming DAS (0), far-left placements reachable at full
  // charge (e.g. Z at 1|-1|11) are excluded from all three maps, whereas dasCharge == -1
  // falls back to DAS_CHARGE_MAX and includes them.
  const std::string tallBoard = "00000000000000000000000000000000000000000000000000"
                                "00000000000000000000000000000000000000000000000000"
                                "00000000001000000000100101100010111111111111111110"
                                "11111111101111111110111111111011111111101111111110";
  REQUIRE(tallBoard.size() == 200);
  auto makeTallInput = [&](int charge) {
    return InputBuilder()
        .withBoard1(tallBoard)
        .withLevel(19)
        .withLines(130)
        .withCurPiece(IDX_Z)
        .withNextPiece(IDX_J)
        .withTimeline("X.....")
        .withPlayoutCount(0)
        .withPlayoutLength(0)
        .withPruningBreadth(20)
        .withAggression("aggro")
        .withDasCharge(charge)
        .build();
  };
  std::string outZero = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, makeTallInput(0));
  std::string outFull = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, makeTallInput(16));
  std::string outDefault = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, makeTallInput(-1));

  CHECK(outZero.find("\"1|-1|11\"") == std::string::npos);
  CHECK(outFull.find("\"1|-1|11\"") != std::string::npos);
  CHECK(outDefault.find("\"1|-1|11\"") != std::string::npos);
  CHECK(outZero.find("\"1|2|10\"") != std::string::npos);
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

TEST_CASE("thirdPieceDasPenalty scales linearly below 15 and caps at charge 5") {
  CHECK(thirdPieceDasPenalty(-1) == doctest::Approx(0.0f));
  CHECK(thirdPieceDasPenalty(0) == doctest::Approx(-20.0f));
  CHECK(thirdPieceDasPenalty(4) == doctest::Approx(-20.0f));
  CHECK(thirdPieceDasPenalty(5) == doctest::Approx(-20.0f));
  CHECK(thirdPieceDasPenalty(6) == doctest::Approx(-18.0f));
  CHECK(thirdPieceDasPenalty(10) == doctest::Approx(-10.0f));
  CHECK(thirdPieceDasPenalty(14) == doctest::Approx(-2.0f));
  CHECK(thirdPieceDasPenalty(15) == doctest::Approx(0.0f));
  CHECK(thirdPieceDasPenalty(16) == doctest::Approx(0.0f));
}

TEST_CASE(
    "getLockValueLookupDasMaps penalizes third-piece DAS loss without mutating shallow lockValue") {
  // On a tall level-19 stack, a flat-dropped O leaves an open board where an O next piece
  // wants to drop near spawn or shift once; starting piece 2 at partial/min charge lowers
  // piece 2's ending DAS charge (penalizing piece 3) even when the same cell is reachable,
  // while shallow maps.lockValue (playoutCount = 0) stays equal to raw evalScoreInclReward.
  GameState g = {};
  g.level = 19;
  g.lines = 0;
  std::string board = makeFlatBoard(14);
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
  const Piece *O = &PIECE_LIST[IDX_O];

  const DasLockValueMaps maps = getLockValueLookupDasMaps(
      g, O, O, /* pruningBreadth= */ 20, /* playoutCount= */ 0,
      /* playoutLength= */ 0, /* firstPieceDasCharge= */ DAS_CHARGE_MAX, "X..", &ctx, contexts);

  // Verify shallow maps.lockValue equals unpenalized full-charge searchDepth2 max eval.
  std::vector<Possibility> rawFull;
  searchDepth2(g, O, O, /* firstPieceDasCharge= */ DAS_CHARGE_MAX, /* secondPieceDasCharge= */ 15,
               "X..", &ctx, rawFull);
  std::unordered_map<std::string, float> expectedRaw;
  for (const Possibility &p : rawFull) {
    const std::string key = encodeLockPosition(p.firstPlacement);
    auto it = expectedRaw.find(key);
    if (it == expectedRaw.end() || p.evalScoreInclReward > it->second) {
      expectedRaw[key] = p.evalScoreInclReward;
    }
  }
  REQUIRE(maps.lockValue.size() == expectedRaw.size());
  for (const auto &entry : expectedRaw) {
    CHECK(maps.lockValue.at(entry.first) == doctest::Approx(entry.second));
  }

  // With the third-piece penalty applied to all three shallow maps, every penalty stays <= 0
  // and at least one placement reflects a non-zero partial/min penalty.
  bool sawPartialPenalty = false;
  bool sawMinPenalty = false;
  for (const auto &entry : maps.partialPenalty) {
    CHECK(entry.second <= 0.0f);
    if (entry.second < 0.0f) {
      sawPartialPenalty = true;
    }
  }
  for (const auto &entry : maps.minPenalty) {
    CHECK(entry.second <= 0.0f);
    if (entry.second < 0.0f) {
      sawMinPenalty = true;
    }
  }
  CHECK(sawPartialPenalty);
  CHECK(sawMinPenalty);
}

/** Splits `s` on `delim` (preserving empty segments). */
static std::vector<std::string> splitByChar(const std::string &s, char delim) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t pos = s.find(delim, start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

/** Extracts a JSON string value starting at `quotePos` (pointing at opening '"') and advances
 * `nextPos` to the character immediately after the closing '"'. */
static std::string parseJsonStringAt(const std::string &json, size_t quotePos, size_t &nextPos) {
  REQUIRE(quotePos < json.size());
  REQUIRE(json[quotePos] == '"');
  std::string out;
  size_t i = quotePos + 1;
  while (i < json.size()) {
    char c = json[i++];
    if (c == '\\') {
      REQUIRE(i < json.size());
      char esc = json[i++];
      switch (esc) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      default:
        out += esc;
        break;
      }
    } else if (c == '"') {
      nextPos = i;
      return out;
    } else {
      CHECK(c != '\n');
      CHECK(c != '\r');
      out += c;
    }
  }
  FAIL("Unterminated JSON string");
  return "";
}

/** Asserts that `placement` matches the 6-field wire format
 * "relRot,relX,relY|inputSequence|boardAfter|levelAfter|linesAfter|dasChargeAfter". */
static void checkFormattedPlacement(const std::string &placement, int minLevel, int minLines,
                                    bool expectDas) {
  std::vector<std::string> parts = splitByChar(placement, '|');
  REQUIRE(parts.size() == 6);

  std::vector<std::string> coords = splitByChar(parts[0], ',');
  REQUIRE(coords.size() == 3);
  int relRot = std::stoi(coords[0]);
  int relX = std::stoi(coords[1]);
  int relY = std::stoi(coords[2]);
  CHECK(relRot >= 0);
  CHECK(relRot <= 3);
  CHECK(relX >= -8);
  CHECK(relX <= 8);
  CHECK(relY >= 0);
  CHECK(relY <= 22);

  CHECK(!parts[1].empty());
  REQUIRE(parts[2].size() == 200);
  for (char c : parts[2]) {
    CHECK((c == '0' || c == '1'));
  }

  int levelAfter = std::stoi(parts[3]);
  int linesAfter = std::stoi(parts[4]);
  int dasAfter = std::stoi(parts[5]);
  CHECK(levelAfter >= minLevel);
  CHECK(linesAfter >= minLines);
  if (expectDas) {
    CHECK(dasAfter >= 0);
    CHECK(dasAfter <= 16);
  } else {
    CHECK(dasAfter == -1);
  }
}

/** Asserts that `s` starts with `prefix` at `pos` and advances `pos` past it. */
static void consumePrefix(const std::string &s, size_t &pos, const std::string &prefix) {
  REQUIRE(pos + prefix.size() <= s.size());
  REQUIRE(s.compare(pos, prefix.size(), prefix) == 0);
  pos += prefix.size();
}

/** Asserts `output` matches `["agree", "<formatted>", {"explanationMain":"..."}]`. */
static void checkAgreeJson(const std::string &output, int minLevel, int minLines, bool expectDas) {
  CHECK(output.find('\n') == std::string::npos);
  size_t pos = 0;
  consumePrefix(output, pos, "[\"agree\", ");
  std::string mainFormatted = parseJsonStringAt(output, pos, pos);
  checkFormattedPlacement(mainFormatted, minLevel, minLines, expectDas);

  consumePrefix(output, pos, ", {\"explanationMain\":");
  std::string expMain = parseJsonStringAt(output, pos, pos);
  CHECK(!expMain.empty());
  consumePrefix(output, pos, "}]");
  CHECK(pos == output.size());
}

TEST_CASE("GET_MOVE_AB_TEST returns valid JSON with formatted placement and explanation") {
  SUBCASE("agree in tap mode") {
    auto input = InputBuilder().build();
    checkAgreeJson(runRequest(GET_MOVE_AB_TEST, input), 18, 0, /* expectDas= */ false);
  }

  SUBCASE("agree in DAS mode") {
    auto input = InputBuilder()
                     .withBoard1(makeFlatBoard(4))
                     .withCurPiece(IDX_I)
                     .withNextPiece(IDX_O)
                     .withTimeline("X..")
                     .withDasCharge(16)
                     .build();
    checkAgreeJson(runRequest(GET_MOVE_AB_TEST, input), 18, 0, /* expectDas= */ true);
  }

  SUBCASE("topped-out board formats no legal move as none") {
    auto input = InputBuilder().withBoard1(makeFlatBoard(20)).build();
    std::string output = runRequest(GET_MOVE_AB_TEST, input);

    CHECK(output == "[\"agree\", \"none\", {\"explanationMain\":\"\"}]");
  }

  SUBCASE("disagree returns both formatted placements and both explanations") {
    // Relies on MAIN_WEIGHTS != TEST_WEIGHTS (coveredWellCoef -10 vs -12):
    // a 4-high stack with a 1-deep well at col 9 and col 7 carved on the top
    // row makes the T piece sit right across the 2-point well-cover gap.
    std::string board = makeWellBoard(4, 1, 9);
    board[(20 - 4) * 10 + 7] = '0';
    auto input = InputBuilder()
                     .withBoard1(board)
                     .withCurPiece(IDX_T)
                     .withNextPiece(-1)
                     .withTimeline("X..")
                     .withPlayoutCount(0)
                     .withPlayoutLength(0)
                     .withPruningBreadth(10)
                     .build();
    std::string output = runRequest(GET_MOVE_AB_TEST, input);

    CHECK(output.find('\n') == std::string::npos);
    size_t pos = 0;
    consumePrefix(output, pos, "[\"disagree\", ");
    std::string mainFormatted = parseJsonStringAt(output, pos, pos);
    checkFormattedPlacement(mainFormatted, 18, 0, /* expectDas= */ false);

    consumePrefix(output, pos, ", ");
    std::string testFormatted = parseJsonStringAt(output, pos, pos);
    checkFormattedPlacement(testFormatted, 18, 0, /* expectDas= */ false);
    CHECK(mainFormatted != testFormatted);

    consumePrefix(output, pos, ", {\"explanationMain\":");
    std::string mainExp = parseJsonStringAt(output, pos, pos);
    CHECK(!mainExp.empty());

    consumePrefix(output, pos, ", \"explanationTest\":");
    std::string testExp = parseJsonStringAt(output, pos, pos);
    CHECK(!testExp.empty());
    consumePrefix(output, pos, "}]");
    CHECK(pos == output.size());
  }
}

TEST_CASE("searchDepth2 validates firstPieceDasCharge and secondPieceDasCharge combinations") {
  // firstPieceDasCharge selects tap (-1) vs DAS (0..16) for piece 1;
  // secondPieceDasCharge inherits dasChargeAfter (-1) or prescribes piece 2's charge (0..16).
  // Prescribing secondPieceDasCharge in tap mode is invalid and returns 0.
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

  std::vector<Possibility> rejected;
  CHECK(searchDepth2(g, J, I, /* firstPieceDasCharge= */ -1, /* secondPieceDasCharge= */ 10, "X..",
                     &ctx, rejected)
        == 0);
  CHECK(rejected.size() == 0);

  std::vector<Possibility> prescribedBoth;
  CHECK(searchDepth2(g, J, I, /* firstPieceDasCharge= */ 16, /* secondPieceDasCharge= */ 10, "X..",
                     &ctx, prescribedBoth)
        > 0);
  std::vector<Possibility> lowFirstPrescribedSecond;
  CHECK(searchDepth2(g, J, I, /* firstPieceDasCharge= */ 0, /* secondPieceDasCharge= */ 10, "X..",
                     &ctx, lowFirstPrescribedSecond)
        > 0);
  // Lower incoming first-piece DAS charge can reach at most as many placements as full charge.
  CHECK(lowFirstPrescribedSecond.size() <= prescribedBoth.size());

  std::vector<Possibility> inheritMode;
  CHECK(searchDepth2(g, J, I, /* firstPieceDasCharge= */ 16, /* secondPieceDasCharge= */ -1, "X..",
                     &ctx, inheritMode)
        > 0);
  std::vector<Possibility> tapMode;
  CHECK(searchDepth2(g, J, I, /* firstPieceDasCharge= */ -1, /* secondPieceDasCharge= */ -1, "X..",
                     &ctx, tapMode)
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
  std::pair<int, float> holes =
      updateSurfaceAndHoles(probe.gameState.surfaceArray, probe.gameState.board,
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
  probe.evalContext =
      getEvalContext(probe.gameState, contexts, /* useTestWeights= */ false, aggressionLevel);
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
    LockPlacement placement = {result.x, result.y,         result.rotationIndex,
                               -1,       result.tuckInput, curPiece};
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

TEST_CASE(
    "PRECOMPUTE_BACKUP returns the exact backup response for the obvious well-fill placement") {
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
                         "I.R.R.R"
                         + std::string(50, '.')
                         + "*****"
                         + std::string(17, '^')
                         + "*****"
                         + "~~|"
                         + std::string(200, '0') // the well fill clears the whole board
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
  CHECK(head
        == "Default:0,-1,18|"
               + std::string("L")
               + std::string(56, '.')
               + std::string(5, '*')
               + std::string(17, '^')
               + std::string(5, '*')
               + "~~|"
               + std::string(190, '0')
               + "0011110000|18|1|-1");
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
    std::string expectedLine =
        "Default:1,4,18|" + best.inputSequence + "~~|" + std::string(200, '0') + "|18|4|16";
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

TEST_CASE("Timed tournament mode (pruningBreadth == -2) supports lookup and precompute") {
  // 0ms budget falls back to the exact eval-only result for lookups and precompute.
  std::string evalOnlyLookup = runRequest(
      GET_LOCK_VALUE_LOOKUP,
      InputBuilder().withBoard1(makeFlatBoard(4)).withCurPiece(IDX_O).withPlayoutCount(0).build());
  std::string zeroMsLookup = runRequest(GET_LOCK_VALUE_LOOKUP, InputBuilder()
                                                                   .withBoard1(makeFlatBoard(4))
                                                                   .withCurPiece(IDX_O)
                                                                   .withPruningBreadth(-2)
                                                                   .withPlayoutCount(0)
                                                                   .build());
  CHECK(zeroMsLookup == evalOnlyLookup);

  // Non-zero timed lookup produces a valid JSON map without uninitialized scores.
  std::string timedLookup = runRequest(GET_LOCK_VALUE_LOOKUP, InputBuilder()
                                                                  .withBoard1(makeFlatBoard(4))
                                                                  .withCurPiece(IDX_O)
                                                                  .withPruningBreadth(-2)
                                                                  .withPlayoutCount(25)
                                                                  .build());
  CHECK(timedLookup.size() >= 2);
  CHECK(timedLookup.front() == '{');
  CHECK(timedLookup.back() == '}');
  CHECK(timedLookup.find("-999999") == std::string::npos);

  // DAS timed lookup produces the three-map JSON array cleanly.
  std::string timedDas = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, InputBuilder()
                                                                   .withBoard1(makeFlatBoard(4))
                                                                   .withCurPiece(IDX_O)
                                                                   .withDasCharge(16)
                                                                   .withPruningBreadth(-2)
                                                                   .withPlayoutCount(25)
                                                                   .build());
  CHECK(timedDas.front() == '[');
  CHECK(timedDas.back() == ']');
  CHECK(countOccurrences(timedDas, ",\n") == 2);

  // PRECOMPUTE with 0ms matches fixed-playout 0-playout PRECOMPUTE byte-for-byte.
  std::string fixedZeroPre = runRequest(PRECOMPUTE, InputBuilder()
                                                            .withBoard1(makeWellBoard(4, 4, 9))
                                                            .withCurPiece(IDX_I)
                                                            .withNextPiece(-1)
                                                            .withTimeline("X.....")
                                                            .withPruningBreadth(-1)
                                                            .withPlayoutCount(0)
                                                            .withPlayoutLength(4)
                                                            .build()
                                                        + "10|");
  std::string timedZeroPre = runRequest(PRECOMPUTE, InputBuilder()
                                                            .withBoard1(makeWellBoard(4, 4, 9))
                                                            .withCurPiece(IDX_I)
                                                            .withNextPiece(-1)
                                                            .withTimeline("X.....")
                                                            .withPruningBreadth(-2)
                                                            .withPlayoutCount(0)
                                                            .withPlayoutLength(4)
                                                            .build()
                                                        + "10|");
  CHECK(timedZeroPre == fixedZeroPre);

  // Already-expired requestReceived deadline falls back cleanly to eval-only PRECOMPUTE.
  std::string timed30Pipe = InputBuilder()
                                .withBoard1(makeWellBoard(4, 4, 9))
                                .withCurPiece(IDX_I)
                                .withNextPiece(-1)
                                .withTimeline("X.....")
                                .withPruningBreadth(-2)
                                .withPlayoutCount(30)
                                .withPlayoutLength(4)
                                .build()
                            + "10|";
  EngineTimingContext expiredTiming;
  expiredTiming.hasRequestReceived = true;
  expiredTiming.requestReceived = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
  expiredTiming.calibratedMovesPerMs7T = 150.0;
  std::string expiredOut = mainProcess(timed30Pipe.c_str(), PRECOMPUTE, &expiredTiming);
  CHECK(expiredOut == fixedZeroPre);

  // Tight non-zero deadline that cannot reach TMT_MIN_PLAYOUTS falls back cleanly to eval-only
  // rather than running noisy sub-30-sample Round 1 playouts.
  std::string tinyMsLookup = runRequest(GET_LOCK_VALUE_LOOKUP, InputBuilder()
                                                                   .withBoard1(makeFlatBoard(4))
                                                                   .withCurPiece(IDX_O)
                                                                   .withPruningBreadth(-2)
                                                                   .withPlayoutCount(8)
                                                                   .withPlayoutLength(8)
                                                                   .build());
  CHECK(tinyMsLookup == evalOnlyLookup);
}

TEST_CASE("playSequence preserves originalAiMode weights across mid-playout mode transitions") {
  const char *dasTimeline = getDasEquivalentInputTimeline("X.....");
  const PieceRangeContext contexts[4] = {
      getPieceRangeContext(dasTimeline, 1, /* gravityDoubled= */ true),
      getPieceRangeContext(dasTimeline, 1, /* gravityDoubled= */ false),
      getPieceRangeContext(dasTimeline, 2, /* gravityDoubled= */ false),
      getPieceRangeContext(dasTimeline, 3, /* gravityDoubled= */ false),
  };

  SUBCASE("STANDARD playout that creates a hole mid-playout still uses STANDARD weights at leaf") {
    // Sawtooth surface [2, 1, 2, 1, 2, 1, 2, 1, 9, 0] with 0 existing holes -> STANDARD mode.
    // Every placement of an O piece on this surface overhangs a step and creates a true hole,
    // flipping the state into DIG mode on ply 1.
    GameState g = {};
    g.level = 18;
    g.lines = 0;
    const int heights[10] = {2, 1, 2, 1, 2, 1, 2, 1, 9, 0};
    for (int col = 0; col < 10; col++) {
      for (int r = 20 - heights[col]; r < 20; r++) {
        g.board[r] |= (1U << (9 - col));
      }
    }
    getSurfaceArray(g.board, g.surfaceArray);
    std::pair<int, float> holes = updateSurfaceAndHoles(g.surfaceArray, g.board, 9, false);
    g.numTrueHoles = holes.first;
    g.numPartialHoles = holes.second;
    REQUIRE(g.numTrueHoles == 0);

    const EvalContext initialCtx = getEvalContext(g, contexts, /* useTestWeights= */ false, SAFE);
    REQUIRE(initialCtx.aiMode == STANDARD);

    int seq[SEQUENCE_LENGTH] = {};
    seq[0] = IDX_O;
    seq[1] = IDX_O;

    // Trace the 2-ply playout manually to verify ply 0 enters DIG and compute the true STANDARD
    // leaf eval.
    std::vector<LockPlacement> locks0;
    fastMoveSearch(g, &PIECE_LIST[seq[0]], initialCtx.pieceRangeContext.inputFrameTimeline, locks0);
    REQUIRE(!locks0.empty());
    LockPlacement best0 = pickLockPlacement(g, &initialCtx, locks0);
    GameState after0 = advanceGameState(g, best0, &initialCtx);
    REQUIRE(after0.numTrueHoles >= 1);
    float reward0 =
        getLineClearFactor(after0.lines - g.lines, initialCtx.weights,
                           initialCtx.shouldRewardLineClears, g.lines, initialCtx.lineoutAt29);

    const EvalContext ply1Ctx = getEvalContext(after0, contexts, /* useTestWeights= */ false, SAFE);
    REQUIRE(ply1Ctx.aiMode == DIG);
    std::vector<LockPlacement> locks1;
    fastMoveSearch(after0, &PIECE_LIST[seq[1]], ply1Ctx.pieceRangeContext.inputFrameTimeline,
                   locks1);
    REQUIRE(!locks1.empty());
    LockPlacement best1 = pickLockPlacement(after0, &ply1Ctx, locks1);

    EvalContext expectedLeafCtx = ply1Ctx;
    expectedLeafCtx.aiMode = STANDARD;
    expectedLeafCtx.weights = getWeights(STANDARD, /* useTestWeights= */ false, SAFE);
    expectedLeafCtx.shouldRewardLineClears = false;
    GameState after1 = advanceGameState(after0, best1, &expectedLeafCtx);
    float expectedStandardEval = fastEval(after0, after1, best1, &expectedLeafCtx);
    float buggyDigEval = fastEval(after0, after1, best1, &ply1Ctx);
    REQUIRE(expectedStandardEval != doctest::Approx(buggyDigEval));

    float actualScore = playSequence(g, contexts, seq, /* playoutLength= */ 2, SAFE,
                                     initialCtx.aiMode, /* playoutDataList= */ nullptr);
    CHECK(actualScore == doctest::Approx(reward0 + expectedStandardEval));
  }

  SUBCASE("DIG playout that clears its hole mid-playout still uses DIG weights at leaf") {
    // Row 19 has a hole at col 0 under a 1-cell roof at (row 18, col 0), with cols 4-7 empty in
    // row 18 and a tall stack in col 8 (height 11). Dropping a horizontal I into cols 4-7 of row 18
    // clears row 18, removing the roof over col 0 so numTrueHoles becomes 0 (STANDARD mode) on
    // ply 1.
    GameState g = {};
    g.level = 18;
    g.lines = 0;
    // Row 19: cols 1..8 filled, col 0 and col 9 empty
    for (int col = 1; col <= 8; col++) {
      g.board[19] |= (1U << (9 - col));
    }
    // Row 18: cols 0..3 and 8..9 filled, cols 4..7 empty (horizontal I slot)
    for (int col : {0, 1, 2, 3, 8, 9}) {
      g.board[18] |= (1U << (9 - col));
    }
    // Col 8 stacked up to height 11 (rows 9..17)
    for (int r = 9; r <= 17; r++) {
      g.board[r] |= (1U << (9 - 8));
    }
    getSurfaceArray(g.board, g.surfaceArray);
    std::pair<int, float> holes = updateSurfaceAndHoles(g.surfaceArray, g.board, 9, true);
    g.numTrueHoles = holes.first;
    g.numPartialHoles = holes.second;
    REQUIRE(g.numTrueHoles == 1);

    const EvalContext initialCtx = getEvalContext(g, contexts, /* useTestWeights= */ false, SAFE);
    REQUIRE(initialCtx.aiMode == DIG);

    int seq[SEQUENCE_LENGTH] = {};
    seq[0] = IDX_I;
    seq[1] = IDX_O;

    std::vector<LockPlacement> locks0;
    fastMoveSearch(g, &PIECE_LIST[seq[0]], initialCtx.pieceRangeContext.inputFrameTimeline, locks0);
    REQUIRE(!locks0.empty());
    LockPlacement best0 = pickLockPlacement(g, &initialCtx, locks0);
    GameState after0 = advanceGameState(g, best0, &initialCtx);
    REQUIRE(after0.numTrueHoles == 0);
    // Intermediate burns in a DIG-rooted playout use DIG weights (matching plies 1-2, the leaf
    // ply, and terminal guaranteedBurns) rather than STANDARD weights.
    float reward0 = getLineClearFactor(after0.lines - g.lines, initialCtx.weights, false, g.lines,
                                       initialCtx.lineoutAt29);

    const EvalContext ply1Ctx = getEvalContext(after0, contexts, /* useTestWeights= */ false, SAFE);
    REQUIRE(ply1Ctx.aiMode == STANDARD);
    std::vector<LockPlacement> locks1;
    fastMoveSearch(after0, &PIECE_LIST[seq[1]], ply1Ctx.pieceRangeContext.inputFrameTimeline,
                   locks1);
    REQUIRE(!locks1.empty());
    LockPlacement best1 = pickLockPlacement(after0, &ply1Ctx, locks1);

    EvalContext expectedLeafCtx = ply1Ctx;
    expectedLeafCtx.aiMode = DIG;
    expectedLeafCtx.weights = getWeights(DIG, /* useTestWeights= */ false, SAFE);
    expectedLeafCtx.shouldRewardLineClears = false;
    GameState after1 = advanceGameState(after0, best1, &expectedLeafCtx);
    float expectedDigEval = fastEval(after0, after1, best1, &expectedLeafCtx);
    float buggyStandardEval = fastEval(after0, after1, best1, &ply1Ctx);
    REQUIRE(expectedDigEval != doctest::Approx(buggyStandardEval));

    float actualScore = playSequence(g, contexts, seq, /* playoutLength= */ 2, SAFE,
                                     initialCtx.aiMode, /* playoutDataList= */ nullptr);
    CHECK(actualScore == doctest::Approx(reward0 + expectedDigEval));

    // Also verify under HYPER aggression: intermediate burns in a DIG-rooted playout must still
    // use DIG's burnCoef (-1), not STANDARD HYPER's -30 (which otherwise makes stalling/covering
    // the well outscore actually digging out a hole at depth >= 4).
    float actualHyperScore = playSequence(g, contexts, seq, /* playoutLength= */ 2, HYPER,
                                          initialCtx.aiMode, /* playoutDataList= */ nullptr);
    CHECK(actualHyperScore == doctest::Approx(reward0 + expectedDigEval));

    // Also verify that when the hole is cleared on the root candidate move before getPlayoutScore
    // is called (so after0 already has numTrueHoles == 0), passing initialCtx.aiMode (DIG) into
    // getPlayoutScore still evaluates the playout leaf with DIG weights rather than STANDARD.
    int seqFromAfter0[SEQUENCE_LENGTH] = {};
    seqFromAfter0[0] = IDX_O;
    float scoreFromPostRootMove = playSequence(after0, contexts, seqFromAfter0,
                                               /* playoutLength= */ 1, SAFE, initialCtx.aiMode,
                                               /* playoutDataList= */ nullptr);
    CHECK(scoreFromPostRootMove == doctest::Approx(expectedDigEval));
  }

  SUBCASE("HYPER DIG depth-8 precompute prefers digging over covering the well") {
    // Regression test: with a hole at row 19 col 3 on level 26 (lines=204, DIG mode) under HYPER
    // aggression at playoutLength=8, scoring intermediate burns at DIG's burnCoef keeps the engine
    // digging (3,-5,17) instead of covering the well on the right (2,3,15) to stall burns past the
    // horizon.
    std::string board = std::string(160, '0')
                        + "0000001110"
                          "0000001110"
                          "0011001110"
                          "1110111111";
    REQUIRE(board.size() == 200);
    std::string pipe = board + "|26|204|2|-1|X..|2000|8|-1|hyper|15|18|";
    std::string response = mainProcess(pipe.c_str(), PRECOMPUTE);
    CHECK(response.rfind("Default:3,-5,17|", 0) == 0);
  }

  SUBCASE("Pre-killscreen playout stops immediately upon reaching Level 29 LINEOUT and evaluates "
          "in Level 28 context") {
    // At level 28, lines = 226, a 4-deep right well with height 10 on cols 0..8 is Tetris-ready.
    // Dropping a vertical I into col 9 on ply 0 clears 4 lines (226 -> 230, advancing to level 29
    // LINEOUT). Even when playoutLength = 6, playSequence must terminate immediately after ply 0
    // and evaluate the post-Tetris board using the Level 28 STANDARD EvalContext rather than
    // simulating 5 more plies under Level 29 gravity or applying LINEOUT's scareHeight = 0.
    GameState g = {};
    g.level = 28;
    g.lines = 226;
    std::string wellBoard = makeWellBoard(/* height= */ 8, /* wellDepth= */ 8, /* wellCol= */ 9);
    encodeBoard(wellBoard.c_str(), g.board);
    getSurfaceArray(g.board, g.surfaceArray);
    std::pair<int, float> holes = updateSurfaceAndHoles(g.surfaceArray, g.board, 9, false);
    g.numTrueHoles = holes.first;
    g.numPartialHoles = holes.second;
    REQUIRE(g.numTrueHoles == 0);

    const EvalContext initialCtx = getEvalContext(g, contexts, /* useTestWeights= */ false, SAFE);
    REQUIRE(initialCtx.aiMode == STANDARD);

    int seq[SEQUENCE_LENGTH] = {IDX_I, IDX_Z, IDX_S, IDX_O, IDX_T, IDX_L};

    std::vector<LockPlacement> locks0;
    fastMoveSearch(g, &PIECE_LIST[seq[0]], initialCtx.pieceRangeContext.inputFrameTimeline, locks0);
    REQUIRE(!locks0.empty());
    LockPlacement best0 = pickLockPlacement(g, &initialCtx, locks0);
    GameState after0 = advanceGameState(g, best0, &initialCtx);
    REQUIRE(after0.level == 29);
    REQUIRE(after0.lines == 230);
    float expectedEarlyStopEval = fastEval(g, after0, best0, &initialCtx);

    std::vector<PlayoutData> playoutDataList;
    float actualScore = playSequence(g, contexts, seq, /* playoutLength= */ 6, SAFE,
                                     initialCtx.aiMode, &playoutDataList);
    CHECK(actualScore == doctest::Approx(expectedEarlyStopEval));
    REQUIRE(playoutDataList.size() == 1);
    CHECK(playoutDataList[0].placements.size() == 1);

    // If the root move itself already crossed into Level 29 (so playSequence is entered with
    // after0.level == 29 while originalAiMode == STANDARD), playSequence stops on entry (0 plies
    // played on Level 29) and returns the Level 28 board evaluation (excluding the root move's
    // immediateReward, which the caller adds).
    float rootImmediateReward =
        getLineClearFactor(after0.lines - g.lines, initialCtx.weights,
                           initialCtx.shouldRewardLineClears, g.lines, initialCtx.lineoutAt29);
    std::vector<PlayoutData> postRootPlayoutData;
    float postRootScore = playSequence(after0, contexts, seq, /* playoutLength= */ 6, SAFE,
                                       initialCtx.aiMode, &postRootPlayoutData);
    CHECK(rootImmediateReward + postRootScore == doctest::Approx(expectedEarlyStopEval));
    REQUIRE(postRootPlayoutData.size() == 1);
    CHECK(postRootPlayoutData[0].placements.empty());

    // Conversely, a search that *starts* on Level 29 (originalAiMode == LINEOUT) runs all requested
    // playout plies in LINEOUT mode.
    const EvalContext level29Ctx =
        getEvalContext(after0, contexts, /* useTestWeights= */ false, SAFE);
    REQUIRE(level29Ctx.aiMode == LINEOUT);
    std::vector<PlayoutData> lineoutPlayoutData;
    playSequence(after0, contexts, seq, /* playoutLength= */ 2, SAFE, level29Ctx.aiMode,
                 &lineoutPlayoutData);
    REQUIRE(lineoutPlayoutData.size() == 1);
    CHECK(lineoutPlayoutData[0].placements.size() == 2);
  }

  SUBCASE("Level 28 playouts prefer scoring Tetrises into 29 over burning or digging into 29") {
    // At level 28, lines = 228, build a clean height-8 stack with a right well (col 9 empty) and
    // row 12 having cols 0..6 filled and cols 7..9 empty (so an L piece can either burn 1 line by
    // filling cols 7..9 or stack cleanly on the left while keeping the Tetris well open).
    GameState g = {};
    g.level = 28;
    g.lines = 228;
    // Rows 13..19 (7 rows): cols 0..8 filled, col 9 empty (7-deep right well)
    for (int r = 13; r < 20; r++) {
      for (int col = 0; col <= 8; col++) {
        g.board[r] |= (1U << (9 - col));
      }
    }
    getSurfaceArray(g.board, g.surfaceArray);
    std::pair<int, float> holes = updateSurfaceAndHoles(g.surfaceArray, g.board, 9, false);
    g.numTrueHoles = holes.first;
    g.numPartialHoles = holes.second;
    REQUIRE(g.numTrueHoles == 0);

    const EvalContext ctx = getEvalContext(g, contexts, /* useTestWeights= */ false, SAFE);
    REQUIRE(ctx.aiMode == STANDARD);

    // Compare a candidate placement that keeps the board clean and Tetris-ready vs a candidate
    // placement that creates a hole over the well to enter DIG mode.
    LockPlacement cleanPlacement = {
        /* x= */ 0,          /* y= */ 10,      /* rotationIndex= */ 0,
        /* tuckFrame= */ -1, NO_TUCK_NOTATION, &PIECE_LIST[IDX_O]};
    LockPlacement holePlacement = {
        /* x= */ 7,          /* y= */ 10,      /* rotationIndex= */ 2,
        /* tuckFrame= */ -1, NO_TUCK_NOTATION, &PIECE_LIST[IDX_L]};
    GameState cleanState = advanceGameState(g, cleanPlacement, &ctx);
    GameState holeState = advanceGameState(g, holePlacement, &ctx);
    REQUIRE(cleanState.numTrueHoles == 0);
    REQUIRE(holeState.numTrueHoles >= 1);

    float cleanScore = getPlayoutScore(cleanState, /* playoutCount= */ 49, /* playoutLength= */ 6,
                                       contexts, IDX_O, SAFE, ctx.aiMode, nullptr);
    float holeScore = getPlayoutScore(holeState, /* playoutCount= */ 49, /* playoutLength= */ 6,
                                      contexts, IDX_L, SAFE, ctx.aiMode, nullptr);
    CHECK(cleanScore > holeScore);
  }

  SUBCASE("Timed precompute at lines=229 does not inflate moves/ms from early-stopped playouts") {
    // Regression pin: at level 28, lines = 229, any line clear crosses into level 29 and stops
    // playSequence early (0 or 1 ply instead of 8). Counting totalPlayoutsRun * playoutLength
    // instead of actual plies executed inflated getObservedMovesPerMs() by ~5x-10x after Round 1,
    // causing Round 2 to schedule 8000-sample full-length playouts and overrun a 60ms deadline by
    // >500ms.
    std::string board =
        "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000011000000001110000000111100100011111111101111111110"
        "11111111101111111110";
    std::string pipe = InputBuilder()
                           .withBoard1(board)
                           .withLevel(28)
                           .withLines(229)
                           .withCurPiece(IDX_S)
                           .withNextPiece(-1)
                           .withTimeline("X..")
                           .withPlayoutCount(60)
                           .withPlayoutLength(8)
                           .withPruningBreadth(-2)
                           .withAggression("aggro")
                           .withDasCharge(16)
                           .build()
                       + "18|";
    auto start = std::chrono::steady_clock::now();
    std::string out = runRequest(PRECOMPUTE, pipe);
    double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(out.rfind("Default:", 0) == 0);
    CHECK(elapsedMs < 150.0);
  }

  SUBCASE("EngineTimingContext::cancelFlag halts in-flight PRECOMPUTE immediately") {
    std::string board =
        "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000011000000001110000000111100100011111111101111111110"
        "11111111101111111110";
    std::string pipe = InputBuilder()
                           .withBoard1(board)
                           .withLevel(18)
                           .withLines(0)
                           .withCurPiece(IDX_T)
                           .withNextPiece(-1)
                           .withTimeline("X..")
                           .withPlayoutCount(2000)
                           .withPlayoutLength(8)
                           .withPruningBreadth(-2)
                           .withAggression("safe")
                           .withDasCharge(16)
                           .build()
                       + "18|";
    std::atomic<bool> cancelRequested{false};
    EngineTimingContext timing;
    timing.cancelFlag = &cancelRequested;

    std::thread canceller([&cancelRequested]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      cancelRequested.store(true, std::memory_order_relaxed);
    });
    auto start = std::chrono::steady_clock::now();
    std::string out = mainProcess(pipe.c_str(), PRECOMPUTE, &timing);
    double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    canceller.join();
    CHECK(out == "none");
    CHECK(elapsedMs < 100.0);
  }

  SUBCASE("Precompute at lines=228 avoids burning to 229 with a covered well on S -> O") {
    // Regression pin from live gameplay: at level 28, lines = 228, S current and O next,
    // burning 1 line into 229 while covering the right well (S=1|6|14, leaving guaranteedBurns=1
    // so lines + guaranteedBurns == 230) must be heavily penalized by KILLSCREEN_PENALTY so the
    // engine keeps the board at 228 lines with an open well instead.
    std::string board =
        "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000100000011110000011111110011111111101111111110"
        "11111111101111111110";
    std::string pipe = InputBuilder()
                           .withBoard1(board)
                           .withLevel(28)
                           .withLines(228)
                           .withCurPiece(IDX_S)
                           .withNextPiece(-1)
                           .withTimeline("X..")
                           .withPlayoutCount(100)
                           .withPlayoutLength(8)
                           .withPruningBreadth(-1)
                           .withAggression("aggro")
                           .withDasCharge(16)
                           .build()
                       + "18|";
    std::string out = runRequest(PRECOMPUTE, pipe);
    // Extract the O: line and verify it stays at 228 lines rather than burning to 229
    size_t oPos = out.find("\nO:");
    REQUIRE(oPos != std::string::npos);
    size_t oEnd = out.find('\n', oPos + 1);
    std::string oLine = out.substr(oPos + 1, oEnd - (oPos + 1));
    CHECK(oLine.find("|28|228|") != std::string::npos);
  }
}

TEST_CASE("preparePrecomputeState offsets phantom prefix slice by 1 when '_' holds through ARE") {
  // With full DAS charge (16) and reactionTime = 7, holding right through ARE prepends '_' at
  // index 0, so the 7 pre-reaction active frames (0..6) occupy indices 1..7 (slice length 8):
  //   - +1 shift (x = 4): "_Rrrrrr." (stops shifting on frame 6)
  //   - +2 shift (x = 5): "_RrrrrrR" (shifts again on frame 6)
  // Both prefixes must be preserved as distinct phantoms, matching preAdjInputSequence.
  std::string dasEquivalent = getDasEquivalentInputTimeline("X..");
  PieceRangeContext contexts[4] = {
      getPieceRangeContext(dasEquivalent.c_str(), 1, /* gravityDoubled= */ true),
      getPieceRangeContext(dasEquivalent.c_str(), 1, /* gravityDoubled= */ false),
      getPieceRangeContext(dasEquivalent.c_str(), 2, /* gravityDoubled= */ false),
      getPieceRangeContext(dasEquivalent.c_str(), 3, /* gravityDoubled= */ false),
  };
  BackupProbe probe = probeStateFrom(makeEmptyBoard(), 18, 0, dasEquivalent, "safe");
  PrecomputeState state = preparePrecomputeState(
      probe.gameState, &PIECE_LIST[IDX_O], "X..", /* dasCharge= */ 16, /* reactionTime= */ 7,
      /* pruningBreadth= */ 4, /* playoutCount= */ 0, /* playoutLength= */ 0, &probe.evalContext,
      contexts);

  bool sawOneShiftPrefix = false;
  bool sawTwoShiftPrefix = false;
  for (const PrecomputePhantom &phantom : state.phantoms) {
    if (phantom.hasInitialPlacement && phantom.hasAdjustmentState) {
      CHECK(phantom.inputSequence == phantom.adjustmentCtx.preAdjInputSequence);
    }
    if (phantom.inputSequence == "_Rrrrrr.") {
      sawOneShiftPrefix = true;
    } else if (phantom.inputSequence == "_RrrrrrR") {
      sawTwoShiftPrefix = true;
    }
  }
  CHECK(sawOneShiftPrefix);
  CHECK(sawTwoShiftPrefix);
}

TEST_CASE("2-ply intermediate eval tiebreak takes an immediate Tetris on I -> I") {
  // On an 8-high Tetris-ready right-well board with current piece I and next piece I, placing
  // the first I onto the stack and saving the Tetris for the second I reaches the identical
  // post-2-piece board as taking the Tetris immediately, while costing fewer post-reaction
  // adjustment inputs (e.g. at reactionTime = 18 with full DAS, the piece is at col 8 one frame
  // short of col 9). The 1-ply intermediate eval tiebreak prefers clearing the Tetris on the
  // first I across eval-only, playout-backed, DAS, low-DAS, and tap modes.
  const std::string board = makeWellBoard(8, 8, 9);

  auto checkPrecomputeITakesTetris = [&](int dasCharge, int pruningBreadth, int playoutCount,
                                         int playoutLength) {
    INFO("dasCharge=", dasCharge, " breadth=", pruningBreadth, " playouts=", playoutCount);
    std::string pipe = InputBuilder()
                           .withBoard1(board)
                           .withLevel(18)
                           .withLines(20)
                           .withCurPiece(IDX_I)
                           .withNextPiece(-1)
                           .withTimeline("X..")
                           .withPlayoutCount(playoutCount)
                           .withPlayoutLength(playoutLength)
                           .withPruningBreadth(pruningBreadth)
                           .withAggression("aggro")
                           .withDasCharge(dasCharge)
                           .build()
                       + "18|";
    std::string out = runRequest(PRECOMPUTE, pipe);
    size_t defEnd = out.find('\n');
    REQUIRE(defEnd != std::string::npos);
    std::string defLine = out.substr(0, defEnd);
    size_t iPos = out.find("\nI:");
    REQUIRE(iPos != std::string::npos);
    size_t iEnd = out.find('\n', iPos + 1);
    std::string iLine = out.substr(iPos + 1, iEnd - (iPos + 1));
    // Taking the Tetris clears 4 lines (20 -> 24); if I continues with Default, check Default.
    const std::string &effectiveILine = (iLine == "I:continue") ? defLine : iLine;
    CHECK(effectiveILine.find("|18|24|") != std::string::npos);
  };

  // Eval-only across full DAS (mid-flight col-8 snapshot), zero DAS, and tap mode
  checkPrecomputeITakesTetris(/* dasCharge= */ 16, 20, 0, 0);
  checkPrecomputeITakesTetris(/* dasCharge= */ 0, 20, 0, 0);
  checkPrecomputeITakesTetris(/* dasCharge= */ -1, 20, 0, 0);

  // Tournament playout mode (where playout scores replace raw evalScoreInclReward)
  checkPrecomputeITakesTetris(/* dasCharge= */ 16, -1, 200, 4);
  checkPrecomputeITakesTetris(/* dasCharge= */ 0, -1, 200, 4);

  // Direct 2-ply GET_MOVE (I -> I): vertical I in col 9 (rot=1, xOffset=+4)
  std::string getMovePipe = InputBuilder()
                                .withBoard1(board)
                                .withLevel(18)
                                .withLines(20)
                                .withCurPiece(IDX_I)
                                .withNextPiece(IDX_I)
                                .withTimeline("X..")
                                .withPlayoutCount(49)
                                .withPlayoutLength(2)
                                .withPruningBreadth(20)
                                .withAggression("aggro")
                                .build();
  int rot = -1, xOff = 0, yOff = 0;
  parseMoveTriple(runRequest(GET_MOVE, getMovePipe), rot, xOff, yOff);
  CHECK(rot == 1);
  CHECK(xOff == 4);
}

TEST_CASE("preparePrecomputeState keeps all initial placements without prefix deduplication") {
  // At reactionTime = 18 with full DAS (16) on an 8-high Tetris-ready right-well board,
  // vertical I at col 8 (relRot=1, relX=3) and vertical I at col 9 (relRot=1, relX=4, Tetris)
  // share the 19-char pre-reaction prefix "_IrrrrrRrrrrrRrrrrr" (the 4th 'R' into col 9 lands
  // on frame 18). Both initial placements remain in state.phantoms (ordered by fewest inputs),
  // and collapseAndFormatPrecompute selects the col-9 Tetris as Default because the majority
  // of next pieces take the Tetris.
  const std::string board = makeWellBoard(8, 8, 9);
  std::string dasEquivalent = getDasEquivalentInputTimeline("X..");
  PieceRangeContext contexts[4] = {
      getPieceRangeContext(dasEquivalent.c_str(), 1, /* gravityDoubled= */ true),
      getPieceRangeContext(dasEquivalent.c_str(), 1, /* gravityDoubled= */ false),
      getPieceRangeContext(dasEquivalent.c_str(), 2, /* gravityDoubled= */ false),
      getPieceRangeContext(dasEquivalent.c_str(), 3, /* gravityDoubled= */ false),
  };
  BackupProbe probe = probeStateFrom(board, 18, 20, dasEquivalent, "aggro");
  PrecomputeState state = preparePrecomputeState(
      probe.gameState, &PIECE_LIST[IDX_I], "X..", /* dasCharge= */ 16, /* reactionTime= */ 18,
      /* pruningBreadth= */ 20, /* playoutCount= */ 0, /* playoutLength= */ 0, &probe.evalContext,
      contexts);

  int sawCol8Idx = -1;
  int sawCol9TetrisIdx = -1;
  for (int idx = 0; idx < static_cast<int>(state.phantoms.size()); idx++) {
    const PrecomputePhantom &phantom = state.phantoms[idx];
    if (phantom.inputSequence == "_IrrrrrRrrrrrRrrrrr" && phantom.initial.rot == 1) {
      if (phantom.initial.x == 6) {
        sawCol8Idx = idx;
      } else if (phantom.initial.x == 7 && phantom.initial.numLinesCleared == 4) {
        sawCol9TetrisIdx = idx;
      }
    }
  }
  CHECK(sawCol8Idx != -1);
  CHECK(sawCol9TetrisIdx != -1);
  CHECK(sawCol8Idx < sawCol9TetrisIdx);

  const std::string response = collapseAndFormatPrecompute(
      state, probe.gameState.board, &PIECE_LIST[IDX_I], 18, 20, SHOULD_PUSHDOWN);
  CHECK(response.rfind("Default:1,4,", 0) == 0);
}

TEST_CASE("tournament transposition check prevents deferred I -> I from beating immediate Tetris") {
  // Regression pin from live gameplay: fastEval ranks (Tetris, col 0) ahead of (Tetris, col 7),
  // whereas 8-ply playouts prefer the (Tetris + col 7) board. Without reconciling transposed
  // post-2-piece states at publish time, Round 3 could lock the Tetris candidate onto (Tetris,
  // col 0) while the col-7 candidate deepened (col 7, Tetris) and outscored it. Reconciling
  // sampled resultingStates at publish time gives (Tetris, col 7) the deeper (col 7, Tetris)
  // estimate plus its higher immediateReward.
  const std::string board = "00000000000000000000000000000000000000000000000000"
                            "00000000000000000000000000000000000000000000000000"
                            "00000000000000000000000001000011001110001111111000"
                            "11111110001111111110111111111011111111101111111110";
  REQUIRE(board.size() == 200);

  auto checkITakesTetris = [&](const std::string &out) {
    size_t defEnd = out.find('\n');
    REQUIRE(defEnd != std::string::npos);
    std::string defLine = out.substr(0, defEnd);
    size_t iPos = out.find("\nI:");
    REQUIRE(iPos != std::string::npos);
    size_t iEnd = out.find('\n', iPos + 1);
    std::string iLine = out.substr(iPos + 1, iEnd - (iPos + 1));
    const std::string &effectiveILine = (iLine == "I:continue") ? defLine : iLine;
    CHECK(effectiveILine.find("|19|110|16") != std::string::npos);
  };

  // Fixed-budget tournament (deep enough that col 7 beats col 0 on playouts)
  std::string fixedPipe = InputBuilder()
                              .withBoard1(board)
                              .withLevel(19)
                              .withLines(106)
                              .withCurPiece(IDX_I)
                              .withNextPiece(-1)
                              .withTimeline("X..")
                              .withPlayoutCount(500)
                              .withPlayoutLength(8)
                              .withPruningBreadth(-1)
                              .withAggression("aggro")
                              .withDasCharge(16)
                              .build()
                          + "18|";
  checkITakesTetris(runRequest(PRECOMPUTE, fixedPipe));

  // Timed tournament at the live server's calibrated 7-thread rate
  std::string timedPipe = InputBuilder()
                              .withBoard1(board)
                              .withLevel(19)
                              .withLines(106)
                              .withCurPiece(IDX_I)
                              .withNextPiece(-1)
                              .withTimeline("X..")
                              .withPlayoutCount(344)
                              .withPlayoutLength(8)
                              .withPruningBreadth(-2)
                              .withAggression("aggro")
                              .withDasCharge(16)
                              .build()
                          + "18|";
  EngineTimingContext timing;
  timing.hasRequestReceived = true;
  timing.requestReceived = std::chrono::steady_clock::now();
  timing.calibratedMovesPerMs7T = 123.0;
  checkITakesTetris(mainProcess(timedPipe.c_str(), PRECOMPUTE, &timing));
}

TEST_CASE("tiered tournament Round 1 rescues top-eval candidate with deceptive cont#0") {
  // Regression test for 2-ply tiered Round 1: on this level 19 hyper board with J (lines=85,
  // das=15) and next=S, 1|4|11 ([1,1,12]) is a top-eval candidate whose eval-best cont#0 creates
  // a central chasm. Tiered R1 screens extra continuations for top-eval candidates so 1|4|11
  // survives Cut 1 and beats 1|7|11 ([1,4,12]) in the deep lookup. (Tested directly on the 2-ply
  // DAS tournament path rather than PRECOMPUTE rt=60, which takes the 1-ply no-next-box path.)
  std::string board = std::string(130, '0')
                      + "1000000100"
                        "1000011110"
                        "1100111110"
                        "1101111111"
                        "1111111110"
                        "1111111110"
                        "1111111110";
  REQUIRE(board.size() == 200);
  std::string pipe = InputBuilder()
                         .withBoard1(board)
                         .withLevel(19)
                         .withLines(85)
                         .withCurPiece(IDX_J)
                         .withNextPiece(IDX_S)
                         .withTimeline("X.....")
                         .withPlayoutCount(3500)
                         .withPlayoutLength(8)
                         .withPruningBreadth(-1)
                         .withAggression("hyper")
                         .withDasCharge(15)
                         .build();
  std::string output = runRequest(GET_LOCK_VALUE_LOOKUP_DAS, pipe);
  size_t sep1 = output.find(",\n");
  size_t sep2 = output.find(",\n", sep1 + 1);
  REQUIRE(sep1 != std::string::npos);
  REQUIRE(sep2 != std::string::npos);
  std::string deepValues = output.substr(sep2 + 2, output.size() - sep2 - 3);
  CHECK(lookupMapValue(deepValues, "1|4|11") > lookupMapValue(deepValues, "1|7|11"));
}

TEST_CASE("runDebugGroundTruth validates arguments and compares placements across 7 pieces") {
  std::string board = makeFlatBoard(4);
  std::string pipe = InputBuilder()
                         .withBoard1(board)
                         .withLevel(18)
                         .withLines(0)
                         .withCurPiece(IDX_L)
                         .withNextPiece(-1)
                         .withTimeline("X..")
                         .withPlayoutCount(0)
                         .withPlayoutLength(2)
                         .withPruningBreadth(20)
                         .withAggression("safe")
                         .withDasCharge(16)
                         .build()
                     + "18|";

  // Validation errors
  CHECK(debug_gt::runDebugGroundTruth(pipe, {}, -1, 5).rfind("Error:", 0) == 0);
  CHECK(debug_gt::runDebugGroundTruth(pipe, {}, 50000, 5).rfind("Error:", 0) == 0);
  CHECK(debug_gt::runDebugGroundTruth(pipe, {}, 10, 0).rfind("Error:", 0) == 0);
  CHECK(debug_gt::runDebugGroundTruth(pipe, {"a", "b", "c", "d", "e", "f", "g"}, 10, 5)
            .rfind("Error:", 0)
        == 0);
  CHECK(debug_gt::runDebugGroundTruth("bad_pipe", {}, 10, 5).rfind("Error:", 0) == 0);

  // Valid eval-only + playout runs
  std::string json = debug_gt::runDebugGroundTruth(pipe, {}, /* playoutCount= */ 20,
                                                   /* maxConts= */ 3);
  REQUIRE(json.rfind("Error:", 0) != 0);
  CHECK(json.find("\"aiMode\":\"STANDARD\"") != std::string::npos);
  CHECK(json.find("\"playoutCount\":20") != std::string::npos);
  CHECK(json.find("\"reachablePlacements\":[") != std::string::npos);
  CHECK(json.find("\"candidates\":[") != std::string::npos);
  CHECK(json.find("\"weightedAvg\":") != std::string::npos);
  CHECK(json.find("\"bestContEvalRank\":") != std::string::npos);
  for (const char *p : {"\"I\":", "\"O\":", "\"L\":", "\"J\":", "\"T\":", "\"S\":", "\"Z\":"}) {
    CHECK(json.find(p) != std::string::npos);
  }

  // Unreachable key is reported cleanly rather than crashing.
  std::string unreachableJson =
      debug_gt::runDebugGroundTruth(pipe, {"9|9|9"}, /* playoutCount= */ 0, /* maxConts= */ 3);
  CHECK(unreachableJson.find("\"key\":\"9|9|9\",\"reachable\":false") != std::string::npos);

  // 1-ply NNB ground truth mode.
  std::string nnbJson = debug_gt::runDebugGroundTruth(pipe, {}, /* playoutCount= */ 20,
                                                      /* maxConts= */ 3, /* noNextBox= */ true);
  REQUIRE(nnbJson.rfind("Error:", 0) != 0);
  CHECK(nnbJson.find("\"isNoNextBox\":true") != std::string::npos);
  CHECK(nnbJson.find("\"avgDasPenalty\":") != std::string::npos);
  CHECK(nnbJson.find("\"bestCont\":\"none\"") != std::string::npos);
}

TEST_CASE("runDebugPrecompute returns ranked phantom collapse breakdown matching winner") {
  std::string board = makeFlatBoard(4);
  std::string pipe = InputBuilder()
                         .withBoard1(board)
                         .withLevel(19)
                         .withLines(0)
                         .withCurPiece(IDX_L)
                         .withNextPiece(-1)
                         .withTimeline("X..")
                         .withPlayoutCount(0)
                         .withPlayoutLength(0)
                         .withPruningBreadth(20)
                         .withAggression("safe")
                         .withDasCharge(16)
                         .build()
                     + "18|";

  // Validation errors
  CHECK(debug_pre::runDebugPrecompute("bad_pipe").rfind("Error:", 0) == 0);

  std::string json = debug_pre::runDebugPrecompute(pipe);
  REQUIRE(json.rfind("Error:", 0) != 0);
  CHECK(json.find("\"aiMode\":\"STANDARD\"") != std::string::npos);
  CHECK(json.find("\"useDas\":true") != std::string::npos);
  CHECK(json.find("\"reactionTime\":18") != std::string::npos);
  CHECK(json.find("\"isWinner\":true") != std::string::npos);
  CHECK(json.find("\"weightedAvg\":") != std::string::npos);
  CHECK(json.find("\"rawLockValue\":") != std::string::npos);
  CHECK(json.find("\"dasPenalty\":") != std::string::npos);
  CHECK(json.find("\"inputCost\":") != std::string::npos);

  // Winner's response matches standard runRequest(PRECOMPUTE, pipe) exactly.
  std::string expectedResponse = runRequest(PRECOMPUTE, pipe);
  CHECK(json.find("\"response\":\"" + debug_gt::jsonEscape(expectedResponse) + "\"")
        != std::string::npos);
}

TEST_CASE("runDebugTournament traces R1/Cut1/R2/Cut2/R3 and validates inputs") {
  std::string board = makeFlatBoard(4);
  std::string tmtPipe = InputBuilder()
                            .withBoard1(board)
                            .withLevel(19)
                            .withLines(0)
                            .withCurPiece(IDX_L)
                            .withNextPiece(-1)
                            .withTimeline("X..")
                            .withPlayoutCount(2500)
                            .withPlayoutLength(4)
                            .withPruningBreadth(-1)
                            .withAggression("safe")
                            .withDasCharge(16)
                            .build()
                        + "18|";
  std::string legacyPipe = InputBuilder()
                               .withBoard1(board)
                               .withLevel(19)
                               .withLines(0)
                               .withCurPiece(IDX_L)
                               .withNextPiece(-1)
                               .withTimeline("X..")
                               .withPlayoutCount(20)
                               .withPlayoutLength(4)
                               .withPruningBreadth(20)
                               .withAggression("safe")
                               .withDasCharge(16)
                               .build()
                           + "18|";

  // Validation checks (-1 is NEXT_PIECE_NNB; -2 and 7 are out of range)
  CHECK(debug_tmt::parseNextPieceArg("NNB") == debug_tmt::NEXT_PIECE_NNB);
  CHECK(debug_tmt::parseNextPieceArg("-1") == debug_tmt::NEXT_PIECE_NNB);
  CHECK(debug_tmt::parseNextPieceArg("X") == debug_tmt::NEXT_PIECE_INVALID);
  CHECK(debug_tmt::runDebugTournament(tmtPipe, debug_tmt::NEXT_PIECE_INVALID).rfind("Error:", 0)
        == 0);
  CHECK(debug_tmt::runDebugTournament(tmtPipe, 7).rfind("Error:", 0) == 0);
  CHECK(debug_tmt::runDebugTournament(legacyPipe, IDX_J).rfind("Error:", 0) == 0);
  CHECK(debug_tmt::runDebugTournament("bad_pipe", IDX_J).rfind("Error:", 0) == 0);

  // Valid 2-ply tournament trace
  std::string json = debug_tmt::runDebugTournament(tmtPipe, IDX_J);
  REQUIRE(json.rfind("Error:", 0) != 0);
  CHECK(json.find("\"isNoNextBox\":false") != std::string::npos);
  CHECK(json.find("\"nextPiece\":\"J\"") != std::string::npos);
  CHECK(json.find("\"usedEvalOnlyFallback\":false") != std::string::npos);
  CHECK(json.find("\"survivorsCut1\":") != std::string::npos);
  CHECK(json.find("\"finalistsCut2\":") != std::string::npos);
  CHECK(json.find("\"publishedScore\":") != std::string::npos);
  CHECK(json.find("\"conts\":[") != std::string::npos);
  CHECK(json.find("\"isR3Cont\":true") != std::string::npos);

  // Valid 1-ply NNB tournament trace
  std::string nnbJson = debug_tmt::runDebugTournament(tmtPipe, debug_tmt::NEXT_PIECE_NNB);
  REQUIRE(nnbJson.rfind("Error:", 0) != 0);
  CHECK(nnbJson.find("\"isNoNextBox\":true") != std::string::npos);
  CHECK(nnbJson.find("\"nextPiece\":\"NNB\"") != std::string::npos);
  CHECK(nnbJson.find("\"useTieredR1\":false") != std::string::npos);
  CHECK(nnbJson.find("\"contKey\":\"none\"") != std::string::npos);
  CHECK(nnbJson.find("\"isR3Cont\":true") != std::string::npos);
}

#endif // SR_REQUEST_TYPE_TESTS
