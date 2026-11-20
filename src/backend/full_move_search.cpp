#include "full_move_search.hpp"

#include "utils.hpp"
#include <algorithm>
#include <stdint.h>
#include <string.h>
#include <type_traits>

using namespace std;

/**
 * Full move search: per-goal frame simulation (goal walks + inline first/last
 * midair dedup), the fall phase, and depth-1 tuck/spin DFS, covering both the
 * spawn path and the adjustment (mid-air) input path. Equivalence of both
 * modes and both entry paths is enforced by the fuzz harness (see TESTING.md).
 * The frame-exact DAS charge model is documented in docs/DAS_explainer.md.
 *
 * Two modes, branched internally on `useDAS = dasCharge != -1`:
 *   - DAS mode (dasCharge >= 0): frame-exact charge bookkeeping, the '_'
 *     hold-through-ARE marker, lowercase held-but-not-shifting inputs, wall
 *     charges, and O(1) tuck readiness (DasTuckTracker).
 *   - Tap mode (dasCharge == -1): inputs gated only by the ARR timeline
 *     (shouldPerformInputsThisFrame); no charge bookkeeping, no '_' marker, no
 *     lowercase inputs, no wall charges. dasChargeAfter stays -1 (the incoming
 *     charge passes through untouched).
 *
 * Adjustment entry (FullMoveSearchContext, non-default): mid-air state.
 * Differences from spawn entry:
 *   - initialX/initialY shift by existingXOffset/existingYOffset; the sim
 *     starts at existingRotation with frameIndex = framesAlreadyElapsed;
 *   - arrFrameIndex starts at framesAlreadyElapsed when isArrContinued, else 0;
 *   - dasWillResetLeft/Right gain the adjustment disjunct
 *     `(isAdjustment && dasButtonHeld != direction)`;
 *   - no '_' hold-through-ARE marker (holdDASduringARE requires !isAdjustment);
 *   - tuck-gating sequence scans (fall phase + dfs) run over the concatenated
 *     preAdjInputSequence + current sequence, while the OUTPUT sequences stay
 *     prefix-free (the prefix feeds only gating, never output).
 *
 * Reaction-time semantics (spawn path, reactionTime > 0): a goal walk that
 * reaches its goal keeps simulating (the piece keeps falling at the goal
 * column) until the pre-advanced frameIndex hits reactionTime, and the mid-air
 * state on that frame -- when the piece did not lock on it -- is saved as the
 * adjTimeSimState snapshot (the entry data an adjustment search started from
 * this placement would need). The snapshot rides the walk's registered sim
 * states (each state carries its arena index only from the reaction-time
 * frame onward), through the midair dedupe and the fall phase into both basic
 * and tuck/spin results, and out through an index-aligned side channel (see
 * AdjTimeSimStateSlot in the header). A walk whose piece locks before the
 * reaction-time frame registers states without a snapshot.
 *
 * Performance specialization: `fullMoveSearchImpl<TrackSeq>` skips
 * `std::string` sequence storage and delay formatting when neither sequence
 * output nor `adjTimeCaptures` is requested, while maintaining identical DAS
 * tuck-readiness state via `DasTuckTracker`.
 */

// 7.5 Hz slowtapping (see docs/DAS_explainer.md)
static const char DAS_SLOW_TAP_TIMELINE[] = "X.......";
static constexpr int DAS_SLOW_TAP_WAIT_FRAMES = (int)sizeof(DAS_SLOW_TAP_TIMELINE) - 2;

// INPUT_LOOKUP: rows = shift increment (-1, 0, +1), cols = rot increment (-1, 0, +1)
static const char INPUT_TABLE[3][3] = {
    {'F', 'L', 'E'}, // shift -1 (left)
    {'B', '.', 'A'}, // shift 0
    {'G', 'R', 'I'}, // shift +1 (right)
};

static int dasArrowDir(char c) {
  switch (c) {
  case 'L':
  case 'E':
  case 'F':
  case 'l':
  case 'e':
  case 'f':
    return -1;
  case 'R':
  case 'I':
  case 'G':
  case 'r':
  case 'i':
  case 'g':
    return 1;
  default:
    return 0; // '.', '_', anything else
  }
}

/**
 * O(1) incremental DAS tuck-readiness tracker over (preAdjInputSequence +
 * current sequence). Equivalent to scanning backwards for the trailing '.' run
 * and checking whether any non-initial index in the last DAS_SLOW_TAP_WAIT_FRAMES
 * had a D-pad button-down transition (`thisDir != 0 && prevDir != thisDir`).
 */
struct DasTuckTracker {
  int len = 0;
  int lastArrowDir = 0;
  int framesSinceInput = 0;
  int lastDpadDownPos = -100;

  inline void push(char c) {
    const int dir = dasArrowDir(c);
    if (len >= 1 && dir != 0 && dir != lastArrowDir) {
      lastDpadDownPos = len;
    }
    framesSinceInput = (c == '.') ? (framesSinceInput + 1) : 0;
    lastArrowDir = dir;
    len++;
  }

