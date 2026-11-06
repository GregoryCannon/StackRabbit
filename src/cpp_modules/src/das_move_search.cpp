#include "das_move_search.hpp"

#include "utils.hpp"
#include <algorithm>
#include <ctype.h>
#include <string.h>
#include <unordered_map>

using namespace std;

/**
 * DAS-aware move search: a 1:1 port of the TS spawn-placement pipeline
 * (move_search.ts tryPlacement + deDupeSortedList + exploreLegalPlacementsUntilLock,
 * and dfs.ts searchForTucksOrSpins/tryInput) for the INITIAL_PLACEMENT path.
 * Equivalence with the TS mirror is enforced by the fuzz harness (see TESTING.md).
 * The frame-exact DAS charge model is documented in docs/DAS_explainer.md.
 *
 * By design, the input sequence string is always tracked internally in
 * DAS mode: tuck-readiness gating scans it directly, exactly like TS. The
 * runtime flag only controls whether sequences are copied into the output.
 */

// DAS_SLOW_TAP_TIMELINE from src/server/params.ts (7.5 Hz slowtapping)
static const char DAS_SLOW_TAP_TIMELINE[] = "X.......";

// TS INPUT_LOOKUP: rows = shift increment (-1, 0, +1), cols = rot increment (-1, 0, +1)
static const char DAS_INPUT_TABLE[3][3] = {
    {'F', 'L', 'E'}, // shift -1 (left)
    {'B', '.', 'A'}, // shift 0
    {'G', 'R', 'I'}, // shift +1 (right)
};

/** Internal simulation state for the DAS search (superset of the C++ SimState). */
struct DasSimState {
  int x;
  int y;
  int rotationIndex;
  int frameIndex;
  int arrFrameIndex;
  int dasCharge;
  int heldDirection; // -1 left / 0 none / +1 right (TS dasButtonHeld)
  bool pendingDasReset;
  std::string inputSequence;
  bool hasAlreadyLocked;
};

