import {
  _modulus,
  getBoardAndLinesClearedAfterPlacement,
  pieceCollision
} from "./board_helper";
import { getPossibilityFromSimState, readyForTuckInputsDas } from "./move_search";
import { Board, LegalPlacementSimState, Possibility, PossibilityChain, SimParams, SimState } from "./types";
import { logBoard } from "./utils";

const SPINTUCK_COST = -0.3;
const SPIN_COST = -0.2;
const TUCK_COST = -0.1;
const INPUT_COST_LOOKUP = {
  E: SPINTUCK_COST,
  F: SPINTUCK_COST,
  I: SPINTUCK_COST,
  G: SPINTUCK_COST,
  L: TUCK_COST,
  R: TUCK_COST,
  A: SPIN_COST,
  B: SPIN_COST,
};

// Determine the x increment to apply based on the encoded input
const X_INCREMENT_LOOKUP = {
  L: -1,
  E: -1, // L + A
  F: -1, // L + B
  R: 1,
  I: 1, // R + A
  G: 1, // R + B
};
// Determine the rotation to apply based on the encoded input
const ROTATION_LOOKUP = {
  A: 1,
  E: 1, // A + L
  I: 1, // A + R
  B: -1,
  F: -1, // B + L
  G: -1, // B + R
};

export function searchForTucksOrSpins(
  potentialTuckSpinStates: Array<LegalPlacementSimState>,
  simParams: SimParams,
  lockHeightLookup: Map<string, number>
): Array<PossibilityChain> {
  const novelPossibilities = searchForTucksOrSpinsInternal(
    potentialTuckSpinStates,
    simParams,
    lockHeightLookup,
  );
  return [...novelPossibilities.values()];
}

const POSSIBLE_INPUT_LOOKUP = {
  1: "LR",
  2: "LRAEI",
  4: "LRABEIFG",
};

function searchForTucksOrSpinsInternal(
  potentialTuckSpinStates: Array<LegalPlacementSimState>,
  simParams: SimParams,
  lockHeightLookup: Map<string, number>,
): Map<string, PossibilityChain> {
  const novelPossibilities = new Map();
  for (const simState of potentialTuckSpinStates) {
    // Find the possible inputs
    const numOrientations = simParams.rotationsList.length;
    const possibleInputs = POSSIBLE_INPUT_LOOKUP[numOrientations];
    // Try each one
    for (const inputChar of possibleInputs) {
      const xDelta = X_INCREMENT_LOOKUP[inputChar] || 0;
      const rotDelta = ROTATION_LOOKUP[inputChar] || 0;
      const newRot = _modulus(
        simState.rotationIndex + rotDelta,
        simParams.rotationsList.length
      );
      const newX = simState.x + xDelta;

      if (!hasBeenVisited(newX, newRot, simState, lockHeightLookup)) {
        // Potential new state! (If it's legal)
        tryInput(
          inputChar,
          newX,
          newRot,
          simState,
          simParams,
          novelPossibilities
        );
      }
    }
  }
  return novelPossibilities;
}

/** A custom visited check that avoids creating an expensive set object with all the states */
function hasBeenVisited(
  newX,
  newRot,
  simState: SimState,
  lockHeightLookup
): boolean {
  const highestYSeen = lockHeightLookup.get(newRot + "," + newX) || 0;
  // If we've already reached this Y value by just placing it in this column normally (or with another tuck), it's been visited
  return simState.y <= highestYSeen;
}

/**
 * Tries out an input from a given state, and if it's valid, adds it to the active list.
 * If the piece locks in after the input, it will also add that finished state to the final list.
 */
