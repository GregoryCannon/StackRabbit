import { NUM_ROW, NUM_COLUMN, VACANT } from "./constants.js";
import { debugPrintBoard } from "./utils.js";

// The Object Piece
export function Piece(pieceData, board, canvas, onGameOver) {
  this.tetromino = pieceData[0];
  this.color = pieceData[1];
  this.id = pieceData[2];
  this.board = board;
  this.canvas = canvas;
  this.onGameOver = onGameOver;

  this.tetrominoN = 0; // Start from the first rotation
  this.activeTetromino = this.tetromino[this.tetrominoN];

  this.x = 3;
  this.y = -2;
}

Piece.prototype.equals = function (otherPiece) {
  return this.id === otherPiece.id;
};

// fill function
Piece.prototype.fill = function (color) {
  for (let r = 0; r < this.activeTetromino.length; r++) {
    for (let c = 0; c < this.activeTetromino.length; c++) {
      // Draw only occupied squares
      if (this.activeTetromino[r][c]) {
        this.canvas.drawSquare(this.x + c, this.y + r, color);
      }
    }
  }
};

// draw a piece to the board
Piece.prototype.draw = function () {
  this.fill(this.color);
};

// undraw a piece
Piece.prototype.unDraw = function () {
  this.fill(VACANT);
};

Piece.prototype.shouldLock = function () {
  return this.collision(0, 1, this.activeTetromino);
};

// move Down the piece
Piece.prototype.moveDown = function () {
  this.unDraw();
  this.y++;
  this.draw();
};

/**
 * Attempt to move the piece right.
 * @returns true if the piece moved */
Piece.prototype.moveRight = function () {
  if (this.collision(1, 0, this.activeTetromino)) {
    return false;
  } else {
    // No collision, move the piece
    this.unDraw();
    this.x++;
    this.draw();
    return true;
  }
};

/**
 * Attempt to move the piece left.
 * @returns true if the piece moved */
Piece.prototype.moveLeft = function () {
  if (this.collision(-1, 0, this.activeTetromino)) {
    return false;
  } else {
    // No collision, move the piece
    this.unDraw();
    this.x--;
    this.draw();
    return true;
  }
};

// rotate the piece
Piece.prototype.rotate = function (directionInversed) {
  const offset = directionInversed ? -1 : 1;
  const nextIndex =
    (this.tetrominoN + offset + this.tetromino.length) % this.tetromino.length;
  const nextPattern = this.tetromino[nextIndex];

  if (!this.collision(0, 0, nextPattern)) {
    this.unDraw();
    this.tetrominoN = nextIndex;
    this.activeTetromino = this.tetromino[this.tetrominoN];
    this.draw();
  }
};

// Lock the piece in place
Piece.prototype.lock = function () {
  for (let r = 0; r < this.activeTetromino.length; r++) {
    for (let c = 0; c < this.activeTetromino.length; c++) {
      // we skip the vacant squares
      if (!this.activeTetromino[r][c]) {
        continue;
      }
      // pieces to lock on top = game over
      if (this.y + r < 0) {
        this.onGameOver();
        break;
      }
      // we lock the piece
      this.board[this.y + r][this.x + c] = this.color;
    }
  }

  // update the board
  this.canvas.drawBoard();
};

// Collision fucntion
Piece.prototype.collision = function (x, y, piece) {
  for (let r = 0; r < piece.length; r++) {
    for (let c = 0; c < piece.length; c++) {
      // if the square is empty, we skip it
      if (!piece[r][c]) {
        continue;
      }
      // coordinates of the piece after movement
      let newX = this.x + c + x;
      let newY = this.y + r + y;

      // conditions
      if (newX < 0 || newX >= NUM_COLUMN || newY >= NUM_ROW) {
        return true;
      }
      // skip newY < 0; board[-1] will crush our game
      if (newY < 0) {
        continue;
      }
      // check if there is a locked piece alrady in place
      if (this.board[newY][newX] != VACANT) {
        return true;
      }
    }
  }
  return false;
};