  inline bool isReady(int minFramesWait) const {
    return lastDpadDownPos < len - DAS_SLOW_TAP_WAIT_FRAMES && framesSinceInput >= minFramesWait;
  }
};

struct EmptySeq {
  inline EmptySeq &operator+=(char) { return *this; }
  inline void pop_back() {}
  inline operator std::string() const { return std::string(); }
};

/** Internal simulation state for the DAS search (superset of the C++ SimState). */
template <bool TrackSeq> struct FullSimState {
  int x;
  int y;
  int rotationIndex;
  int frameIndex;
  int arrFrameIndex;
  int dasCharge;
  int heldDirection; // -1 left / 0 none / +1 right
  bool pendingDasReset;
  bool hasAlreadyLocked;
  // adjTimeSimState snapshot arena index (-1 = none), set once the walk
  // passes its reaction-time frame airborne, carried by every sim state
  // registered from that frame onward.
  int adjCaptureIdx;
  DasTuckTracker tuckTracker;
  std::conditional_t<TrackSeq, std::string, EmptySeq> inputSequence;

  inline void appendInputChar(char c, bool useDAS) {
    if (useDAS) {
      tuckTracker.push(c);
    }
    if (TrackSeq) {
      inputSequence += c;
    }
  }
};

static int numPieceOrientations(const Piece *piece) {
  for (int r = 0; r < 4; r++) {
    if (piece->rowsByRotation[r][0] == NONE) {
      return r;
    }
  }
  return 4;
}

static void inputDeltas(char c, OUT int &xDelta, OUT int &rotDelta) {
  switch (c) {
  case 'L':
    xDelta = -1;
    rotDelta = 0;
    break;
  case 'R':
    xDelta = 1;
    rotDelta = 0;
    break;
  case 'A':
    xDelta = 0;
    rotDelta = 1;
    break;
  case 'B':
    xDelta = 0;
    rotDelta = -1;
    break;
  case 'E':
    xDelta = -1;
    rotDelta = 1;
    break;
  case 'I':
    xDelta = 1;
    rotDelta = 1;
    break;
  case 'F':
    xDelta = -1;
    rotDelta = -1;
    break;
  case 'G':
    xDelta = 1;
    rotDelta = -1;
    break;
  default:
    xDelta = 0;
    rotDelta = 0;
    break;
  }
}

int inputCostX10ForChar(char c) {
  switch (c) {
  case 'E':
  case 'F':
  case 'I':
  case 'G':
    return -3; // spin-tuck
  case 'A':
  case 'B':
    return -2; // spin
  case 'L':
  case 'R':
    return -1; // tuck
  default:
    return 0;
  }
}

/** Wall-charge detection against both sides; left takes priority. */
static char dasFindWallCharge(unsigned int board[20], const Piece *piece, int x, int y,
                              int rotationIndex) {
  if (collision(board, piece, x - 1, y, rotationIndex)) {
    return 'l'; // lowercase: the piece won't actually shift
  }
  if (collision(board, piece, x + 1, y, rotationIndex)) {
    return 'r';
  }
  return 0;
}

/**
 * Places the piece on a copy of the board and returns the number of cleared lines.
 *
 * TODO: this counts EVERY full row after stamping, including rows that were
 * already full before the piece locked; getNewBoardAndLinesCleared
 * (move_result.cpp) counts only rows the piece itself completed. The two
 * conventions coexist: search/response paths collapse pre-existing full rows,
 * the eval path does not. Unifying them changes response board/lines fields
 * and requires re-baselining the golden fixtures (pinned by the backup tests).
 */
static int placePieceAndClearLines(unsigned int board[20], const Piece *piece, int rotationIndex,
                                   int x, int y) {
  unsigned int temp[20];
  memcpy(temp, board, sizeof(temp));
  for (int r = 0; r < 4; r++) {
    const int pieceRow = piece->rowsByRotation[rotationIndex][r];
    if (pieceRow == NONE) {
      break; // no more rows for this rotation
    }
    if (pieceRow == 0) {
      continue;
    }
    if (y + r < 0 || y + r >= 20) {
      continue; // out-of-bounds cells are ignored when stamping the board
    }
    temp[y + r] |= SHIFTBY(pieceRow, x) & 0x3FF;
  }

  int cleared = 0;
  for (int r = 0; r < 20; r++) {
    cleared += ((temp[r] & 0x3FFU) == 0x3FFU);
  }
  return cleared;
}

