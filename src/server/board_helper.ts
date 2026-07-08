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
