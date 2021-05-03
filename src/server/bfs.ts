import { PIECE_LOOKUP } from "../tetrominoes";
import {
  getBoardAndLinesClearedAfterPlacement,
  getTestBoardWithHeight,
  pieceCollision,
  _modulus,
  _validateIntParam,
} from "./board_helper";
import { GetGravity, logBoard, shouldPerformInputsThisFrame } from "./utils";

// Determine the x increment to apply based on the encoded input
const X_INCREMENT_LOOKUP = {
  L: -1,
  e: -1, // L + A
  f: -1, // L + B
  R: 1,
  i: 1, // R + A
  g: 1, // R + B
};
// Determine the rotation to apply based on the encoded input
const ROTATION_LOOKUP = {
  A: 1,
  e: 1, // A + L
  i: 1, // A + R
  B: -1,
  f: -1, // B + L
  g: -1, // B + R
};

export function getPossibleMovesBfs(
  startingBoard: Board,
  currentPieceId: string,
  level: number,
  existingXOffset: number,
  existingYOffset: number,
  framesAlreadyElapsed: number,
  inputFrameTimeline: string,
  existingRotation: number,
  canFirstFrameShift: boolean
) {
  _validateIntParam(level, 0, 999);
  _validateIntParam(existingXOffset, -5, 4);
  _validateIntParam(existingYOffset, 0, 20);
  _validateIntParam(framesAlreadyElapsed, 0, 999);
  _validateIntParam(existingRotation, 0, 4);

  const initialX = 3 + existingXOffset;
  const initialY = (currentPieceId == "I" ? -2 : -1) + existingYOffset;
  const gravity = GetGravity(level);
  const rotationsList = PIECE_LOOKUP[currentPieceId][0];

  const simParams = {
    board: startingBoard,
    initialX,
    initialY,
    framesAlreadyElapsed,
    gravity,
    inputFrameTimeline,
    rotationsList,
    existingRotation,
    canFirstFrameShift,
  };

  const startingState: BFSState = {
    x: initialX,
    y: initialY,
    rotationIndex: existingRotation,
    arrFrameIndex: canFirstFrameShift ? 0 : framesAlreadyElapsed,
    frameIndex: framesAlreadyElapsed,
    hasTuckOrSpin: false,
    hasPassedOnInput: false,
    canInputImmediately: false,
    initialDirection: "",
    rotationsRemaining: getNumAllowedRotations(simParams),
    inputSequence: "",
  };

  // Check for immediate collisions
  if (
    pieceCollision(
      startingBoard,
      startingState.x,
      startingState.y,
      rotationsList[startingState.rotationIndex]
    )
  ) {
    return false;
  }

  let activeList = [startingState];
  let visitedStates: Set<string> = new Set();
  let finalStates: Array<BFSState> = [];
  visitedStates.add(encodeState(startingState));

  while (activeList.length > 0) {
    // Dequeue a state from the list and add its neighbors to the new list
    const activeState = activeList.shift();
    // console.log(
    //   "\nSearching from: ",
    //   activeState.inputSequence,
    //   "      ",
    //   encodeState(activeState)
    // );
    addNeighbors(
      activeState,
      simParams,
      activeList,
      finalStates,
      visitedStates
    );
  }

  console.log("FINAL STATES:");
  console.log(finalStates.map((x) => x.inputSequence + " | " + encodeState(x)));
}

function addNeighbors(
  simState: BFSState,
  simParams: SimParams,
  activeList: Array<BFSState>,
  finalStates: Array<BFSState>,
  visitedStates: Set<string>
) {
  const isInputFrame =
    !simState.hasPassedOnInput &&
    shouldPerformInputsThisFrame(
      simParams.inputFrameTimeline,
      simState.arrFrameIndex
    );

  // If it's an input frame, we can do any input. Otherwise we still check the 'do nothing' case
  // The possible inputs are ordered strategically such that the final sequences will mimic human inputs
  let possibleInputs = "";
  if (isInputFrame) {
    const numOrientations = simParams.rotationsList.length;
    if (numOrientations == 2 && simState.rotationsRemaining > 0) {
      possibleInputs += "ei";
    }
    if (numOrientations == 4 && simState.rotationsRemaining > 0) {
      possibleInputs += "fg";
    }
    if (numOrientations == 2 && simState.rotationsRemaining > 0) {
      possibleInputs += "A";
    }
    if (numOrientations == 4 && simState.rotationsRemaining > 0) {
      possibleInputs += "B";
    }
    if (simState.initialDirection !== "L") {
      possibleInputs += "R";
    }
    if (simState.initialDirection !== "R") {
      possibleInputs += "L";
    }
  }

  possibleInputs += ".";
  // const possibleInputs = isInputFrame ? "eifgLRAB." : ".";

  for (const input of possibleInputs) {
    tryInput(
      input,
      simState,
      simParams,
      activeList,
      finalStates,
      visitedStates,
      isInputFrame
    );
  }
}

/**
 * Tries out an input from a given state, and if it's valid, adds it to the active list.
 * If the piece locks in after the input, it will also add that finished state to the final list.
 */
