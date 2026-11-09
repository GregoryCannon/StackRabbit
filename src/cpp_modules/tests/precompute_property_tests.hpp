// Precompute pipeline property/invariant tests on real golden inputs.
//
// Runs preparePrecomputeState + collapseAndFormatPrecompute over the baked
// golden fixtures (precompute_pipeline_fixtures.hpp: every production-tier
// record plus hand-picked eval-only edge records) and asserts the pipeline's
// output contracts, independent of any particular value:
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
//   f. the golden outputs are reproduced byte for byte.
//
// The state comes from the prepared pipeline (the searches and the seven
// concurrent lock lookups really run here); the expectations are recomputed
// from the state's maps, never from the response under test.

#ifndef SR_PRECOMPUTE_PROPERTY_TESTS
#define SR_PRECOMPUTE_PROPERTY_TESTS

#include "precompute_pipeline_fixtures.hpp"

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

static std::vector<std::string> splitLines(const std::string &response) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    const size_t end = response.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(response.substr(start));
      break;
    }
    lines.push_back(response.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

/** The game state and request fields one fixture runs under. */
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
  prepared.state = preparePrecomputeState(
      gameState, &PIECE_LIST[curPieceIdx], fields[5].c_str(), dasCharge, reactionTime,
      atoi(fields[8].c_str()), atoi(fields[6].c_str()), atoi(fields[7].c_str()), &evalContext,
      pieceRangeContextLookup);
  return prepared;
}

static std::string collapseFixture(const PreparedFixture &prepared, const PrecomputeState &state) {
  unsigned int board[20];
  encodeBoard(prepared.boardStr.c_str(), board);
  return collapseAndFormatPrecompute(state, board, &PIECE_LIST[prepared.curPieceIdx], prepared.level,
                                     prepared.lines, SHOULD_PUSHDOWN);
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
        const double lock = map.find(key) == map.end()
                                ? std::numeric_limits<double>::quiet_NaN()
                                : map.at(key);
        const double value = lock + inputCostX10ForChar(phantom.initial.tuckInput) / 10.0
                             + recomputedDasPenalty(state.useDAS, phantom.initial.dasChargeAfter,
                                                    key, state.partialPenalties[i],
                                                    state.minPenalties[i]);
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
          const double value = state.lockValues[i].at(key) + adjustmentInputCost(adjustment)
                               + recomputedDasPenalty(state.useDAS, adjustment.dasChargeAfter, key,
                                                      state.partialPenalties[i],
                                                      state.minPenalties[i]);
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
  const std::vector<std::string> lines = splitLines(response);
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
  const std::vector<std::string> lines = splitLines(response);
  REQUIRE(lines.size() == 8);
  for (int i = 1; i < 8; i++) {
    const std::string body = lines[i].substr(2);
    if (body == "continue" || body == "none") {
      continue;
    }
    std::vector<std::string> segments;
    size_t start = 0;
    while (true) {
      const size_t end = body.find('|', start);
      if (end == std::string::npos) {
        segments.push_back(body.substr(start));
        break;
      }
      segments.push_back(body.substr(start, end - start));
      start = end + 1;
    }
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

TEST_CASE("precompute pipeline invariants hold on every golden fixture") {
  for (int fixtureIdx = 0; fixtureIdx < PRECOMPUTE_PIPELINE_FIXTURE_COUNT; fixtureIdx++) {
    const PrecomputePipelineFixture &fixture = PRECOMPUTE_PIPELINE_FIXTURES[fixtureIdx];
    INFO("fixture ", fixtureIdx, " tier=", fixture.production ? "production" : "edge");
    const PreparedFixture prepared = prepareFixtureFromPipe(fixture.input);

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

    // (a) + (b) output shape
    checkResponseWellFormed(response);
    checkPieceLinesWellFormed(response);

    if (response == "none") {
      // No phantom qualified: the recomputed argmax must agree that there is
      // nothing to answer with.
      CHECK(winner == -1);
      continue;
    }
    REQUIRE(winner != -1);
    REQUIRE(winner < static_cast<int>(prepared.state.phantoms.size()));

    // (c) fallback markers occur exactly under their conditions
    const PrecomputePhantom &winningPhantom = prepared.state.phantoms[winner];
    const bool canContinue =
        winningPhantom.hasInitialPlacement
        && winningPhantom.initial.inputSequence.length() > (size_t)prepared.reactionTime;
    const std::vector<std::string> lines = splitLines(response);
    for (int i = 1; i < 8; i++) {
      const std::string body = lines[i].substr(2);
      const bool marker = body == "continue" || body == "none";
      bool anyAdjustmentHit = false;
      for (const PrecomputePossibility &adjustment : winningPhantom.possibleAdjustments) {
        if (prepared.state.lockValues[i - 1].count(
                encodeLockCell(adjustment.rot, adjustment.x, adjustment.y))) {
          anyAdjustmentHit = true;
          break;
        }
      }
      const bool expectedMarker =
          winningPhantom.possibleAdjustments.empty() || !anyAdjustmentHit;
      if (expectedMarker) {
        CHECK(marker);
        if (marker) {
          CHECK(body == (canContinue ? "continue" : "none"));
        }
      } else {
        CHECK_FALSE(marker);
      }
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

TEST_CASE("prepare + collapse reproduce the recorded goldens byte for byte") {
  for (int fixtureIdx = 0; fixtureIdx < PRECOMPUTE_PIPELINE_FIXTURE_COUNT; fixtureIdx++) {
    const PrecomputePipelineFixture &fixture = PRECOMPUTE_PIPELINE_FIXTURES[fixtureIdx];
    INFO("fixture ", fixtureIdx, " tier=", fixture.production ? "production" : "edge");
    const PreparedFixture prepared = prepareFixtureFromPipe(fixture.input);
    CHECK(collapseFixture(prepared, prepared.state) == fixture.golden);

    // Sampled cross-check through the whole engine surface (mainProcess):
    // it re-runs the pipeline, so only the edge records plus every tenth
    // production record go through it here; the full-set equivalent is the
    // precompute_verify battery item. This spot-check pins that the context
    // setup above matches main.cpp's own.
    if (!fixture.production || fixtureIdx % 10 == 0) {
      CHECK(runRequest(PRECOMPUTE, fixture.input) == fixture.golden);
    }
  }
}

#endif // SR_PRECOMPUTE_PROPERTY_TESTS
