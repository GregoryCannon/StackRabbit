import { PIECE_LOOKUP } from "../tetrominoes";
import {
  generateInputSequence,
  getBoardAndLinesClearedAfterPlacement,
  pieceCollision,
  _modulus,
  _validateIntParam,
} from "./board_helper";
import { searchForTucksOrSpins } from "./dfs";
import {
  GetGravity,
  getSurfaceArrayAndHoles,
  logBoard,
  shouldPerformInputsThisFrame,
} from "./utils";

/**
 * Generates a list of possible moves, given a board and a piece. It achieves this by
 * placing it in each possible rotation and each possible starting column, and then
 * dropping it into the stack and letting the result play out.
 */
export function getPossibleMoves(
  startingBoard: Board,
  currentPieceId: string,
  level: number,
  existingXOffset: number,
  existingYOffset: number,
  framesAlreadyElapsed: number,
  inputFrameTimeline: string,
  existingRotation: number,
  canFirstFrameShift: boolean,
  shouldLog: boolean
): Array<Possibility> {
  // If the piece has already fallen off the bottom of the board, there are no legal moves
  if (existingYOffset >= 20) {
    return [];
  }

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

  const legalPlacementSimStates: Array<SimState> = [];

  const NUM_ROTATIONS_FOR_PIECE = rotationsList.length;
  explorePlacementsHorizontally(
    currentPieceId,
    simParams,
    legalPlacementSimStates
  );

  // Loop over the range and validate the moves with more rotations than shifts
  for (
    let rotationIndex = 0;
    rotationIndex < NUM_ROTATIONS_FOR_PIECE;
    rotationIndex++
  ) {
    const rotationDifference = _modulus(
      rotationIndex - existingRotation,
      NUM_ROTATIONS_FOR_PIECE
    );
    const numRotationInputs = rotationDifference === 3 ? 1 : rotationDifference;

    const rangeStart = numRotationInputs == 2 ? -1 : 0;
    const rangeEnd = numRotationInputs == 2 ? 1 : 0;

    for (let xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
      // Check if the placement is legal
      // (if it is, it will be added to the set of legal sim states as a side effect)
      placementIsLegal(
        rotationIndex,
        xOffset,
        simParams,
        /* shouldLog= */ false,
        legalPlacementSimStates
      );
    }
  }

  const [
    basicPossibilities,
    lockHeightLookup,
    potentialTuckSpinStates,
  ] = exploreLegalPlacementsUntilLock(legalPlacementSimStates, simParams);

  const tuckSpinPossibilites = searchForTucksOrSpins(
    potentialTuckSpinStates,
    simParams,
    lockHeightLookup
  );
  return basicPossibilities.concat(tuckSpinPossibilites);
}

/** Starts with a set of states that are legal placements, but still have the piece hovering in the air.
 * Simulates letting them drop by gravity, and notes down any frames where it could maybe do a tuck/spin.
 * When the piece hits the stack, it generates a formal possibility, and notes the lock height.
 *
 * (The lock height is useful for detecting if a state is novel: if after an input we end up lower than the lock
 * height for that column and piece rotation, we know we tuck/spinned into a novel state. This avoids having to concatenate
 * and store all the visited states somewhere -- extremely expensive considering how often this is run)
 *
 * @returns [
 *  basicPossibilities,      - possibilities that can be reached without a tuck or spin
 *  lockHeightLookup,        - map from row and rotation index to lock height
 *  potentialTuckSpinStates  - states that could lead to a tuck or spin
 * ]
 */