/** Converts a sim state at lock time into a result possibility. */
template <bool TrackSeq>
static FullMovePossibility getPossibilityFromSimState(unsigned int board[20], const Piece *piece,
                                                      const FullSimState<TrackSeq> &simState,
                                                      char tuckInput) {
  FullMovePossibility p;
  p.x = simState.x;
  p.y = simState.y;
  p.rotationIndex = simState.rotationIndex;
  p.dasChargeAfter = simState.dasCharge;
  p.tuckInput = tuckInput;
  p.numLinesCleared =
      placePieceAndClearLines(board, piece, simState.rotationIndex, simState.x, simState.y);

  if (TrackSeq) {
    p.inputSequence = simState.inputSequence;

    const int startingY = piece->id == 'I' ? -2 : -1;
    const int lockHeight = 20 - (simState.y - startingY);
    const int numEntryDelayFrames = min(18, 10 + ((lockHeight + 1) / 4) * 2);
    p.inputSequence.append(numEntryDelayFrames - 5, '*');
    if (p.numLinesCleared > 0) {
      p.inputSequence.append(17, '^');
    }
    p.inputSequence.append(5, '*');
  }
  return p;
}

/**
 * Saves the adjTimeSimState for one goal walk: the mid-air state at the frame
 * the walk waits for (piece still airborne there). Appends it to the search's
 * snapshot arena and returns the arena index.
 */
template <bool TrackSeq>
static int saveAdjTimeState(const FullSimState<TrackSeq> &simState, bool isArrContinued,
                            OUT std::vector<AdjTimeSimState> &captureArena) {
  AdjTimeSimState capture;
  capture.frameIndex = simState.frameIndex;
  capture.x = simState.x;
  capture.y = simState.y;
  capture.rotationIndex = simState.rotationIndex;
  capture.dasCharge = simState.dasCharge;
  capture.isArrContinued = isArrContinued;
  capture.dasButtonHeld = simState.heldDirection;
  if (TrackSeq) {
    capture.preAdjInputSequence = simState.inputSequence;
  }
  captureArena.push_back(std::move(capture));
  return (int)captureArena.size() - 1;
}

/** Per-goal placement simulation (spawn path via the default context,
 * adjustment path via a mid-air context), keeping only the first and last
 * midair state at the reached goal. Walks with reactionTime > 0 keep
 * simulating at the reached goal until the reaction-time frame, saving the
 * adjTimeSimState there (see saveAdjTimeState). */
