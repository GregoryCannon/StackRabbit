import { getRowsNeedingToBurn } from "./evaluator";
import { canDoPlacement, placementIsLegal } from "./move_search";
import { getSurfaceArrayAndHoles, parseBoard } from "./utils";

const utils = require("./utils");
const NUM_COLUMN = utils.NUM_COLUMN;
const NUM_ROW = utils.NUM_ROW;
const SquareState = utils.SquareState;

// Collision function
export function pieceCollision(
  board: Board,
  x: number,
  y: number,
  piece: PieceArray
) {
  if (!piece || piece.length < 1) {
    throw new Error("Unknown piece passed into collision function");
  }
  for (let r = 0; r < piece.length; r++) {
    for (let c = 0; c < piece[r].length; c++) {
      // If the square is empty, we skip it
      if (!piece[r][c]) {
        continue;
      }
      // Coordinates of the piece after movement
      let newX = c + x;
      let newY = r + y;

      // If out of bounds on left, right or bottom, say it does collide
      if (newX < 0 || newX >= NUM_COLUMN || newY >= NUM_ROW) {
        return true;
      }
      // If over the top of the board, ignore
      if (newY < 0) {
        continue;
      }
      // Check if it overlaps the board
      if (board[newY][newX] != 0) {
        return true;
      }
    }
  }
  return false;
}

/** Clear all filled lines on a board
 * @returns the number of lines cleared
 */