function exploreLegalPlacementsUntilLock(
  legalPlacementSimStates: Array<SimState>,
  simParams: SimParams
): [Array<Possibility>, Map<string, number>, Array<DFSState>] {
  const lockPossibilities = [];
  const lockHeightLookup: Map<string, number> = new Map();
  const potentialTuckSpinStates: Array<DFSState> = [];

  for (const simState of legalPlacementSimStates) {
    const currentRotationPiece =
      simParams.rotationsList[simState.rotationIndex];
    let y = simState.y;
    const inputSequence = generateInputSequence(
      _modulus(simState.rotationIndex - simParams.existingRotation, 4),
      simState.x - simParams.initialX,
      simParams.inputFrameTimeline,
      simParams.canFirstFrameShift ? 0 : simParams.framesAlreadyElapsed
    );
    let inputSequenceWithWait = inputSequence;

    // Move the piece down until it hits the stack
    while (
      !pieceCollision(simParams.board, simState.x, y + 1, currentRotationPiece)
    ) {
      y++;
    }

    let startedLookingForTuckSpins = false;
    let highestRegisteredY = -1; // Tracks the Y values already registered to avoid duplicates
    while (true) {
      // Run simulated frames of only gravity
      const isGravityFrame =
        simState.frameIndex % simParams.gravity === simParams.gravity - 1; // Returns true every Nth frame, where N = gravity

      // Start looking for tucks/spins as soon as it's allowed to submit inputs
      if (
        !startedLookingForTuckSpins &&
        shouldPerformInputsThisFrame(
          simParams.inputFrameTimeline,
          simState.arrFrameIndex
        )
      ) {
        startedLookingForTuckSpins = true;
      }

      // If we're ready to input and at a new Y value, then we're good to go!
      if (startedLookingForTuckSpins && simState.y > highestRegisteredY) {
        potentialTuckSpinStates.push({
          ...simState,
          inputSequence: inputSequenceWithWait,
        });
        highestRegisteredY = simState.y;
      }

      if (isGravityFrame) {
        if (
          pieceCollision(
            simParams.board,
            simState.x,
            simState.y + 1,
            currentRotationPiece
          )
        ) {
          // Piece would lock in!
          // Do some housekeeping and then generate the possibility
          lockHeightLookup.set(
            simState.rotationIndex + "," + simState.x,
            simState.y
          );
          lockPossibilities.push(
            getPossibilityFromSimState(simState, simParams, inputSequence)
          );
          break;
        }
        simState.y++;
      }

      inputSequenceWithWait += ".";
      simState.frameIndex += 1;
      simState.arrFrameIndex += 1;
    }
  }

  return [lockPossibilities, lockHeightLookup, potentialTuckSpinStates];
}

export function getPossibilityFromSimState(
  simState: SimState,
  simParams: SimParams,
  inputSequence: string
): Possibility {
  // Make a new board with that piece locked in
  const [boardAfter, numLinesCleared] = getBoardAndLinesClearedAfterPlacement(
    simParams.board,
    simParams.rotationsList[simState.rotationIndex],
    simState.x,
    simState.y
  );
  let [surfaceArray, numHoles, holeCells] = getSurfaceArrayAndHoles(boardAfter);

  // Add the possibility to the list
  return {
    placement: [simState.rotationIndex, simState.x - simParams.initialX],
    inputSequence,
    surfaceArray,
    numHoles,
    holeCells,
    numLinesCleared,
    boardAfter,
  };
}

/**
 * Calculates how far in each direction a hypothetical piece can be tapped (for each rotation), given the AI's tap speed and
 * the piece's current position.
 *
 * (!!) NB: It performs one rotation for each shift. So if the placement requires 2 rotations, this result will only be valid for
 * placements with abs(xOffset) >= 2.
 */
function explorePlacementsHorizontally(
  pieceId: string,
  simParams: SimParams,
  legalPlacementSimStates: Array<SimState>
) {
  const rotationsList = PIECE_LOOKUP[pieceId][0];

  // Piece ranges, indexed by rotation index
  const rangesLeft = [];
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    rangesLeft.push(
      repeatedlyShiftPiece(
        -1,
        rotationIndex,
        simParams,
        legalPlacementSimStates
      )
    );
  }
  const rangesRight = [];
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    rangesRight.push(
      repeatedlyShiftPiece(1, rotationIndex, simParams, legalPlacementSimStates)
    );
  }
  return { rangesLeft, rangesRight };
}

/**
 * Helper function for getPieceRanges that shifts a hypothetical piece as many times as it can in
 * each direction, before it hits the stack or the edge of the screen.
 */
