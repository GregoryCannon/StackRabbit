const pieceListElement = document.getElementById("piece-sequence");

import { PIECE_LIST } from "./tetrominoes.js";
import { Piece } from "./piece.js";

let m_pieceString = "";

export function PieceSelector(board, canvas, onGameOver) {
  this.board = board;
  this.canvas = canvas;
  this.onGameOver = onGameOver;
}

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

// Get a new random piece. Will soon allow for inputted piece sequences
PieceSelector.prototype.chooseNextPiece = function(currentPieceId) {
  return this.randomPiece(currentPieceId);
};
