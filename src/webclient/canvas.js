const mainCanvas = document.getElementById("main-canvas");
const context = mainCanvas.getContext("2d");

import {
  NUM_ROW,
  NUM_COLUMN,
  SQUARE_SIZE,
  PIXEL_SIZE,
  NEXT_BOX_WIDTH,
  VACANT,
  COLOR_PALETTE,
  BOARD_WIDTH,
  SquareState,
} from "./constants.js";
import { GetLevel, GetCurrentPiece, calcParity } from "./index.js";
const GameSettings = require("./game_settings_manager");

// Resize the canvas based on the square size
mainCanvas.setAttribute("height", SQUARE_SIZE * NUM_ROW);
mainCanvas.setAttribute("width", SQUARE_SIZE * (NUM_COLUMN + 7)); // +6 for next boxk

export function Canvas(board) {
  this.board = board;
}

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
    context.fillStyle = "black";
    context.fillRect(
      leftColToClear * SQUARE_SIZE,
      rowNum * SQUARE_SIZE,
      SQUARE_SIZE,
      SQUARE_SIZE
    );
    context.fillRect(
      rightColToClear * SQUARE_SIZE,
      rowNum * SQUARE_SIZE,
      SQUARE_SIZE,
      SQUARE_SIZE
    );
  }
};

// draw a square
Canvas.prototype.drawSquare = function (x, y, color, border = false) {
  if (color == VACANT) {
    context.fillStyle = "black";
    context.fillRect(
      x * SQUARE_SIZE,
      y * SQUARE_SIZE,
      SQUARE_SIZE,
      SQUARE_SIZE
    );
    return;
  }

  // For I, T, and O
  context.fillStyle = color;
  context.fillRect(
    x * SQUARE_SIZE,
    y * SQUARE_SIZE,
    7 * PIXEL_SIZE,
    7 * PIXEL_SIZE
  );

  if (border && color !== VACANT) {
    context.fillStyle = "white";
    context.fillRect(
      x * SQUARE_SIZE + PIXEL_SIZE,
      y * SQUARE_SIZE + PIXEL_SIZE,
      5 * PIXEL_SIZE,
      5 * PIXEL_SIZE
    );
  }
  // Draw 'shiny' part
  if (color !== VACANT) {
    context.fillStyle = "white";
    context.fillRect(x * SQUARE_SIZE, y * SQUARE_SIZE, PIXEL_SIZE, PIXEL_SIZE);
    context.fillRect(
      x * SQUARE_SIZE + PIXEL_SIZE,
      y * SQUARE_SIZE + PIXEL_SIZE,
      PIXEL_SIZE,
      PIXEL_SIZE
    );
    context.fillRect(
      x * SQUARE_SIZE + PIXEL_SIZE + PIXEL_SIZE,
      y * SQUARE_SIZE + PIXEL_SIZE,
      PIXEL_SIZE,
      PIXEL_SIZE
    );
    context.fillRect(
      x * SQUARE_SIZE + PIXEL_SIZE,
      y * SQUARE_SIZE + PIXEL_SIZE + PIXEL_SIZE,
      PIXEL_SIZE,
      PIXEL_SIZE
    );
  }
};

/**
 * Draws the next box. If nextPiece is nonnull, draws the piece in it.
 * @param {Piece object} nextPiece
 */
Canvas.prototype.drawNextBox = function (nextPiece) {
  // All in units of SQUARE_SIZE
  const startX = NUM_COLUMN + 1;
  const startY = 8;
  const width = 5;
  const height = 4.5;

  // background
  context.fillStyle = "BLACK";
  context.fillRect(
    startX * SQUARE_SIZE,
    startY * SQUARE_SIZE,
    width * SQUARE_SIZE,
    height * SQUARE_SIZE
  );

  if (nextPiece != null) {
    const pieceStartX =
      nextPiece.id === "I" || nextPiece.id === "O" ? startX + 0.5 : startX;
    const pieceStartY = nextPiece.id === "I" ? startY - 0.25 : startY + 0.25;
    const color = COLOR_PALETTE[nextPiece.colorId][GetLevel() % 10];

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
  }
};

Canvas.prototype.drawScoreDisplay = function (score) {
  const pastMax = score >= 1000000;

  const width = SQUARE_SIZE * (pastMax ? 6 : 5);
  const startX = BOARD_WIDTH + SQUARE_SIZE * (pastMax ? 0.5 : 1);
  const startY = 0.5 * SQUARE_SIZE;

  const formattedScore = ("0".repeat(7) + score).slice(pastMax ? -7 : -6);
  this.drawMultiLineText(
    ["SCORE", formattedScore],
    startX,
    startY,
    width,
    "center"
  );
};

Canvas.prototype.drawLinesDisplay = function (numLines) {
  const width = NEXT_BOX_WIDTH;
  const startX = BOARD_WIDTH + SQUARE_SIZE;
  const startY = 3 * SQUARE_SIZE;

  const formattedScore = ("0".repeat(3) + numLines).slice(-3);
  this.drawMultiLineText(
    ["LINES", formattedScore],
    startX,
    startY,
    width,
    "center"
  );
};