function repeatedlyShiftPiece(
  shiftIncrement: number,
  goalRotationIndex: number,
  simParams: SimParams,
  legalPlacementSimStates: Array<SimState>
) {
  const {
    board,
    initialX,
    initialY,
    framesAlreadyElapsed,
    inputFrameTimeline,
    gravity,
    rotationsList,
    existingRotation,
    canFirstFrameShift,
  } = simParams;

  // Get initial sim state
  const simState = {
    x: initialX,
    y: initialY,
    frameIndex: framesAlreadyElapsed,
    arrFrameIndex: canFirstFrameShift ? 0 : framesAlreadyElapsed,
    rotationIndex: existingRotation,
  };
  let rangeCurrent = 0;

  // Check for immediate collisions
  if (
    pieceCollision(
      board,
      simState.x,
      simState.y,
      rotationsList[simState.rotationIndex]
    )
  ) {
    return rangeCurrent;
  }

  while (true) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does
    const isInputFrame = shouldPerformInputsThisFrame(
      inputFrameTimeline,
      simState.arrFrameIndex
    );
    const isGravityFrame = simState.frameIndex % gravity === gravity - 1; // Returns true every Nth frame, where N = gravity

    if (isInputFrame) {
      const inputsSucceeded = performSimulationShift(
        shiftIncrement,
        simState,
        board,
        rotationsList[simState.rotationIndex]
      );
      if (!inputsSucceeded) {
        return rangeCurrent;
      }
    }

    if (isInputFrame) {
      const inputSucceeded = performSimulationRotation(
        goalRotationIndex,
        simState,
        board,
        rotationsList
      );
      if (!inputSucceeded) {
        return rangeCurrent;
      }
      // If both the input and the rotations went through, we're good
      rangeCurrent += shiftIncrement;
    }

    if (isGravityFrame) {
      if (
        pieceCollision(
          board,
          simState.x,
          simState.y + 1,
          rotationsList[simState.rotationIndex]
        )
      ) {
        debugLog(simState, simParams, "GRAVITY");
        return rangeCurrent; // Piece would lock in
      }
      simState.y++;
    }

    debugLog(
      simState,
      simParams,
      simState.frameIndex +
        " wasinput? " +
        isInputFrame +
        " isGrav? " +
        isGravityFrame
    );

    simState.frameIndex += 1;
    simState.arrFrameIndex += 1;

    // If we just shifted and are in the intended rotation, then this is a legal placement
    if (
      legalPlacementSimStates !== null &&
      isInputFrame &&
      simState.rotationIndex === goalRotationIndex
    ) {
      legalPlacementSimStates.push({ ...simState });
    }
  }
}

export function canDoPlacement(
  board: Board,
  level: number,
  pieceId: string,
  rotationIndex: number,
  xOffset: number,
  inputFrameTimeline: string
) {
  if (!inputFrameTimeline) {
    throw new Error("Unknown input timeline when checking placement");
  }
  const gravity = GetGravity(level); // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const rotationsList = PIECE_LOOKUP[pieceId][0];
  const simParams: SimParams = {
    board,
    initialX: 3,
    initialY: pieceId === "I" ? -2 : -1,
    framesAlreadyElapsed: 0,
    gravity,
    rotationsList,
    existingRotation: 0,
    inputFrameTimeline,
    canFirstFrameShift: false, // This function refers to doing a placement from the start, not starting from an adjustment or anything
  };
  return placementIsLegal(rotationIndex, xOffset, simParams);
}

