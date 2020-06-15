const cvs = document.getElementById("main-canvas");
const ctx = cvs.getContext("2d");

import {
  NUM_ROW,
  NUM_COLUMN,
  SQUARE_SIZE,
  VACANT,
  COLOR_PALETTE,
} from "./constants.js";
import { GetLevel } from "./tetris.js";

export function Canvas(board) {
  this.board = board;
}
const borderWidth = SQUARE_SIZE / 7;

/** Runs an animation to clear the lines passed in in an array.
 * Doesn't affect the actual board, those updates come at the end of the animation. */
Canvas.prototype.drawLineClears = function (rowsArray, frameNum) {
  if (frameNum >= 15) {
    // animation already done
    return;
  }
  const rightColToClear = 5 + Math.floor(frameNum / 3);
  const leftColToClear = 9 - rightColToClear;
  for (const rowNum of rowsArray) {
    ctx.fillStyle = "black";
    ctx.fillRect(
      leftColToClear * SQUARE_SIZE,
      rowNum * SQUARE_SIZE,
      SQUARE_SIZE,
      SQUARE_SIZE
    );
    ctx.fillRect(
      rightColToClear * SQUARE_SIZE,
      rowNum * SQUARE_SIZE,
      SQUARE_SIZE,
      SQUARE_SIZE
    );
  }
};

// draw a square
Canvas.prototype.drawSquare = function (x, y, color, border = false) {
  // For I, T, and O
  ctx.fillStyle = color;
  ctx.fillRect(x * SQUARE_SIZE, y * SQUARE_SIZE, SQUARE_SIZE, SQUARE_SIZE);

  if (border && color !== VACANT) {
    ctx.fillStyle = "white";
    ctx.fillRect(
      x * SQUARE_SIZE + borderWidth,
      y * SQUARE_SIZE + borderWidth,
      SQUARE_SIZE - borderWidth * 2,
      SQUARE_SIZE - borderWidth * 2
    );
  }
  // Draw 'shiny' part
  if (color !== VACANT) {
    ctx.fillStyle = "white";
    ctx.fillRect(x * SQUARE_SIZE, y * SQUARE_SIZE, borderWidth, borderWidth);
    ctx.fillRect(
      x * SQUARE_SIZE + borderWidth,
      y * SQUARE_SIZE + borderWidth,
      borderWidth,
      borderWidth
    );
    ctx.fillRect(
      x * SQUARE_SIZE + borderWidth + borderWidth,
      y * SQUARE_SIZE + borderWidth,
      borderWidth,
      borderWidth
    );
    ctx.fillRect(
      x * SQUARE_SIZE + borderWidth,
      y * SQUARE_SIZE + borderWidth + borderWidth,
      borderWidth,
      borderWidth
    );
  }
  // Outline
  ctx.strokeStyle = "BLACK";
  ctx.strokeRect(x * SQUARE_SIZE, y * SQUARE_SIZE, SQUARE_SIZE, SQUARE_SIZE);
  ctx.strokeRect(x * SQUARE_SIZE, y * SQUARE_SIZE, SQUARE_SIZE, SQUARE_SIZE);
};

// draw the next box
Canvas.prototype.drawNextBox = function (nextPiece) {
  // All in units of SQUARE_SIZE
  const startX = NUM_COLUMN + 1;
  const startY = 2;
  const width = 5;
  const height = 4.5;
  const pieceStartX =
    nextPiece.id === "I" || nextPiece.id === "O" ? startX + 0.5 : startX;
  const pieceStartY = nextPiece.id === "I" ? startY - 0.25 : startY + 0.25;
  const color = COLOR_PALETTE[nextPiece.colorId][GetLevel() % 10];
  // background
  ctx.fillStyle = "BLACK";
  ctx.fillRect(
    startX * SQUARE_SIZE,
    startY * SQUARE_SIZE,
    width * SQUARE_SIZE,
    height * SQUARE_SIZE
  );

  // draw the piece

  for (let r = 0; r < nextPiece.activeTetromino.length; r++) {
    for (let c = 0; c < nextPiece.activeTetromino[r].length; c++) {
      // Draw only occupied squares
      if (nextPiece.activeTetromino[r][c]) {
        this.drawSquare(
          pieceStartX + c,
          pieceStartY + r,
          color,
          nextPiece.colorId === 1
        );
      }
    }
  }
};

Canvas.prototype.drawPieceStatusString = function (displayString) {
  const startX = (NUM_COLUMN + 1) * SQUARE_SIZE;
  const startY = SQUARE_SIZE;

  // Clear previous text
  ctx.fillStyle = "WHITE";
  ctx.fillRect(startX, startY - 20, 100, 40);

  // Write "x of x" text
  ctx.font = "13px monospace";
  ctx.fillStyle = "BLACK";
  ctx.fillText(displayString, startX, startY, 100);
};

// draw the board
Canvas.prototype.drawBoard = function () {
  const level = GetLevel();
  for (let r = 0; r < NUM_ROW; r++) {
    for (let c = 0; c < NUM_COLUMN; c++) {
      let square = this.board[r][c];
      if (square !== 0) {
        this.drawSquare(c, r, COLOR_PALETTE[square][level % 10], square === 1);
      } else {
        this.drawSquare(c, r, VACANT, square === 1);
      }
    }
  }
};
