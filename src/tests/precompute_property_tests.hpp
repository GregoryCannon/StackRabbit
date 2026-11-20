// Precompute pipeline property/invariant tests on inline representative cases.
//
// Runs preparePrecomputeState + collapseAndFormatPrecompute over a compact set
// of inline cases covering each structural regime (tap, DAS, reactionTime == 0,
// per-piece continue fallback, per-piece none fallback, whole-response none,
// and a level transition) and asserts the pipeline's output contracts,
// independent of any particular eval weights:
//
//   a. well-formedness: exactly "none", or "Default:" + seven piece lines in
//      IOLJTSZ order;
//   b. piece lines are content lines (six |-segments, 200-char board, sane
//      level/lines/das fields) or exactly the fallback markers;
//   c. fallback markers occur exactly under their conditions, recomputed
//      from the state's own maps against the winning phantom;
//   d. every lock/penalty map value is finite and within +-1e6 -- the cheap
//      NaN / forgotten-scale-factor guard for eval changes;
//   e. the winning phantom is the argmax recomputed from the maps, ties keep
//      the first phantom, and a reduced single-phantom state reproduces the
//      full response;
//   f. runRequest(PRECOMPUTE, pipe) matches prepare + collapse byte for byte,
//      pinning the test harness's context setup to main.cpp.
//
// Full golden byte-parity over all 3,470 fixtures is checked separately by
// `npm run precompute_verify`.

#ifndef SR_PRECOMPUTE_PROPERTY_TESTS
#define SR_PRECOMPUTE_PROPERTY_TESTS

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// --- plumbing ------------------------------------------------------------

static std::vector<std::string> splitPipeFields(const std::string &pipe) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t end = pipe.find('|', start);
    if (end == std::string::npos) {
      break;
    }
    fields.push_back(pipe.substr(start, end - start));
    start = end + 1;
  }
  return fields; // 12 fields; the trailing '|' produces no extra entry
}

