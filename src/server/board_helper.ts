import { Board, PieceArray } from "./types";

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
    surfaceArray.slice(2, 8).reduce((x, y) => x + y) / 6;

  if (aiMode === AiMode.KILLSCREEN) {
    // On killscreen, we mainly access the left with 4-taps. So we need either
    // 1) a left built as high as the 4 tap height
    // 2) access to the left with a 4 tap
    if (
      col1Height >= col2Height &&
      col1Height > aiParams.MAX_4_TAP_LOOKUP[level] &&
      col1Height >= avgHeightOfMiddle - 1
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
    col1Height > aiParams.MAX_5_TAP_LOOKUP[level]
    // col1Height >= avgHeightOfMiddle - 1
  ) {
    return 1;
  }
  // If we can 5 tap, then we're fine
  if (canDoPlacement(board, level, "T", 3, -5, aiParams.INPUT_FRAME_TIMELINE)) {
    return 1;
  }
  // If an O can reach the left, then we're mostly fine
  if (
    col1Height === col2Height &&
    canDoPlacement(board, level, "O", 0, -4, aiParams.INPUT_FRAME_TIMELINE)
  ) {
    return 0.9;
  }
  // If an L can reach the left, then we're fine
  if (
    col1Height === col2Height - 1 &&
    col2Height == col3Height &&
    canDoPlacement(board, level, "L", 0, -4, aiParams.INPUT_FRAME_TIMELINE)
  ) {
    return 0.9;
  }
  return canDoPlacement(board, level, "T", 3, -5, aiParams.INPUT_FRAME_TIMELINE)
    ? 1
    : 0;
  return 0;
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
  if (canDoPlacement(board, level, "I", 1, 4, aiParams.INPUT_FRAME_TIMELINE)) {
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