template <bool TrackSeq>
static void
tryPlacement(unsigned int board[20], const Piece *piece, int goalRotationIndex, int goalOffsetX,
             bool useDAS, bool dasWillReset, char const *inputFrameTimeline, int timelineLen,
             int gravity, bool gravityDoubled, int initialX, int initialY, int dasCharge,
             const FullMoveSearchContext &ctx, int reactionTime, const DasTuckTracker &baseTracker,
             OUT std::vector<AdjTimeSimState> &captureArena,
             OUT std::vector<FullSimState<TrackSeq>> &dedupedStates) {

  // Adjustments never get the '_' hold-through-ARE marker: holdDASduringARE
  // requires a non-adjustment context.
  const bool holdDASduringARE = useDAS && !ctx.isAdjustment && !dasWillReset && goalOffsetX != 0;

  FullSimState<TrackSeq> simState;
  simState.x = initialX;
  simState.y = initialY;
  simState.frameIndex = ctx.framesAlreadyElapsed;
  // isArrContinued continues the ARR phase from the pre-adjustment timeline.
  simState.arrFrameIndex = ctx.isArrContinued ? ctx.framesAlreadyElapsed : 0;
  simState.rotationIndex = ctx.existingRotation;
  simState.dasCharge = dasCharge;
  simState.heldDirection = 0; // each goal's held-direction tracking starts at NONE
  simState.pendingDasReset = dasWillReset;
  simState.hasAlreadyLocked = false;
  simState.adjCaptureIdx = -1;
  simState.tuckTracker = baseTracker;
  if (holdDASduringARE) {
    simState.appendInputChar('_', useDAS);
  }

  const int goalX = initialX + goalOffsetX;
  const int shiftIncrement = goalOffsetX == 0 ? 0 : (goalOffsetX > 0 ? 1 : -1);
  int rotIncrement = 0;
  if (simState.rotationIndex != goalRotationIndex) {
    rotIncrement = MOD_4(simState.rotationIndex - 1) == goalRotationIndex ? -1 : 1;
  }

  // Immediate spawn collision: this goal is impossible
  if (collision(board, piece, simState.x, simState.y, simState.rotationIndex)) {
    return;
  }

  // Already at the goal with no reaction-time capture to wait for: hand off
  // directly to the fall phase so this frame is eligible for wall charges and
  // tucks/spins instead of spending a no-op '.' frame here.
  if (simState.rotationIndex == goalRotationIndex
      && simState.x == goalX
      && (ctx.isAdjustment || reactionTime <= 0)) {
    dedupedStates.push_back(std::move(simState));
    return;
  }

  // An ARR input window that goes by without a real input breaks the ARR
  // phase (isArrContinued = false in the capture). DAS-mode shift and
  // hold frames do not count as ARR inputs -- only the rotation-bearing
  // inputs do.
  bool arrFrameVoluntarilyMissed = false;

  int foundCount = 0;
  FullSimState<TrackSeq> firstState{};
  FullSimState<TrackSeq> lastState{};

  while (true) {
    const bool tapShouldInputThisFrame =
        inputFrameTimeline[simState.arrFrameIndex % timelineLen] == 'X';
    const bool canShiftThisFrame =
        useDAS ? (simState.dasCharge >= 15 || simState.pendingDasReset) : tapShouldInputThisFrame;
    const bool atGoalX = simState.x == goalX;
    const bool willShiftThisFrame = canShiftThisFrame && !atGoalX;
    const bool willHoldDasButtonThisFrame =
        useDAS && (!atGoalX || (simState.heldDirection != 0 && simState.dasCharge < 15));
    const bool willRotateThisFrame =
        tapShouldInputThisFrame && simState.rotationIndex != goalRotationIndex;
    const bool isGravityFrame = isGravityDropFrame(simState.frameIndex, gravity);

    bool lockAfterThisFrame = false;

    if (willShiftThisFrame) {
      if (collision(board, piece, simState.x + shiftIncrement, simState.y,
                    simState.rotationIndex)) {
        return; // horizontally blocked: the whole goal is impossible
      }
      simState.x += shiftIncrement;
      if (useDAS) {
        simState.dasCharge = simState.pendingDasReset ? 0 : 10; // charge lifecycle
        simState.pendingDasReset = false;
      }
    } else if (willHoldDasButtonThisFrame) {
      simState.dasCharge++;
    }

    if (willRotateThisFrame) {
      const int rotationAfter = MOD_4(simState.rotationIndex + rotIncrement);
      if (collision(board, piece, simState.x, simState.y, rotationAfter)) {
        return;
      }
      simState.rotationIndex = rotationAfter;
    }

    int effRot = willRotateThisFrame ? rotIncrement : 0;
    int effShift = (willShiftThisFrame || willHoldDasButtonThisFrame) ? shiftIncrement : 0;
    char thisFrameStr = INPUT_TABLE[effShift + 1][effRot + 1];
    if (useDAS && shiftIncrement != 0 && !willShiftThisFrame && thisFrameStr != '.') {
      thisFrameStr = (char)(thisFrameStr + ('a' - 'A')); // DAS held but no shift executed
    }
    simState.appendInputChar(thisFrameStr, useDAS);

    simState.heldDirection = effShift;

    bool didArrInputThisFrame;
    if (useDAS) {
      didArrInputThisFrame = effRot != 0;
    } else {
      didArrInputThisFrame = thisFrameStr != '.';
    }
    if (tapShouldInputThisFrame && !didArrInputThisFrame) {
      arrFrameVoluntarilyMissed = true;
    }

    if (isGravityFrame) {
      for (int i = 0; i < (gravityDoubled ? 2 : 1); i++) {
        if (collision(board, piece, simState.x, simState.y + 1, simState.rotationIndex)) {
          lockAfterThisFrame = true;
          break;
        }
        simState.y++;
      }
    }

    simState.frameIndex++;
    simState.arrFrameIndex++;

    // Save the adjTimeSimState at the reaction-time frame. Sim states
    // registered on this frame and later carry its arena index (earlier ones
    // register without one).
    if (TrackSeq && simState.frameIndex == reactionTime && !lockAfterThisFrame) {
      simState.adjCaptureIdx = saveAdjTimeState(simState, !arrFrameVoluntarilyMissed, captureArena);
    }

    const bool foundPlacement = simState.rotationIndex == goalRotationIndex && simState.x == goalX;
    if (foundPlacement) {
      // Keep only the first and last frames at the goal column (equivalent to midair dedup).
      if (foundCount == 0) {
        firstState = simState;
        firstState.hasAlreadyLocked = lockAfterThisFrame;
      } else {
        lastState = simState;
        lastState.hasAlreadyLocked = lockAfterThisFrame;
      }
      foundCount++;
    }

    if (lockAfterThisFrame) {
      break;
    }

    // Wait for the adjTimeSimState snapshot unless this search is itself an
    // adjustment or no reaction time applies.
    const bool doneWaitingForAdjustment =
        simState.adjCaptureIdx >= 0 || ctx.isAdjustment || reactionTime <= 0;
    const bool canKeepHoldingDpadForMoreDAS =
        useDAS && simState.dasCharge < 15 && simState.heldDirection != 0;
    if (!canKeepHoldingDpadForMoreDAS && doneWaitingForAdjustment && foundPlacement) {
      break;
    }
  }

  if (foundCount >= 1) {
    dedupedStates.push_back(std::move(firstState));
    if (foundCount >= 2) {
      dedupedStates.push_back(std::move(lastState));
    }
  }
}

