import { PieceSelector } from "./piece_selector.js";
import { BoardLoader } from "./board_loader.js";
import { Canvas } from "./canvas.js";
import {
  NUM_ROW,
  NUM_COLUMN,
  VACANT,
  GRAVITY,
  REWARDS,
  GameState,
  GameSubState,
  LINE_CLEAR_DELAY,
} from "./constants.js";
import { Piece } from "./piece.js";
import { InputManager } from "./input_manager.js";

const scoreTextElement = document.getElementById("score");
const headerTextElement = document.getElementById("header-text");
const debugTextElement = document.getElementById("debug");
const statsTextElement = document.getElementById("stats");
const gameOptionsForm = document.getElementById("game-options-form");
const startGameButton = document.getElementById("start-game");
const restartGameButton = document.getElementById("restart-game");
const levelSelectElement = document.getElementById("level-select");

// Initial empty board
// 0 is empty space, 1 is T piece color, 2 is L piece color, 3 is J piece color
export const SquareState = {
  empty: 0,
  color1: 1,
  color2: 2,
  color3: 3,
};
let m_board = [];
for (let r = 0; r < NUM_ROW; r++) {
  m_board[r] = [];
  for (let c = 0; c < NUM_COLUMN; c++) {
    m_board[r][c] = SquareState.empty;
  }
}
let m_inputManager;
let m_canvas = new Canvas(m_board);
let m_pieceSelector = new PieceSelector();
let m_boardLoader = new BoardLoader(m_board, m_canvas);
let m_currentPiece;
let m_nextPiece;

let m_level;
let m_gameState;
let m_score;
let m_gravityFrameCount;
let m_ARE;
let m_lineClearDelay;
let m_linesCleared;

export const TriggerGameOver = () => {
  onGameOver();
};

const onGameOver = () => {
  m_gameState = GameState.GAME_OVER;
  refreshHeaderText();
};

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

export const GetLevel = () => {
  return m_level;
};

function refreshDebugText() {
  debugTextElement.innerText = m_inputManager.getDebugText();
}

function refreshStats() {
  // Calculate parity, where the top left square is "1" and adjacent squares are "-1"
  let parity = 0;
  for (let r = 0; r < NUM_ROW; r++) {
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (m_board[r][c] != SquareState.empty) {
        // Add 1 or -1 to parity total based on the square's location
        const cellConstant = (r + c) % 2 == 0 ? 1 : -1;
        parity += cellConstant;
      }
    }
  }

  statsTextElement.innerText = "Parity: " + parity;
}

function getFullRows() {
  let fullLines = [];
  for (let r = 0; r < NUM_ROW; r++) {
    let isRowFull = true;
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (m_board[r][c] == SquareState.empty) {
        isRowFull = false;
        break;
      }
    }
    if (isRowFull) {
      fullLines.push(r);
    }
  }
  return fullLines;
}

function removeFullRows() {
  for (const r of m_linesCleared) {
    // Move down all the rows above it
    for (let y = r; y > 1; y--) {
      for (let c = 0; c < NUM_COLUMN; c++) {
        m_board[y][c] = m_board[y - 1][c];
      }
    }
    // Clear out the very top row (newly shifted into the screen)
    for (let c = 0; c < NUM_COLUMN; c++) {
      m_board[0][c] = SquareState.empty;
    }
  }
  const numLinesCleared = m_linesCleared.length;
  if (numLinesCleared > 0) {
    // Update the board
    m_canvas.drawBoard();

    // Update the score
    m_score += REWARDS[numLinesCleared] * (m_level + 1);
    scoreTextElement.innerText = "Score: " + m_score;
  }
  m_linesCleared = [];
}

function getNewPiece() {
  m_currentPiece = m_nextPiece;
  // Piece status is drawn first since the read index increments when the next
  // piece is selected
  m_canvas.drawPieceStatusString(m_pieceSelector.getStatusString());
  m_nextPiece = new Piece(
    m_pieceSelector.chooseNextPiece(m_currentPiece.id),
    m_board,
    m_canvas
  );
  m_canvas.drawNextBox(m_nextPiece);
}

