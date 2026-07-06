import { PIECE_LOOKUP } from "../../docs/tetrominoes";
import {
  generateInputSequence,
  getBoardAndLinesClearedAfterPlacement,
  pieceCollision,
  _modulus,
  _validateIntParam,
} from "./board_helper";
import { searchForTucksOrSpins } from "./dfs";
import { CAN_TUCK, IS_DAS } from "./params";
import {
  GetGravity,
  getLevelAfterLineClears,
  IsGravityDoubled,
  logBoard,
  NUM_ROW,
  shouldPerformInputsThisFrame,
} from "./utils";

export function getSearchStateAfter(
  prevSearchState: SearchState,
  possibility: Possibility
): SearchState {
  const levelAfter = getLevelAfterLineClears(
    prevSearchState.level,
    prevSearchState.lines,
    possibility.numLinesCleared
  );
  return {
    board: possibility.boardAfter,
    currentPieceId: prevSearchState.nextPieceId,
    nextPieceId: null,
    level: levelAfter,
    lines: prevSearchState.lines + possibility.numLinesCleared,
    framesAlreadyElapsed: 0,
    dasCharge: possibility.dasChargeAfter, // DAS is preserved between pieces
    dasButtonHeld: DasButtonHeld.NONE, // Direction can change during ARE between pieces
    reactionTime: prevSearchState.reactionTime,
    existingXOffset: 0,
    existingYOffset: 0,
    existingRotation: 0,
    canFirstFrameShift: false,
  };
}

/**
 * Generates a list of possible moves, given a board and a piece. It achieves this by
 * placing it in each possible rotation and each possible starting column, and then
 * dropping it into the stack and letting the result play out.
 */
export function getPossibleMoves(
  startingBoard: Board,
  currentPieceId: PieceId,
  level: number,
  existingXOffset: number,
  existingYOffset: number,
  framesAlreadyElapsed: number,
  inputFrameTimeline: string,
  existingRotation: number,
  canFirstFrameShift: boolean,
  dasCharge: number,
  dasButtonHeld: DasButtonHeld,
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
  const doubleGravity = IsGravityDoubled(level);
  const rotationsList = PIECE_LOOKUP[currentPieceId][0] as Array<PieceArray>;

  const simParams: SimParams = {
    board: startingBoard,
    initialX,
    initialY,
    framesAlreadyElapsed,
    gravity,
    doubleGravity,
    inputFrameTimeline,
    rotationsList,
    pieceId: currentPieceId,
    existingRotation,
    canFirstFrameShift,
    dasCharge,
  };
  if (shouldLog)
    logBoard(
      getBoardAndLinesClearedAfterPlacement(
        startingBoard,
        rotationsList[existingRotation],
        initialX,
        initialY
      )[0]
    );

  const legalPlacementSimStates: Array<LegalPlacementSimState> = [];

  // Explore for standard placements (those with more shifts than rotations)
  // Placements are added to legalPlacementSimStates as a side effect
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    repeatedlyShiftPiece(
      -1,
      rotationIndex,
      simParams,
      /* dasWillReset= */ dasCharge == 0 || dasButtonHeld != DasButtonHeld.LEFT,
      legalPlacementSimStates
    );
  }
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    repeatedlyShiftPiece(
      1,
      rotationIndex,
      simParams,
      /* dasWillReset= */ dasCharge == 0 ||
        dasButtonHeld != DasButtonHeld.RIGHT,
      legalPlacementSimStates
    );
  }

  // Loop over the range and validate the moves with more rotations than shifts
  const numRotationsForPiece = rotationsList.length;
  for (
    let rotationIndex = 0;
    rotationIndex < numRotationsForPiece;
    rotationIndex++
  ) {
    const rotationDifference = _modulus(
      rotationIndex - existingRotation,
      numRotationsForPiece
    );
    const numRotationInputs = rotationDifference === 3 ? 1 : rotationDifference;

    const rangeStart = numRotationInputs == 2 ? -1 : 0;
    const rangeEnd = numRotationInputs == 2 ? 1 : 0;

    for (let xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
      const dasWillReset =
        dasCharge == 0 ||
        (xOffset > 0 && dasButtonHeld != DasButtonHeld.RIGHT) ||
        (xOffset < 0 && dasButtonHeld != DasButtonHeld.LEFT);
      // console.log(
      //   "Testing rot",
      //   rotationIndex,
      //   "xoffset",
      //   xOffset,
      //   "daswillReset",
      //   dasWillReset
      // );
      // Check if the placement is legal
      // (if it is, it will be added to the set of legal sim states as a side effect)
      placementIsLegal(
        rotationIndex,
        xOffset,
        simParams,
        dasWillReset,
        /* shouldLog= */ false,
        legalPlacementSimStates
      );
    }
  }

  // console.log("legalplcaementsimstates");
  // console.log(legalPlacementSimStates);

  const [
    basicPossibilities,
    lockHeightLookup,
    potentialTuckSpinStates,
  ] = exploreLegalPlacementsUntilLock(legalPlacementSimStates, simParams);

  if (!CAN_TUCK) {
    return basicPossibilities;
  }

  const tuckSpinPossibilites = searchForTucksOrSpins(
    potentialTuckSpinStates,
    simParams,
    lockHeightLookup
  );
  // console.log("tuckspins");
  // console.log(tuckSpinPossibilites);
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
  legalPlacementSimStates: Array<LegalPlacementSimState>,
  simParams: SimParams
): [Array<Possibility>, Map<string, number>, Array<DFSState>] {
  const lockPossibilities = [];
  const lockHeightLookup: Map<string, number> = new Map();
  const potentialTuckSpinStates: Array<DFSState> = [];

  for (const simState of legalPlacementSimStates) {
    const currentRotationPiece =
      simParams.rotationsList[simState.rotationIndex];
    const rotIndex = _modulus(
      simState.rotationIndex - simParams.existingRotation,
      4
    );

    let startedLookingForTuckSpins = false;
    let highestRegisteredY = -1; // Tracks the Y values already registered to avoid duplicates

    infiniteloop: while (true) {
      const registerPossibility = () => {
        // Piece would lock in!
        // Do some housekeeping and then generate the possibility
        lockHeightLookup.set(
          simState.rotationIndex + "," + simState.x,
          simState.y
        );
        lockPossibilities.push(
          getPossibilityFromSimState(simState, simParams, /* inputCost= */ 0)
        );
      };

      // If it collides immediately, quit
      if (
        simState.hasAlreadyLocked ||
        pieceCollision(
          simParams.board,
          simState.x,
          simState.y,
          currentRotationPiece
        )
      ) {
        registerPossibility();
        break infiniteloop;
      }

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
        });
        highestRegisteredY = simState.y;
      }

      // This kind of acts like an "else", since the paths where an input happened this frame branch off into the dfs file
      simState.inputSequence += ".";

      if (isGravityFrame) {
        for (
          let repeat = 0;
          repeat < (simParams.doubleGravity ? 2 : 1);
          repeat++
        ) {
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
              getPossibilityFromSimState(
                simState,
                simParams,
                /* inputCost= */ 0
              )
            );
            break infiniteloop;
          }
          simState.y++;
        }
      }

      simState.frameIndex += 1;
      simState.arrFrameIndex += 1;
    }
  }

  return [lockPossibilities, lockHeightLookup, potentialTuckSpinStates];
}