/**
 * Compact 64-slot lookup table for lock heights keyed by (rot, x) with rot in [0, 3]
 * and x in [-4, 11], using a 64-bit presence mask so y <= 0 values are distinguished
 * from missing entries (missing reads as 0).
 */
struct LockHeightTable {
  int y[64];
  uint64_t seenMask = 0;

  inline void set(int rot, int x, int lockY) {
    const unsigned xOff = (unsigned)(x + 4);
    if ((unsigned)rot < 4 && xOff < 16) {
      const int idx = (rot << 4) + (int)xOff;
      y[idx] = lockY;
      seenMask |= (1ULL << idx);
    }
  }

  inline int getOrZero(int rot, int x) const {
    const unsigned xOff = (unsigned)(x + 4);
    if ((unsigned)rot < 4 && xOff < 16) {
      const int idx = (rot << 4) + (int)xOff;
      return (seenMask & (1ULL << idx)) ? y[idx] : 0;
    }
    return 0;
  }
};

template <bool TrackSeq> struct FallPhaseResult {
  std::vector<FullMovePossibility> lockPossibilities;
  // adjTimeSimState arena index per lock possibility (-1 = none),
  // index-aligned with lockPossibilities.
  std::vector<int> lockCaptureIdx;
  std::vector<FullSimState<TrackSeq>> potentialTuckSpinStates;
  LockHeightTable lockHeightLookup;
};

/** Tuck/spin dfs output: the novel possibilities plus, index-aligned, the
 * adjTimeSimState arena index each one inherited from its parent sim state. */
struct TuckSpinResult {
  std::vector<FullMovePossibility> possibilities;
  std::vector<int> adjCaptureIdx;
};

/** Fall phase: simulates legal placements down to lock and collects
 * possibilities plus tuck-spin candidate states. */
template <bool TrackSeq>
static void
exploreLegalPlacementsUntilLock(unsigned int board[20], const Piece *piece,
                                const std::vector<FullSimState<TrackSeq>> &dedupedStates,
                                bool useDAS, char const *inputFrameTimeline, int timelineLen,
                                int gravity, bool gravityDoubled,
                                OUT FallPhaseResult<TrackSeq> &result) {

  const int minFramesWait = timelineLen - 1;
  result.lockPossibilities.reserve(dedupedStates.size());
  result.lockCaptureIdx.reserve(dedupedStates.size());
  result.potentialTuckSpinStates.reserve(128);

  for (const FullSimState<TrackSeq> &original : dedupedStates) {
    FullSimState<TrackSeq> simState = original; // the fall mutates a copy of the state
    int highestRegisteredY = -1;
    bool startedLookingForTuckSpins = false;

    while (true) {
      auto registerPossibility = [&]() {
        result.lockHeightLookup.set(simState.rotationIndex, simState.x, simState.y);
        result.lockCaptureIdx.push_back(simState.adjCaptureIdx);
        result.lockPossibilities.push_back(
            getPossibilityFromSimState(board, piece, simState, /* tuckInput= */ '.'));
      };

      // If it collides immediately, quit
      if (simState.hasAlreadyLocked
          || collision(board, piece, simState.x, simState.y, simState.rotationIndex)) {
        registerPossibility();
        break;
      }

      const bool isGravityFrame = isGravityDropFrame(simState.frameIndex, gravity);

      // Start looking for tucks/spins as soon as inputs are allowed. DAS mode
      // gates via tuckTracker (covering preAdjInputSequence + current sequence);
      // tap mode gates on the ARR phase alone (isArrContinued already shaped
      // the starting arrFrameIndex).
      if (!startedLookingForTuckSpins) {
        const bool readyForInputs =
            useDAS ? simState.tuckTracker.isReady(minFramesWait)
                   : (inputFrameTimeline[simState.arrFrameIndex % timelineLen] == 'X');
        if (readyForInputs) {
          startedLookingForTuckSpins = true;
        }
      }

      if (startedLookingForTuckSpins && simState.y > highestRegisteredY) {
        result.potentialTuckSpinStates.push_back(simState);
        highestRegisteredY = simState.y;
      }

      // Wall charges (DAS only): charge frozen during the fall except for this re-press
      char wallCharge = 0;
      if (useDAS && simState.dasCharge < 16 && startedLookingForTuckSpins) {
        wallCharge =
            dasFindWallCharge(board, piece, simState.x, simState.y, simState.rotationIndex);
      }
      if (wallCharge != 0) {
        simState.appendInputChar(wallCharge, useDAS);
        simState.dasCharge = 16;
      } else {
        simState.appendInputChar('.', useDAS);
      }

      if (isGravityFrame) {
        bool didLock = false;
        for (int i = 0; i < (gravityDoubled ? 2 : 1); i++) {
          if (collision(board, piece, simState.x, simState.y + 1, simState.rotationIndex)) {
            registerPossibility();
            didLock = true;
            break;
          }
          simState.y++;
        }
        if (didLock) {
          break;
        }
      }

      simState.frameIndex++;
      simState.arrFrameIndex++;
    }
  }
}