static int dasNumOrientations(const Piece *piece) {
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

static int dasHeldDirectionOfChar(char c) {
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

static void dasInputDeltas(char c, OUT int &xDelta, OUT int &rotDelta) {
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

static int dasInputCostX10(char c) {
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

/** Port of TS readyForTuckInputsDas : direct scan of the sequence string. */
static bool dasReadyForTuckInputs(const std::string &inputSequence,
                                  char const *inputFrameTimeline) {
  // Frames since the last input at all (trailing run of '.')
  int framesSinceLastInput = 0;
  for (int i = (int)inputSequence.size() - 1; i >= 0; i--) {
    if (inputSequence[i] == '.') {
      framesSinceLastInput++;
    } else {
      break;
    }
  }

  // Did the Dpad have a button_down event in the last N frames (N = slow-tap ARR)?
  const int len = (int)inputSequence.size();
  const int slowTapWaitFrames = (int)strlen(DAS_SLOW_TAP_TIMELINE) - 1;
  bool didStartHoldingDpad = false;
  for (int i = max(1, len - slowTapWaitFrames); i < len; i++) {
    const int lastFr = dasArrowDir(inputSequence[i - 1]);
    const int thisFr = dasArrowDir(inputSequence[i]);
    if (thisFr != 0 && lastFr != thisFr) {
      didStartHoldingDpad = true;
    }
  }

  const int minFramesWait = (int)strlen(inputFrameTimeline) - 1;
  return !didStartHoldingDpad && framesSinceLastInput >= minFramesWait;
}

/** Port of TS findWallCharge: left priority. */
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
static int dasPlacePieceAndClearLines(unsigned int board[20], const Piece *piece, int rotationIndex,
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
      continue; // TS ignores out-of-bounds cells when stamping the board
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

/** Port of TS getPossibilityFromSimState . */
static DasPossibility dasGetPossibilityFromSimState(unsigned int board[20], const Piece *piece,
                                                    const DasSimState &simState, int inputCostX10,
                                                    char tuckInput) {
  DasPossibility p;
  p.x = simState.x;
  p.y = simState.y;
  p.rotationIndex = simState.rotationIndex;
  p.dasChargeAfter = simState.dasCharge;
  p.inputCostX10 = inputCostX10;
  p.tuckInput = tuckInput;
  p.numLinesCleared =
      dasPlacePieceAndClearLines(board, piece, simState.rotationIndex, simState.x, simState.y);
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

/** Port of TS tryPlacement  for the INITIAL_PLACEMENT spawn path. */
static void dasTryPlacement(unsigned int board[20], const Piece *piece, int goalRotationIndex,
                            int goalOffsetX, bool dasWillReset, char const *inputFrameTimeline,
                            int gravity, bool gravityDoubled, int initialX, int initialY,
                            int dasCharge, OUT std::vector<DasSimState> &legalPlacementSimStates) {

  const bool holdDASduringARE = !dasWillReset && goalOffsetX != 0; // '_' hold-through-ARE prefix

  DasSimState simState;
  simState.x = initialX;
  simState.y = initialY;
  simState.frameIndex = 0;
  simState.arrFrameIndex = 0;
  simState.rotationIndex = 0; // existingRotation is always 0 on the spawn path
  simState.dasCharge = dasCharge;
  simState.heldDirection = 0;
  simState.pendingDasReset = dasWillReset;
  simState.inputSequence = holdDASduringARE ? "_" : "";
  simState.hasAlreadyLocked = false;

  const int shiftIncrement = goalOffsetX == 0 ? 0 : (goalOffsetX > 0 ? 1 : -1);
  int rotIncrement = 0;
  if (simState.rotationIndex != goalRotationIndex) {
    rotIncrement = MOD_4(simState.rotationIndex - 1) == goalRotationIndex ? -1 : 1;
  }

  // Immediate spawn collision: this goal is impossible
  if (collision(board, piece, simState.x, simState.y, simState.rotationIndex)) {
    return;
  }

  while (true) {
    const bool tapShouldInputThisFrame =
        shouldPerformInputsThisFrame(simState.arrFrameIndex, inputFrameTimeline) != 0;
    const bool canShiftThisFrame = simState.dasCharge >= 15 || simState.pendingDasReset;
    const bool willShiftThisFrame = canShiftThisFrame && simState.x != initialX + goalOffsetX;
    const bool willHoldDasButtonThisFrame =
        simState.x != initialX + goalOffsetX
        || (simState.heldDirection != 0 && simState.dasCharge < 15);
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
      simState.dasCharge = simState.pendingDasReset ? 0 : 10; // charge lifecycle
      simState.pendingDasReset = false;
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
    char thisFrameStr = DAS_INPUT_TABLE[effShift + 1][effRot + 1];
    if (shiftIncrement != 0 && !willShiftThisFrame) {
      thisFrameStr = tolower(thisFrameStr); // DAS held but no shift executed
    }
    simState.inputSequence += thisFrameStr;

    simState.heldDirection = dasHeldDirectionOfChar(thisFrameStr);

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

    const bool foundPlacement =
        simState.rotationIndex == goalRotationIndex && simState.x == initialX + goalOffsetX;
    if (foundPlacement) {
      // TS registers a copy every frame while the piece sits at the goal
      DasSimState copy = simState;
      copy.hasAlreadyLocked = lockAfterThisFrame;
      legalPlacementSimStates.push_back(copy);
    }

    if (lockAfterThisFrame) {
      break;
    }

    // Spawn path: doneWaitingForAdjustment is always true, so exit as soon as
    // the placement is found and there is no more charge to gain .
    const bool canKeepHoldingDpadForMoreDAS =
        simState.dasCharge < 15 && simState.heldDirection != 0;
    if (!canKeepHoldingDpadForMoreDAS && foundPlacement) {
      break;
    }
  }
}

/** Port of TS deDupeSortedList : keep first + last entry per (rot, x) group. */
static std::vector<DasSimState> dasDedupeSortedStates(const std::vector<DasSimState> &states) {
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

  std::vector<DasSimState> out;
  for (int key : groupOrder) {
    const pair<int, int> bounds = groupBounds[key];
    out.push_back(states[bounds.first]);
    if (bounds.second != bounds.first) {
      out.push_back(states[bounds.second]);
    }
  }
  return out;
}

struct DasFallResult {
  std::vector<DasPossibility> lockPossibilities;
  std::vector<DasSimState> potentialTuckSpinStates;
  // Keyed by rotationIndex * 100 + x -> y. Missing keys read as 0 (TS `|| 0` quirk).
  unordered_map<int, int> lockHeightLookup;
};

/** Port of TS exploreLegalPlacementsUntilLock . */
static void
dasExploreLegalPlacementsUntilLock(unsigned int board[20], const Piece *piece,
                                   const std::vector<DasSimState> &legalPlacementSimStates,
                                   char const *inputFrameTimeline, int gravity, bool gravityDoubled,
                                   OUT DasFallResult &result) {

  for (const DasSimState &original : legalPlacementSimStates) {
    DasSimState simState = original; // TS mutates the state while falling
    int highestRegisteredY = -1;
    bool startedLookingForTuckSpins = false;

    while (true) {
      auto registerPossibility = [&]() {
        result.lockHeightLookup[simState.rotationIndex * 100 + simState.x] = simState.y;
        result.lockPossibilities.push_back(dasGetPossibilityFromSimState(
            board, piece, simState, /* inputCostX10= */ 0, /* tuckInput= */ '.'));
      };

      // If it collides immediately, quit
      if (simState.hasAlreadyLocked
          || collision(board, piece, simState.x, simState.y, simState.rotationIndex)) {
        registerPossibility();
        break;
      }

      const bool isGravityFrame = simState.frameIndex % gravity == gravity - 1;

      // Start looking for tucks/spins as soon as inputs are allowed
      const bool readyForInputs = dasReadyForTuckInputs(simState.inputSequence, inputFrameTimeline);
      if (!startedLookingForTuckSpins && readyForInputs) {
        startedLookingForTuckSpins = true;
      }

      if (startedLookingForTuckSpins && simState.y > highestRegisteredY) {
        result.potentialTuckSpinStates.push_back(simState);
        highestRegisteredY = simState.y;
      }

      // Wall charges : charge frozen during the fall except for this re-press
      char wallCharge = 0;
      if (simState.dasCharge < 16 && startedLookingForTuckSpins) {
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

/** Port of TS dfs.tryInput : one tuck/spin input, then ride gravity to lock. */
static bool dasTryInput(unsigned int board[20], const Piece *piece, char inputChar, int newX,
                        int newRotationIndex, const DasSimState &parentSimState,
                        char const *inputFrameTimeline, int gravity,
                        OUT DasPossibility &outPossibility) {

  DasSimState simState = parentSimState;
  bool hasDoneInput = false;
  bool isWallChargeTuck = false;
  int xDelta = 0;
  int rotDelta = 0;
  dasInputDeltas(inputChar, xDelta, rotDelta);
  const char wallChargeInput = xDelta == 1 ? 'R' : 'L';

  while (true) {
    const bool isGravityFrame = simState.frameIndex % gravity == gravity - 1;

    if (!hasDoneInput) {
      if (newX != simState.x) {
        // Shift collision: this input is impossible
        if (collision(board, piece, newX, simState.y, simState.rotationIndex)) {
          return false;
        }

        // wall-charge tuck: the piece could have wall-charged on the
        // previous (gravity) frame; then the tuck shift leaves charge at 10.
        const bool couldWallChargeLastFrame =
            collision(board, piece, newX, simState.y - 1, simState.rotationIndex) != 0;
        const bool lastFrameWasGravityFrame = (simState.frameIndex - 1) % gravity == gravity - 1;
        if (couldWallChargeLastFrame && lastFrameWasGravityFrame) {
          isWallChargeTuck = true;
          simState.dasCharge = 10;
        } else {
          simState.dasCharge = 0; // fresh down-press
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
      if (simState.dasCharge < 15) {
        if (isWallChargeTuck) {
          // Keep holding after the wall-charge tuck, accruing charge while falling
          newInput = char(tolower(wallChargeInput));
          simState.dasCharge++;
        } else if (dasReadyForTuckInputs(simState.inputSequence, inputFrameTimeline)) {
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
    // (TS quirk preserved; double kill screen is disabled).
    if (isGravityFrame) {
      if (collision(board, piece, simState.x, simState.y + 1, simState.rotationIndex)) {
        outPossibility = dasGetPossibilityFromSimState(board, piece, simState,
                                                       dasInputCostX10(inputChar), inputChar);
        return true;
      }
      simState.y++;
    }

    simState.frameIndex++;
  }
}

/** Port of TS searchForTucksOrSpins : depth-1 tuck/spin exploration. */
static std::vector<DasPossibility>
dasSearchForTucksOrSpins(unsigned int board[20], const Piece *piece,
                         const std::vector<DasSimState> &potentialTuckSpinStates,
                         const unordered_map<int, int> &lockHeightLookup,
                         char const *inputFrameTimeline, int gravity) {
  const int numOrientations = dasNumOrientations(piece);
  const char *possibleInputs = numOrientations == 1   ? "LR"
                               : numOrientations == 2 ? "LRAEI"
                                                      : "LRABEIFG";

  // Novel possibilities keyed by the lock cell, insertion order preserved
  // (TS Map iteration order), keeping the higher inputCost per cell.
  std::vector<DasPossibility> novel;
  auto findNovel = [&novel](int key) -> DasPossibility * {
    for (DasPossibility &p : novel) {
      if ((p.rotationIndex * 100 + p.x) * 1000 + p.y == key) {
        return &p;
      }
    }
    return NULL;
  };

  for (const DasSimState &simState : potentialTuckSpinStates) {
    for (const char *input = possibleInputs; *input != '\0'; input++) {
      const char inputChar = *input;
      int xDelta = 0;
      int rotDelta = 0;
      dasInputDeltas(inputChar, xDelta, rotDelta);
      int newRot = ((simState.rotationIndex + rotDelta) % numOrientations + numOrientations)
                   % numOrientations;
      const int newX = simState.x + xDelta;

      // Visited check : skip if this hover position adds nothing novel.
      auto it = lockHeightLookup.find(newRot * 100 + newX);
      const int highestYSeen = it != lockHeightLookup.end() ? it->second : 0;
      if (simState.y <= highestYSeen) {
        continue;
      }

      DasPossibility possibility;
      if (dasTryInput(board, piece, inputChar, newX, newRot, simState, inputFrameTimeline, gravity,
                      possibility)) {
        const int key = (possibility.rotationIndex * 100 + possibility.x) * 1000 + possibility.y;
        DasPossibility *existing = findNovel(key);
        if (existing == NULL || possibility.inputCostX10 > existing->inputCostX10) {
          if (existing != NULL) {
            *existing = possibility;
          } else {
            novel.push_back(possibility);
          }
        }
      }
    }
  }
  return novel;
}

/** Port of TS deDupeLockPossibilities : adjacent duplicate lock cells keep the later (max-charge)
 * variant. */
static void dasDedupeLockPossibilities(std::vector<DasPossibility> &list) {
  for (int i = 0; i + 1 < (int)list.size(); i++) {
    if (list[i].rotationIndex == list[i + 1].rotationIndex
        && list[i].x == list[i + 1].x
        && list[i].y == list[i + 1].y) {
      list.erase(list.begin() + i); // remove the first (worse DAS charge)
    }
  }
}

int dasMoveSearch(GameState gameState, const Piece *piece, char const *inputFrameTimeline,
                  int dasCharge, bool trackInputSequences,
                  OUT std::vector<DasPossibility> &results) {
  const int gravity = getGravity(gameState.level);
  const bool gravityDoubled = isGravityDoubled(gameState.level);
  const int numOrientations = dasNumOrientations(piece);
  const int initialX = INITIAL_X; // existingXOffset = 0 on the spawn path
  const int initialY = piece->initialY;

  // Immediate spawn collision: every goal aborts (mirrors the TS per-goal aborts)
  if (collision(gameState.board, piece, initialX, initialY, /* rotationIndex= */ 0)) {
    return 0;
  }

  // Phase A: per-goal frame simulation
  std::vector<DasSimState> legalPlacementSimStates;
  for (int goalRotationIndex = 0; goalRotationIndex < numOrientations; goalRotationIndex++) {
    const int rangeStart = -2 - initialX;
    const int rangeEnd = 7 - initialX;
    for (int xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
      // Spawn path: dasWillReset reduces to "incoming charge <= 5 and the goal
      // requires movement" ; zero-shift goals never reset or hold.
      const bool dasWillReset = xOffset != 0 && dasCharge <= 5;
      dasTryPlacement(gameState.board, piece, goalRotationIndex, xOffset, dasWillReset,
                      inputFrameTimeline, gravity, gravityDoubled, initialX, initialY, dasCharge,
                      legalPlacementSimStates);
    }
  }

  // Phase A dedup
  const std::vector<DasSimState> dedupedStates = dasDedupeSortedStates(legalPlacementSimStates);

  // Phase B: fall phase
  DasFallResult fall;
  dasExploreLegalPlacementsUntilLock(gameState.board, piece, dedupedStates, inputFrameTimeline,
                                     gravity, gravityDoubled, fall);

  // Phase C: tuck/spin DFS
  std::vector<DasPossibility> tuckPossibilities =
      dasSearchForTucksOrSpins(gameState.board, piece, fall.potentialTuckSpinStates,
                               fall.lockHeightLookup, inputFrameTimeline, gravity);

  // Lock dedup on the basic list
  dasDedupeLockPossibilities(fall.lockPossibilities);

  // Assemble: basics then tucks, no cross-list dedup
  for (const DasPossibility &p : fall.lockPossibilities) {
    results.push_back(p);
    if (!trackInputSequences) {
      results.back().inputSequence.clear();
    }
  }
  for (const DasPossibility &p : tuckPossibilities) {
    results.push_back(p);
    if (!trackInputSequences) {
      results.back().inputSequence.clear();
    }
  }
  return (int)results.size();
}
