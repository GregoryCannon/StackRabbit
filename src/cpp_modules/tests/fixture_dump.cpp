// Dumps the C++ move-search results for fuzz fixtures in the same entry format
// as src/server/move_search_fixtures.json, for external diffing.
//
// Usage:
//   ./build/fixture_dump --das    all DAS fixtures (dasCharge >= 0), with sequences
//   ./build/fixture_dump --all    every fixture (tap fixtures print empty sequences)
//   ./build/fixture_dump 3 17     specific fixture indexes
//
// Tap fixtures use the legacy walk-based moveSearch (no sequences -- they are
// derivable downstream in tap mode). DAS fixtures use dasMoveSearch with
// sequence tracking enabled.
//
// Consumed by `npm run fuzz_verify_sequences` (src/server/move_search_fuzz_verify.js).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "../src/main.cpp"
#include "move_search_fixtures.hpp"

static int tuckCostX10(char tuckInput) {
  switch (tuckInput) {
    case 'E': case 'F': case 'I': case 'G': return -3;
    case 'A': case 'B': return -2;
    case 'L': case 'R': return -1;
    default: return 0;
  }
}

static void dumpFixture(int idx) {
  const MoveSearchFixture &f = MOVE_SEARCH_FIXTURES[idx];
  GameState gameState = {};
  gameState.level = f.level;
  gameState.lines = 0;
  encodeBoard(FIXTURE_BOARD_POOL[f.boardIdx], gameState.board);
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  updateSurfaceAndHoles(gameState.surfaceArray, gameState.board, /*excludeHolesColumn=*/9, /*isDigMode=*/false);

  const Piece *piece = &PIECE_LIST[f.pieceIndex];
  std::vector<std::string> rows;
  if (f.dasCharge == -1) {
    std::vector<LockPlacement> locks;
    moveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], locks);
    for (const LockPlacement &lp : locks) {
      char buf[128];
      snprintf(buf, sizeof(buf), "r%d x%d y%d c%d k%d seq=",
               lp.rotationIndex, lp.x, lp.y, -1, tuckCostX10(lp.tuckInput));
      rows.push_back(std::string(buf));
    }
  } else {
    std::vector<DasPossibility> results;
    dasMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                  /*trackInputSequences=*/true, results);
    for (const DasPossibility &p : results) {
      char buf[128];
      snprintf(buf, sizeof(buf), "r%d x%d y%d c%d k%d seq=",
               p.rotationIndex, p.x, p.y, p.dasChargeAfter, p.inputCostX10);
      rows.push_back(std::string(buf) + p.inputSequence);
    }
  }

  std::sort(rows.begin(), rows.end());
  printf("fixture %d (%c L%d %s c%d) n=%d\n", idx, piece->id, f.level,
         FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge, (int)rows.size());
  for (const std::string &r : rows) {
    printf("  %s\n", r.c_str());
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
  } else {
    for (int a = 1; a < argc; a++) {
      dumpFixture(atoi(argv[a]));
    }
  }
  return 0;
}
