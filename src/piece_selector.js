const pieceListElement = document.getElementById("piece-sequence");

import { PIECE_LIST, PIECE_LOOKUP } from "./tetrominoes.js";

let m_pieceSequenceStr = "";
let m_readIndex = 0;
let m_isReadingFromSequence = false;

export function PieceSelector() {}

/**
  Public functions
  */

PieceSelector.prototype.startReadingPieceSequence = function () {
  // Get piece sequence
  m_pieceSequenceStr = pieceListElement.value;
  if (m_pieceSequenceStr.length > 0) {
    m_isReadingFromSequence = true;
    m_readIndex = 0;
  }
};

// Get the next piece, whether that be specified or random
PieceSelector.prototype.chooseNextPiece = function (currentPieceId) {
  // If there is a next specified piece, select that
  if (m_isReadingFromSequence) {
    return this.getPresetPiece();
  }
  // Otherwise pick one randomly
  m_isReadingFromSequence = false;
  return this.getRandomPiece(currentPieceId);
};

// Get summary of piece status (e.g. "Random piece" or e.g. "Piece 5 of 13")
PieceSelector.prototype.getStatusString = function () {
  if (m_isReadingFromSequence) {
    return "Piece " + (m_readIndex + 1) + " of " + m_pieceSequenceStr.length;
  }
  return "Random piece";
};

/**
  "Private" functions - unused outside of this file
  */

PieceSelector.prototype.getPresetPiece = function () {
  const nextPieceId = m_pieceSequenceStr[m_readIndex];
  const nextPieceData = PIECE_LOOKUP[nextPieceId];
  m_readIndex += 1;

  // Check if we've reached the end of the sequence
  if (m_readIndex >= m_pieceSequenceStr.length) {
    m_isReadingFromSequence = false;
    m_readIndex = 0;
  }

  return nextPieceData;
};

// Get a random piece, following the original RNG of NES tetris
PieceSelector.prototype.getRandomPiece = function (previousPieceId) {
  // Roll once 0-7, where 7 is a dummy value
  let r = Math.floor(Math.random() * (PIECE_LIST.length + 1));
  if (r == PIECE_LIST.length || previousPieceId === PIECE_LIST[r][2]) {
    // Reroll once for repeats (or dummy) to reduce repeated pieces
    r = Math.floor(Math.random() * PIECE_LIST.length);
  }
  const nextPieceData = PIECE_LIST[r];
  return nextPieceData;
};