function tryInput(
  inputChar: string,
  newX: number,
  newRotationIndex: number,
  parentSimState: LegalPlacementSimState,
  simParams: SimParams,
  novelPossibilities: Map<string, Possibility>
): void {
  let hasDoneInput = false;
  let simState = { ...parentSimState };
  const useDAS = simParams.dasCharge !== -1;
  let isWallChargeTuck = false;
  const wallChargeInput = X_INCREMENT_LOOKUP[inputChar] == 1 ? "R" : "L";

  debugLog(simState, simParams, "TRYING " + simState.inputSequence + inputChar)

  while (true) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    const willGravity = (frameIndex: number) => frameIndex % simParams.gravity === simParams.gravity - 1;
    const isGravityFrame = willGravity(simState.frameIndex)

    if (!hasDoneInput) {
      // Try shifting if needed
      if (newX !== simState.x) {
        // Check if the shift works at all
        if (
          pieceCollision(
            simParams.board,
            newX,
            simState.y,
            simParams.rotationsList[simState.rotationIndex]
          )
        ) {
          debugLog(simState, simParams, "SHIFT COLLISION");

          // Failed to perform input, don't add this state
          return;
        }

        // When playing DAS, you shift at least 1 frame early on tucks so that you wall-charge right before executing the input. That leaves you with a DAS charge of at least 10.
        if (useDAS) {
          const couldWallChargeLastFrame = pieceCollision(
            simParams.board,
            newX,
            simState.y - 1,
            simParams.rotationsList[simState.rotationIndex]
          )
          // This is almost always true based on how we search for potentialTuckSpinStates, 
          // but can be false on the very first frame that "readyForInputsDas" becomes true.
          const lastFrameWasGravityFrame = willGravity(simState.frameIndex - 1)

          if (couldWallChargeLastFrame && lastFrameWasGravityFrame) {
            isWallChargeTuck = true;
            simState.dasCharge = 10;
          } else {
            // Not able to wallcharge first
            simState.dasCharge = 0;
          }
        }

        // Successfully performed tuck, adjust X value
        simState.x = newX;
      }

      // Try rotating if needed
      if (
        newRotationIndex !== simState.rotationIndex &&
        pieceCollision(
          simParams.board,
          simState.x,
          simState.y,
          simParams.rotationsList[newRotationIndex]
        )
      ) {
        debugLog(simState, simParams, "ROTATION COLLISION");

        // Failed to perform input, don't add this state
        return;
      }
      simState.rotationIndex = newRotationIndex;

      // It worked! Update the input sequence
      if (isWallChargeTuck) {
        // Replace the . right before this input with either an L or an R to wallcharge
        simState.inputSequence = simState.inputSequence.slice(0, -1) + wallChargeInput.toLowerCase() + inputChar;
      } else {
        simState.inputSequence += inputChar;
      }
      hasDoneInput = true;
    }

    // (else means hasDoneInput = true)
    // Now that the tuck is done, just wait for the piece to lock (unless we have DAS shenanigans to do)
    else {
      let newInput = ".";

      if (useDAS && simState.dasCharge < 15) {
        // When looking for quicktaps right after adjustment time, the context of what was happening prior to the adjustment is relevant. 
        // The only way to differentiate between a standard long-bar-right QT and a random 20 Hz 2-tap "adjustment" in the middle of the stack
        // is to analyze the prior input sequence. Notably, the standard quicktaps start with a long period of the button being held down.
        const fullInputSequence = simParams.adjustmentState.preAdjInputSequence + simState.inputSequence
        const readyForInputs = readyForTuckInputsDas(fullInputSequence, simParams.inputFrameTimeline)

        if (isWallChargeTuck) {
          // If we did a wall charge tuck, keep holding it for more DAS charge after the tuck is done
          newInput = wallChargeInput.toLowerCase();
          simState.dasCharge++;
        } else if (readyForInputs) {
          const wallCharge = findWallCharge(simState, simParams, newRotationIndex);
          if (wallCharge != null) {
            newInput = wallCharge;
            simState.dasCharge = 16;
          }
        }
      }

      simState.inputSequence += newInput;
    }

    // Apply gravity on gravity frames, regardless of what inputChar is
    if (isGravityFrame) {
      if (
        pieceCollision(
          simParams.board,
          simState.x,
          simState.y + 1,
          simParams.rotationsList[simState.rotationIndex]
        )
      ) {
        debugLog(simState, simParams, "SUCCESS - GRAVITY");
        // Piece locked into the stack, so maybe add it if it's not redundant!
        const newPossibility = getPossibilityFromSimState(
          { ...simState, hasAlreadyLocked: true },
          simParams,
          INPUT_COST_LOOKUP[inputChar]
        )

        // Determine if we've seen it before, and if so, if it's an improvement over the existing one
        const encodedEndingSpot =
          simState.x + "|" + simState.y + "|" + simState.rotationIndex;
        const existing = novelPossibilities.get(encodedEndingSpot);
        // Come up with a teeny numeric heuristic to compare multiple factors at once. Adjustability matters first, and inputCost is a tiebreaker.
        const getScore = (p: Possibility) => p.inputCost + (p.adjTimeSimState == null ? -1000 : 0);
        if (existing === undefined || getScore(newPossibility) > getScore(existing)) {
          novelPossibilities.set(encodedEndingSpot, newPossibility);
        }
        return;
      }
      // Otherwise it shifts down and we keep searching
      simState.y++;
    }
    simState.frameIndex += 1;
  }
}

function findWallCharge(simState: SimState, simParams: SimParams, newRotationIndex: number): string | null {
  if (pieceCollision(
    simParams.board,
    simState.x - 1,
    simState.y,
    simParams.rotationsList[newRotationIndex]
  )) {
    return "l"; // Lowercase to indicate that the piece won't actually shift
  }
  if (pieceCollision(
    simParams.board,
    simState.x + 1,
    simState.y,
    simParams.rotationsList[newRotationIndex]
  )) {
    return "r"; // // Lowercase to indicate that the piece won't actually shift
  }
  return null;
}

function debugLog(simState: SimState, simParams: SimParams, reason: string) {
  // if (reason == "SUCCESS - GRAVITY" && simState.x == 0 && simState.rotationIndex == 0) {
  //   console.log("WAHOO!");
  // }
  // if (simState.x !== 1 || simState.rotationIndex !== 1) {
  //   return
  // }
  // console.log("\nDEBUG: " + reason);
  // logBoard(
  //   getBoardAndLinesClearedAfterPlacement(
  //     simParams.board,
  //     simParams.rotationsList[simState.rotationIndex],
  //     simState.x,
  //     simState.y
  //   )[0]
  // );
}