// Dumps the C++ move-search results for fuzz fixtures in the same entry format
// as src/server/move_search_fixtures.json, for external diffing.
//
// TODO(Phase 5 cutover): at cutover the JSON oracle becomes a hand-maintained
// characterization baseline re-baselined from C++ itself (see TESTING.md).
//
// Usage:
//   ./build/fixture_dump --das    all DAS fixtures (dasCharge >= 0)
//   ./build/fixture_dump --all    every fixture
//   ./build/fixture_dump 3 17     specific fixture indexes
//   ./build/fixture_dump --adj 0 5  specific adjustment fixtures
//   ./build/fixture_dump --rt 0 5   specific reaction-time fixtures
//
// Every fixture runs through fullMoveSearch with sequence tracking enabled.
// Tap-mode fixtures (dasCharge == -1) exercise fullMoveSearch's ARR-gated tap
// path and come out with dasChargeAfter = -1 and their full input sequences.
//
// Consumed by `npm run fuzz_verify_sequences` (src/server/move_search_fuzz_verify.js).
//
// Adjustment-mode fixtures are dumped
// under "adjustment <idx>" headers from the ADJUSTMENT_FIXTURES section: each
// runs fullMoveSearch with a FullMoveSearchContext built from the recorded
// mid-air entry parameters (isAdjustment always true, no reaction time).
//
// Reaction-time fixtures are dumped under "reactiontime <idx>" headers from
// the REACTION_TIME_FIXTURES section: each runs fullMoveSearch as a spawn
// search with the recorded reaction time and dumps every result's adj-time
// adjTimeSimState snapshot slot. NO sorting here -- the reaction-time section
// is compared in result order (the result-to-snapshot mapping is part of the
// golden).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/main.cpp"
#include "move_search_fixtures.hpp"

static GameState prepareGameState(int level, int boardIdx) {
  GameState gameState = {};
  gameState.level = level;
  gameState.lines = 0;
  encodeBoard(FIXTURE_BOARD_POOL[boardIdx], gameState.board);
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  updateSurfaceAndHoles(gameState.surfaceArray, gameState.board, /*excludeHolesColumn=*/9,
                        /*isDigMode=*/false);
  return gameState;
}

static void dumpRows(const Piece *piece, int idx, const char *kind, const char *detail,
                     const std::vector<FullMovePossibility> &results) {
  std::vector<std::string> rows;
  for (const FullMovePossibility &p : results) {
    char buf[128];
    snprintf(buf, sizeof(buf), "r%d x%d y%d c%d k%d seq=", p.rotationIndex, p.x, p.y,
             p.dasChargeAfter, inputCostX10ForChar(p.tuckInput));
    rows.push_back(std::string(buf) + p.inputSequence);
  }

  // Dumped in result order: the dump feeds the order-exact comparison, and
  // the result order is load-bearing downstream.
  printf("%s %d (%c %s) n=%d\n", kind, idx, piece->id, detail, (int)rows.size());
  for (const std::string &r : rows) {
    printf("  %s\n", r.c_str());
  }
  fflush(stdout);
}

static void dumpFixture(int idx) {
  const MoveSearchFixture &f = MOVE_SEARCH_FIXTURES[idx];
  GameState gameState = prepareGameState(f.level, f.boardIdx);
  const Piece *piece = &PIECE_LIST[f.pieceIndex];
  std::vector<FullMovePossibility> results;
  fullMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                 /*trackInputSequences=*/true, results);
  char detail[64];
  snprintf(detail, sizeof(detail), "L%d %s c%d", f.level, FIXTURE_TIMELINE_POOL[f.timelineIdx],
           f.dasCharge);
  dumpRows(piece, idx, "fixture", detail, results);
}

