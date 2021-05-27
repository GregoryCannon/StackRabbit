const pasteAreaElement = document.getElementById("paste-area");
const pastedImageElement = document.getElementById("pasted-image");

import { NUM_ROW, NUM_COLUMN, VACANT } from "./constants.js";
import { SquareState } from "./constants.js";

let m_loadedStateFromImage = false;
let m_loadedBoard = [];

export function BoardLoader(board, canvas) {
  this.board = board;
  this.canvas = canvas;

  setUpPasteability(this);
}

BoardLoader.prototype.resetBoard = function () {
  // Reload the board from the image, or reset the board
  // (have to iterate manually (not use this.board = ) to preserve the board reference that's passed around to all the files
  for (let r = 0; r < NUM_ROW; r++) {
    for (let c = 0; c < NUM_COLUMN; c++) {
      this.board[r][c] = m_loadedStateFromImage
        ? m_loadedBoard[r][c]
        : SquareState.EMPTY;
    }
  }
};

// Get whether the board has been loaded from an image
BoardLoader.prototype.didLoadBoardStateFromImage = function () {
  return m_loadedStateFromImage;
};

BoardLoader.prototype.getBoardStateFromImage = function (img) {
  var dummy_canvas = document.getElementById("dummy-canvas");
  var context = dummy_canvas.getContext("2d");
  dummy_canvas.width = img.width;
  dummy_canvas.height = img.height;
  context.drawImage(img, 0, 0);
  this.resetBoard();

  const cropOffset = -0.1;
  const SQ = (img.height / 20 + img.width / 10) / 2 + cropOffset;
  const rgbEmptyThreshold = 30; // If all three channels are <30/255, then the cell is "empty"

  // Iterate over the image and read the square colors into the board
  for (let c = 0; c < NUM_COLUMN; c++) {
    for (let r = 0; r < NUM_ROW; r++) {
      const x = Math.round((c + 0.5) * SQ);
      const y = Math.round((r + 0.5) * SQ);
      const pixelData = context.getImageData(x, y, 1, 1).data;
      if (
        Math.max(pixelData[0], pixelData[1], pixelData[2]) > rgbEmptyThreshold
      ) {
        context.fillStyle = "RED";
        this.board[r][c] = SquareState.COLOR2;
      } else {
        context.fillStyle = "GREEN";

        this.board[r][c] = SquareState.EMPTY;
      }
      context.fillRect(x, y, 3, 3);
    }
  }

  // Edit out the currently falling piece from the boardstate
  clearFloatingPiece(this.board, context, SQ);
  m_loadedBoard = JSON.parse(JSON.stringify(this.board)); // Save a copy of the loaded board
  this.canvas.drawBoard();
  m_loadedStateFromImage = true;

  // Hide the dummy canvas afterwards
  setTimeout(() => {
    dummy_canvas.style.display = "none";
  }, 3000);
};

// Remove the piece from midair when loading a board from a screenshot
function clearFloatingPiece(board, dummyContext, SQ) {
  // Start from the bottom, look for an empty row, and then clear all rows above that
  let startedClearing = false;
  for (let r = NUM_ROW - 1; r >= 0; r--) {
    if (startedClearing) {
      for (let c = 0; c < NUM_COLUMN; c++) {
        board[r][c] = SquareState.EMPTY;
      }
    } else {
      let rowEmpty = true;
      for (let c = 0; c < NUM_COLUMN; c++) {
        if (board[r][c] != SquareState.EMPTY) {
          rowEmpty = false;
          break;
        }
      }
      if (rowEmpty) {
        startedClearing = true;

        dummyContext.fillStyle = "BLACK";
        dummyContext.fillRect(0, 0, SQ * NUM_COLUMN, r * SQ);
      }
    }
  }
}

function setUpPasteability(boardLoaderThis) {
  // When an image is pasted, get the board state from it
  pasteAreaElement.onpaste = function (event) {
    // use event.originalEvent.clipboard for newer chrome versions
    var items = (event.clipboardData || event.originalEvent.clipboardData)
      .items;
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
      reader.onload = function (event) {
        pastedImageElement.onload = function () {
          boardLoaderThis.getBoardStateFromImage(pastedImageElement);
        };
        pastedImageElement.src = event.target.result;
      };
      reader.readAsDataURL(blob);
    }
  };
}