function clearLines(board: Board) {
  let numLinesCleared = 0;
  for (let r = 0; r < NUM_ROW; r++) {
    let isRowFull = true;
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (board[r][c] == SquareState.EMPTY) {
        isRowFull = false;
        break;
      }
    }
    if (isRowFull) {
      board.splice(r, 1);
      board.unshift([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
      numLinesCleared++;
    }
  }
  return numLinesCleared;
}

export function getBoardAndLinesClearedAfterPlacement(
  board: Board,
  currentRotationPiece: PieceArray,
  x: number,
  y: number
): [Board, number] {
  let tempBoard = utils.cloneBoard(board);
  for (let r = 0; r < currentRotationPiece.length; r++) {
    for (let c = 0; c < currentRotationPiece[r].length; c++) {
      // If the square is empty, we skip it
      if (!currentRotationPiece[r][c]) {
        continue;
      }
      // Coordinates of the piece after movement
      let newX = c + x;
      let newY = r + y;

      // If out of bounds, ignore
      if (newX < 0 || newY < 0 || newX >= NUM_COLUMN || newY >= NUM_ROW) {
        continue;
      }
      // Add to board
      tempBoard[newY][newX] = SquareState.FULL;
    }
  }
  const numLinesCleared = clearLines(tempBoard);

  // utils.logBoard(tempBoard);
  return [tempBoard, numLinesCleared];
}

export function _validateIntParam(value: number, min: number, max: number) {
  if (isNaN(value)) {
    throw new Error("Expected a number, but got NaN:" + value);
  }
  if (!(value >= min && value <= max)) {
    throw new Error(
      `Number parameter ${value} was out of range: ${min} to ${max}`
    );
  }
}

/** Generates a string representing the inputs to perform, frame-by-frame. It uses the following encoding:
 * . = do nothing
 * A = press A
 * B = press B
 * L = press L
 * R = press R
 * E = press L + B (2nd letter of 'left')
 * F = press L + A (3rd letter of 'left')
 * I = press R + B (2nd letter of 'right')
 * G = press R + A (3rd letter of 'right')
 *
 * e.g. L piece 5-tap left, 12Hz tapping: F....L....L....L....L
 */
export function generateInputSequence(
  rotationIndex,
  xOffset,
  inputFrameTimeline,
  framesAlreadyElapsed = 0
) {
  let inputsLeft = xOffset < 0 && Math.abs(xOffset);
  let inputsRight = xOffset > 0 && xOffset;
  let rotationsLeft = rotationIndex === 3 && 1;
  let rotationsRight = rotationIndex < 3 && rotationIndex;

  if (inputsLeft > 0 && inputsRight > 0) {
    throw new Error("Invalid shift parsing");
  }
  if (rotationsLeft > 0 && rotationsRight > 0) {
    throw new Error("Invalid rotation parsing");
  }

  let inputSequence = "";
  for (
    let i = framesAlreadyElapsed;
    inputsLeft + inputsRight + rotationsLeft + rotationsRight > 0;
    i++
  ) {
    if (utils.shouldPerformInputsThisFrame(inputFrameTimeline, i)) {
      if (inputsLeft > 0) {
        // Do a left shift, possibly with a rotation
        if (rotationsRight > 0) {
          inputSequence += "E";
          rotationsRight--;
        } else if (rotationsLeft > 0) {
          inputSequence += "F";
          rotationsLeft--;
        } else {
          inputSequence += "L";
        }
        inputsLeft--;
      } else if (inputsRight > 0) {
        // Do a right shift, possibly with a rotation
        if (rotationsRight > 0) {
          inputSequence += "I";
          rotationsRight--;
        } else if (rotationsLeft > 0) {
          inputSequence += "G";
          rotationsLeft--;
        } else {
          inputSequence += "R";
        }
        inputsRight--;
      } else {
        // Do a rotation
        if (rotationsLeft > 0) {
          inputSequence += "B";
          rotationsLeft--;
        } else {
          inputSequence += "A";
          rotationsRight--;
        }
      }
    } else {
      inputSequence += ".";
    }
  }
  return inputSequence;
}

/**
 * Checks whether the killscreen left surface will resolve into a clean left after all
 * the doable line clears are performed.
 * @returns an array [ doesResolve, linesClearedInTheProcess ]
 */
function leftSurfaceResolvesCleanly(
  board: Board,
  surfaceArray: Array<number>,
  leftHoleCells: Set<number>,
  allHoleCells: Set<number>,
  minReachableX,
  maxReachableX
): [boolean, Set<number>] {
  if (leftHoleCells.size === 0) {
    throw new Error(
      "Queried whether left surface resolves but there were no holes"
    );
  }
  // Find the rows that can easily clear
  const linesCleared: Set<number> = new Set();
  const highestRowOfLeftSurface = Math.max(
    surfaceArray[0],
    surfaceArray[1],
    surfaceArray[2]
  );
  let lowestRowWithHole = -1;
  for (const x of leftHoleCells) {
    lowestRowWithHole = Math.max(lowestRowWithHole, Math.floor(x / 10));
  }
  for (
    let row = NUM_ROW - highestRowOfLeftSurface;
    row < lowestRowWithHole;
    row++
  ) {
    let canClear = true;
    for (let col = 0; col < NUM_COLUMN; col++) {
      if (board[row][col] == SquareState.FULL) {
        continue;
      }
      if (
        col < minReachableX ||
        col > maxReachableX ||
        allHoleCells.has(row * 10 + col)
      ) {
        // This row can't easily be cleared
        // console.log(`Can't clear row ${row} because of col ${col}`);
        canClear = false;
        break;
      }
    }
    // If it got this far, then it can be cleared
    if (canClear) {
      linesCleared.add(row);
    }
  }
  // console.log("Potential line clears", linesCleared);

  // See if the holes are still holes after the clears
  for (const x of leftHoleCells) {
    const holeRow = Math.floor(x / 10);
    const holeCol = x % 10;

    // Check that all filled rows above the hole are potentially clearable
    for (let row = holeRow - 1; row >= NUM_ROW - surfaceArray[holeCol]; row--) {
      // console.log("Checking row", row);
      if (board[row][holeCol] && !linesCleared.has(row)) {
        // Still a hole, so the left can't resolve
        return [false, null];
      }
    }
  }

  return [true, linesCleared];
}


/** Gets the surface of just the left-most 3 columns. (This is used for killscreen play).
 * The heights are RELATIVE to the average heights in columns 3-10, such that the surface
 * is invariant to line clears.
 *
 * @returns an array [ currentSurface, eventualSurface ], where eventualSurface refers to the
 * surface after all easily-completed lines are cleared.
 */
export function getLeftSurfaces(
  board: Board,
  max4TapHeight,
  holeCells: Set<number>,
  surfaceArray: Array<number>,
  minReachableX,
  maxReachableX
): [string, string] {
  // Get the normal surface
  const maxHeight = Math.min(9, max4TapHeight + 2);
  const currentLeftSurface = getRelativeLeftSurface(board, maxHeight);

  // Check for holes in the left 3 cols
  const leftColsHoles: Set<number> = new Set();
  for (const x of holeCells) {
    const holeCol = x % 10;
    if (holeCol <= 2) {
      // Check if the hole can be reasonably be resolved
      leftColsHoles.add(x);
    }
  }

  if (leftColsHoles.size == 0) {
    return [currentLeftSurface, null];
  }

  const [doesResolve, linesCleared] = leftSurfaceResolvesCleanly(
    board,
    surfaceArray,
    leftColsHoles,
    holeCells,
    minReachableX,
    maxReachableX
  );
  if (doesResolve) {
    // Clear the lines on a shallow copy of the board
    const newBoard = [];
    for (let i = 0; i < linesCleared.size; i++) {
      newBoard.push([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
    }
    for (let i = 0; i < NUM_ROW; i++) {
      if (!linesCleared.has(i)) {
        newBoard.push(board[i]);
      }
    }
    const eventualSurface = getRelativeLeftSurface(newBoard, maxHeight);
    return [currentLeftSurface, eventualSurface];
  } else {
    // Surface doesn't resolve
    return [null, null];
  }
}

export function getRelativeLeftSurface(board: Board, maxHeight): string {
  const getColHeight = (col) =>
    Math.min(maxHeight, getBoardHeightAtColumn(board, col));
  let col1 = getBoardHeightAtColumn(board, 0);
  let col2 = getBoardHeightAtColumn(board, 1);
  let col3 = getBoardHeightAtColumn(board, 2);

  const surfaceLevelAboveFloor = Math.min(col1, col2, col3);
  const avgHeightOfLeft = (col1 + col2) / 2;

  // Adjust for transposed surfaces
  col1 = Math.min(maxHeight, col1 - surfaceLevelAboveFloor);
  col2 = Math.min(maxHeight, col2 - surfaceLevelAboveFloor);
  col3 = Math.min(maxHeight, col3 - surfaceLevelAboveFloor);

  // Get the height of the rest of the columns
  let totalHeightOfMiddle = 0;
  for (let i = 2; i < NUM_COLUMN; i++) {
    totalHeightOfMiddle += getColHeight(i);
  }
  const avgHeightOfMiddle = totalHeightOfMiddle / 8;
  const heightDiff = Math.max(
    -1 * maxHeight,
    avgHeightOfLeft - avgHeightOfMiddle
  );

  return `${col1}${col2}${col3}|${Math.round(heightDiff)}`;
}

/** Gets the height in a particular column. */
export function getBoardHeightAtColumn(board: Board, col: number) {
  let row = 0;
  while (row < NUM_ROW && board[row][col] == 0) {
    row++;
  }
  return 20 - row;
}

/** Searches a column for holes.
 * @param heightFromTop (Optional) limits the search to N rows below the top of the stack in that column
 */
export function hasHoleInColumn(board: Board, col: number, heightFromTop = 20) {
  let row = 0;
  while (row < NUM_ROW && board[row][col] === SquareState.EMPTY) {
    row++;
  }
  for (let i = 0; i < heightFromTop && row + i < NUM_ROW; i++) {
    if (board[row + i][col] === SquareState.EMPTY) {
      return true;
    }
  }
  return false;
}

/** Returns 1 if the left is accesible, and 0 otherwise. */
export function rateLeftAccessibility(
  board: Board,
  surfaceArray: Array<number>,
  level: number,
  aiParams: AiParams,
  aiMode: AiMode
): number {
  const col1Height = surfaceArray[0];
  const col2Height = surfaceArray[1];
  const col3Height = surfaceArray[2];

  const avgHeightOfMiddle =
    surfaceArray.slice(3, 7).reduce((x, y) => x + y) / 4;

  if (aiMode === AiMode.KILLSCREEN) {
    // On killscreen, we mainly access the left with 4-taps. So we need either
    // 1) a left built as high as the 4 tap height
    // 2) access to the left with a 4 tap
    if (
      col1Height >= col2Height &&
      col1Height > aiParams.MAX_4_TAP_LOOKUP[level] &&
      col1Height >= avgHeightOfMiddle
    ) {
      return 1;
    }
    const canDo4TapLeft = canDoPlacement(
      board,
      level,
      col1Height === col2Height ? "O" : "L",
      0,
      -4,
      aiParams.INPUT_FRAME_TIMELINE
    );
    return canDo4TapLeft ? 1 : 0;
  }

  //In normal stacking, we mainly access the left with 5-taps. So we need either
  // 1) a left built as high as the 5 tap height
  // 2) access to the left with a 5 tap
  if (
    col1Height >= col2Height &&
    col1Height > aiParams.MAX_5_TAP_LOOKUP[level] &&
    col1Height >= avgHeightOfMiddle
  ) {
    return 1;
  }
  // If an L can reach the left, then we're fine
  // Observe that other 4-tap cases, like O & J are covered in the previous case
  if (
    col1Height === col2Height - 1 &&
    col2Height == col3Height &&
    canDoPlacement(board, level, "L", 0, -4, aiParams.INPUT_FRAME_TIMELINE)
  ) {
    return 1;
  }
  return canDoPlacement(board, level, "T", 3, -5, aiParams.INPUT_FRAME_TIMELINE)
    ? 1
    : 0;
}

/** Returns true if the tap speed is not sufficient to get a long bar to the right. */
export function rateRightAccessibility(
  board: Board,
  surfaceArray: Array<number>,
  level: number,
  aiParams: AiParams,
  aiMode: AiMode
) {
  const col9Height = getBoardHeightAtColumn(board, NUM_COLUMN - 2);
  const col10Height = getBoardHeightAtColumn(board, NUM_COLUMN - 1);
  const avgHeightOfMiddle =
    surfaceArray.slice(0, 8).reduce((x, y) => x + y) / 8;

  // (killscreen-only) if right is built out, we're good
  if (aiMode === AiMode.KILLSCREEN) {
    if (
      col10Height >= col9Height &&
      col10Height > aiParams.MAX_4_TAP_LOOKUP[level] &&
      col10Height >= avgHeightOfMiddle
    ) {
      return 1;
    }
  }

  // Otherwise we need a 4 tap
  if (canDoPlacement(board, level, "I", 1, 4, aiParams.INPUT_FRAME_TIMELINE)){
    return 1;
  }

  // (killscreen-only) If an S/Z 3-tap makes it, we're somewhat ok
  if (aiMode === AiMode.KILLSCREEN) {
    if (
      col10Height == col9Height + 1 &&
      canDoPlacement(board, level, "Z", 1, 3, aiParams.INPUT_FRAME_TIMELINE)
    ) {
      return 1;
    }
    if (
      col10Height == col9Height - 1 &&
      canDoPlacement(board, level, "S", 1, 3, aiParams.INPUT_FRAME_TIMELINE)
    ) {
      return 1;
    }
  }

  return 0;
}

/** A modulus function that correctly handles negatives. */
export function _modulus(n: number, m: number) {
  return (n + m) % m;
}

/** Helper method for testing. */
export function getTestBoardWithHeight(height: number) {
  const board = [];
  for (let i = 0; i < NUM_ROW; i++) {
    board.push(
      i < NUM_ROW - height
        ? [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
        : [1, 1, 1, 1, 1, 1, 1, 1, 1, 0]
    );
  }
  return board;
}

/** Gets the max height that can be cleared, given a level, ARR, delay, and number of taps desired.
 * If no tap cannot be cleared, it returns -1.
 */
export function calculateTapHeight(level, inputFrameTimeline, numTaps) {
  let height = 0;
  while (
    canDoPlacement(
      getTestBoardWithHeight(height),
      level,
      "I",
      1,
      -1 * numTaps,
      inputFrameTimeline
    )
  ) {
    height++;
  }
  height--;
  return height;
}
