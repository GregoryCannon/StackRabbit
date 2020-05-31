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
} from "./constants.js";
import { Piece } from "./piece.js";
import { InputManager } from "./input_manager.js";

const scoreTextElement = document.getElementById("score");
const headerTextElement = document.getElementById("header-text");
const debugTextElement = document.getElementById("debug");
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
}
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
let m_framecount;
let m_ARE;

export const TriggerGameOver = () => {
  onGameOver();
}

const onGameOver = () => {
  m_gameState = GameState.GAME_OVER;
  refreshHeaderText();
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

export const GetLevel = () => {
  return m_level;
}

function refreshDebugText() {
  debugTextElement.innerText = m_inputManager.getDebugText();
}

function removeFullRows() {
  let numRowsCleared = 0;
  for (let r = 0; r < NUM_ROW; r++) {
    let isRowFull = true;
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (m_board[r][c] == SquareState.empty) {
        isRowFull = false;
        break;
      }
    }
    if (isRowFull) {
      numRowsCleared += 1;
      // Move down all the rows above it
      for (let y = r; y > 1; y--) {
        for (let c = 0; c < NUM_COLUMN; c++) {
          m_board[y][c] = m_board[y - 1][c];
        }
      }
      // Clear out the very top row
      for (let c = 0; c < NUM_COLUMN; c++) {
        m_board[0][c] = SquareState.empty;
      }
    }
  }
  if (numRowsCleared > 0) {
    // Update the board
    m_canvas.drawBoard();

    // Update the score
    m_score += REWARDS[numRowsCleared] * (m_level + 1);
    scoreTextElement.innerText = "Score: " + m_score;

    // Return true to indicate lines cleared
    return true;
  }
  // Return false to indicate no lines cleared
  return false;
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
  m_framecount = 0;
  m_ARE = 0;
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
  m_inputManager.handleInputsThisFrame();

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
  window.setTimeout(gameLoop, 17);
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
    m_currentPiece.lock();
    removeFullRows();
    getNewPiece();
    m_ARE = 12; // Frame delay before next piece
    return false;
  } else {
    // Move down as usual
    m_currentPiece.moveDown();
    return true;
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
