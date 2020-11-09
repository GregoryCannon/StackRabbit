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
} from "./constants.js";
import { GetLevel, GetCurrentPiece } from "./index.js";

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
  const width = 5 * SQUARE_SIZE;
  const startX = BOARD_WIDTH + SQUARE_SIZE;
  const startY = 0.5 * SQUARE_SIZE;

  const formattedScore = ("0".repeat(6) + score).slice(-6);
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
