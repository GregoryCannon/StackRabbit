import { PIECE_LOOKUP } from "../tetrominoes";
import {
  generateInputSequence,
  getBoardAndLinesClearedAfterPlacement,
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
  // console.time("OG setup");
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

  const legalPlacements: Array<Placement> = [];
  const legalPlacementSimStates: Array<SimState> = [];

  // The spawn placement is always legal
  const spawnState: SimState = {
    x: initialX,
    y: initialY,
    frameIndex: framesAlreadyElapsed,
    arrFrameIndex: canFirstFrameShift ? 0 : framesAlreadyElapsed,
    rotationIndex: existingRotation,
  };
  legalPlacementSimStates.push(spawnState);

  const NUM_ROTATIONS_FOR_PIECE = rotationsList.length;
  explorePlacementsHorizontally(
    currentPieceId,
    simParams,
    legalPlacementSimStates
  );

  // Loop over the range and validate the moves with more rotations than shifts
  if (NUM_ROTATIONS_FOR_PIECE > 1) {
    for (
      let rotationIndex = 0;
      rotationIndex < NUM_ROTATIONS_FOR_PIECE;
      rotationIndex++
    ) {
      const rotationDifference = _modulus(
        rotationIndex - existingRotation,
        NUM_ROTATIONS_FOR_PIECE
      );
      const numRotationInputs =
        rotationDifference === 3 ? 1 : rotationDifference;

      const rangeStart = numRotationInputs == 2 ? -1 : 0;
      const rangeEnd = numRotationInputs == 2 ? 1 : 0;

      for (let xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
        // Don't double count the spawn state
        if (rotationIndex == 0) {
          continue;
        }
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
  }

  const [
    basicPossibilities,
    lockHeightLookup,
    potentialTuckSpinStates,
  ] = exploreLegalPlacementsUntilLock(legalPlacementSimStates, simParams);

  // const res = _generatePossibilityList(
  //   legalPlacements,
  //   startingBoard,
  //   currentPieceId,
  //   initialX,
  //   initialY,
  //   inputFrameTimeline,
  //   framesAlreadyElapsed,
  //   existingRotation,
  //   shouldLog
  // );
  return basicPossibilities;
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
): [Array<Possibility>, Map<string, number>, Array<SimState>] {
  const lockPossibilities = [];
  const lockHeightLookup: Map<string, number> = new Map();
  const potentialTuckSpinStates: Array<SimState> = [];

  for (const simState of legalPlacementSimStates) {
    const currentRotationPiece =
      simParams.rotationsList[simState.rotationIndex];
    let y = simState.y;

    // Move the piece down until it hits the stack
    while (
      !pieceCollision(simParams.board, simState.x, y + 1, currentRotationPiece)
    ) {
      y++;
    }

    while (true) {
      // Run a simulated 'frame' of gravity, shifting, and collision checking
      // We simulate shifts and rotations on the ARR triggers, just like the Lua script does
      const isInputFrame = shouldPerformInputsThisFrame(
        simParams.inputFrameTimeline,
        simState.arrFrameIndex
      );
      const isGravityFrame =
        simState.frameIndex % simParams.gravity === simParams.gravity - 1; // Returns true every Nth frame, where N = gravity

      if (isInputFrame) {
        potentialTuckSpinStates.push({ ...simState });
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
          // Piece would lock in! Generate the lock possibility now
          lockPossibilities.push(
            getPossibilityFromSimState(simState, simParams)
          );
          break;
        }
        simState.y++;
      }

      simState.frameIndex += 1;
      simState.arrFrameIndex += 1;
    }
  }

  return [lockPossibilities, lockHeightLookup, potentialTuckSpinStates];
}

function getPossibilityFromSimState(
  simState: SimState,
  simParams: SimParams
): Possibility {
  // Make a new board with that piece locked in
  const [boardAfter, numLinesCleared] = getBoardAndLinesClearedAfterPlacement(
    simParams.board,
    simParams.rotationsList[simState.rotationIndex],
    simState.x,
    simState.y
  );
  let [surfaceArray, numHoles, holeCells] = getSurfaceArrayAndHoles(boardAfter);
  surfaceArray = surfaceArray.slice(0, 9);

  // Add the possibility to the list
  const xOffset = simState.x - simParams.initialX;
  return {
    placement: [simState.rotationIndex, xOffset],
    inputSequence: generateInputSequence(
      _modulus(simState.rotationIndex - simParams.existingRotation, 4),
      xOffset,
      simParams.inputFrameTimeline,
      simParams.framesAlreadyElapsed
    ),
    surfaceArray,
    numHoles,
    holeCells,
    numLinesCleared,
    boardAfter,
  };
}

function _generatePossibilityList(
  legalPlacements: Array<Placement>,
  startingBoard: Board,
  currentPieceId: string,
  startingX: number,
  startingY: number,
  inputFrameTimeline: string,
  framesAlreadyElapsed: number,
  existingRotation: number,
  shouldLog: boolean
): Array<PossibilityChain> {
  const possibilityList = [];

  for (const [rotationIndex, xOffset] of legalPlacements) {
    const currentRotationPiece = PIECE_LOOKUP[currentPieceId][0][rotationIndex];
    const x = startingX + xOffset;
    let y = startingY;

    // Move the piece down until it hits the stack
    while (!pieceCollision(startingBoard, x, y + 1, currentRotationPiece)) {
      y++;
    }

    // Make a new board with that piece locked in
    const [boardAfter, numLinesCleared] = getBoardAndLinesClearedAfterPlacement(
      startingBoard,
      currentRotationPiece,
      x,
      y
    );
    let [surfaceArray, numHoles, holeCells] = getSurfaceArrayAndHoles(
      boardAfter
    );
    surfaceArray = surfaceArray.slice(0, 9);

    // Add the possibility to the list
    if (shouldLog) {
      console.log(
        `Adding possibility [Index ${rotationIndex}, xOffset ${xOffset}], would make surface ${surfaceArray}`
      );
    }
    possibilityList.push({
      placement: [rotationIndex, xOffset],
      inputSequence: generateInputSequence(
        _modulus(rotationIndex - existingRotation, 4),
        xOffset,
        inputFrameTimeline,
        framesAlreadyElapsed
      ),
      surfaceArray,
      numHoles,
      holeCells,
      numLinesCleared,
      boardAfter,
    });
  }

  if (shouldLog) {
    console.log(
      `Result: ${possibilityList.length} possibilities for ${currentPieceId}`
    );
  }
  return possibilityList;
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
