const pieceListElement = document.getElementById("piece-sequence");

import { PIECE_LIST, PIECE_LOOKUP } from "./tetrominoes.js";
import { Piece } from "./piece.js";

let m_pieceString = "";
let m_readIndex = -1;
pieceListElement.addEventListener("input", function(event) {
  m_pieceString = this.value;
  m_readIndex = 0;
});

export function PieceSelector(board, canvas, onGameOver) {
  this.board = board;
  this.canvas = canvas;
  this.onGameOver = onGameOver;
}

PieceSelector.prototype.presetPiece = function() {
  const nextPieceId = m_pieceString[m_readIndex];
  const nextPieceData = PIECE_LOOKUP[nextPieceId];
  m_readIndex += 1;
  return new Piece(
    nextPieceData[0],
    nextPieceData[1],
    nextPieceData[2],
    this.board,
    this.canvas,
    this.onGameOver
  );
};

// Get a random piece, following the original RNG of NES tetris
PieceSelector.prototype.randomPiece = function(previousPieceId) {
  // Roll once 0-7, where 7 is a dummy value
  let r = Math.floor(Math.random() * (PIECE_LIST.length + 1));
  if (r == PIECE_LIST.length || previousPieceId === PIECE_LIST[r][2]) {
    // Reroll once for repeats (or dummy) to reduce repeated pieces
    r = Math.floor(Math.random() * PIECE_LIST.length);
  }
  return new Piece(
    PIECE_LIST[r][0], // tetromino
    PIECE_LIST[r][1], // color
    PIECE_LIST[r][2], // string ID
    this.board, // reference to the board
    this.canvas, // reference to the canvas
    this.onGameOver // callback function for game over
  );
};

// Get the next piece, whether that be specified or random
PieceSelector.prototype.chooseNextPiece = function(currentPieceId) {
  let retVal;
  // If there is a next specified piece, select that
  if (m_readIndex != -1 && m_readIndex < m_pieceString.length) {
    retVal = this.presetPiece();
    console.log(retVal);
    return retVal;
  }
  // Otherwise pick one randomly
  retVal = this.randomPiece(currentPieceId);
  console.log(retVal);
  return retVal;
};
