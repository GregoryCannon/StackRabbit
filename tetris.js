const scoreElement = document.getElementById("score");

import { PIECES } from "./tetrominoes.js";
import { Piece } from "./piece.js";
import { Canvas } from "./canvas.js";

export const ROW = 20;
export const COLUMN = 10;
export const SQUARE_SIZE = 20;
export const VACANT = "BLACK"; // color of an empty square

const DROP_TIME_MS = 200;
const rewards = {
  1: 40,
  2: 100,
  3: 300,
  4: 1200
};

let m_board = [];
for (let r = 0; r < ROW; r++) {
  m_board[r] = [];
  for (let c = 0; c < COLUMN; c++) {
    m_board[r][c] = VACANT;
  }
}

let m_canvas = new Canvas(m_board);
m_canvas.drawBoard();

let m_level = 0;
let m_gameOver = false;
let m_currentPiece = randomPiece();
let m_score = 0;

function onGameOver(argument) {
  m_gameOver = true;
  alert("Game over!");
}

function randomPiece() {
  let r = Math.floor(Math.random() * PIECES.length); // 0 -> 6
  return new Piece(PIECES[r][0], PIECES[r][1], m_board, m_canvas, onGameOver);
}

function incrementScore(numRowsCleared) {
  return rewards[numRowsCleared];
}

function removeFullRows() {
  let numRowsCleared = 0;
  for (let r = 0; r < ROW; r++) {
    let isRowFull = true;
    for (let c = 0; c < COLUMN; c++) {
      if (m_board[r][c] == VACANT) {
        isRowFull = false;
        break;
      }
    }
    if (isRowFull) {
      numRowsCleared += 1;
      // Move down all the rows above it
      for (let y = r; y > 1; y--) {
        for (let c = 0; c < COLUMN; c++) {
          m_board[y][c] = m_board[y - 1][c];
        }
      }
      // Clear out the very top row
      for (let c = 0; c < COLUMN; c++) {
        m_board[0][c] = VACANT;
      }
    }
  }
  if (numRowsCleared > 0) {
    // Update the board
    m_canvas.drawBoard();

    // Update the score
    incrementScore(numRowsCleared);
    scoreElement.innerHTML = m_score;
  }
}

function moveCurrentPieceDown() {
  if (m_currentPiece.shouldLock()) {
    m_currentPiece.lock();
    removeFullRows();
    m_currentPiece = randomPiece();
  } else {
    m_currentPiece.moveDown();
  }
}

// Control the piece
document.addEventListener("keydown", keyDownListener);
document.addEventListener("keyup", keyUpListener);
function keyDownListener(event) {
  if (event.keyCode == 37) {
    m_currentPiece.moveLeft();
  } else if (event.keyCode == 38) {
    m_currentPiece.rotate();
  } else if (event.keyCode == 39) {
    m_currentPiece.moveRight();
  } else if (event.keyCode == 40) {
    moveCurrentPieceDown();
  }
}

function keyUpListener(event) {}

let framecount = 0;
function gameLoop() {
  framecount += 1;
  if (framecount >= 10) {
    moveCurrentPieceDown();
    framecount = 0;
  }
  if (!m_gameOver) {
    requestAnimationFrame(gameLoop);
  }
}
gameLoop();
