const pieceListElement = document.getElementById("piece-sequence");

import { PIECE_LIST, PIECE_LOOKUP } from "./tetrominoes.js";

let m_pieceString = "";
let m_readIndex = -1;

export function PieceSelector() {}

PieceSelector.prototype.restart = function() {
  // Get piece list
  m_pieceString = pieceListElement.value;
  if (m_pieceString.length > 0) {
    m_readIndex = 0;
  }
};

// Get the next piece, whether that be specified or random
PieceSelector.prototype.chooseNextPiece = function(currentPieceId) {
  // If there is a next specified piece, select that
  if (m_readIndex != -1 && m_readIndex < m_pieceString.length) {
    return this.presetPiece();
  }
  // Otherwise pick one randomly
  return this.randomPiece(currentPieceId);
};

// Get summary of piece status (e.g. "random piece" or e.g. "piece 5 of 13")
PieceSelector.prototype.getStatusString = function() {
  if (m_readIndex != -1 && m_readIndex <= m_pieceString.length) {
    return "piece " + m_readIndex + " of " + m_pieceString.length;
  }
  return "random piece";
};

/**
  "Private" functions - unused outside of this file
  */

PieceSelector.prototype.presetPiece = function() {
  const nextPieceId = m_pieceString[m_readIndex];
  const nextPieceData = PIECE_LOOKUP[nextPieceId];
  m_readIndex += 1;
  return nextPieceData;
};

// Get a random piece, following the original RNG of NES tetris
PieceSelector.prototype.randomPiece = function(previousPieceId) {
  // Roll once 0-7, where 7 is a dummy value
  let r = Math.floor(Math.random() * (PIECE_LIST.length + 1));
  if (r == PIECE_LIST.length || previousPieceId === PIECE_LIST[r][2]) {
    // Reroll once for repeats (or dummy) to reduce repeated pieces
    r = Math.floor(Math.random() * PIECE_LIST.length);
  }
  const nextPieceData = PIECE_LIST[r];
  return nextPieceData;
};