/** Tuck/spin input trial: one input, then ride gravity to lock. */
template <bool TrackSeq>
static bool tryInput(unsigned int board[20], const Piece *piece, char inputChar, int newX,
                     int newRotationIndex, bool useDAS,
                     const FullSimState<TrackSeq> &parentSimState, int minFramesWait, int gravity,
                     int framesAlreadyElapsed, OUT FullMovePossibility &outPossibility) {

  FullSimState<TrackSeq> simState = parentSimState;
  bool hasDoneInput = false;
  bool isWallChargeTuck = false;
  char wallChargeChar = 0;

  while (true) {
    const bool isGravityFrame = isGravityDropFrame(simState.frameIndex, gravity);

    if (!hasDoneInput) {
      if (newX != simState.x) {
        // Shift collision: this input is impossible
        if (collision(board, piece, newX, simState.y, simState.rotationIndex)) {
          return false;
        }

        // DAS only: wall-charge tuck -- the piece could have wall-charged on the
        // previous (gravity) frame; then the tuck shift leaves charge at 10.
        // Only eligible when the previous frame belonged to this search (an
        // adjustment cannot retroactively rewrite a pre-reaction frame).
        if (useDAS) {
          const bool couldWallChargeLastFrame =
              collision(board, piece, newX, simState.y - 1, simState.rotationIndex) != 0;
          const bool lastFrameWasGravityFrame =
              simState.frameIndex > framesAlreadyElapsed
              && isGravityDropFrame(simState.frameIndex - 1, gravity);
          if (couldWallChargeLastFrame && lastFrameWasGravityFrame) {
            isWallChargeTuck = true;
            wallChargeChar = newX > simState.x ? 'r' : 'l';
            simState.dasCharge = 10;
          } else {
            simState.dasCharge = 0; // fresh down-press
          }
        }
        simState.x = newX;
      }

      if (newRotationIndex != simState.rotationIndex
          && collision(board, piece, simState.x, simState.y, newRotationIndex)) {
        return false;
      }
      simState.rotationIndex = newRotationIndex;

      if (isWallChargeTuck) {
        if (TrackSeq) {
          // Replace the '.' right before this input with the wall-charge char
          simState.inputSequence.pop_back();
          simState.inputSequence += wallChargeChar;
          simState.inputSequence += inputChar;
        }
        // Once isWallChargeTuck is true, tuckTracker is not consulted again in this trial.
      } else {
        simState.appendInputChar(inputChar, useDAS);
      }
      hasDoneInput = true;
    } else {
      char newInput = '.';
      if (useDAS && simState.dasCharge < 15) {
        if (isWallChargeTuck) {
          // Keep holding after the wall-charge tuck, accruing charge while falling
          newInput = wallChargeChar;
          simState.dasCharge++;
        } else if (simState.tuckTracker.isReady(minFramesWait)) {
          const char wallCharge =
              dasFindWallCharge(board, piece, simState.x, simState.y, simState.rotationIndex);
          if (wallCharge != 0) {
            newInput = wallCharge;
            simState.dasCharge = 16;
          }
        }
      }
      simState.appendInputChar(newInput, useDAS && !isWallChargeTuck);
    }

    // NB: dfs gravity is one row per gravity frame even under double gravity
    // (double kill screen is disabled).
    if (isGravityFrame) {
      if (collision(board, piece, simState.x, simState.y + 1, simState.rotationIndex)) {
        outPossibility = getPossibilityFromSimState(board, piece, simState, inputChar);
        return true;
      }
      simState.y++;
    }

    simState.frameIndex++;
  }
}

/** Novel-keeping score for the dfs: adjustability first (a walk that saved its
 * adjTimeSimState beats one that locked too early), input cost as the
 * tiebreaker. The cost spans -3..0, so the 10000-point no-snapshot penalty
 * dominates. */
static int tuckScore(char tuckInput, int adjCaptureIdx) {
  return inputCostX10ForChar(tuckInput) + (adjCaptureIdx >= 0 ? 0 : -10000);
}

