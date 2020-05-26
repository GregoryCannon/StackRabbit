import { PieceSelector } from "./piece_selector.js";
import { BoardLoader } from "./board_loader.js";
import { Canvas } from "./canvas.js";
import {
  ROW,
  COLUMN,
  VACANT,
  GRAVITY,
  REWARDS,
  GameState,
  Direction,
  DAS_TRIGGER,
  DAS_CHARGED,
  DAS_DOWN_CHARGED,
} from "./constants.js";
import { Piece } from "./piece.js";

const scoreTextElement = document.getElementById("score");
const headerTextElement = document.getElementById("header-text");
const debugTextElement = document.getElementById("debug");
const startGameButton = document.getElementById("start-game");
const restartGameButton = document.getElementById("restart-game");
const levelSelectElement = document.getElementById("level-select");

// Initial empty board
let m_board = [];
for (let r = 0; r < ROW; r++) {
  m_board[r] = [];
  for (let c = 0; c < COLUMN; c++) {
    m_board[r][c] = VACANT;
  }
}
let m_canvas = new Canvas(m_board);
let m_pieceSelector = new PieceSelector();
let m_boardLoader = new BoardLoader(m_board, m_canvas);
let m_currentPiece;
let m_nextPiece;

let m_level;
let m_gameState;
let m_score;
let m_framecount;
let m_ARE;

// Controls
let m_left_held;
let m_right_held;
let m_down_held;
let m_DAS_count;

// Default control setup
let LEFT_KEYCODE = 37;
let RIGHT_KEYCODE = 39;
let ROTATE_LEFT_KEYCODE = 90;
let ROTATE_RIGHT_KEYCODE = 88;
let DOWN_KEYCODE = 40;

// My personal control setup
if (false) {
  LEFT_KEYCODE = 90;
  RIGHT_KEYCODE = 88;
  ROTATE_LEFT_KEYCODE = 86;
  ROTATE_RIGHT_KEYCODE = 66;
  DOWN_KEYCODE = 18;
}

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
  headerTextElement.innerText = newText;
}

function refreshDebugText() {
  let debugStr = "";
  debugStr += "DAS: " + m_DAS_count;
  debugStr += "\nLeftKey: " + m_left_held;
  debugStr += "\nRightKey: " + m_right_held;
  debugStr += "\nDownKey: " + m_down_held;
  debugTextElement.innerText = debugStr;
}

function onGameOver(argument) {
  m_gameState = GameState.GAME_OVER;
  refreshHeaderText();
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
    m_score += REWARDS[numRowsCleared] * (m_level + 1);
    scoreTextElement.innerText = "Score: " + m_score;
  }
}

function getNewPiece() {
  m_currentPiece = m_nextPiece;
  m_nextPiece = new Piece(
    m_pieceSelector.chooseNextPiece(m_currentPiece.id),
    m_board,
    m_canvas,
    onGameOver
  );
  m_canvas.drawNextBox(m_nextPiece);
  m_canvas.drawPieceStatusString(m_pieceSelector.getStatusString());
}

function moveCurrentPieceDown() {
  if (m_currentPiece.shouldLock()) {
    // Lock in piece and get another piece
    m_currentPiece.lock();
    removeFullRows();
    getNewPiece();
    m_down_held = false; // make the player press down again if currently holding down
    m_ARE = 12; // 10 frame delay before next piece
  } else {
    // Move down as usual
    m_currentPiece.moveDown();
  }
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
    tickDAS(function () {
      m_currentPiece.moveLeft();
    });
  }
  // DAS right
  else if (m_right_held) {
    tickDAS(function () {
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
    // Letter 'P' pauses and unpauses
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

function resetLocalVariables() {
  m_score = 0;
  m_framecount = 0;
  m_ARE = 0;
  m_level = 0;
  m_gameState = GameState.START_SCREEN;

  m_left_held = false;
  m_right_held = false;
  m_down_held = false;
  m_DAS_count = 0;
}

function startGame() {
  // Reset game values
  resetLocalVariables();
  m_pieceSelector.startReadingPieceSequence();
  m_boardLoader.resetBoard();

  // Refresh UI
  m_canvas.drawBoard();
  refreshHeaderText();

  // Parse the level
  const levelSelected = parseInt(levelSelectElement.value);
  if (Number.isInteger(levelSelected)) {
    m_level = levelSelected;
  } else {
    m_level = 0;
  }

  // Get the first piece and put it in the next piece slot. Will be bumped to current in getNewPiece()
  m_nextPiece = new Piece(
    m_pieceSelector.chooseNextPiece(""),
    m_board,
    m_canvas,
    onGameOver
  );
  getNewPiece();

  m_gameState = GameState.RUNNING;
}
startGameButton.addEventListener("click", startGame);

// 60 FPS game loop
function gameLoop() {
  updateDAS();

  if (m_gameState == GameState.RUNNING) {
    if (m_ARE > 0) {
      // Waiting for next piece
      m_ARE -= 1;
    } else {
      m_framecount += 1;
      refreshDebugText();
      // Move the piece down when appropriate
      if (m_framecount >= GRAVITY[m_level]) {
        moveCurrentPieceDown();
        m_framecount = 0;
      }
    }
  }
  requestAnimationFrame(gameLoop);
}

resetLocalVariables();
m_canvas.drawBoard();
refreshHeaderText();
gameLoop();