static std::vector<std::string> splitPrecomputeByChar(const std::string &str, char delim) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t end = str.find(delim, start);
    if (end == std::string::npos) {
      parts.push_back(str.substr(start));
      break;
    }
    parts.push_back(str.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

static std::string makePrecomputePipe(const std::string &board, int level, int lines, int curPiece,
                                      const std::string &timeline, int playoutCount,
                                      int playoutLength, int pruningBreadth, int dasCharge,
                                      int reactionTime) {
  return InputBuilder()
             .withBoard1(board)
             .withLevel(level)
             .withLines(lines)
             .withCurPiece(curPiece)
             .withNextPiece(-1)
             .withTimeline(timeline)
             .withPlayoutCount(playoutCount)
             .withPlayoutLength(playoutLength)
             .withPruningBreadth(pruningBreadth)
             .withAggression("safe")
             .withDasCharge(dasCharge)
             .build()
         + std::to_string(reactionTime)
         + "|";
}

/** The game state and request fields one test case runs under. */
struct PreparedFixture {
  PrecomputeState state;
  std::string boardStr;
  int level;
  int lines;
  int reactionTime;
  int dasCharge;
  int curPieceIdx;
};

// Mirrors main.cpp's PRECOMPUTE setup: board parse, surface/holes, the
// piece-range and eval contexts, then preparePrecomputeState (whose seven
// per-next-piece lock lookups run on their threads exactly as in production).
static PreparedFixture prepareFixtureFromPipe(const std::string &pipe) {
  const std::vector<std::string> fields = splitPipeFields(pipe);
  REQUIRE(fields.size() == 12);

  GameState gameState = {};
  encodeBoard(fields[0].c_str(), gameState.board);
  gameState.level = atoi(fields[1].c_str());
  gameState.lines = atoi(fields[2].c_str());
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  std::pair<int, float> holes = updateSurfaceAndHoles(gameState.surfaceArray, gameState.board,
                                                      /* wellColumn= */ 9, /* isDigMode= */ false);
  gameState.numTrueHoles = holes.first;
  gameState.numPartialHoles = holes.second;

  const int dasCharge = atoi(fields[10].c_str());
  std::string dasEquivalentStorage;
  char const *contextTimeline = fields[5].c_str();
  if (dasCharge >= 0) {
    dasEquivalentStorage = getDasEquivalentInputTimeline(fields[5].c_str());
    contextTimeline = dasEquivalentStorage.c_str();
  }
  PieceRangeContext pieceRangeContextLookup[4] = {
      getPieceRangeContext(contextTimeline, 1, /* gravityDoubled= */ true),
      getPieceRangeContext(contextTimeline, 1, /* gravityDoubled= */ false),
      getPieceRangeContext(contextTimeline, 2, /* gravityDoubled= */ false),
      getPieceRangeContext(contextTimeline, 3, /* gravityDoubled= */ false),
  };
  AggressionLevel aggression = SAFE;
  if (fields[9] == "aggro") {
    aggression = AGGRO;
  } else if (fields[9] == "hyper") {
    aggression = HYPER;
  }
  const EvalContext evalContext = getEvalContext(gameState, pieceRangeContextLookup,
                                                 /* useTestWeights= */ false, aggression);
  std::pair<int, float> holes2 = updateSurfaceAndHoles(
      gameState.surfaceArray, gameState.board,
      evalContext.countWellHoles ? -1 : evalContext.wellColumn, evalContext.aiMode == DIG);
  gameState.numTrueHoles = holes2.first;
  gameState.numPartialHoles = holes2.second;

  const int curPieceIdx = atoi(fields[3].c_str());
  const int reactionTime = atoi(fields[11].c_str());

  PreparedFixture prepared;
  prepared.boardStr = fields[0];
  prepared.level = gameState.level;
  prepared.lines = gameState.lines;
  prepared.reactionTime = reactionTime;
  prepared.dasCharge = dasCharge;
  prepared.curPieceIdx = curPieceIdx;
  prepared.state =
      preparePrecomputeState(gameState, &PIECE_LIST[curPieceIdx], fields[5].c_str(), dasCharge,
                             reactionTime, atoi(fields[8].c_str()), atoi(fields[6].c_str()),
                             atoi(fields[7].c_str()), &evalContext, pieceRangeContextLookup);
  return prepared;
}

static std::string collapseFixture(const PreparedFixture &prepared, const PrecomputeState &state) {
  unsigned int board[20];
  encodeBoard(prepared.boardStr.c_str(), board);
  return collapseAndFormatPrecompute(state, board, &PIECE_LIST[prepared.curPieceIdx],
                                     prepared.level, prepared.lines, SHOULD_PUSHDOWN);
}

// --- independent recomputation ------------------------------------------

static double recomputedDasPenalty(bool useDas, int dasChargeAfter, const std::string &key,
                                   const PrecomputeValueMap &partialPenalties,
                                   const PrecomputeValueMap &minPenalties) {
  // tap mode => 0; a key missing from either penalty map => 0;
  // dasChargeAfter >= 15 => 0; >= 10 => partial map; otherwise the min map.
  if (!useDas) {
    return 0;
  }
  if (partialPenalties.find(key) == partialPenalties.end()
      || minPenalties.find(key) == minPenalties.end()) {
    return 0;
  }
  if (dasChargeAfter >= 15) {
    return 0;
  }
  if (dasChargeAfter >= 10) {
    return partialPenalties.at(key);
  }
  return minPenalties.at(key);
}

static std::vector<double> recomputedPhantomTotals(const PrecomputeState &state,
                                                   const Piece *curPiece) {
  std::vector<double> totals;
  for (const PrecomputePhantom &phantom : state.phantoms) {
    double totalValue = 0;
    if (phantom.possibleAdjustments.empty()) {
      for (int i = 0; i < 7; i++) {
        const std::string key =
            encodeLockCell(phantom.initial.rot, phantom.initial.x, phantom.initial.y);
        const PrecomputeValueMap &map = state.lockValues[i];
        const double lock =
            map.find(key) == map.end() ? std::numeric_limits<double>::quiet_NaN() : map.at(key);
        const double value =
            lock
            + inputCostX10ForChar(phantom.initial.tuckInput) / 10.0
            + recomputedDasPenalty(state.useDAS, phantom.initial.dasChargeAfter, key,
                                   state.partialPenalties[i], state.minPenalties[i]);
        const double term = value * pieceProbability(curPiece, &PIECE_LIST[i]);
        totalValue += term;
      }
    } else {
      for (int i = 0; i < 7; i++) {
        double maxValue = PRECOMPUTE_MIN_VALUE;
        for (const PrecomputePossibility &adjustment : phantom.possibleAdjustments) {
          const std::string key = encodeLockCell(adjustment.rot, adjustment.x, adjustment.y);
          if (state.lockValues[i].find(key) == state.lockValues[i].end()) {
            continue;
          }
          const double value =
              state.lockValues[i].at(key)
              + adjustmentInputCost(adjustment, phantom, state.reactionTime)
              + recomputedDasPenalty(state.useDAS, adjustment.dasChargeAfter, key,
                                     state.partialPenalties[i], state.minPenalties[i]);
          // equal value keeps the later adjustment, mirroring the collapse
          if (value >= maxValue) {
            maxValue = value;
          }
        }
        const double term = maxValue * pieceProbability(curPiece, &PIECE_LIST[i]);
        totalValue += term;
      }
    }
    totals.push_back(totalValue / 7);
  }
  return totals;
}

static int winningPhantomIndex(const std::vector<double> &totals) {
  int winner = -1;
  double bestValue = PRECOMPUTE_MIN_VALUE;
  for (int i = 0; i < static_cast<int>(totals.size()); i++) {
    // strict >: a tie keeps the earlier phantom
    if (totals[i] > bestValue) {
      bestValue = totals[i];
      winner = i;
    }
  }
  return winner;
}

// --- the tests -----------------------------------------------------------

static void checkResponseWellFormed(const std::string &response) {
  if (response == "none") {
    return;
  }
  REQUIRE(response.size() >= 8);
  CHECK(response.substr(0, 8) == "Default:");
  const std::vector<std::string> lines = splitPrecomputeByChar(response, '\n');
  REQUIRE(lines.size() == 8);
  for (int i = 1; i < 8; i++) {
    REQUIRE(lines[i].size() >= 2);
    CHECK(lines[i][0] == "IOLJTSZ"[i - 1]);
    CHECK(lines[i][1] == ':');
  }
}

static void checkPieceLinesWellFormed(const std::string &response) {
  if (response == "none") {
    return;
  }
  const std::vector<std::string> lines = splitPrecomputeByChar(response, '\n');
  REQUIRE(lines.size() == 8);
  for (int i = 1; i < 8; i++) {
    const std::string body = lines[i].substr(2);
    if (body == "continue" || body == "none") {
      continue;
    }
    const std::vector<std::string> segments = splitPrecomputeByChar(body, '|');
    CHECK(segments.size() == 6);
    if (segments.size() != 6) {
      continue;
    }
    CHECK(segments[2].size() == 200); // rendered board
    CHECK(atoi(segments[3].c_str()) >= 18);
    CHECK(atoi(segments[4].c_str()) >= 0);
    const int das = atoi(segments[5].c_str());
    CHECK(das >= -1);
    CHECK(das <= 16);
  }
}

TEST_CASE("precompute pipeline invariants hold across all structural regimes") {
  enum RegimeExpectation {
    EXPECT_TAP_RT_POS_CONTENT,
    EXPECT_DAS_RT_POS_CONTENT,
    EXPECT_ZERO_RT,
    EXPECT_PER_PIECE_CONTINUE,
    EXPECT_PER_PIECE_NONE,
    EXPECT_WHOLE_RESPONSE_NONE,
    EXPECT_LEVEL_TRANSITION,
  };

  struct PrecomputePropertyCase {
    const char *name;
    RegimeExpectation expectedRegime;
    std::string input;
  };

  const std::vector<PrecomputePropertyCase> cases = {
      // 1. Tap mode with reaction time > 0 and production-shaped playouts.
      {"tap_rt8_production", EXPECT_TAP_RT_POS_CONTENT,
       makePrecomputePipe(makeWellBoard(4, 4, 9), 18, 0, IDX_T, "X.....", 49, 2, 20,
                          /* dasCharge= */ -1, /* reactionTime= */ 8)},
      // 2. DAS mode with reaction time > 0 and production-shaped playouts.
      {"das_rt8_production", EXPECT_DAS_RT_POS_CONTENT,
       makePrecomputePipe(makeWellBoard(4, 4, 9), 18, 0, IDX_L, "X.....", 49, 2, 20,
                          /* dasCharge= */ 10, /* reactionTime= */ 8)},
      // 3. Zero reaction time ("Default:N/A (0 reaction time)").
      {"reaction_time_zero", EXPECT_ZERO_RT,
       makePrecomputePipe(makeEmptyBoard(), 18, 0, IDX_I, "X.....", 0, 0, 20,
                          /* dasCharge= */ -1, /* reactionTime= */ 0)},
      // 4. Per-piece "continue" fallback: 16-high well board locks at frame 9
      //    (< reactionTime=12), while the full sequence (with lock/ARE frames) extends past
      //    reactionTime=12.
      {"per_piece_continue_fallback", EXPECT_PER_PIECE_CONTINUE,
       makePrecomputePipe(makeWellBoard(16, 16, 9), 18, 0, IDX_O, "X.....", 0, 0, 20,
                          /* dasCharge= */ -1, /* reactionTime= */ 12)},
      // 5. Per-piece "none" fallback: same 16-high well board locks before reactionTime=30
      //    and the full sequence (27 frames) does not extend past reactionTime=30.
      {"per_piece_none_fallback", EXPECT_PER_PIECE_NONE,
       makePrecomputePipe(makeWellBoard(16, 16, 9), 18, 0, IDX_O, "X.....", 0, 0, 20,
                          /* dasCharge= */ -1, /* reactionTime= */ 30)},
      // 6. Whole-response "none": topped-out right-well board with no legal O placement.
      {"whole_response_none", EXPECT_WHOLE_RESPONSE_NONE,
       makePrecomputePipe(makeWellBoard(20, 20, 9), 18, 0, IDX_O, "X.....", 0, 0, 20,
                          /* dasCharge= */ -1, /* reactionTime= */ 8)},
      // 7. Level transition: clearing the 4-deep right well at lines=129 advances level 18 -> 19.
      //    Relies on the evaluator taking the available Tetris clear for at least one next piece.
      {"level_transition_18_to_19", EXPECT_LEVEL_TRANSITION,
       makePrecomputePipe(makeWellBoard(4, 4, 9), 18, 129, IDX_I, "X.....", 0, 0, 20,
                          /* dasCharge= */ 15, /* reactionTime= */ 8)},
  };

  for (const PrecomputePropertyCase &testCase : cases) {
    INFO("case=", testCase.name);
    const PreparedFixture prepared = prepareFixtureFromPipe(testCase.input);

    for (const PrecomputePhantom &phantom : prepared.state.phantoms) {
      if (phantom.hasInitialPlacement && phantom.hasAdjustmentState) {
        const std::string &preAdj = phantom.adjustmentCtx.preAdjInputSequence;
        REQUIRE(phantom.inputSequence.size() == preAdj.size());
        if (phantom.inputSequence != preAdj && !preAdj.empty()) {
          // A wall-charge tuck executed on the exact reaction-time frame rewrites the
          // preceding gravity frame's '.' to 'l' or 'r' after the snapshot was recorded.
          CHECK(phantom.inputSequence.substr(0, preAdj.size() - 1)
                == preAdj.substr(0, preAdj.size() - 1));
          CHECK(preAdj.back() == '.');
          CHECK((phantom.inputSequence.back() == 'l' || phantom.inputSequence.back() == 'r'));
        }
      }
    }

    // (d) lock values and penalties: finite, and within a generous +-1e6
    // bound (real eval values are tens to hundreds at most).
    for (int i = 0; i < 7; i++) {
      for (const auto &entry : prepared.state.lockValues[i]) {
        CHECK(std::isfinite(entry.second));
        CHECK(std::abs(entry.second) <= 1e6);
      }
      for (const auto &entry : prepared.state.partialPenalties[i]) {
        CHECK(std::isfinite(entry.second));
        CHECK(std::abs(entry.second) <= 1e6);
      }
      for (const auto &entry : prepared.state.minPenalties[i]) {
        CHECK(std::isfinite(entry.second));
        CHECK(std::abs(entry.second) <= 1e6);
      }
    }

    const std::vector<double> totals =
        recomputedPhantomTotals(prepared.state, &PIECE_LIST[prepared.curPieceIdx]);
    const int winner = winningPhantomIndex(totals);
    const std::string response = collapseFixture(prepared, prepared.state);

    // (f) cross-check through mainProcess so prepareFixtureFromPipe stays pinned to main.cpp.
    CHECK(runRequest(PRECOMPUTE, testCase.input) == response);

    // (a) + (b) output shape
    checkResponseWellFormed(response);
    checkPieceLinesWellFormed(response);

    if (response == "none") {
      CHECK(testCase.expectedRegime == EXPECT_WHOLE_RESPONSE_NONE);
      // No phantom qualified: the recomputed argmax must agree that there is
      // nothing to answer with.
      CHECK(winner == -1);
      continue;
    }
    CHECK(testCase.expectedRegime != EXPECT_WHOLE_RESPONSE_NONE);
    REQUIRE(winner != -1);
    REQUIRE(winner < static_cast<int>(prepared.state.phantoms.size()));

    const std::vector<std::string> lines = splitPrecomputeByChar(response, '\n');

    // (c) fallback / continue markers occur exactly under their conditions
    const PrecomputePhantom &winningPhantom = prepared.state.phantoms[winner];
    const bool canContinue =
        winningPhantom.hasInitialPlacement
        && winningPhantom.initial.inputSequence.length() > (size_t)prepared.reactionTime;
    std::string defaultPostReactionSeq;
    if (winningPhantom.hasInitialPlacement && !winningPhantom.initial.inputSequence.empty()) {
      const int sliceIndex =
          reactionSliceIndex(winningPhantom.initial.inputSequence, prepared.reactionTime);
      if (static_cast<size_t>(sliceIndex) < winningPhantom.initial.inputSequence.size()) {
        defaultPostReactionSeq = winningPhantom.initial.inputSequence.substr(sliceIndex);
      }
    }
    bool caseSawContentLine = false;
    bool caseSawContinue = false;
    bool caseSawPerPieceNone = false;
    bool caseSawLevelIncrease = false;
    if (winningPhantom.hasInitialPlacement) {
      const std::vector<std::string> defSegments = splitPrecomputeByChar(lines[0].substr(8), '|');
      if (defSegments.size() == 6 && atoi(defSegments[3].c_str()) > prepared.level) {
        caseSawLevelIncrease = true;
      }
    }
    for (int i = 1; i < 8; i++) {
      const std::string body = lines[i].substr(2);
      const bool marker = body == "continue" || body == "none";
      if (body == "continue") {
        caseSawContinue = true;
      } else if (body == "none") {
        caseSawPerPieceNone = true;
      } else {
        caseSawContentLine = true;
        const std::vector<std::string> segments = splitPrecomputeByChar(body, '|');
        if (segments.size() == 6 && atoi(segments[3].c_str()) > prepared.level) {
          caseSawLevelIncrease = true;
        }
      }

      const PrecomputePossibility *bestAdj = nullptr;
      double maxVal = PRECOMPUTE_MIN_VALUE;
      for (const PrecomputePossibility &adjustment : winningPhantom.possibleAdjustments) {
        const std::string key = encodeLockCell(adjustment.rot, adjustment.x, adjustment.y);
        if (prepared.state.lockValues[i - 1].find(key) == prepared.state.lockValues[i - 1].end()) {
          continue;
        }
        const double value =
            prepared.state.lockValues[i - 1].at(key)
            + adjustmentInputCost(adjustment, winningPhantom, prepared.reactionTime)
            + recomputedDasPenalty(prepared.state.useDAS, adjustment.dasChargeAfter, key,
                                   prepared.state.partialPenalties[i - 1],
                                   prepared.state.minPenalties[i - 1]);
        if (value >= maxVal) {
          maxVal = value;
          bestAdj = &adjustment;
        }
      }

      if (winningPhantom.possibleAdjustments.empty() || bestAdj == nullptr) {
        CHECK(marker);
        if (marker) {
          CHECK(body == (canContinue ? "continue" : "none"));
        }
      } else {
        const int levelAfter =
            getLevelAfterLineClears(prepared.level, prepared.lines, bestAdj->numLinesCleared);
        const std::string pieceSeq = (SHOULD_PUSHDOWN && levelAfter == 18)
                                         ? pushDownSequence(bestAdj->inputSequence)
                                         : bestAdj->inputSequence;
        if (!defaultPostReactionSeq.empty() && pieceSeq == defaultPostReactionSeq) {
          CHECK(body == "continue");
        } else {
          CHECK_FALSE(marker);
        }
      }
    }

    // Per-case regime check so one case cannot silently drift and be masked by another.
    switch (testCase.expectedRegime) {
    case EXPECT_TAP_RT_POS_CONTENT:
      CHECK(prepared.dasCharge == -1);
      CHECK(prepared.reactionTime > 0);
      CHECK_FALSE(winningPhantom.possibleAdjustments.empty());
      CHECK(caseSawContentLine);
      break;
    case EXPECT_DAS_RT_POS_CONTENT:
      CHECK(prepared.dasCharge >= 0);
      CHECK(prepared.reactionTime > 0);
      CHECK_FALSE(winningPhantom.possibleAdjustments.empty());
      CHECK(caseSawContentLine);
      break;
    case EXPECT_ZERO_RT:
      CHECK(lines[0] == "Default:N/A (0 reaction time)");
      break;
    case EXPECT_PER_PIECE_CONTINUE:
      CHECK(caseSawContinue);
      break;
    case EXPECT_PER_PIECE_NONE:
      CHECK(caseSawPerPieceNone);
      break;
    case EXPECT_WHOLE_RESPONSE_NONE:
      break; // handled before this switch
    case EXPECT_LEVEL_TRANSITION:
      CHECK(caseSawLevelIncrease);
      break;
    }

    // (e) the response is the winner's: reducing the state to the winning
    // phantom alone must reproduce the full response, and a state of two
    // identical (winning) phantoms must keep the first one's response.
    PrecomputeState reduced = prepared.state;
    reduced.phantoms.assign(1, prepared.state.phantoms[winner]);
    CHECK(collapseFixture(prepared, reduced) == response);

    PrecomputeState tiedTwice = prepared.state;
    tiedTwice.phantoms.assign(2, prepared.state.phantoms[winner]);
    CHECK(collapseFixture(prepared, tiedTwice) == response);
  }
}

#endif // SR_PRECOMPUTE_PROPERTY_TESTS