/** Depth-1 tuck/spin exploration. */
template <bool TrackSeq>
static TuckSpinResult
searchForTucksOrSpins(unsigned int board[20], const Piece *piece, int numOrientations,
                      const std::vector<FullSimState<TrackSeq>> &potentialTuckSpinStates,
                      const LockHeightTable &lockHeightLookup, bool useDAS, int minFramesWait,
                      int gravity, int framesAlreadyElapsed) {
  const char *possibleInputs = numOrientations == 1   ? "LR"
                               : numOrientations == 2 ? "LRAEI"
                                                      : "LRABEIFG";
  const int rotMask = numOrientations - 1;

  // Novel possibilities keyed by the lock cell, insertion order preserved
  // (insertion order of first discovery), keeping the higher-score entry per
  // cell (see tuckScore). Each entry carries its parent walk's
  // adjTimeSimState arena index.
  struct NovelTuck {
    FullMovePossibility possibility;
    int adjCaptureIdx;
  };
  std::vector<int> novelKeys;
  std::vector<NovelTuck> novel;
  auto findNovel = [&novelKeys, &novel](int key) -> NovelTuck * {
    for (size_t i = 0; i < novelKeys.size(); i++) {
      if (novelKeys[i] == key) {
        return &novel[i];
      }
    }
    return NULL;
  };

  for (const FullSimState<TrackSeq> &simState : potentialTuckSpinStates) {
    const int parentCaptureIdx = simState.adjCaptureIdx;
    for (const char *input = possibleInputs; *input != '\0'; input++) {
      const char inputChar = *input;
      int xDelta = 0;
      int rotDelta = 0;
      inputDeltas(inputChar, xDelta, rotDelta);
      const int newRot = (simState.rotationIndex + rotDelta + 4) & rotMask;
      const int newX = simState.x + xDelta;

      // Visited check : skip if this hover position adds nothing novel.
      const int highestYSeen = lockHeightLookup.getOrZero(newRot, newX);
      if (simState.y <= highestYSeen) {
        continue;
      }

      FullMovePossibility possibility;
      if (tryInput(board, piece, inputChar, newX, newRot, useDAS, simState, minFramesWait, gravity,
                   framesAlreadyElapsed, possibility)) {
        const int key = (possibility.rotationIndex * 100 + possibility.x) * 1000 + possibility.y;
        NovelTuck *existing = findNovel(key);
        if (existing == NULL
            || tuckScore(possibility.tuckInput, parentCaptureIdx)
                   > tuckScore(existing->possibility.tuckInput, existing->adjCaptureIdx)) {
          if (existing != NULL) {
            existing->possibility = std::move(possibility);
            existing->adjCaptureIdx = parentCaptureIdx;
          } else {
            novelKeys.push_back(key);
            novel.push_back({std::move(possibility), parentCaptureIdx});
          }
        }
      }
    }
  }

  TuckSpinResult out;
  out.possibilities.reserve(novel.size());
  out.adjCaptureIdx.reserve(novel.size());
  for (NovelTuck &t : novel) {
    out.possibilities.push_back(std::move(t.possibility));
    out.adjCaptureIdx.push_back(t.adjCaptureIdx);
  }
  return out;
}

/** Adjacent duplicate lock cells keep the later (max-charge) variant. */
static void dedupeLockPossibilities(OUT std::vector<FullMovePossibility> &possibilities,
                                    OUT std::vector<int> &captureIdx) {
  const size_t n = possibilities.size();
  size_t write = 0;
  for (size_t i = 0; i < n; i++) {
    if (i + 1 < n
        && possibilities[i].rotationIndex == possibilities[i + 1].rotationIndex
        && possibilities[i].x == possibilities[i + 1].x
        && possibilities[i].y == possibilities[i + 1].y) {
      i++; // skip the first (worse DAS charge) and keep i+1
    }
    if (write != i) {
      possibilities[write] = std::move(possibilities[i]);
      captureIdx[write] = captureIdx[i];
    }
    write++;
  }
  possibilities.resize(write);
  captureIdx.resize(write);
}

/** Side-channel slot for one result from its walk-capture arena index. */
static AdjTimeSimStateSlot captureSlot(int adjCaptureIdx,
                                       const std::vector<AdjTimeSimState> &captureArena) {
  AdjTimeSimStateSlot slot;
  slot.hasCapture = adjCaptureIdx >= 0;
  slot.capture = adjCaptureIdx >= 0 ? captureArena[adjCaptureIdx] : AdjTimeSimState();
  return slot;
}

