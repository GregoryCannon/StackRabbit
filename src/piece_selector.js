const pieceListElement = document.getElementById("piece-sequence");
const pasteAreaElement = document.getElementById("paste-area");

import { PIECE_LIST, PIECE_LOOKUP } from "./tetrominoes.js";
import { Piece } from "./piece.js";

// Read piece string from input box
let m_pieceString = "";
let m_readIndex = -1;
pieceListElement.addEventListener("input", function(event) {
  m_pieceString = this.value;
  m_readIndex = 0;
});

pasteAreaElement.onpaste = function(event) {
  // use event.originalEvent.clipboard for newer chrome versions
  var items = (event.clipboardData || event.originalEvent.clipboardData).items;
  console.log(JSON.stringify(items)); // will give you the mime types
  // find pasted image among pasted items
  var blob = null;
  for (var i = 0; i < items.length; i++) {
    if (items[i].type.indexOf("image") === 0) {
      blob = items[i].getAsFile();
    }
  }
  // load image if there is a pasted image
  if (blob !== null) {
    var reader = new FileReader();
    reader.onload = function(event) {
      console.log(event.target.result); // data url!
      document.getElementById("pastedImage").src = event.target.result;
    };
    reader.readAsDataURL(blob);
  }
};

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
  // If there is a next specified piece, select that
  if (m_readIndex != -1 && m_readIndex < m_pieceString.length) {
    return this.presetPiece();
  }
  // Otherwise pick one randomly
  return this.randomPiece(currentPieceId);
};
