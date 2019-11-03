const cvs = document.getElementById("tetris");
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

// draw the board
Canvas.prototype.drawBoard = function() {
  for (let r = 0; r < ROW; r++) {
    for (let c = 0; c < COLUMN; c++) {
      this.drawSquare(c, r, this.board[r][c]);
    }
  }
};