static void dumpAdjustmentFixture(int idx) {
  const AdjustmentSearchFixture &f = ADJUSTMENT_FIXTURES[idx];
  GameState gameState = prepareGameState(f.level, f.boardIdx);
  const Piece *piece = &PIECE_LIST[f.pieceIndex];

  FullMoveSearchContext ctx;
  ctx.existingXOffset = f.existingXOffset;
  ctx.existingYOffset = f.existingYOffset;
  ctx.framesAlreadyElapsed = f.framesAlreadyElapsed;
  ctx.existingRotation = f.existingRotation;
  ctx.isAdjustment = true;
  ctx.isArrContinued = f.isArrContinued != 0;
  ctx.dasButtonHeld = f.dasButtonHeld;
  ctx.preAdjInputSequence = FIXTURE_PREFIX_POOL[f.preAdjSeqIdx];

  std::vector<FullMovePossibility> results;
  fullMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                 /*trackInputSequences=*/true, results, ctx);
  char detail[160];
  snprintf(detail, sizeof(detail), "L%d %s c%d f%d x%d y%d r%d arr%d held%d seq=%s", f.level,
           FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge, f.framesAlreadyElapsed,
           f.existingXOffset, f.existingYOffset, f.existingRotation, f.isArrContinued,
           f.dasButtonHeld, FIXTURE_PREFIX_POOL[f.preAdjSeqIdx]);
  dumpRows(piece, idx, "adjustment", detail, results);
}

static void dumpReactionTimeFixture(int idx) {
  const ReactionTimeSearchFixture &f = REACTION_TIME_FIXTURES[idx];
  GameState gameState = prepareGameState(f.level, f.boardIdx);
  const Piece *piece = &PIECE_LIST[f.pieceIndex];

  std::vector<FullMovePossibility> results;
  std::vector<AdjTimeSimStateSlot> captures;
  fullMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                 /*trackInputSequences=*/true, results, FullMoveSearchContext(), f.reactionTime,
                 &captures);

  char detail[96];
  snprintf(detail, sizeof(detail), "L%d %s c%d rt%d", f.level,
           FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge, f.reactionTime);
  printf("reactiontime %d (%c %s) n=%d\n", idx, piece->id, detail, (int)results.size());
  for (size_t i = 0; i < results.size(); i++) {
    const FullMovePossibility &p = results[i];
    const FixtureAdjTimeCapture &c = REACTION_TIME_CAPTURE_POOL[f.captureOffset + i];
    char buf[512];
    snprintf(buf, sizeof(buf),
             "  r%d x%d y%d c%d k%d seq=%s adj=%d f%d x%d y%d rot%d c%d arr%d held%d pre=%s",
             p.rotationIndex, p.x, p.y, p.dasChargeAfter, inputCostX10ForChar(p.tuckInput),
             p.inputSequence.c_str(), c.hasCapture, c.frameIndex, c.x, c.y, c.rotationIndex,
             c.dasCharge, c.isArrContinued, c.dasButtonHeld,
             c.hasCapture ? FIXTURE_RT_PREFIX_POOL[c.preAdjSeqIdx] : "");
    printf("%s\n", buf);
  }
  fflush(stdout);
}

int main(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "--das") == 0) {
    for (int i = 0; i < MOVE_SEARCH_FIXTURE_COUNT; i++) {
      if (MOVE_SEARCH_FIXTURES[i].dasCharge != -1) {
        dumpFixture(i);
      }
    }
  } else if (argc >= 2 && strcmp(argv[1], "--all") == 0) {
    for (int i = 0; i < MOVE_SEARCH_FIXTURE_COUNT; i++) {
      dumpFixture(i);
    }
    for (int i = 0; i < ADJUSTMENT_FIXTURE_COUNT; i++) {
      dumpAdjustmentFixture(i);
    }
    for (int i = 0; i < REACTION_TIME_FIXTURE_COUNT; i++) {
      dumpReactionTimeFixture(i);
    }
  } else if (argc >= 3 && strcmp(argv[1], "--adj") == 0) {
    for (int a = 2; a < argc; a++) {
      dumpAdjustmentFixture(atoi(argv[a]));
    }
  } else if (argc >= 3 && strcmp(argv[1], "--rt") == 0) {
    for (int a = 2; a < argc; a++) {
      dumpReactionTimeFixture(atoi(argv[a]));
    }
  } else {
    for (int a = 1; a < argc; a++) {
      dumpFixture(atoi(argv[a]));
    }
  }
  return 0;
}
