import { PIECE_LOOKUP } from "../../docs/tetrominoes";
import {
  _modulus,
  _validateIntParam,
  getBoardAndLinesClearedAfterPlacement,
  pieceCollision
} from "./board_helper";
import { searchForTucksOrSpins } from "./dfs";
import { CAN_TUCK, IS_DAS } from "./params";
import { AdjustmentState, Board, DasButtonHeld, DFSState, getAdjustmentState, INITIAL_PLACEMENT, LegalPlacementSimState, NonNullPieceId, PieceArray, PieceId, Possibility, SearchState, SimParams, SimState } from "./types";
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
    dasCharge: possibility.dasChargeAfter || 0, // DAS is preserved between pieces
    adjustmentState: INITIAL_PLACEMENT,
    reactionTime: prevSearchState.reactionTime,
    existingXOffset: 0,
    existingYOffset: 0,
    existingRotation: 0,
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
  adjustmentState: AdjustmentState,
  dasCharge: number,
  reactionTime: number = -1
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
    adjustmentState,
    dasCharge,
  };

  const legalPlacementSimStates: Array<LegalPlacementSimState> = [];

  // If the DAS charge when a piece spawns is <= 5, it's better to not hold DAS during ARE
  // and get the first frame shift. Otherwise, it's better to use the existing charge.
  const dasWillResetLeft = dasCharge <= 5 || adjustmentState.isAdjustment && adjustmentState.dasButtonHeld != DasButtonHeld.LEFT
  const dasWillResetRight = dasCharge <= 5 || adjustmentState.isAdjustment && adjustmentState.dasButtonHeld != DasButtonHeld.RIGHT

  // Loop over the range and validate the moves with more rotations than shifts
  const numRotationsForPiece = rotationsList.length;
  for (
    let rotationIndex = 0;
    rotationIndex < numRotationsForPiece;
    rotationIndex++
  ) {
    const rangeStart = (-2) - initialX // -2 is the lowest X position a piece can occupy (-5 + the offset 3)
    const rangeEnd = 7 - initialX // 7 is the max X a position can occupy (4 + the offset 3)

    for (let xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
      const dasWillReset =
        (xOffset > 0 && dasWillResetRight) ||
        (xOffset < 0 && dasWillResetLeft);
      // Check if the placement is legal
      // (if it is, it will be added to the set of legal sim states as a side effect)
      tryPlacement(
        rotationIndex,
        xOffset,
        simParams,
        dasWillReset,
        legalPlacementSimStates,
        reactionTime
      );
    }
  }

  // console.log("legalplcaementsimstates");
  // console.log(legalPlacementSimStates);

  deDupeSortedList(legalPlacementSimStates);

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

  deDupeLockPossibilities(basicPossibilities);

  // console.log("tuckspins");
  // console.log(tuckSpinPossibilites);
  return basicPossibilities.concat(tuckSpinPossibilites);
}

function deDupeLockPossibilities(list: Array<Possibility>) {
  function eq(a: Possibility, b: Possibility) {
    return a.lockPositionEncoded == b.lockPositionEncoded
  }

  for (let i = 0; i + 1 < list.length; i++) {
    if (list[i].lockPositionEncoded == list[i + 1].lockPositionEncoded) {
      // Remove the first element (worse DAS charge)
      list.splice(i, 1)
    }
  }
}

