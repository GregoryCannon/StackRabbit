#ifndef FULL_MOVE_SEARCH
#define FULL_MOVE_SEARCH

#include "types.hpp"
#include <string>
#include <vector>

/**
 * Full move search (frame-exact charge model in DAS mode: docs/DAS_explainer.md).
 *
 * Per-goal frame simulation, midair dedup, the fall phase, and depth-1 tuck/spin
 * DFS. One entry point with an internal mode branch on `dasCharge == -1`:
 *   - DAS mode (dasCharge >= 0): charge bookkeeping, '_' hold-through-ARE marker,
 *     lowercase held inputs, wall charges, sequence-scan tuck readiness.
 *   - Tap mode (dasCharge == -1): ARR-timeline-gated inputs only, no charge
 *     bookkeeping; dasChargeAfter stays -1 in the results.
 *
 * Two entry contexts, selected purely by FullMoveSearchContext:
 *   - Spawn entry (default context): piece at the standard spawn spot with no
 *     elapsed frames. With reactionTime > 0, each goal walk keeps simulating
 *     (falling at the reached goal) until the reaction-time frame, and the
 *     mid-air piece state there is saved as the adjTimeSimState snapshot for
 *     the adjustment pipeline.
 *   - Adjustment entry: mid-air state with an AdjustmentState -- existing
 *     offsets/rotation, framesAlreadyElapsed, preAdjInputSequence prefix on
 *     tuck-gating scans, isArrContinued ARR phase, dasButtonHeld feeding
 *     dasWillResetLeft/Right, and no '_' hold-through-ARE marker. Adjustment
 *     searches run with reactionTime <= 0: no waiting, no adjTimeSimState
 *     snapshot.
 *
 * Deliberately a SEPARATE entry point from fastMoveSearch(): DAS results carry
 * extra data (charge, sequence, line clears), and keeping LockPlacement /
 * fastMoveSearch byte-identical protects the playout hot path from any struct
 * growth (a +40-byte LockPlacement regressed the speed benchmark ~9%).
 */

/** dasButtonHeld encoding in FullMoveSearchContext and AdjTimeSimState. */
enum { DAS_BUTTON_LEFT = -1, DAS_BUTTON_NONE = 0, DAS_BUTTON_RIGHT = 1 };

/**
 * Mid-air entry context for adjustment-mode searches. The default-constructed
 * value is exactly the spawn entry, so existing call sites keep spawn
 * semantics unchanged.
 */
struct FullMoveSearchContext {
  // Mid-air entry state; offsets are relative to the standard spawn position
  // (INITIAL_X / piece->initialY).
  int existingXOffset = 0;
  int existingYOffset = 0;
  int framesAlreadyElapsed = 0;
  int existingRotation = 0;
  // AdjustmentState. dasButtonHeld uses the DAS_BUTTON_* encoding above.
  bool isAdjustment = false;
  bool isArrContinued = false;
  int dasButtonHeld = DAS_BUTTON_NONE;
  std::string preAdjInputSequence;
};

/**
 * Input cost (x10) for one tuck/spin notation character:
 * '.'/anything unlisted = 0 (basic); L/R = -1 (tuck); A/B = -2 (spin);
 * E/F/I/G = -3 (spin-tuck).
 */
int inputCostX10ForChar(char c);

/** One computed placement outcome in DAS mode. */
struct FullMovePossibility {
  int x;
  int y;
  int rotationIndex;
  int dasChargeAfter; // 0..16 charge at the lock frame (-1 in tap mode)
  char tuckInput;     // '.' for basic placements
  int numLinesCleared;
  std::string inputSequence;
};

/**
 * Snapshot of the mid-air piece state at the reaction-time frame of one goal
 * walk (referred to as the adjTimeSimState by consumers): exactly the entry
 * data an adjustment search started from this placement would need.
 * dasButtonHeld uses the DAS_BUTTON_* encoding; preAdjInputSequence is the
 * walk's input sequence up to and including the reaction-time frame.
 */
struct AdjTimeSimState {
  int frameIndex;
  int x;
  int y;
  int rotationIndex;
  int dasCharge;
  bool isArrContinued; // no ARR input frame went by without an input before the reaction-time frame
  int dasButtonHeld;   // DAS_BUTTON_* encoding
  std::string preAdjInputSequence;
};

/**
 * Side-channel slot, index-aligned with fullMoveSearch's results vector (slot
 * i describes the goal walk that produced result i). hasCapture is false when
 * the walk's piece locked before the reaction-time frame, or when the result
 * came from a sim state registered before the walk reached the reaction-time
 * frame (a walk registers a state per frame at its goal, and the reaction-time
 * frame may land mid-walk). Kept out of FullMovePossibility deliberately: the
 * result struct is copied per placement throughout the search, while
 * snapshots only exist for spawn searches with reactionTime > 0.
 */
struct AdjTimeSimStateSlot {
  bool hasCapture;
  AdjTimeSimState capture;
};

/**
 * Full move search for a piece entering at the standard spawn location
 * (default ctx) or at a mid-air adjustment state (ctx).
 * @param dasCharge - incoming DAS charge: -1 = tap mode, 0..16 = DAS mode
 * @param trackInputSequences - when false, sequences are still tracked
 *   internally (DAS-mode tuck gating scans them) but not copied into the results.
 * @param ctx - mid-air entry context; default = spawn entry (INITIAL_PLACEMENT).
 * @param reactionTime - frame the per-goal walks wait for: a walk keeps
 *   simulating until it saves the adjTimeSimState at this frame (piece still
 *   airborne) or locks. <= 0 disables both the waiting and the snapshot, and
 *   walks exit as soon as the goal is reached and DAS charging is done.
 * @param adjTimeCaptures - optional side channel index-aligned with the
 *   results vector (see AdjTimeSimStateSlot); NULL skips it.
 */
int fullMoveSearch(GameState gameState, const Piece *piece, char const *inputFrameTimeline,
                   int dasCharge, bool trackInputSequences,
                   OUT std::vector<FullMovePossibility> &results,
                   const FullMoveSearchContext &ctx = FullMoveSearchContext(),
                   int reactionTime = -1,
                   OUT std::vector<AdjTimeSimStateSlot> *adjTimeCaptures = NULL);

#endif // FULL_MOVE_SEARCH
