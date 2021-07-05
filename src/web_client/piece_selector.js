import { PIECE_LIST, PIECE_LOOKUP } from "./tetrominoes.js";
const GameSettings = require("./game_settings_manager");

/**
 * A class that handles the piece selection logic.
 * Pregenerates a list of 2000 pieces at the start of the game, which can start out with
 * a custon piece sequence if the user has opted to do that.
 */
export function PieceSelector() {
  this.sequence = []; // Array of piece IDs
  this.startOfRandomSequence = 0; // The first index that is randomly generated, i.e. not part of a custom inputted sequence
  this.readIndex = 0;
}

/**
  Public functions
  */

PieceSelector.prototype.generatePieceSequence = function () {
  // Get piece sequence (with spaces trimmed)
  const pieceSequenceStr = GameSettings.getPieceSequence();

  let writeIndex = 0;

  // Copy the custom sequence into the main sequence
  if (pieceSequenceStr.length > 0) {
    for (let i = 0; i < pieceSequenceStr.length; i++) {
      this.sequence[i] = pieceSequenceStr.charAt(i);
      writeIndex++;
    }
  }

  // Fill the rest with a random sequence
  while (writeIndex < 2000) {
    const prevPieceId = writeIndex == 0 ? null : this.sequence[writeIndex - 1];
    this.sequence[writeIndex] = getRandomPiece(prevPieceId);
    writeIndex++;
  }

  this.readIndex = 0;
  this.startOfRandomSequence = pieceSequenceStr.length;
};

PieceSelector.prototype.getReadIndex = function () {
  return this.readIndex;
};

PieceSelector.prototype.setReadIndex = function (value) {
  this.readIndex = value;
};

// Get the next piece, whether that be specified or random
PieceSelector.prototype.getNextPiece = function () {
  const nextPieceId = this.sequence[this.readIndex];
  this.readIndex++;
  return PIECE_LOOKUP[nextPieceId];
};

/**
 * Get summary of piece status (e.g. "Random piece" or e.g. "Piece 5 of 13"),
 * split over two lines.
 */
PieceSelector.prototype.getStatusDisplay = function () {
  if (this.readIndex < this.startOfRandomSequence) {
    // The piece number equals the read index since the read index is always pointing to the piece *after* the one on screen
    return ["Piece ", this.readIndex + "/" + this.startOfRandomSequence];
  }
  return [];
};

/**
  "Private" functions - unused outside of this file
  */

// Get the ID of a random piece, following the original RNG of NES tetris
function getRandomPiece(previousPieceId) {
  // Roll once 0-7, where 7 is a dummy value
  let r = Math.floor(Math.random() * (PIECE_LIST.length + 1));
  const tempPieceId = r !== PIECE_LIST.length ? PIECE_LIST[r][2] : "";
  if (
    r == PIECE_LIST.length || // Re-roll dummy value
    tempPieceId === previousPieceId || // Re-roll repeat piece
    (GameSettings.shouldReduceLongBars() && tempPieceId == "I") // Re-roll I pieces when drought mode on
  ) {
    // Reroll once for repeats (or dummy) to reduce repeated pieces
    r = Math.floor(Math.random() * PIECE_LIST.length);
  }
  const nextPieceData = PIECE_LIST[r];
  return nextPieceData[2]; // Return the string ID of the new piece
}