export function getPossibilityFromSimState(
  simState: SimState,
  simParams: SimParams,
  inputCost: number
): Possibility {
  let inputSequence = simState.inputSequence;

  // Make a new board with that piece locked in
  const [boardAfter, numLinesCleared] = getBoardAndLinesClearedAfterPlacement(
    simParams.board,
    simParams.rotationsList[simState.rotationIndex],
    simState.x,
    simState.y
  );
  const numEntryDelayFrames = calculateEntryDelayFrames(simState, simParams);

  // Add pre-lineclear ARE frames to the input sequence
  for (let i = 0; i < numEntryDelayFrames - 5; i++) {
    inputSequence += "*";
  }

  // Add line clear frames to the input sequence
  if (numLinesCleared > 0) {
    for (let i = 0; i < 17; i++) {
      inputSequence += "^";
    }
  }

  // Add post-lineclear ARE frames to the input sequence
  for (let i = 0; i < 5; i++) {
    inputSequence += "*";
  }

  // Add the possibility to the list
  const numOrientations = simParams.rotationsList.length;
  const lockPositionEncoded =
    simState.rotationIndex + "|" + simState.x + "|" + simState.y;
  return {
    placement: [
      (simState.rotationIndex - simParams.existingRotation + numOrientations) %
        numOrientations,
      simState.x - simParams.initialX,
      simState.y - simParams.initialY,
    ],
    inputSequence,
    numLinesCleared,
    boardAfter,
    inputCost,
    lockPositionEncoded,
    dasChargeAfter: simState.dasCharge,
  };
}

/** Calculate the ARE as a function of the "lock height" (the height of the highest cell in the piece)  */
function calculateEntryDelayFrames(
  simState: SimState,
  simParams: SimParams
): number {
  const startingY = simParams.pieceId === "I" ? -2 : -1;
  const yOffset = simState.y - startingY;
  const lockHeight = NUM_ROW - yOffset;
  return Math.min(18, 10 + Math.floor((lockHeight + 1) / 4) * 2);
}

/**
 * Helper function that shifts a hypothetical piece as many times as it can in
 * each direction, before it hits the stack or the edge of the screen.
 */
