#ifndef DAS_MOVE_SEARCH
#define DAS_MOVE_SEARCH

#include <string>
#include <vector>
#include "types.hpp"

/**
 * DAS-aware move search (docs/DAS_move_search_spec.md).
 *
 * A 1:1 port of the TS spawn-placement pipeline (move_search.ts + dfs.ts,
 * INITIAL_PLACEMENT path) mirroring per-goal frame simulation, midair dedup,
 * the fall phase, and depth-1 tuck/spin DFS. Tap mode (dasCharge < 0) never
 * enters this file -- the legacy walk-based search is untouched.
 *
 * Deliberately a SEPARATE entry point from moveSearch(): DAS results carry
 * extra data (charge, sequence, line clears), and keeping LockPlacement /
 * moveSearch byte-identical protects the playout hot path from any struct
 * growth (a +40-byte LockPlacement regressed the speed benchmark ~9%).
 */

/** One computed placement outcome in DAS mode. */
struct DasPossibility {
  int x;
  int y;
  int rotationIndex;
  int dasChargeAfter;      // 0..16 charge at the lock frame
  int inputCostX10;        // 0 = basic; -1 tuck (L/R); -2 spin (A/B); -3 spin-tuck (E/F/I/G)
  char tuckInput;          // '.' for basic placements
  int numLinesCleared;
  std::string inputSequence;
};

/**
 * DAS move search for a piece spawning at the standard location.
 * @param dasCharge - incoming DAS charge 0..16
 * @param trackInputSequences - when false, sequences are still tracked
 *   internally (tuck gating scans them) but not copied into the results.
 */
int dasMoveSearch(GameState gameState,
                  const Piece *piece,
                  char const *inputFrameTimeline,
                  int dasCharge,
                  bool trackInputSequences,
                  OUT std::vector<DasPossibility> &results);

#endif // DAS_MOVE_SEARCH
