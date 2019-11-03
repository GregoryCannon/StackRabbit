const scoreElement = document.getElementById("score");
const headerText = document.getElementById("header-text");
const debugText = document.getElementById("debug");

import { PIECE_LIST } from "./tetrominoes.js";
import { Piece } from "./piece.js";
import { Canvas } from "./canvas.js";
import {
  ROW,
  COLUMN,
  VACANT,
  GRAVITY,
  REWARDS,
  GameState
} from "./constants.js";

// Initial empty board
let m_board = [];
for (let r = 0; r < ROW; r++) {
  m_board[r] = [];
  for (let c = 0; c < COLUMN; c++) {
    m_board[r][c] = VACANT;
  }
}

let m_canvas = new Canvas(m_board);
let m_level = 0;
let m_gameState = GameState.RUNNING;
let m_currentPiece = randomPiece();
let m_score = 0;
let m_framecount = 0;

function refreshHeaderText() {
  let newText = "";
  switch (m_gameState) {
    case GameState.START_SCREEN:
      newText = "Welcome to Tetris Trainer!";
      break;
    case GameState.RUNNING:
      newText = "";
      break;
    case GameState.GAME_OVER:
      newText = "Game over!";
      break;
    case GameState.PAUSED:
      newText = "Paused";
      break;
  }
  headerText.innerText = newText;
}

function refreshDebugText() {
  let debugStr = "";
  debugStr += "DAS: " + m_DAS_count;
  debugStr += "\nLeftKey: " + m_left_held;
  debugStr += "\nRightKey: " + m_right_held;
  debugStr += "\nDownKey: " + m_down_held;
  debugText.innerText = debugStr;
}

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
    // Lock in piece and get another
    m_currentPiece.lock();
    removeFullRows();
    m_currentPiece = randomPiece();
    m_down_held = false; // make the player press down again if held
  } else {
    // Move down as usual
    m_currentPiece.moveDown();
  }
}

let m_left_held = false;
let m_right_held = false;
let m_down_held = false;
const Direction = {
  LEFT: "left",
  RIGHT: "right",
  DOWN: "down",
  NONE: "none"
};
const DAS_TRIGGER = 16;
const DAS_CHARGED = 10;
const DAS_DOWN_CHARGED = 14;
let m_DAS_count = 0;

let LEFT_KEYCODE = 37;
let RIGHT_KEYCODE = 39;
let ROTATE_LEFT_KEYCODE = 90;
let ROTATE_RIGHT_KEYCODE = 88;
let DOWN_KEYCODE = 40;

// My personal control setup
if (true) {
  LEFT_KEYCODE = 90;
  RIGHT_KEYCODE = 88;
  ROTATE_LEFT_KEYCODE = 86;
  ROTATE_RIGHT_KEYCODE = 66;
  DOWN_KEYCODE = 18;
}

function resetDAS() {
  m_DAS_count = 0;
}

function tickDAS(callback) {
  m_DAS_count += 1;
  if (m_DAS_count >= DAS_TRIGGER) {
    m_DAS_count = DAS_CHARGED;
    callback();
  }
}

function updateDAS() {
  const numKeysHeld = m_down_held + m_left_held + m_right_held;
  // If holding none or multiple keys, do nothing
  if (numKeysHeld != 1) {
    m_DAS_count = 0;
  }
  // Move piece down
  if (m_down_held) {
    // No need to wait when pressing down
    if (m_DAS_count == 0) {
      m_DAS_count = DAS_DOWN_CHARGED;
    }
    m_DAS_count += 1;
    if (m_DAS_count >= DAS_TRIGGER) {
      m_DAS_count = DAS_DOWN_CHARGED;
      moveCurrentPieceDown();
    }
  }
  // DAS left
  if (m_left_held) {
    tickDAS(function() {
      m_currentPiece.moveLeft();
    });
  }
  // DAS right
  else if (m_right_held) {
    tickDAS(function() {
      m_currentPiece.moveRight();
    });
  }
}

function keyDownListener(event) {
  // Override the browser's built-in key repeating
  if (event.repeat) {
    return;
  }
  // Piece movement - on key down
  // Move the piece once, and if appropriate, save that the key is held (for DAS)
  if (m_gameState == GameState.RUNNING) {
    switch (event.keyCode) {
      case LEFT_KEYCODE:
        m_left_held = true;
        m_currentPiece.moveLeft();
        break;
      case RIGHT_KEYCODE:
        m_right_held = true;
        m_currentPiece.moveRight();
        break;
      case ROTATE_LEFT_KEYCODE:
        m_currentPiece.rotate(true);
        break;
      case ROTATE_RIGHT_KEYCODE:
        m_currentPiece.rotate(false);
        break;
      case DOWN_KEYCODE:
        m_down_held = true;
        moveCurrentPieceDown();
        break;
    }
  }

  // Client controls
  if (event.keyCode == 80) {
    // Letter 'P'
    if (m_gameState == GameState.RUNNING) {
      m_gameState = GameState.PAUSED;
      refreshHeaderText();
    } else if (m_gameState == GameState.PAUSED) {
      m_gameState = GameState.RUNNING;
      refreshHeaderText();
    }
  }
}
function keyUpListener(event) {
  // Piece movement - on key up
  if (m_gameState == GameState.RUNNING) {
    if (event.keyCode == LEFT_KEYCODE) {
      m_left_held = false;
      m_DAS_count = 0;
    } else if (event.keyCode == RIGHT_KEYCODE) {
      m_right_held = false;
      m_DAS_count = 0;
    } else if (event.keyCode == DOWN_KEYCODE) {
      m_down_held = false;
      m_DAS_count = 0;
    }
  }
}
document.addEventListener("keydown", keyDownListener);
document.addEventListener("keyup", keyUpListener);

function init() {
  m_canvas.drawBoard();
  refreshHeaderText();
}

init();

// 60 FPS game loop
function gameLoop() {
  updateDAS();

  if (m_gameState == GameState.RUNNING) {
    m_framecount += 1;
    refreshDebugText();
    if (m_framecount >= GRAVITY[m_level]) {
      moveCurrentPieceDown();
      m_framecount = 0;
    }
  }
  requestAnimationFrame(gameLoop);
}
gameLoop();
