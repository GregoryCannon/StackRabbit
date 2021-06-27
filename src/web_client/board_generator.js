// const utils = require("../../built/src/server/utils");

const { NUM_COLUMN, NUM_ROW, SquareState } = require("./constants");

export function BoardGenerator(board, canvas) {
  this.board = board;
}

function getRandomInt(min, max) {
  min = Math.ceil(min);
  max = Math.floor(max);
  return Math.floor(Math.random() * (max - min + 1)) + min;
}

/**
 * Clears out the game board, in-place.
 */
BoardGenerator.prototype.loadEmptyBoard = function () {
  for (let r = 0; r < NUM_ROW; r++) {
    for (let c = 0; c < NUM_COLUMN; c++) {
      this.board[r][c] = SquareState.EMPTY;
    }
  }
};

/**
 * Loads a standard-ish sloping board, in-place.
 */
BoardGenerator.prototype.loadStandardBoard = function () {
  this.loadEmptyBoard();

  let currentHeight = getRandomInt(6, 10);
  for (let col = 0; col < NUM_COLUMN - 1; col++) {
    const heightThisCol = Math.min(12, Math.max(0, currentHeight));
    // There's some subtraction here because the origin is at the top
    // but 0 height is at the bottom
    for (let row = NUM_ROW - heightThisCol - 1; row < NUM_ROW; row++) {
      const colorIndex = row % 3;
      const rowColor = [
        SquareState.COLOR1,
        SquareState.COLOR2,
        SquareState.COLOR3,
      ][colorIndex];

      this.board[row][col] = rowColor;
    }

    // Apply a random factor that makes it favor sloping right
    currentHeight += getRandomInt(-2, 1);
  }
};

const HOLE_PROBABILITIES = {
  NONE: 0.5,
  ONE: 0.4,
  TWO: 0.1,
};

/**
 * Loads a board with holes in it, in-place.
 */
BoardGenerator.prototype.loadDigBoard = function () {
  this.loadStandardBoard();
  // const digBoard = utils.generateDigPracticeBoard(5, 6);
  // for (let r = 0; r < NUM_ROW; r++) {
  //   for (let c = 0; c < NUM_COLUMN; c++) {
  //     this.board[r][c] = digBoard[r][c];
  //   }
  // }
  // return;

  for (let row = 0; row < NUM_ROW; row++) {
    let numHoles;
    const rand0To1 = Math.random();
    if (rand0To1 < HOLE_PROBABILITIES.NONE) {
      numHoles = 0;
    } else if (rand0To1 < HOLE_PROBABILITIES.NONE + HOLE_PROBABILITIES.ONE) {
      numHoles = 1;
    } else {
      numHoles = 2;
    }

    for (let i = 0; i < numHoles; i++) {
      this.board[row][getRandomInt(0, NUM_COLUMN - 1)] = SquareState.EMPTY;
    }
  }
};