function resetLocalVariables() {
  m_score = 0;
  m_gravityFrameCount = 0;
  m_ARE = 0;
  m_lineClearDelay = 0;
  m_linesCleared = [];
  m_level = 0;
  m_gameState = GameState.START_SCREEN;
  m_inputManager.resetLocalVariables();
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
  if (Number.isInteger(levelSelected) && levelSelected > 0) {
    m_level = levelSelected;
  } else {
    m_level = 0;
  }

  // Get the first piece and put it in the next piece slot. Will be bumped to current in getNewPiece()
  m_nextPiece = new Piece(
    m_pieceSelector.chooseNextPiece(""),
    m_board,
    m_canvas
  );
  getNewPiece();

  m_gameState = GameState.RUNNING;
}

// 60 FPS game loop
function gameLoop() {
  if (m_gameState == GameState.RUNNING) {
    if (m_lineClearDelay > 0) {
      // Still animating line clear
      m_lineClearDelay -= 1;
      // Do subtraction so animation frames count up
      m_canvas.drawLineClears(
        m_linesCleared,
        LINE_CLEAR_DELAY - m_lineClearDelay
      );
      if (m_lineClearDelay == 0) {
        // Clear the lines for real and shift stuff down
        removeFullRows();
      }
    } else if (m_ARE > 0) {
      // Waiting for next piece
      m_ARE -= 1;
    } else {
      m_inputManager.handleInputsThisFrame();
      m_gravityFrameCount += 1;
      refreshDebugText();
      refreshStats();
      // Move the piece down when appropriate
      if (
        !m_inputManager.isSoftDropping() &&
        m_gravityFrameCount >= GRAVITY[m_level]
      ) {
        moveCurrentPieceDown();
        m_gravityFrameCount = 0;
      }
    }
  }
  window.setTimeout(gameLoop, 16.33);

  // Slo-mo testing
  // window.setTimeout(gameLoop, 50);
}

/** Delegate functions to controls code */

/** @returns whether the piece moved */
function movePieceLeft() {
  return m_currentPiece.moveLeft();
}

/** @returns whether the piece moved */
function movePieceRight() {
  return m_currentPiece.moveRight();
}

/** @returns whether the piece moved */
function moveCurrentPieceDown() {
  if (m_currentPiece.shouldLock()) {
    // Lock in piece and get another piece
    const lockHeight = m_currentPiece.getHeightFromBottom();
    m_currentPiece.lock();
    getNewPiece();

    // Clear lines
    m_linesCleared = getFullRows();
    if (m_linesCleared.length > 0) {
      m_lineClearDelay = LINE_CLEAR_DELAY; // Clear delay counts down from max val
    }

    // Get the ARE based on piece lock height
    /* ARE (frame delay before next piece) is 10 frames for 0-2 height, then an additional 
      2 frames for each group of 4 above that.
        E.g. 9 high would be: 10 + 2 + 2 = 14 frames */
    m_ARE = 10 + Math.floor((lockHeight + 2) / 4) * 2;

    return false; // Return false because the piece didn't shift down
  } else {
    // Move down as usual
    m_currentPiece.moveDown();
    return true; // Return true because the piece moved down
  }
}

function rotatePieceLeft() {
  m_currentPiece.rotate(true);
}

function rotatePieceRight() {
  m_currentPiece.rotate(false);
}

function togglePause() {
  if (m_gameState == GameState.RUNNING) {
    m_gameState = GameState.PAUSED;
    refreshHeaderText();
  } else if (m_gameState == GameState.PAUSED) {
    m_gameState = GameState.RUNNING;
    refreshHeaderText();
  }
}

function getGameSubState() {
  if (m_lineClearDelay > 0) {
    return GameSubState.LINE_CLEAR;
  } else if (m_ARE > 0) {
    return GameSubState.ARE;
  } else {
    return GameSubState.PIECE_ACTIVE;
  }
}

function getGameState() {
  return m_gameState;
}

function getARE() {
  return m_ARE;
}

/**
 * SCRIPT START
 */
m_inputManager = new InputManager(
  moveCurrentPieceDown,
  movePieceLeft,
  movePieceRight,
  rotatePieceLeft,
  rotatePieceRight,
  togglePause,
  getGameState,
  getGameSubState,
  getARE
);

document.addEventListener("keydown", (e) => {
  m_inputManager.keyDownListener(e);
});
document.addEventListener("keyup", (e) => {
  m_inputManager.keyUpListener(e);
});

gameOptionsForm.addEventListener("submit", (e) => {
  e.preventDefault();
  startGameButton.focus();
  startGame();
});

resetLocalVariables();
m_canvas.drawBoard();
refreshHeaderText();
gameLoop();
