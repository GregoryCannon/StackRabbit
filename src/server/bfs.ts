import { PIECE_LOOKUP } from "../tetrominoes";
import {
  getBoardAndLinesClearedAfterPlacement,
  getTestBoardWithHeight,
  pieceCollision,
  _modulus,
  _validateIntParam,
} from "./board_helper";
import {
  GetGravity,
  getSurfaceArrayAndHoles,
  logBoard,
  shouldPerformInputsThisFrame,
} from "./utils";

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

const INPUT_GRAMMAR = {
  ".": ".",
  L: "L.",
  R: "R.",
  a: ".",
  b: ".",
  A: "a.",
  e: "L.",
  i: "R.",
  f: "L.",
  g: "R.",
  E: "eLa.",
  I: "iRa.",
  PIECE1: "LR.",
  PIECE2: "eiLRab.",
  PIECE4: "EIfgLRAb.",
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
): Array<PossibilityChain> {
  console.time("BFS setup");
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
    hasPassedOnInput: false,
    inputSequence: "",
    grammarToken: "PIECE" + rotationsList.length,
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
    return [];
  }

  let activeList = [startingState];
  let visitedStates: Set<string> = new Set();
  let finalStates: Array<BFSState> = [];
  visitedStates.add(encodeState(startingState));

  console.timeEnd("BFS setup");
  console.time("BFS Main");

  while (activeList.length > 0) {
    // Dequeue a state from the list and add its neighbors to the new list
    const activeState = activeList.shift();
    addNeighbors(
      activeState,
      simParams,
      activeList,
      finalStates,
      visitedStates
    );
  }

  console.timeEnd("BFS Main");
  console.time("BFS generate");

  // console.log("FINAL STATES:");
  // console.log(finalStates.map((x) => x.inputSequence + " | " + encodeState(x)));
  // console.log(finalStates.length);
  const res = generatePossibilityList(finalStates, simParams);
  console.timeEnd("BFS generate");
  return res;
}

/**
 * Adds all successors for an input state to the active list.
 * Successor calculation follows the input grammar defined at the top of the file.
 * @param simState
 * @param simParams
 * @param activeList
 * @param finalStates
 * @param visitedStates
 */
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
  const possibleInputs = isInputFrame
    ? INPUT_GRAMMAR[simState.grammarToken]
    : ".";

  for (const input of possibleInputs) {
    tryInput(
      input.toUpperCase(),
      {
        ...simState,
        grammarToken: isInputFrame ? input : simState.grammarToken, // Maybe update grammar token
      },
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
  simState: BFSState,
  simParams: SimParams,
  activeList: Array<BFSState>,
  finalStates: Array<BFSState>,
  visitedStates: Set<string>,
  isInputFrame: boolean
): void {
  // Check if the resulting state has been visited
  const xDelta = X_INCREMENT_LOOKUP[inputChar] || 0;
  const rotDelta = ROTATION_LOOKUP[inputChar] || 0;
  const newRot = _modulus(
    simState.rotationIndex + rotDelta,
    simParams.rotationsList.length
  );
  const potentialNextEncoded = encode(
    simState.x + xDelta,
    simState.y,
    newRot,
    simState.frameIndex + 1
  );
  if (visitedStates.has(potentialNextEncoded)) {
    console.log("VISITED:", simState.inputSequence + inputChar);
    return;
  }

  // Run a simulated 'frame' of gravity, shifting, and collision checking
  const isGravityFrame =
    simState.frameIndex % simParams.gravity === simParams.gravity - 1; // Returns true every Nth frame, where N = gravity

  // Update other properties of the state (doesn't affect frame simulation)
  simState.inputSequence += inputChar;
  if (inputChar == "." && isInputFrame) {
    simState.hasPassedOnInput = true;
    simState.arrFrameIndex = 0;
  } else {
    simState.arrFrameIndex += 1;
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

  visitedStates.add(encodeState(simState)); // Don't look for other inputs that get to this state
  activeList.push(simState);
}

function generatePossibilityList(
  finalStates: Array<BFSState>,
  simParams: SimParams
): Array<PossibilityChain> {
  const possibilityList = [];

  for (const simState of finalStates) {
    // Make a new board with that piece locked in
    const [boardAfter, numLinesCleared] = getBoardAndLinesClearedAfterPlacement(
      simParams.board,
      simParams.rotationsList[simState.rotationIndex],
      simState.x,
      simState.y
    );
    let [surfaceArray, numHoles, holeCells] = getSurfaceArrayAndHoles(
      boardAfter
    );
    surfaceArray = surfaceArray.slice(0, 9);

    // Add the possibility to the list
    possibilityList.push({
      placement: [simState.rotationIndex, simState.x - simParams.initialX],
      inputSequence: simState.inputSequence,
      surfaceArray,
      numHoles,
      holeCells,
      numLinesCleared,
      boardAfter,
    });
  }

  return possibilityList;
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
    (state.y + 1) +
    "," +
    state.rotationIndex +
    "," +
    state.frameIndex
  );
}

function encode(x, y, rot, frameIndex) {
  return x - 3 + "," + (y + 1) + "," + rot + "," + frameIndex;
}

// console.log("T");
// logBoard(getTestBoardWithHeight(10));
// getPossibleMovesBfs(
//   getTestBoardWithHeight(10),
//   "L",
//   18,
//   0,
//   0,
//   0,
//   "X...",
//   0,
//   false
// );

function speedTest(x) {
  // console.time("\nspeedtest");
  for (let i = 0; i < 1; i++) {
    getPossibleMovesBfs(
      getTestBoardWithHeight(x),
      "L",
      18,
      0,
      0,
      0,
      "X...",
      0,
      false
    );
  }
  // console.timeEnd("\nspeedtest");
}
for (let i = 0; i < 30; i++) {
  speedTest(0);
}
