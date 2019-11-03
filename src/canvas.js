const cvs = document.getElementById("main-canvas");
const ctx = cvs.getContext("2d");

import { ROW, COLUMN, SQUARE_SIZE } from "./constants.js";

export function Canvas(board) {
  this.board = board;
}

// draw a square
Canvas.prototype.drawSquare = function(x, y, color) {
  ctx.fillStyle = color;
  ctx.fillRect(x * SQUARE_SIZE, y * SQUARE_SIZE, SQUARE_SIZE, SQUARE_SIZE);

  ctx.strokeStyle = "BLACK";
  ctx.strokeRect(x * SQUARE_SIZE, y * SQUARE_SIZE, SQUARE_SIZE, SQUARE_SIZE);
};

// draw the next box
Canvas.prototype.drawNextBox = function(nextPiece) {
  console.log("Drawing next piece for ", nextPiece);
  const startX = (COLUMN + 1) * SQUARE_SIZE;
  const startY = 2 * SQUARE_SIZE;
  const width = 6 * SQUARE_SIZE;
  const height = 6 * SQUARE_SIZE;
  const pieceStartX = startX;
  const pieceStartY = startY + SQUARE_SIZE;

  // background
  ctx.fillStyle = "BLACK";
  ctx.fillRect(startX, startY, width, height);

  // draw the piece
  for (let r = 0; r < nextPiece.activeTetromino.length; r++) {
    for (let c = 0; c < nextPiece.activeTetromino.length; c++) {
      // Draw only occupied squares
      if (nextPiece.activeTetromino[r][c]) {
        this.drawSquare(pieceStartX, pieceStartY, nextPiece.color);
      }
    }
  }
};

// draw the board
Canvas.prototype.drawBoard = function() {
  for (let r = 0; r < ROW; r++) {
    for (let c = 0; c < COLUMN; c++) {
      this.drawSquare(c, r, this.board[r][c]);
    }
  }
};
