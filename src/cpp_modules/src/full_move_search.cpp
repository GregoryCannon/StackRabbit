#include "full_move_search.hpp"

#include "utils.hpp"
#include <algorithm>
#include <ctype.h>
#include <string.h>
#include <unordered_map>

using namespace std;

/**
 * Full move search: per-goal frame simulation (goal walks + midair dedup),
 * the fall phase, and depth-1 tuck/spin DFS, covering both the
 * spawn path and the adjustment (mid-air) input path. Equivalence of both
 * modes and both entry paths is enforced by the fuzz harness (see TESTING.md).
 * The frame-exact DAS charge model is documented in docs/DAS_explainer.md.
 *
 * Two modes, branched internally on `useDAS = dasCharge != -1`:
 *   - DAS mode (dasCharge >= 0): frame-exact charge bookkeeping, the '_'
 *     hold-through-ARE marker, lowercase held-but-not-shifting inputs, wall
 *     charges, and sequence-scan tuck readiness (readyForTuckInputsDas).
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
 * By design, the input sequence string is always tracked internally in
 * DAS mode: tuck-readiness gating scans it directly. The runtime flag only
 * controls whether sequences are copied into the output.
 */

// 7.5 Hz slowtapping (see docs/DAS_explainer.md)
static const char DAS_SLOW_TAP_TIMELINE[] = "X.......";

// INPUT_LOOKUP: rows = shift increment (-1, 0, +1), cols = rot increment (-1, 0, +1)
static const char INPUT_TABLE[3][3] = {
    {'F', 'L', 'E'}, // shift -1 (left)
    {'B', '.', 'A'}, // shift 0
    {'G', 'R', 'I'}, // shift +1 (right)
};

/** Internal simulation state for the DAS search (superset of the C++ SimState). */
struct FullSimState {
  int x;
  int y;
  int rotationIndex;
  int frameIndex;
  int arrFrameIndex;
  int dasCharge;
  int heldDirection; // -1 left / 0 none / +1 right
  bool pendingDasReset;
  std::string inputSequence;
  bool hasAlreadyLocked;
  // adjTimeSimState snapshot arena index (-1 = none), set once the walk
  // passes its reaction-time frame airborne, carried by every sim state
  // registered from that frame onward.
  int adjCaptureIdx;
};

static int numPieceOrientations(const Piece *piece) {
  for (int r = 0; r < 4; r++) {
    if (piece->rowsByRotation[r][0] == NONE) {
      return r;
    }
  }
  return 4;
}

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

