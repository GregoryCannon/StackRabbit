const scoreElement = document.getElementById("score");
const headerText = document.getElementById("header-text");

import { PIECE_LIST } from "./tetrominoes.js";
import { Piece } from "./piece.js";
import { Canvas } from "./canvas.js";

export const ROW = 20;
export const COLUMN = 10;
export const SQUARE_SIZE = 20;
export const VACANT = "BLACK"; // color of an empty square

// How many points for X lines at a time (before scaling by level)
const REWARDS = {
  1: 40,
  2: 100,
  3: 300,
  4: 1200
};
// How many frames it takes to drop one square
const GRAVITY = {
  0: 48,
  1: 43,
  2: 38,
  3: 33,
  4: 28,
  5: 23,
  6: 18,
  7: 13,
  8: 8,
  9: 6,
  10: 5,
  11: 5,
  12: 5,
  13: 4,
  14: 4,
  15: 4,
  16: 3,
  17: 3,
  18: 3,
  19: 2
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

const GameState = {
  RUNNING: "running",
  PAUSED: "paused",
  GAME_OVER: "game over",
  START_SCREEN: "start screen"
};

let m_level = 0;
let m_gameState = GameState.RUNNING;
let m_currentPiece = randomPiece();
let m_score = 0;

function refreshHeaderText() {
  let newText = "";
  switch (m_gameState) {
    case GameState.START_SCREEN:
      newText = "Welcome to Tetris Trainer!";
      break;
    case GameState.RUNNING:
      newText = "    ";
      break;
    case GameState.GAME_OVER:
      newText = "Game over!";
      break;
    case GameState.PAUSED:
      newText = "Pauseds";
      break;
  }
  headerText.innerText = newText;
}

refreshHeaderText();

function onGameOver(argument) {
  m_gameState = GameState.GAME_OVER;
  refreshHeaderText();
}

function randomPiece() {
  let r = Math.floor(Math.random() * PIECE_LIST.length); // 0 -> 6
  return new Piece(
    PIECE_LIST[r][0],
    PIECE_LIST[r][1],
    m_board,
    m_canvas,
    onGameOver
  );
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
    m_score += REWARDS[numRowsCleared];
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
function keyDownListener(event) {
  if (m_gameState == GameState.RUNNING) {
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
}
function keyUpListener(event) {}
document.addEventListener("keydown", keyDownListener);
document.addEventListener("keyup", keyUpListener);

let framecount = 0;
function gameLoop() {
  if (m_gameState == GameState.RUNNING) {
    framecount += 1;
    if (framecount >= GRAVITY[m_level]) {
      moveCurrentPieceDown();
      framecount = 0;
    }
  }
  requestAnimationFrame(gameLoop);
}
gameLoop();