function tryInput(
  inputChar: string,
  parentState: BFSState,
  simParams: SimParams,
  activeList: Array<BFSState>,
  finalStates: Array<BFSState>,
  visitedStates: Set<string>,
  isInputFrame: boolean
): BFSState | null {
  // Make sure we don't modify the parent state
  const simState = cloneState(parentState);

  // Check if the resulting state has been visited
  const xDelta = X_INCREMENT_LOOKUP[inputChar] || 0;
  const rotDelta = ROTATION_LOOKUP[inputChar] || 0;
  const potentialNextState = {
    ...simState,
    x: simState.x + xDelta,
    rotationIndex: _modulus(
      simState.rotationIndex + rotDelta,
      simParams.rotationsList.length
    ),
    frameIndex: simState.frameIndex + 1,
  };
  const potentialNextEncoded = encodeState(potentialNextState);
  // console.log(inputChar, "\tTrying to get to " + potentialNextEncoded);
  if (visitedStates.has(potentialNextEncoded)) {
    // console.log("\tVISITED:", potentialNextEncoded);
    return;
  } else {
    // console.log("\tNOVEL:", potentialNextEncoded);
  }

  // Run a simulated 'frame' of gravity, shifting, and collision checking
  const isGravityFrame =
    simState.frameIndex % simParams.gravity === simParams.gravity - 1; // Returns true every Nth frame, where N = gravity

  // Update other properties of the state
  simState.inputSequence += inputChar;
  if (inputChar == "." && isInputFrame) {
    simState.canInputImmediately = true; // If the agent passed up on the ability to perform inputs, it can do the next one at any time
    simState.hasPassedOnInput = true;
    simState.arrFrameIndex = 0;
  } else {
    simState.canInputImmediately = false;
    simState.arrFrameIndex += 1;
  }
  if (xDelta === -1) {
    simState.initialDirection = "L";
  } else if (xDelta == 1) {
    simState.initialDirection = "R";
  }
  if (rotDelta !== 0) {
    simState.rotationsRemaining--;
  }
  simState.frameIndex += 1;

  // Handle the given input
  if (inputChar !== ".") {
    // Try shifting if needed
    if (X_INCREMENT_LOOKUP.hasOwnProperty(inputChar)) {
      const xIncrement = X_INCREMENT_LOOKUP[inputChar];
      if (
        pieceCollision(
          simParams.board,
          simState.x + xIncrement,
          simState.y,
          simParams.rotationsList[simState.rotationIndex]
        )
      ) {
        debugLog(simState, simParams, "SHIFT COLLISION");

        // Failed to perform input, don't add this state
        return;
      }
      // Otherwise the shift succeeded
      simState.x += xIncrement;
    }

    // Try rotating if needed
    if (ROTATION_LOOKUP.hasOwnProperty(inputChar)) {
      const rotationDelta = ROTATION_LOOKUP[inputChar];
      const newRotation = _modulus(
        simState.rotationIndex + rotationDelta,
        simParams.rotationsList.length
      );

      if (
        pieceCollision(
          simParams.board,
          simState.x,
          simState.y,
          simParams.rotationsList[newRotation]
        )
      ) {
        debugLog(simState, simParams, "ROTATION COLLISION");

        // Failed to perform input, don't add this state
        return;
      }
      // Otherwise the rotation succeeded
      simState.rotationIndex = newRotation;
    }
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
      debugLog(simState, simParams, "GRAVITY");

      // Piece locked into the stack, so add it to the finished states (no more searching to be done)
      visitedStates.add(encodeState(simState));
      finalStates.push(simState);
      return;
    }
    // Otherwise it shifts down and we keep searching
    simState.y++;
  }

  // console.log("\tADDING:", encodeState(simState));
  visitedStates.add(encodeState(simState)); // Don't look for other inputs that get to this state
  activeList.push(simState);
}

function debugLog(simState: SimState, simParams: SimParams, reason: string) {
  // console.log("DEBUG: " + reason);
  // logBoard(
  //   getBoardAndLinesClearedAfterPlacement(
  //     simParams.board,
  //     simParams.rotationsList[simState.rotationIndex],
  //     simState.x,
  //     simState.y
  //   )[0]
  // );
}

/** Encode a state to a string so it can be used as the key to a hashmap */
function encodeState(state: BFSState): string {
  return (
    state.x -
    3 +
    "," +
    (state.y + 2) +
    "," +
    state.rotationIndex +
    "," +
    state.frameIndex
  );
}

/** Shallow clone a state to avoid pass-by-reference bugs */
function cloneState(state: BFSState) {
  return {
    ...state,
  };
}

function getNumAllowedRotations(simParams: SimParams) {
  switch (simParams.rotationsList.length) {
    case 4:
      return 2;
    case 2:
      return 1;
    case 1:
      return 0;
    default:
      throw Error("Rotationlist had unexpected length!");
  }
}

console.log("I");
logBoard(getTestBoardWithHeight(10));
getPossibleMovesBfs(
  getTestBoardWithHeight(10),
  "I",
  18,
  0,
  0,
  0,
  "X...",
  0,
  false
);