function repeatedlyShiftPiece(
  shiftIncrement: number,
  goalRotationIndex: number,
  simParams: SimParams,
  dasWillReset: boolean,
  legalPlacementSimStates: Array<LegalPlacementSimState>
) {
  // console.log("repeatedly shift piece", shiftIncrement, dasWillReset);
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
    dasCharge: simParams.dasCharge,
    inputSequence: "",
  };
  // Tracks if the next shift will be immediate but also reset DAS
  let pendingDasReset = dasWillReset;

  // Check for immediate collisions
  if (
    pieceCollision(
      board,
      simState.x,
      simState.y,
      rotationsList[simState.rotationIndex]
    )
  ) {
    return;
  }

  while (true) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does
    const isTapInputFrame = shouldPerformInputsThisFrame(
      inputFrameTimeline,
      simState.arrFrameIndex
    );
    const rotIncrement = getRotationIncrement(
      goalRotationIndex,
      simState.rotationIndex
    );

    const isShiftFrame = IS_DAS
      ? pendingDasReset || simState.dasCharge >= 15
      : isTapInputFrame;
    const isRotationFrame = isTapInputFrame && rotIncrement !== 0;
    const isGravityFrame = simState.frameIndex % gravity === gravity - 1; // Returns true every Nth frame, where N = gravity

    // (These are stored as local variables so that all the checks in the loop finish before action is taken on their values.)
    let addNewPlacement = false;
    let lockAfterThisFrame = false;

    // console.log(simState.arrFrameIndex, simState.dasCharge, isShiftFrame);
    if (isShiftFrame) {
      // Try the shift input, then the rotation input
      const shiftSucceeded = performSimulationShift(
        shiftIncrement,
        simState,
        board,
        rotationsList[simState.rotationIndex]
      );
      if (!shiftSucceeded) {
        return;
      }
      if (IS_DAS) {
        simState.dasCharge = pendingDasReset ? 0 : 10; // Update DAS charge after successful shift
        pendingDasReset = false;
      }
    } else {
      simState.dasCharge++; // Not on a shift frame, charge up DAS
    }

    if (isRotationFrame) {
      const rotationSucceeded = performSimulationRotation(
        rotIncrement,
        simState,
        board,
        rotationsList
      );
      if (!rotationSucceeded) {
        return;
      }
    }

    simState.inputSequence += getInputThisFrame(
      isShiftFrame,
      isRotationFrame,
      shiftIncrement,
      rotIncrement
    );

    // If we just shifted or rotated and are in the intended rotation, then this is a legal placement
    if (
      legalPlacementSimStates !== null &&
      (isShiftFrame || isRotationFrame) &&
      simState.rotationIndex === goalRotationIndex
    ) {
      addNewPlacement = true;
    }

    if (isGravityFrame) {
      for (
        let repeat = 0;
        repeat < (simParams.doubleGravity ? 2 : 1);
        repeat++
      ) {
        if (
          pieceCollision(
            board,
            simState.x,
            simState.y + 1,
            rotationsList[simState.rotationIndex]
          )
        ) {
          // console.log("DEBUG GRAVITY");
          lockAfterThisFrame = true;
          break;
        } else {
          simState.y++;
        }
      }
    }

    debugLog(
      simState,
      simParams,
      simState.frameIndex +
        " wasinput? " +
        isShiftFrame +
        " isGrav? " +
        isGravityFrame
    );

    simState.frameIndex += 1;
    simState.arrFrameIndex += 1;

    // If we previously marked that we reached a legal placement this frame, save this as a legal placement.
    // We do this at the very end such that the whole frame is done processing, and simState is ready for the next frame (e.g. looking for tucks)
    if (addNewPlacement) {
      legalPlacementSimStates.push({
        ...simState,
        hasAlreadyLocked: lockAfterThisFrame,
      });
    }

    if (lockAfterThisFrame) {
      return; // Piece would lock in, no further search needed
    }
  }
}