function deDupeSortedList(legalPlacementSimStates: Array<SimState>) {
  function getKey(a: SimState) {
    return `${a.rotationIndex}|${a.x}|}`
  }
  // Fill the map
  let dupeMap = new Map();
  for (const ss of legalPlacementSimStates) {
    const key = getKey(ss);
    if (!dupeMap.has(key)) {
      dupeMap.set(key, [])
    }
    dupeMap.get(key).push(ss)
  }

  // Clear the array
  legalPlacementSimStates.length = 0;

  // Re-add the non-duplicate elements from the map
  for (const key of dupeMap.keys()) {
    const list = dupeMap.get(key);

    // Unconditionally add the first time we got to the position
    legalPlacementSimStates.push(list[0])

    // If we're using DAS, we may wait some frames for DAS to charge more
    if (IS_DAS && list.length >= 2) {
      legalPlacementSimStates.push(list[list.length - 1])
    }
  }
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
  simState: LegalPlacementSimState,
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
    adjTimeSimState: simState.adjTimeSimState
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

const INPUT_LOOKUP = new Map([
  [0, new Map([[0, '.'], [1, 'A'], [-1, 'B']])],
  [1, new Map([[0, 'R'], [1, 'I'], [-1, 'G']])],
  [-1, new Map([[0, 'L'], [1, 'E'], [-1, 'F']])]
]);

// To look up a value using a Map, you use .get():

function getInputThisFrame(
  willShiftThisFrame: boolean,
  willHoldDasButtonThisFrame: boolean,
  willRotateThisFrame: boolean,
  shiftIncrement: number,
  rotIncrement: number
): string {
  let effRotIncrement = willRotateThisFrame ? rotIncrement : 0;
  // DAS holds the button continuously, hypertap only presses the button on the shift frames
  let effShiftIncrement = (willShiftThisFrame || willHoldDasButtonThisFrame) ? shiftIncrement : 0

  let inputThisFrame = INPUT_LOOKUP.get(effShiftIncrement)?.get(effRotIncrement);

  if (!inputThisFrame) {
    throw new Error(`Failed to calculate input this frame: Shift(${effShiftIncrement}), Rot(${effRotIncrement})`);
  }

  // We use a lowercase letter to indicate an input where DAS is being held but there's no shift.
  // This is purely visual for debugging purposes, and the Lua script will turn it back to uppercase for execution.
  if (IS_DAS && shiftIncrement !== 0 && !willShiftThisFrame) {
    return inputThisFrame.toLowerCase();
  }

  return inputThisFrame;
}

// export function canDoFullRange(
//   searchState: SearchState,
//   inputFrameTimeline: string,
//   dasCharge: number
// ) {
//   const pieceId = searchState.currentPieceId as NonNullPieceId
//   const simParams: SimParams = {
//     board: searchState.board,
//     initialX: 3,
//     initialY: (searchState.currentPieceId == "I" ? -2 : -1),
//     pieceId: searchState.currentPieceId,
//     framesAlreadyElapsed: 0,
//     gravity: GetGravity(searchState.level),
//     rotationsList: PIECE_LOOKUP[pieceId][0] as Array<PieceArray>,
//     existingRotation: 0,
//     adjustmentState: INITIAL_PLACEMENT,
//     doubleGravity: IsGravityDoubled(searchState.level),
//     inputFrameTimeline,
//     dasCharge
//   }

//   // The hardest placements in each direction for each piece
//   const LEFT_PLACEMENTS = {
//     I: [1, -5],
//     O: [0, -4],
//     L: [3, -5],
//     J: [3, -5],
//     T: [3, -5],
//     S: [1, -5],
//     Z: [1, -5]
//   }
//   const RIGHT_PLACEMENTS = {
//     I: [1, 4],
//     O: [0, 4],
//     L: [1, 4],
//     J: [1, 4],
//     T: [1, 4],
//     S: [0, 3],
//     Z: [0, 3]
//   }

//   let [goalRotationIndex, goalOffsetX] = LEFT_PLACEMENTS[pieceId]
//   const canLeft = tryPlacement()
// }

/**
 * @VisibleForTesting
 * Tests if an individual placement is possible without storing any intermediate results.
 * Used to check placements with more rotations than shifts, since those are a blind spot of the repeatedlyShiftPiece() method.
 */
export function tryPlacement(
  goalRotationIndex: number,
  goalOffsetX: number,
  simParams: SimParams,
  dasWillReset: boolean,
  legalPlacementSimStates: Array<LegalPlacementSimState>,
  reactionTime: number
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
    adjustmentState,
  } = simParams;

  const holdDASduringARE = !adjustmentState.isAdjustment && !dasWillReset && goalOffsetX != 0

  // Get initial sim state
  const simState: SimState = {
    x: initialX,
    y: initialY,
    frameIndex: framesAlreadyElapsed,
    arrFrameIndex: adjustmentState.isArrContinued ? framesAlreadyElapsed : 0,
    rotationIndex: existingRotation,
    // Add a marker to indicate that the agent should hold DAS during ARE
    inputSequence: holdDASduringARE ? "_" : "",
    dasCharge: simParams.dasCharge,
  };
  const shiftIncrement = goalOffsetX == 0 ? 0 : goalOffsetX > 0 ? 1 : -1;
  const rotIncrement = getRotationIncrement(
    goalRotationIndex,
    simState.rotationIndex
  );

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

  // Tracks if the next shift will be immediate but also reset DAS
  let pendingDasReset = dasWillReset;
  let dasButtonHeld = DasButtonHeld.NONE
  let adjTimeSimState;
  let foundPlacement = false;
  let doneChargingDas = false;
  let arrFrameVoluntarilyMissed = false;
  while (true) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does

    const tapShouldInputThisFrame = shouldPerformInputsThisFrame(
      inputFrameTimeline,
      simState.arrFrameIndex
    );
    const canShiftThisFrame = IS_DAS
      ? (simState.dasCharge >= 15 || pendingDasReset)
      : tapShouldInputThisFrame;
    const willShiftThisFrame =
      canShiftThisFrame && simState.x != initialX + goalOffsetX;
    const willHoldDasButtonThisFrame = IS_DAS &&
      (
        simState.x != initialX + goalOffsetX || // Has shifts remaining
        (dasButtonHeld != DasButtonHeld.NONE && simState.dasCharge < 15) // Is already holding an arrow and can keep holding it for bonus DAS charge
      )
    const willRotateThisFrame =
      tapShouldInputThisFrame && simState.rotationIndex !== goalRotationIndex;
    const isGravityFrame = simState.frameIndex % gravity === gravity - 1; // Returns true every Nth frame, where N = gravity

    // (These are stored as local variables so that all the checks in the loop finish before action is taken on their values.)
    let addNewPlacement = false;
    let lockAfterThisFrame = false;

    if (willShiftThisFrame && simState.x !== initialX + goalOffsetX) {
      const inputSucceeded = performSimulationShift(
        shiftIncrement,
        simState,
        board,
        rotationsList[simState.rotationIndex]
      );
      if (!inputSucceeded) {
        return;
      }
      if (IS_DAS) {
        simState.dasCharge = pendingDasReset ? 0 : 10; // Update DAS charge after successful shift
        pendingDasReset = false;
      }
    } else if (willHoldDasButtonThisFrame) {
      simState.dasCharge++; // Not shifting this frame, increase dasCharge
    }

    if (willRotateThisFrame) {
      const inputSucceeded = performSimulationRotation(
        rotIncrement,
        simState,
        board,
        rotationsList
      );
      if (!inputSucceeded) {
        return;
      }
    }

    const thisFrameStr = getInputThisFrame(
      willShiftThisFrame,
      willHoldDasButtonThisFrame,
      willRotateThisFrame,
      shiftIncrement,
      rotIncrement
    );
    simState.inputSequence += thisFrameStr

    // Track the DAS button held
    if ("LEF".includes(thisFrameStr.toUpperCase())) {
      dasButtonHeld = DasButtonHeld.LEFT;
    } else if ("RIG".includes(thisFrameStr.toUpperCase())) {
      dasButtonHeld = DasButtonHeld.RIGHT;
    } else {
      dasButtonHeld = DasButtonHeld.NONE;
    }

    // Track the ARR input counts
    const didArrInputThisFrame =
      (IS_DAS && "ABEFIG".includes(thisFrameStr.toUpperCase())) ||
      (!IS_DAS && thisFrameStr !== ".")
    if (tapShouldInputThisFrame && !didArrInputThisFrame) {
      arrFrameVoluntarilyMissed = true;
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
          lockAfterThisFrame = true;
          break; // From just the doublegravity for loop
        } else {
          simState.y++;
        }
      }
    }

    // ------------------------
    // "Pre-advance" the frame such that if we store the state anywhere, it's ready to go for the next frame (e.g. if searching for tucks or adjustments)
    // ------------------------
    simState.frameIndex += 1;
    simState.arrFrameIndex += 1;

    // Take note of the simState at reaction time (so we don't have to try to predict it elsewhere!)
    if (simState.frameIndex == reactionTime && !lockAfterThisFrame) {
      adjTimeSimState = {
        ...simState,
        adjustmentState: getAdjustmentState(!arrFrameVoluntarilyMissed, dasButtonHeld)
      }
    }

    foundPlacement = simState.rotationIndex === goalRotationIndex && simState.x === initialX + goalOffsetX
    if (
      // If we found the intended placement, and ...
      foundPlacement &&
      // ... if this is not an adjustment itself, attempt to wait until adjTimeSimState is calculated, unless the piece locks before that.
      (simParams.adjustmentState.isAdjustment || adjTimeSimState != null || lockAfterThisFrame)
    ) {
      // Then add it!
      legalPlacementSimStates.push({
        ...simState,
        hasAlreadyLocked: lockAfterThisFrame,
        adjTimeSimState
      });
    }

    // Quit if the piece locked in
    if (lockAfterThisFrame) {
      break;
    }
    // Quit if we have no reason to keep looking
    doneChargingDas = !IS_DAS || simState.dasCharge >= 15
    if (doneChargingDas && foundPlacement && adjTimeSimState !== undefined) {
      break;
    }
  }
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
    return 0; // No rotations needed
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
