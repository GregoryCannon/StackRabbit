import { PIECE_LIST, PIECE_LOOKUP } from "./tetrominoes.js";
const GameSettings = require("./game_settings_manager");

let m_pieceSequenceStr = "";
let m_readIndex = 0;
let m_isReadingFromSequence = false;

export function PieceSelector() {}

/**
  Public functions
  */

PieceSelector.prototype.startReadingPieceSequence = function () {
  // Get piece sequence (with spaces trimmed)
  m_pieceSequenceStr = GameSettings.GetPieceSequence();

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

/**
 * Get summary of piece status (e.g. "Random piece" or e.g. "Piece 5 of 13"),
 * split over two lines.
 */
PieceSelector.prototype.getStatusDisplay = function () {
  if (m_isReadingFromSequence) {
    return ["Piece ", m_readIndex + 1 + "/" + m_pieceSequenceStr.length];
  }
  return ["Random", "Piece"];
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
  const tempPieceId = r !== PIECE_LIST.length ? PIECE_LIST[r][2] : "";
  console.log(tempPieceId);
  if (
    r == PIECE_LIST.length || // Re-roll dummy value
    tempPieceId === previousPieceId || // Re-roll repeat piece
    (GameSettings.ShouldReduceLongBars() && tempPieceId == "I") // Re-roll I pieces when drought mode on
  ) {
    // Reroll once for repeats (or dummy) to reduce repeated pieces
    r = Math.floor(Math.random() * PIECE_LIST.length);
  }
  const nextPieceData = PIECE_LIST[r];
  return nextPieceData;
};