function getInputThisFrame(
  isShiftFrame: boolean,
  isRotationFrame: boolean,
  shiftIncrement: number,
  rotIncrement: number
) {
  if (IS_DAS) {
    if (!isShiftFrame && !isRotationFrame) {
      // Keep holding DAS
      return shiftIncrement === 1 ? "r" : "l";
    }
    if (isShiftFrame && !isRotationFrame) {
      // DAS shift
      return shiftIncrement === 1 ? "R" : "L";
    }
    if (isRotationFrame && !isShiftFrame) {
      // Rotate while holding DAS
      return shiftIncrement === 1
        ? rotIncrement === 1
          ? "i"
          : "g" // Hold DAS right + rotate
        : rotIncrement === 1
        ? "e"
        : "f"; // Hold DAS left + rotate
    }
    if (isRotationFrame && isShiftFrame) {
      // Rotate while holding DAS
      return shiftIncrement === 1
        ? rotIncrement === 1
          ? "I"
          : "G" // Hold DAS right + rotate
        : rotIncrement === 1
        ? "E"
        : "F"; // Hold DAS left + rotate
    }
    throw new Error("Failed to calculate input this frame");
  } else {
    if (isShiftFrame) {
      if (shiftIncrement === -1) {
        // Do a left shift, possibly with a rotation
        if (isRotationFrame) {
          return rotIncrement === 1 ? "E" : "F";
        }
        return "L";
      } else {
        // Do a right shift, possibly with a rotation
        if (isRotationFrame) {
          return rotIncrement === 1 ? "I" : "G";
        }
        return "R";
      }
    } else if (isRotationFrame) {
      // Do a rotation
      if (rotIncrement === 1) {
        return "B";
      } else {
        return "A";
      }
    } else {
      return ".";
    }
  }
}

export function canDoPlacement(
  board: Board,
  level: number,
  pieceId: string,
  rotationIndex: number,
  xOffset: number,
  inputFrameTimeline: string,
  dasCharge: number = 16
) {
  if (!inputFrameTimeline) {
    throw new Error("Unknown input timeline when checking placement");
  }
  const gravity = GetGravity(level); // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const doubleGravity = IsGravityDoubled(level);
  const rotationsList = PIECE_LOOKUP[pieceId][0];
  const simParams: SimParams = {
    board,
    initialX: 3,
    initialY: pieceId === "I" ? -2 : -1,
    framesAlreadyElapsed: 0,
    gravity,
    doubleGravity,
    rotationsList,
    pieceId: pieceId as PieceId,
    existingRotation: 0,
    inputFrameTimeline,
    canFirstFrameShift: false, // This function refers to doing a placement from the start, not starting from an adjustment or anything
    dasCharge,
  };
  return placementIsLegal(
    rotationIndex,
    xOffset,
    simParams,
    /* dasWillReset */ false
  );
}

export function placementIsLegal(
  goalRotationIndex: number,
  goalOffsetX: number,
  simParams: SimParams,
  dasWillReset: boolean,
  shouldLog: boolean = false,
  legalPlacementSimStates: Array<LegalPlacementSimState> = null
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
    inputSequence: "",
    dasCharge: simParams.dasCharge,
  };
  // Tracks if the next shift will be immediate but also reset DAS
  let pendingDasReset = dasWillReset;

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

    const tapShouldInputThisFrame = shouldPerformInputsThisFrame(
      inputFrameTimeline,
      simState.arrFrameIndex
    );

    const isShiftFrame = IS_DAS
      ? simState.dasCharge >= 15
      : tapShouldInputThisFrame;
    const isRotationFrame = tapShouldInputThisFrame;

    const isGravityFrame = simState.frameIndex % gravity === gravity - 1; // Returns true every Nth frame, where N = gravity

    if (isShiftFrame && simState.x !== initialX + goalOffsetX) {
      const inputSucceeded = performSimulationShift(
        shiftIncrement,
        simState,
        board,
        rotationsList[simState.rotationIndex]
      );
      if (!inputSucceeded) {
        return false;
      }
      simState.dasCharge = pendingDasReset ? 0 : 10; // Update DAS charge after successful shift
      pendingDasReset = false;
    } else {
      simState.dasCharge++; // Not shifting this frame, increase dasCharge
    }

    if (isRotationFrame) {
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
      for (
        let repeat = 0;
        repeat < (simParams.doubleGravity ? 2 : 1);
        repeat++
      ) {
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
  // If it gets to this point, the placement is legal
  if (legalPlacementSimStates !== null) {
    legalPlacementSimStates.push({ ...simState, hasAlreadyLocked: false });
  }
  return true;
}

export function performSimulationShift(
  xIncrement: number,
  simState: SimState,
  board: Board,
  currentRotationPiece: PieceArray
): number {
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
    return 0; // Piece moved 0 cells
  }
  simState.x += xIncrement;
  return xIncrement; // Piece moved x cells
}

function getRotationIncrement(
  goalRotationIndex: number,
  curRotationIndex: number
) {
  // Plan for a rotation if needed
  if (curRotationIndex === goalRotationIndex) {
    return 0; // No rotations were applied
  }
  return _modulus(curRotationIndex - 1, 4) === goalRotationIndex
    ? -1 // Left rotation
    : 1; // Right rotation
}

function performSimulationRotation(
  rotIncrement: number,
  simState: SimState,
  board: Board,
  rotationsList: Array<PieceArray>
): boolean {
  simState.rotationIndex += rotIncrement;
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
    return false; // 0 rotations were applied
  }
  return true;
}

function debugLog(simState: SimState, simParams: SimParams, reason: string) {
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