template <bool TrackSeq>
static int
fullMoveSearchImpl(GameState &gameState, const Piece *piece, char const *inputFrameTimeline,
                   int dasCharge, bool trackInputSequences,
                   OUT std::vector<FullMovePossibility> &results, const FullMoveSearchContext &ctx,
                   int reactionTime, OUT std::vector<AdjTimeSimStateSlot> *adjTimeCaptures) {
  const bool useDAS = dasCharge != -1;
  const int gravity = getGravity(gameState.level);
  const bool gravityDoubled = isGravityDoubled(gameState.level);
  const int numOrientations = numPieceOrientations(piece);
  const int timelineLen = (int)strlen(inputFrameTimeline);
  const int minFramesWait = timelineLen - 1;
  const int initialX = INITIAL_X + ctx.existingXOffset;
  const int initialY = piece->initialY + ctx.existingYOffset;

  // Immediate entry collision: every goal aborts -- every goal starts from
  // the same entry state and rotation, so a collision there voids them all.
  if (collision(gameState.board, piece, initialX, initialY, ctx.existingRotation)) {
    return 0;
  }

  DasTuckTracker baseTracker;
  if (useDAS) {
    for (char c : ctx.preAdjInputSequence) {
      baseTracker.push(c);
    }
  }

  // dasWillResetLeft/Right: && binds tighter than ||. The selection between
  // them is per goal-shift direction below. On the spawn path
  // (isAdjustment = false) this reduces to "DAS mode with incoming charge <= 5
  // and the goal requires movement".
  const bool dasWillResetLeft =
      (useDAS && dasCharge <= 5) || (ctx.isAdjustment && ctx.dasButtonHeld != DAS_BUTTON_LEFT);
  const bool dasWillResetRight =
      (useDAS && dasCharge <= 5) || (ctx.isAdjustment && ctx.dasButtonHeld != DAS_BUTTON_RIGHT);

  // Phase A: per-goal frame simulation. Each walk appends at most one capture
  // to the arena; kept first/last states reference it by index.
  std::vector<AdjTimeSimState> captureArena;
  std::vector<FullSimState<TrackSeq>> dedupedStates;
  dedupedStates.reserve(numOrientations * 20);
  const int rangeStart = -2 - initialX;
  const int rangeEnd = 7 - initialX;
  for (int goalRotationIndex = 0; goalRotationIndex < numOrientations; goalRotationIndex++) {
    for (int xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
      const bool dasWillReset =
          (xOffset > 0 && dasWillResetRight) || (xOffset < 0 && dasWillResetLeft);
      tryPlacement<TrackSeq>(gameState.board, piece, goalRotationIndex, xOffset, useDAS,
                             dasWillReset, inputFrameTimeline, timelineLen, gravity, gravityDoubled,
                             initialX, initialY, dasCharge, ctx, reactionTime, baseTracker,
                             captureArena, dedupedStates);
    }
  }

  // Phase B: fall phase
  FallPhaseResult<TrackSeq> fall;
  exploreLegalPlacementsUntilLock<TrackSeq>(gameState.board, piece, dedupedStates, useDAS,
                                            inputFrameTimeline, timelineLen, gravity,
                                            gravityDoubled, fall);

  // Phase C: tuck/spin DFS
  TuckSpinResult tuckResult = searchForTucksOrSpins<TrackSeq>(
      gameState.board, piece, numOrientations, fall.potentialTuckSpinStates, fall.lockHeightLookup,
      useDAS, minFramesWait, gravity, ctx.framesAlreadyElapsed);

  // Lock dedup on the basic list
  dedupeLockPossibilities(fall.lockPossibilities, fall.lockCaptureIdx);

  // Assemble: basics then tucks, no cross-list dedup; the side channel stays
  // index-aligned with the results.
  const size_t totalCount = fall.lockPossibilities.size() + tuckResult.possibilities.size();
  results.reserve(results.size() + totalCount);
  if (adjTimeCaptures != NULL) {
    adjTimeCaptures->reserve(adjTimeCaptures->size() + totalCount);
  }
  for (size_t i = 0; i < fall.lockPossibilities.size(); i++) {
    if (!trackInputSequences) {
      fall.lockPossibilities[i].inputSequence.clear();
    }
    results.push_back(std::move(fall.lockPossibilities[i]));
    if (adjTimeCaptures != NULL) {
      adjTimeCaptures->push_back(captureSlot(fall.lockCaptureIdx[i], captureArena));
    }
  }
  for (size_t i = 0; i < tuckResult.possibilities.size(); i++) {
    if (!trackInputSequences) {
      tuckResult.possibilities[i].inputSequence.clear();
    }
    results.push_back(std::move(tuckResult.possibilities[i]));
    if (adjTimeCaptures != NULL) {
      adjTimeCaptures->push_back(captureSlot(tuckResult.adjCaptureIdx[i], captureArena));
    }
  }
  return (int)results.size();
}

int fullMoveSearch(GameState gameState, const Piece *piece, char const *inputFrameTimeline,
                   int dasCharge, bool trackInputSequences,
                   OUT std::vector<FullMovePossibility> &results, const FullMoveSearchContext &ctx,
                   int reactionTime, OUT std::vector<AdjTimeSimStateSlot> *adjTimeCaptures) {
  if (adjTimeCaptures != NULL) {
    adjTimeCaptures->clear();
  }

  // A piece that has already fallen past the bottom of the board has no
  // legal moves (reachable from mid-air adjustment states).
  if (ctx.existingYOffset >= 20) {
    return 0;
  }

  // Capture slots also store preAdjInputSequence even when result sequences are omitted.
  if (trackInputSequences || adjTimeCaptures != NULL) {
    return fullMoveSearchImpl<true>(gameState, piece, inputFrameTimeline, dasCharge,
                                    trackInputSequences, results, ctx, reactionTime,
                                    adjTimeCaptures);
  }
  return fullMoveSearchImpl<false>(gameState, piece, inputFrameTimeline, dasCharge,
                                   trackInputSequences, results, ctx, reactionTime,
                                   adjTimeCaptures);
}