export function placementIsLegal(
  goalRotationIndex: number,
  goalOffsetX: number,
  simParams: SimParams,
  shouldLog: boolean = false,
  legalPlacementSimStates: Array<SimState> = null
) {
  const {
    board,
    initialX,
    initialY,
    framesAlreadyElapsed,
    gravity,
    rotationsList,
    existingRotation,
    inputFrameTimeline,
    canFirstFrameShift,
  } = simParams;

  // Get initial sim state
  const shiftIncrement = goalOffsetX < 0 ? -1 : 1;
  const simState: SimState = {
    x: initialX,
    y: initialY,
    frameIndex: framesAlreadyElapsed,
    arrFrameIndex: canFirstFrameShift ? 0 : framesAlreadyElapsed,
    rotationIndex: existingRotation,
  };

  // Check for immediate collisions
  if (
    pieceCollision(
      board,
      simState.x,
      simState.y,
      rotationsList[simState.rotationIndex]
    )
  ) {
    return false;
  }

  while (
    simState.x !== initialX + goalOffsetX ||
    simState.rotationIndex !== goalRotationIndex
  ) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does
    const isInputFrame = shouldPerformInputsThisFrame(
      inputFrameTimeline,
      simState.arrFrameIndex
    );
    const isGravityFrame = simState.frameIndex % gravity === gravity - 1; // Returns true every Nth frame, where N = gravity

    if (isInputFrame) {
      const xIncrement =
        simState.x !== initialX + goalOffsetX ? shiftIncrement : 0;
      const inputSucceeded = performSimulationShift(
        xIncrement,
        simState,
        board,
        rotationsList[simState.rotationIndex]
      );
      if (!inputSucceeded) {
        return false;
      }
    }

    if (isInputFrame) {
      const inputSucceeded = performSimulationRotation(
        goalRotationIndex,
        simState,
        board,
        rotationsList
      );
      if (!inputSucceeded) {
        return false;
      }
    }

    if (isGravityFrame) {
      if (
        pieceCollision(
          board,
          simState.x,
          simState.y + 1,
          rotationsList[simState.rotationIndex]
        )
      ) {
        debugLog(simState, simParams, "GRAVITY");
        return (
          simState.x == initialX + goalOffsetX &&
          simState.rotationIndex === goalRotationIndex
        ); // Piece would lock in
      }
      simState.y++;
    }

    if (shouldLog) {
      logBoard(
        getBoardAndLinesClearedAfterPlacement(
          board,
          rotationsList[simState.rotationIndex],
          simState.x,
          simState.y
        )[0]
      );
    }
    simState.frameIndex += 1;
    simState.arrFrameIndex += 1;
  }
  if (legalPlacementSimStates !== null) {
    legalPlacementSimStates.push(simState);
  }
  return true;
}

export function performSimulationShift(
  xIncrement: number,
  simState: SimState,
  board: Board,
  currentRotationPiece: PieceArray
): boolean {
  if (
    pieceCollision(
      board,
      simState.x + xIncrement,
      simState.y,
      currentRotationPiece
    )
  ) {
    // console.log("DEBUG: COLLISION");
    // utils.logBoard(
    //   getBoardAndLinesClearedAfterPlacement(
    //     board,
    //     currentRotationPiece,
    //     simState.x,
    //     simState.y
    //   )[0]
    // );
    return false; // We're done, can't go any further
  }
  simState.x += xIncrement;
  return true;
}

function performSimulationRotation(
  goalRotationIndex: number,
  simState: SimState,
  board: Board,
  rotationsList: Array<PieceArray>
): boolean {
  // Plan for a rotation if needed
  if (simState.rotationIndex === goalRotationIndex) {
    return true;
  }
  const prevRotationIndex = simState.rotationIndex;
  if (_modulus(simState.rotationIndex - 1, 4) === goalRotationIndex) {
    // Left rotation
    simState.rotationIndex--;
  } else {
    simState.rotationIndex++;
  }
  simState.rotationIndex = _modulus(simState.rotationIndex, 4);

  if (
    simState.rotationIndex >= rotationsList.length ||
    simState.rotationIndex < 0
  ) {
    throw new Error(`Invalid rotation index ${simState.rotationIndex}`);
  }

  if (
    pieceCollision(
      board,
      simState.x,
      simState.y,
      rotationsList[simState.rotationIndex]
    )
  ) {
    // console.log("DEBUG: COLLISION");
    // utils.logBoard(
    //   getBoardAndLinesClearedAfterPlacement(
    //     board,
    //     rotationsList[prevRotationIndex],
    //     simState.x,
    //     simState.y
    //   )[0]
    // );
    return false; // We're done, can't go any further
  }
  return true;
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