Canvas.prototype.drawLevelDisplay = function (level) {
  const width = NEXT_BOX_WIDTH;
  const startX = BOARD_WIDTH + SQUARE_SIZE;
  const startY = 14 * SQUARE_SIZE;

  const formattedScore = ("0".repeat(2) + level).slice(-2);
  this.drawMultiLineText(
    ["LEVEL", formattedScore],
    startX,
    startY,
    width,
    "center"
  );
};

Canvas.prototype.drawTetrisRateDisplay = function (tetrisCount, lines) {
  const width = NEXT_BOX_WIDTH;
  const startX = BOARD_WIDTH + SQUARE_SIZE;
  const startY = 17 * SQUARE_SIZE;

  let tetrisRate = 0;
  if (lines > 0) {
    tetrisRate = (4 * tetrisCount) / lines;
  }
  const formattedTetrisRate = parseInt(tetrisRate * 100);
  this.drawMultiLineText(
    ["TRT", formattedTetrisRate + "%"],
    startX,
    startY,
    width,
    "center"
  );
};

Canvas.prototype.drawPieceStatusDisplay = function (linesOfText) {
  const width = NEXT_BOX_WIDTH;
  const startX = BOARD_WIDTH + SQUARE_SIZE;
  const startY = 6 * SQUARE_SIZE;

  this.drawMultiLineText(linesOfText, startX, startY, width, "center");
};

Canvas.prototype.drawMultiLineText = function (
  linesOfText,
  startX,
  startY,
  width,
  align
) {
  const lineHeight = 20;

  // Clear previous text
  context.clearRect(startX, startY, width, linesOfText.length * lineHeight);

  // Write "x of x" text
  context.textAlign = "center";
  context.font = "18px 'Press Start 2P'";
  context.fillStyle = "BLACK";

  const alignOffsetFactor = align == "center" ? width / 2 : 0;

  let lineIndex = 0;
  for (let line of linesOfText) {
    context.fillText(
      line.toUpperCase(),
      startX + alignOffsetFactor,
      startY + (lineIndex + 1) * lineHeight
    );
    lineIndex++;
  }
};

Canvas.prototype.drawPiece = function (piece) {
  if (piece == undefined) {
    return;
  }
  const level = GetLevel();
  const border = piece.id === "T" || piece.id === "O" || piece.id === "I";
  for (let r = 0; r < piece.activeTetromino.length; r++) {
    for (let c = 0; c < piece.activeTetromino[r].length; c++) {
      // Draw only occupied squares
      if (piece.activeTetromino[r][c]) {
        if (piece.colorId !== 0) {
          this.drawSquare(
            piece.x + c,
            piece.y + r,
            COLOR_PALETTE[piece.colorId][level % 10],
            border
          );
        } else {
          this.drawSquare(piece.x + c, piece.y + r, VACANT, border);
        }
      }
    }
  }
};

Canvas.prototype.unDrawPiece = function (piece) {
  if (piece == undefined) {
    return;
  }
  for (let r = 0; r < piece.activeTetromino.length; r++) {
    for (let c = 0; c < piece.activeTetromino[r].length; c++) {
      // Erase occupied squares
      if (piece.activeTetromino[r][c]) {
        this.drawSquare(piece.x + c, piece.y + r, VACANT, false);
      }
    }
  }
};

Canvas.prototype.drawCurrentPiece = function () {
  this.drawPiece(GetCurrentPiece());
};

Canvas.prototype.unDrawCurrentPiece = function () {
  this.unDrawPiece(GetCurrentPiece());
};

// Draw the pieces locked into the board (NB: does not render the current piece)
Canvas.prototype.drawBoard = function () {
  // const drawStart = window.performance.now();
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

  if (GameSettings.shouldShowDiggingHints()) {
    this.drawDiggingHints();
  }
  if (GameSettings.shouldShowParityHints()) {
    this.drawParityHints();
  }
  // const drawEnd = window.performance.now();
  // console.log(drawEnd - drawStart);
};

function filledIfExists(row, col, board) {
  if (col < 0 || col >= NUM_COLUMN || row < 0 || row >= NUM_ROW) {
    return true;
  }
  return board[row][col] != SquareState.EMPTY;
}

function getCellsThatNeedToBeFilled(board, doesWantCleanCol10) {
  const linesNeededToClear = new Set();
  for (let col = 0; col < NUM_COLUMN; col++) {
    // Go down to the top of the stack in that column
    let row = 0;
    while (row < NUM_ROW && board[row][col] == SquareState.EMPTY) {
      row++;
    }
    // Track the full rows we pass through
    const rowsAboveHole = new Set();
    while (row < NUM_ROW - 1) {
      rowsAboveHole.add(row);
      row++;
      if (board[row][col] === SquareState.EMPTY) {
        // If not on col 10, we found a hole. Add all the full rows we passed through to the set
        // of lines needing to be cleared. Otherwise we ignore tempSet.
        for (const line of rowsAboveHole) {
          linesNeededToClear.add(line);
        }
      }
    }
  }

  // Any row that has col 10 filled above the highest hole will need to be cleared
  if (linesNeededToClear.size > 0) {
    for (let row = 0; row < NUM_ROW; row++) {
      if (board[row][NUM_COLUMN - 1] == SquareState.FULL) {
        linesNeededToClear.add(row);
      }
    }
  }
  return linesNeededToClear.size;
}

