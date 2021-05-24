import { canDoPlacement, placementIsLegal } from "./move_search";

const PIECE_LOOKUP = require("../tetrominoes").PIECE_LOOKUP;
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
  let fullLines = [];
  for (let r = 0; r < NUM_ROW; r++) {
    let isRowFull = true;
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (board[r][c] == SquareState.EMPTY) {
        isRowFull = false;
        break;
      }
    }
    if (isRowFull) {
      fullLines.push(r);
    }
  }
  for (const r of fullLines) {
    // Move down all the rows above it
    for (let y = r; y >= 1; y--) {
      for (let c = 0; c < NUM_COLUMN; c++) {
        board[y][c] = board[y - 1][c];
      }
    }
    // Clear out the very top row (newly shifted into the screen)
    for (let c = 0; c < NUM_COLUMN; c++) {
      board[0][c] = SquareState.EMPTY;
    }
  }
  return fullLines.length;
}

export function getBoardAndLinesClearedAfterPlacement(
  board: Board,
  currentRotationPiece: PieceArray,
  x: number,
  y: number
) {
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
 * e.g. L piece 5-tap left, 12Hz tapping: f....L....L....L....L
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

/** Gets the surface of just the left-most 3 columns, if they don't have any holes.
 * (This is used for killscreen play)
 */
export function getLeftSurface(board: Board, maxHeight) {
  if (
    hasHoleInColumn(board, 0) ||
    hasHoleInColumn(board, 1) ||
    hasHoleInColumn(board, 2)
  ) {
    return null;
  }

  const getColHeight = (col) =>
    Math.min(Math.min(9, maxHeight), getBoardHeightAtColumn(board, col));

  return "" + getColHeight(0) + getColHeight(1) + getColHeight(2);
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

/** Returns true if the board needs a 5 tap to resolve, and the tap speed is not sufficient to get a piece there. */
export function boardHasInaccessibileLeft(
  board: Board,
  surfaceArray: Array<number>,
  level: number,
  aiParams: AiParams,
  aiMode: AiMode
) {
  const col1Height = surfaceArray[0];
  const col2Height = surfaceArray[1];
  const col3Height = surfaceArray[2];

  if (aiMode === AiMode.KILLSCREEN) {
    // On killscreen, we mainly access the left with 4-taps. So we need either
    // 1) a left built as high as the 4 tap height
    // 2) access to the left with a 4 tap
    if (
      col1Height >= col2Height &&
      col1Height > aiParams.MAX_4_TAP_LOOKUP[level]
    ) {
      return false;
    }
    const canDo4TapLeft = canDoPlacement(
      board,
      level,
      "L",
      0,
      -4,
      aiParams.INPUT_FRAME_TIMELINE
    );
    return !canDo4TapLeft;
  }

  //In normal stacking, we mainly access the left with 5-taps. So we need either
  // 1) a left built as high as the 5 tap height
  // 2) access to the left with a 5 tap
  if (
    col1Height >= col2Height &&
    col1Height > aiParams.MAX_5_TAP_LOOKUP[level]
  ) {
    return false;
  }
  // If an L can reach the left, then we're fine
  if (
    col1Height === col2Height - 1 &&
    col2Height == col3Height &&
    canDoPlacement(board, level, "L", 0, -4, aiParams.INPUT_FRAME_TIMELINE)
  ) {
    return false;
  }
  return !canDoPlacement(
    board,
    level,
    "I",
    1,
    -5,
    aiParams.INPUT_FRAME_TIMELINE
  );
}

/** Returns true if the tap speed is not sufficient to get a long bar to the right. */
export function boardHasInaccessibileRight(
  board: Board,
  level: number,
  aiParams: AiParams,
  aiMode: AiMode
) {
  const col9Height = getBoardHeightAtColumn(board, NUM_COLUMN - 2);
  const col10Height = getBoardHeightAtColumn(board, NUM_COLUMN - 1);
  // (killscreen-only) if right is built out, we're good
  if (
    aiMode === AiMode.KILLSCREEN &&
    col10Height >= col9Height &&
    col10Height > aiParams.MAX_4_TAP_LOOKUP[level]
  ) {
    return false;
  }

  // Otherwise we need a 4 tap
  return !canDoPlacement(
    board,
    level,
    "I",
    1,
    4,
    aiParams.INPUT_FRAME_TIMELINE
  );
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