static int heldDirectionOfChar(char c) {
  switch (toupper(c)) {
  case 'L':
  case 'E':
  case 'F':
    return -1;
  case 'R':
  case 'I':
  case 'G':
    return 1;
  default:
    return 0;
  }
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

static int inputCostX10ForChar(char c) {
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

/**
 * Tuck-readiness scan: direct scan of the sequence string.
 * The scanned sequence is the concatenation preAdjInputSequence + current
 * sequence (the "full" sequence); the two-part view avoids per-frame
 * concatenation. Note '_' and '*' scan as non-input/non-dot chars
 * (they are absent from the arrow-direction lookup, and '_' still breaks the
 * trailing-dot run).
 */
static bool dasReadyForTuckInputs(const std::string &prefix, const std::string &seq,
                                  char const *inputFrameTimeline) {
  const int prefixLen = (int)prefix.size();
  const int len = prefixLen + (int)seq.size();
  auto charAt = [prefixLen, &prefix, &seq](int i) -> char {
    return i < prefixLen ? prefix[i] : seq[i - prefixLen];
  };

  // Frames since the last input at all (trailing run of '.')
  int framesSinceLastInput = 0;
  for (int i = len - 1; i >= 0; i--) {
    if (charAt(i) == '.') {
      framesSinceLastInput++;
    } else {
      break;
    }
  }

  // Did the Dpad have a button_down event in the last N frames (N = slow-tap ARR)?
  const int slowTapWaitFrames = (int)strlen(DAS_SLOW_TAP_TIMELINE) - 1;
  bool didStartHoldingDpad = false;
  for (int i = max(1, len - slowTapWaitFrames); i < len; i++) {
    const int lastFr = dasArrowDir(charAt(i - 1));
    const int thisFr = dasArrowDir(charAt(i));
    if (thisFr != 0 && lastFr != thisFr) {
      didStartHoldingDpad = true;
    }
  }

  const int minFramesWait = (int)strlen(inputFrameTimeline) - 1;
  return !didStartHoldingDpad && framesSinceLastInput >= minFramesWait;
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

/** Places the piece on a copy of the board and returns the number of cleared lines. */
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
  int writeRow = 19;
  for (int r = 19; r >= 0; r--) {
    if ((temp[r] & 0x3FF) == 0x3FF) {
      cleared++;
      continue;
    }
    temp[writeRow--] = temp[r];
  }
  for (; writeRow >= 0; writeRow--) {
    temp[writeRow] = 0;
  }
  return cleared;
}

/** Converts a sim state at lock time into a result possibility. */
static FullMovePossibility getPossibilityFromSimState(unsigned int board[20], const Piece *piece,
                                                    const FullSimState &simState, int inputCostX10,
                                                    char tuckInput) {
  FullMovePossibility p;
  p.x = simState.x;
  p.y = simState.y;
  p.rotationIndex = simState.rotationIndex;
  p.dasChargeAfter = simState.dasCharge;
  p.inputCostX10 = inputCostX10;
  p.tuckInput = tuckInput;
  p.numLinesCleared =
      placePieceAndClearLines(board, piece, simState.rotationIndex, simState.x, simState.y);
  p.inputSequence = simState.inputSequence;

  const int startingY = piece->id == 'I' ? -2 : -1;
  const int lockHeight = 20 - (simState.y - startingY);
  const int numEntryDelayFrames = min(18, 10 + ((lockHeight + 1) / 4) * 2);
  for (int i = 0; i < numEntryDelayFrames - 5; i++) {
    p.inputSequence += "*";
  }
  if (p.numLinesCleared > 0) {
    for (int i = 0; i < 17; i++) {
      p.inputSequence += "^";
    }
  }
  for (int i = 0; i < 5; i++) {
    p.inputSequence += "*";
  }
  return p;
}

/**
 * Saves the adjTimeSimState for one goal walk: the mid-air state at the frame
 * the walk waits for (piece still airborne there). Appends it to the search's
 * snapshot arena and returns the arena index.
 */
static int saveAdjTimeState(const FullSimState &simState, bool isArrContinued,
                                  OUT std::vector<AdjTimeSimState> &captureArena) {
  AdjTimeSimState capture;
  capture.frameIndex = simState.frameIndex;
  capture.x = simState.x;
  capture.y = simState.y;
  capture.rotationIndex = simState.rotationIndex;
  capture.dasCharge = simState.dasCharge;
  capture.isArrContinued = isArrContinued;
  capture.dasButtonHeld = simState.heldDirection;
  capture.preAdjInputSequence = simState.inputSequence;
  captureArena.push_back(capture);
  return (int)captureArena.size() - 1;
}

/** Per-goal placement simulation (spawn path via the default context,
 * adjustment path via a mid-air context). Walks with reactionTime > 0 keep
 * simulating at the reached goal until the reaction-time frame, saving the
 * adjTimeSimState there (see saveAdjTimeState). */
static void tryPlacement(unsigned int board[20], const Piece *piece, int goalRotationIndex,
                            int goalOffsetX, bool useDAS, bool dasWillReset,
                            char const *inputFrameTimeline, int gravity, bool gravityDoubled,
                            int initialX, int initialY, int dasCharge,
                            const FullMoveSearchContext &ctx, int reactionTime,
                            OUT std::vector<AdjTimeSimState> &captureArena,
                            OUT std::vector<FullSimState> &legalPlacementSimStates) {

  // TS: holdDASduringARE requires !adjustmentState.isAdjustment -- adjustments
  // never get the '_' hold-through-ARE marker.
  const bool holdDASduringARE =
      useDAS && !ctx.isAdjustment && !dasWillReset && goalOffsetX != 0;

  FullSimState simState;
  simState.x = initialX;
  simState.y = initialY;
  simState.frameIndex = ctx.framesAlreadyElapsed;
  // isArrContinued continues the ARR phase from the pre-adjustment timeline.
  simState.arrFrameIndex = ctx.isArrContinued ? ctx.framesAlreadyElapsed : 0;
  simState.rotationIndex = ctx.existingRotation;
  simState.dasCharge = dasCharge;
  simState.heldDirection = 0; // each goal's held-direction tracking starts at NONE
  simState.pendingDasReset = dasWillReset;
  simState.inputSequence = holdDASduringARE ? "_" : "";
  simState.hasAlreadyLocked = false;
  simState.adjCaptureIdx = -1;

  const int shiftIncrement = goalOffsetX == 0 ? 0 : (goalOffsetX > 0 ? 1 : -1);
  int rotIncrement = 0;
  if (simState.rotationIndex != goalRotationIndex) {
    rotIncrement = MOD_4(simState.rotationIndex - 1) == goalRotationIndex ? -1 : 1;
  }

  // Immediate spawn collision: this goal is impossible
  if (collision(board, piece, simState.x, simState.y, simState.rotationIndex)) {
    return;
  }

  // An ARR input window that goes by without a real input breaks the ARR
  // phase (isArrContinued = false in the capture). DAS-mode shift and
  // hold frames do not count as ARR inputs -- only the rotation-bearing
  // inputs do.
  bool arrFrameVoluntarilyMissed = false;

  while (true) {
    const bool tapShouldInputThisFrame =
        shouldPerformInputsThisFrame(simState.arrFrameIndex, inputFrameTimeline) != 0;
    const bool canShiftThisFrame =
        useDAS ? (simState.dasCharge >= 15 || simState.pendingDasReset) : tapShouldInputThisFrame;
    const bool willShiftThisFrame = canShiftThisFrame && simState.x != initialX + goalOffsetX;
    const bool willHoldDasButtonThisFrame =
        useDAS
        && (simState.x != initialX + goalOffsetX
            || (simState.heldDirection != 0 && simState.dasCharge < 15));
    const bool willRotateThisFrame =
        tapShouldInputThisFrame && simState.rotationIndex != goalRotationIndex;
    const bool isGravityFrame = simState.frameIndex % gravity == gravity - 1;

    bool lockAfterThisFrame = false;

    if (willShiftThisFrame && simState.x != initialX + goalOffsetX) {
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
    if (useDAS && shiftIncrement != 0 && !willShiftThisFrame) {
      thisFrameStr = tolower(thisFrameStr); // DAS held but no shift executed
    }
    simState.inputSequence += thisFrameStr;

    simState.heldDirection = heldDirectionOfChar(thisFrameStr);

    bool didArrInputThisFrame;
    if (useDAS) {
      const char upper = (char)toupper((unsigned char)thisFrameStr);
      didArrInputThisFrame = upper == 'A' || upper == 'B' || upper == 'E' || upper == 'F'
                             || upper == 'I' || upper == 'G';
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
    if (simState.frameIndex == reactionTime && !lockAfterThisFrame) {
      simState.adjCaptureIdx =
          saveAdjTimeState(simState, !arrFrameVoluntarilyMissed, captureArena);
    }

    const bool foundPlacement =
        simState.rotationIndex == goalRotationIndex && simState.x == initialX + goalOffsetX;
    if (foundPlacement) {
      // a copy is registered every frame once the goal is reached (the piece
      // keeps falling at the goal column)
      FullSimState copy = simState;
      copy.hasAlreadyLocked = lockAfterThisFrame;
      legalPlacementSimStates.push_back(copy);
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
}

/** Midair dedupe: keep first + last entry per (rot, x) group. */
static std::vector<FullSimState> dedupeSortedStates(const std::vector<FullSimState> &states) {
  std::vector<int> groupOrder;                    // keys in first-seen order
  unordered_map<int, pair<int, int>> groupBounds; // key -> {firstIdx, lastIdx}
  for (int i = 0; i < (int)states.size(); i++) {
    const int key = states[i].rotationIndex * 100 + states[i].x;
    auto it = groupBounds.find(key);
    if (it == groupBounds.end()) {
      groupBounds[key] = make_pair(i, i);
      groupOrder.push_back(key);
    } else {
      it->second.second = i;
    }
  }

  std::vector<FullSimState> out;
  for (int key : groupOrder) {
    const pair<int, int> bounds = groupBounds[key];
    out.push_back(states[bounds.first]);
    if (bounds.second != bounds.first) {
      out.push_back(states[bounds.second]);
    }
  }
  return out;
}

struct FallPhaseResult {
  std::vector<FullMovePossibility> lockPossibilities;
  // adjTimeSimState arena index per lock possibility (-1 = none),
  // index-aligned with lockPossibilities.
  std::vector<int> lockCaptureIdx;
  std::vector<FullSimState> potentialTuckSpinStates;
  // Keyed by rotationIndex * 100 + x -> y. Missing keys read as 0 (legacy TS
  // `|| 0` behavior; possibly a bug, preserved for parity).
  unordered_map<int, int> lockHeightLookup;
};

/** Tuck/spin dfs output: the novel possibilities plus, index-aligned, the
 * adjTimeSimState arena index each one inherited from its parent sim state. */
struct TuckSpinResult {
  std::vector<FullMovePossibility> possibilities;
  std::vector<int> adjCaptureIdx;
};

/** Fall phase: simulates legal placements down to lock and collects
 * possibilities plus tuck-spin candidate states. `preAdjInputSequence` is the
 * adjustment-state prefix that DAS tuck gating scans (empty on the spawn
 * path); output sequences stay prefix-free. */
static void
exploreLegalPlacementsUntilLock(unsigned int board[20], const Piece *piece,
                                   const std::vector<FullSimState> &legalPlacementSimStates,
                                   bool useDAS, const std::string &preAdjInputSequence,
                                   char const *inputFrameTimeline, int gravity,
                                   bool gravityDoubled, OUT FallPhaseResult &result) {

  for (const FullSimState &original : legalPlacementSimStates) {
    FullSimState simState = original; // the fall mutates a copy of the state
    int highestRegisteredY = -1;
    bool startedLookingForTuckSpins = false;

    while (true) {
      auto registerPossibility = [&]() {
        result.lockHeightLookup[simState.rotationIndex * 100 + simState.x] = simState.y;
        result.lockCaptureIdx.push_back(simState.adjCaptureIdx);
        result.lockPossibilities.push_back(getPossibilityFromSimState(
            board, piece, simState, /* inputCostX10= */ 0, /* tuckInput= */ '.'));
      };

      // If it collides immediately, quit
      if (simState.hasAlreadyLocked
          || collision(board, piece, simState.x, simState.y, simState.rotationIndex)) {
        registerPossibility();
        break;
      }

      const bool isGravityFrame = simState.frameIndex % gravity == gravity - 1;

      // Start looking for tucks/spins as soon as inputs are allowed. DAS mode
      // gates on the full sequence: preAdjInputSequence + current;
      // tap mode gates on the ARR phase alone (isArrContinued already shaped
      // the starting arrFrameIndex).
      const bool readyForInputs =
          useDAS
              ? dasReadyForTuckInputs(preAdjInputSequence, simState.inputSequence,
                                      inputFrameTimeline)
              : shouldPerformInputsThisFrame(simState.arrFrameIndex, inputFrameTimeline) != 0;
      if (!startedLookingForTuckSpins && readyForInputs) {
        startedLookingForTuckSpins = true;
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
        simState.inputSequence += wallCharge;
        simState.dasCharge = 16;
      } else {
        simState.inputSequence += ".";
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

/** Tuck/spin input trial: one input, then ride gravity to lock.
 * `preAdjInputSequence` participates in DAS quicktap gating only. */
static bool tryInput(unsigned int board[20], const Piece *piece, char inputChar, int newX,
                        int newRotationIndex, bool useDAS, const FullSimState &parentSimState,
                        const std::string &preAdjInputSequence, char const *inputFrameTimeline,
                        int gravity, OUT FullMovePossibility &outPossibility) {

  FullSimState simState = parentSimState;
  bool hasDoneInput = false;
  bool isWallChargeTuck = false;
  int xDelta = 0;
  int rotDelta = 0;
  inputDeltas(inputChar, xDelta, rotDelta);
  const char wallChargeInput = xDelta == 1 ? 'R' : 'L';

  while (true) {
    const bool isGravityFrame = simState.frameIndex % gravity == gravity - 1;

    if (!hasDoneInput) {
      if (newX != simState.x) {
        // Shift collision: this input is impossible
        if (collision(board, piece, newX, simState.y, simState.rotationIndex)) {
          return false;
        }

        // DAS only: wall-charge tuck -- the piece could have wall-charged on the
        // previous (gravity) frame; then the tuck shift leaves charge at 10.
        if (useDAS) {
          const bool couldWallChargeLastFrame =
              collision(board, piece, newX, simState.y - 1, simState.rotationIndex) != 0;
          const bool lastFrameWasGravityFrame = (simState.frameIndex - 1) % gravity == gravity - 1;
          if (couldWallChargeLastFrame && lastFrameWasGravityFrame) {
            isWallChargeTuck = true;
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
        // Replace the '.' right before this input with the wall-charge char
        simState.inputSequence = simState.inputSequence.substr(0, simState.inputSequence.size() - 1)
                                 + char(tolower(wallChargeInput))
                                 + inputChar;
      } else {
        simState.inputSequence += inputChar;
      }
      hasDoneInput = true;
    } else {
      char newInput = '.';
      if (useDAS && simState.dasCharge < 15) {
        if (isWallChargeTuck) {
          // Keep holding after the wall-charge tuck, accruing charge while falling
          newInput = char(tolower(wallChargeInput));
          simState.dasCharge++;
        } else if (dasReadyForTuckInputs(preAdjInputSequence, simState.inputSequence,
                                         inputFrameTimeline)) {
          const char wallCharge =
              dasFindWallCharge(board, piece, simState.x, simState.y, simState.rotationIndex);
          if (wallCharge != 0) {
            newInput = wallCharge;
            simState.dasCharge = 16;
          }
        }
      }
      simState.inputSequence += newInput;
    }

    // NB: dfs gravity is one row per gravity frame even under double gravity
    // (double kill screen is disabled).
    if (isGravityFrame) {
      if (collision(board, piece, simState.x, simState.y + 1, simState.rotationIndex)) {
        outPossibility = getPossibilityFromSimState(board, piece, simState,
                                                       inputCostX10ForChar(inputChar), inputChar);
        return true;
      }
      simState.y++;
    }

    simState.frameIndex++;
  }
}

/** Novel-keeping score for the dfs: adjustability first (a walk that saved its
 * adjTimeSimState beats one that locked too early), input cost as the
 * tiebreaker. inputCostX10 spans -3..0, so the 10000-point no-snapshot penalty
 * dominates. */
static int tuckScore(int inputCostX10, int adjCaptureIdx) {
  return inputCostX10 + (adjCaptureIdx >= 0 ? 0 : -10000);
}

/** Depth-1 tuck/spin exploration. */
static TuckSpinResult
searchForTucksOrSpins(unsigned int board[20], const Piece *piece,
                         const std::vector<FullSimState> &potentialTuckSpinStates,
                         const unordered_map<int, int> &lockHeightLookup, bool useDAS,
                         const std::string &preAdjInputSequence, char const *inputFrameTimeline,
                         int gravity) {
  const int numOrientations = numPieceOrientations(piece);
  const char *possibleInputs = numOrientations == 1   ? "LR"
                               : numOrientations == 2 ? "LRAEI"
                                                      : "LRABEIFG";

  // Novel possibilities keyed by the lock cell, insertion order preserved
  // (insertion order of first discovery), keeping the higher-score entry per
  // cell (see tuckScore). Each entry carries its parent walk's
  // adjTimeSimState arena index.
  struct NovelTuck {
    FullMovePossibility possibility;
    int adjCaptureIdx;
  };
  std::vector<NovelTuck> novel;
  auto findNovel = [&novel](int key) -> NovelTuck * {
    for (NovelTuck &t : novel) {
      if ((t.possibility.rotationIndex * 100 + t.possibility.x) * 1000 + t.possibility.y == key) {
        return &t;
      }
    }
    return NULL;
  };

  for (const FullSimState &simState : potentialTuckSpinStates) {
    const int parentCaptureIdx = simState.adjCaptureIdx;
    for (const char *input = possibleInputs; *input != '\0'; input++) {
      const char inputChar = *input;
      int xDelta = 0;
      int rotDelta = 0;
      inputDeltas(inputChar, xDelta, rotDelta);
      int newRot = ((simState.rotationIndex + rotDelta) % numOrientations + numOrientations)
                   % numOrientations;
      const int newX = simState.x + xDelta;

      // Visited check : skip if this hover position adds nothing novel.
      auto it = lockHeightLookup.find(newRot * 100 + newX);
      const int highestYSeen = it != lockHeightLookup.end() ? it->second : 0;
      if (simState.y <= highestYSeen) {
        continue;
      }

      FullMovePossibility possibility;
      if (tryInput(board, piece, inputChar, newX, newRot, useDAS, simState, preAdjInputSequence,
                      inputFrameTimeline, gravity, possibility)) {
        const int key = (possibility.rotationIndex * 100 + possibility.x) * 1000 + possibility.y;
        NovelTuck *existing = findNovel(key);
        if (existing == NULL
            || tuckScore(possibility.inputCostX10, parentCaptureIdx)
                   > tuckScore(existing->possibility.inputCostX10, existing->adjCaptureIdx)) {
          if (existing != NULL) {
            existing->possibility = possibility;
            existing->adjCaptureIdx = parentCaptureIdx;
          } else {
            novel.push_back({possibility, parentCaptureIdx});
          }
        }
      }
    }
  }

  TuckSpinResult out;
  for (const NovelTuck &t : novel) {
    out.possibilities.push_back(t.possibility);
    out.adjCaptureIdx.push_back(t.adjCaptureIdx);
  }
  return out;
}

/** Adjacent duplicate lock cells keep the later (max-charge) variant. */
static void dedupeLockPossibilities(OUT std::vector<FullMovePossibility> &list,
                                       OUT std::vector<int> &captureIdx) {
  for (int i = 0; i + 1 < (int)list.size(); i++) {
    if (list[i].rotationIndex == list[i + 1].rotationIndex
        && list[i].x == list[i + 1].x
        && list[i].y == list[i + 1].y) {
      list.erase(list.begin() + i); // remove the first (worse DAS charge)
      captureIdx.erase(captureIdx.begin() + i);
    }
  }
}

/** Side-channel slot for one result from its walk-capture arena index. */
static AdjTimeSimStateSlot captureSlot(int adjCaptureIdx,
                                            const std::vector<AdjTimeSimState> &captureArena) {
  AdjTimeSimStateSlot slot;
  slot.hasCapture = adjCaptureIdx >= 0;
  slot.capture = adjCaptureIdx >= 0 ? captureArena[adjCaptureIdx] : AdjTimeSimState();
  return slot;
}

int fullMoveSearch(GameState gameState, const Piece *piece, char const *inputFrameTimeline,
                   int dasCharge, bool trackInputSequences,
                   OUT std::vector<FullMovePossibility> &results, const FullMoveSearchContext &ctx,
                   int reactionTime, OUT std::vector<AdjTimeSimStateSlot> *adjTimeCaptures) {
  const bool useDAS = dasCharge != -1; // TS: useDAS = dasCharge !== -1
  const int gravity = getGravity(gameState.level);
  const bool gravityDoubled = isGravityDoubled(gameState.level);
  const int numOrientations = numPieceOrientations(piece);
  const int initialX = INITIAL_X + ctx.existingXOffset;
  const int initialY = piece->initialY + ctx.existingYOffset;

  if (adjTimeCaptures != NULL) {
    adjTimeCaptures->clear();
  }

  // TS: a piece that has already fallen past the bottom of the board has no
  // legal moves (reachable from mid-air adjustment states).
  if (ctx.existingYOffset >= 20) {
    return 0;
  }

  // Immediate entry collision: every goal aborts -- every goal starts from
  // the same entry state and rotation, so a collision there voids them all.
  if (collision(gameState.board, piece, initialX, initialY, ctx.existingRotation)) {
    return 0;
  }

  // dasWillResetLeft/Right: && binds tighter than ||. The selection between
  // them is per goal-shift direction below. On the spawn path
  // (isAdjustment = false) this reduces to "DAS mode with incoming charge <= 5
  // and the goal requires movement".
  const bool dasWillResetLeft = (useDAS && dasCharge <= 5)
                                || (ctx.isAdjustment && ctx.dasButtonHeld != DAS_BUTTON_LEFT);
  const bool dasWillResetRight = (useDAS && dasCharge <= 5)
                                 || (ctx.isAdjustment && ctx.dasButtonHeld != DAS_BUTTON_RIGHT);

  // Phase A: per-goal frame simulation. Each walk appends at most one capture
  // to the arena; registered sim states reference it by index.
  std::vector<AdjTimeSimState> captureArena;
  std::vector<FullSimState> legalPlacementSimStates;
  for (int goalRotationIndex = 0; goalRotationIndex < numOrientations; goalRotationIndex++) {
    const int rangeStart = -2 - initialX;
    const int rangeEnd = 7 - initialX;
    for (int xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
      const bool dasWillReset =
          (xOffset > 0 && dasWillResetRight) || (xOffset < 0 && dasWillResetLeft);
      tryPlacement(gameState.board, piece, goalRotationIndex, xOffset, useDAS, dasWillReset,
                      inputFrameTimeline, gravity, gravityDoubled, initialX, initialY, dasCharge,
                      ctx, reactionTime, captureArena, legalPlacementSimStates);
    }
  }

  // Phase A dedup
  const std::vector<FullSimState> dedupedStates = dedupeSortedStates(legalPlacementSimStates);

  // Phase B: fall phase
  FallPhaseResult fall;
  exploreLegalPlacementsUntilLock(gameState.board, piece, dedupedStates, useDAS,
                                     ctx.preAdjInputSequence, inputFrameTimeline, gravity,
                                     gravityDoubled, fall);

  // Phase C: tuck/spin DFS
  TuckSpinResult tuckResult = searchForTucksOrSpins(
      gameState.board, piece, fall.potentialTuckSpinStates, fall.lockHeightLookup, useDAS,
      ctx.preAdjInputSequence, inputFrameTimeline, gravity);

  // Lock dedup on the basic list
  dedupeLockPossibilities(fall.lockPossibilities, fall.lockCaptureIdx);

  // Assemble: basics then tucks, no cross-list dedup; the side channel stays
  // index-aligned with the results.
  for (size_t i = 0; i < fall.lockPossibilities.size(); i++) {
    results.push_back(fall.lockPossibilities[i]);
    if (!trackInputSequences) {
      results.back().inputSequence.clear();
    }
    if (adjTimeCaptures != NULL) {
      adjTimeCaptures->push_back(captureSlot(fall.lockCaptureIdx[i], captureArena));
    }
  }
  for (size_t i = 0; i < tuckResult.possibilities.size(); i++) {
    results.push_back(tuckResult.possibilities[i]);
    if (!trackInputSequences) {
      results.back().inputSequence.clear();
    }
    if (adjTimeCaptures != NULL) {
      adjTimeCaptures->push_back(captureSlot(tuckResult.adjCaptureIdx[i], captureArena));
    }
  }
  return (int)results.size();
}