function getTopmostHole(board) {
  for (let r = 0; r < NUM_ROW; r++) {
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (
        board[r][c] == SquareState.EMPTY &&
        filledIfExists(r - 1, c, board) &&
        filledIfExists(r + 1, c, board) &&
        filledIfExists(r, c - 1, board) &&
        filledIfExists(r, c + 1, board)
      ) {
        return [r, c];
      }
    }
  }
  return [];
}

function getRowsCoveringWell(board) {
  let r = NUM_ROW - 1;
  let rowsCoveringWell = [];
  while (!filledIfExists(r, NUM_COLUMN - 1, board)) {
    r--;
  }
  while (r >= 0 && filledIfExists(r, NUM_COLUMN - 1, board)) {
    rowsCoveringWell.push(r);
    r--;
  }
  return rowsCoveringWell;
}

function fillSquare(row, col, color) {
  context.fillStyle = color;
  context.fillRect(
    col * SQUARE_SIZE + PIXEL_SIZE,
    row * SQUARE_SIZE + PIXEL_SIZE,
    SQUARE_SIZE - 3 * PIXEL_SIZE,
    SQUARE_SIZE - 3 * PIXEL_SIZE
  );
}

function fillRow(row, color, board) {
  for (let loopCol = 0; loopCol < NUM_COLUMN; loopCol++) {
    if (board[row][loopCol] == SquareState.EMPTY) {
      fillSquare(row, loopCol, color);
    }
  }
}

Canvas.prototype.drawDiggingHints = function () {
  const topMostHole = getTopmostHole(this.board);
  const rowsCoveringWell = getRowsCoveringWell(this.board);

  if (topMostHole.length > 0) {
    // Find the rows that need to be cleared
    const row = topMostHole[0];
    const col = topMostHole[1];
    let rowsToClear = [];

    let currentRow = row - 1;
    while (currentRow >= 0 && filledIfExists(currentRow, col, this.board)) {
      rowsToClear.push(currentRow);
      currentRow -= 1;
    }

    // Draw a yellow circle in the topmost hole
    const centerY = (row + 0.45) * SQUARE_SIZE;
    const centerX = (col + 0.45) * SQUARE_SIZE;
    const radius = SQUARE_SIZE / 4;
    context.fillStyle = "yellow";
    context.beginPath();
    context.arc(centerX, centerY, radius, 0, 2 * Math.PI, false);
    context.fill();

    // Fill in the empty spaces with red in rows that need to be cleared
    for (let loopRow of rowsToClear) {
      fillRow(loopRow, "#842424", this.board);
    }
  } else if (rowsCoveringWell.length > 0) {
    // Fill in the empty spaces with red in rows that need to be cleared
    for (let loopRow of rowsCoveringWell) {
      fillRow(loopRow, "#215E30", this.board);
    }
  }
};

function numToSingleDigiHex(num) {
  num = Math.floor(num);
  if (num < 0) {
    throw new Error("Can't convert negative num to hex");
  }
  if (num < 10) {
    return "" + num;
  } else if (num < 16) {
    return ["a", "b", "c", "d", "e", "f"][num - 10];
  } else {
    // Max out at 16 because single digit
    return "f";
  }
}

Canvas.prototype.drawParityHints = function () {
  const localParities = [];
  for (let c = 0; c < NUM_COLUMN; c++) {
    localParities[c] = calcParity(c - 2, c + 3);
  }

  // Normalize over the nearby columns
  let normalizedLocalParities = [];
  for (let i = 0; i < NUM_COLUMN; i++) {
    let total = 0;
    let numAdded = 0;
    [i - 1, i, i + 1].forEach((x) => {
      if (x >= 0 && x < NUM_COLUMN) {
        total += localParities[x];
        numAdded += 1;
      }
    });
    if (numAdded == 0) {
      throw new Error("None added");
    }
    normalizedLocalParities[i] = total / numAdded; // numAdded is never 0 since the col itself will always be added
  }

  for (let c = 0; c < NUM_COLUMN; c++) {
    const normalizedLocalParity = normalizedLocalParities[c];
    for (let r = 0; r < NUM_ROW; r++) {
      if (this.board[r][c] == SquareState.EMPTY) {
        // Get a shade of red proportional to the parity
        const transformedParity = Math.max(5 * normalizedLocalParity - 4, 0);
        const alphaChannel = numToSingleDigiHex(transformedParity);
        const fillColor = "#ff0000" + alphaChannel + alphaChannel;
        fillSquare(r, c, fillColor);
      }
    }
  }
};
