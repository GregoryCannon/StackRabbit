import { NUM_ROW, NUM_COLUMN, VACANT, COLOR_PALETTE } from "./constants.js";
import { GetLevel, TriggerGameOver } from "./tetris";

// The Object Piece
export function Piece(pieceData, board, canvas, onGameOver) {
  this.tetromino = pieceData[0];
  this.colorId = pieceData[1];
  this.id = pieceData[2];
  this.board = board;
  this.canvas = canvas;

  this.tetrominoN = 0; // Start from the first rotation
  this.activeTetromino = this.tetromino[this.tetrominoN];

  this.x = 3;
  this.y = -2;
}

Piece.prototype.equals = function (otherPiece) {
  return this.id === otherPiece.id;
};

// fill function
Piece.prototype.fill = function (colorId) {
  let border = false
  const level = GetLevel();
  if(this.id === "T" || this.id === "O" || this.id === "I") {
    border = true;
  }
  for (let r = 0; r < this.activeTetromino.length; r++) {
    for (let c = 0; c < this.activeTetromino[r].length; c++) {
      // Draw only occupied squares
      if (this.activeTetromino[r][c]) {
        if (colorId !== 0) {
          this.canvas.drawSquare(this.x + c, this.y + r, COLOR_PALETTE[colorId][level%10], border);
        } else {
          this.canvas.drawSquare(this.x + c, this.y + r, VACANT, border);
        }
      }
    }
  }
};

// draw a piece to the board
Piece.prototype.draw = function () {
  this.fill(this.colorId);
};

// undraw a piece
Piece.prototype.unDraw = function () {
  this.fill(0);
};

// Get the height of the lowest row that the piece occupies
Piece.prototype.getHeightFromBottom = function () {
  let maxY = 0;
  for (let r = 0; r < this.activeTetromino.length; r++) {
    for (let c = 0; c < this.activeTetromino[r].length; c++) {
      // If the square is occupied by the piece, update the max
      if (this.activeTetromino[r][c]) {
        this.canvas.drawSquare(this.x + c, this.y + r, color);
        maxY = Math.max(maxRow, this.y + r);
      }
    }
  }
  return NUM_ROW - maxY;
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
    for (let c = 0; c < this.activeTetromino[r].length; c++) {
      // we skip the vacant squares
      if (!this.activeTetromino[r][c]) {
        continue;
      }
      // pieces to lock on top = game over
      if (this.y + r < 0) {
        TriggerGameOver()
        break;
      }
      // we lock the piece
      this.board[this.y + r][this.x + c] = this.colorId;
    }
  }

  // update the board
  this.canvas.drawBoard();
};

// Collision fucntion
Piece.prototype.collision = function (x, y, piece) {
  for (let r = 0; r < piece.length; r++) {
    for (let c = 0; c < piece[r].length; c++) {
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
      if (this.board[newY][newX] != 0) {
        return true;
      }
    }
  }
  return false;
};
