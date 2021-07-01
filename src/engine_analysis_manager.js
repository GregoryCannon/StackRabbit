import { GetLevel, GetLines } from ".";

const engineTable = document.getElementById("engine-table");
const curPieceSelect = document.getElementById("engine-cur-piece");
const nextPieceSelect = document.getElementById("engine-next-piece");
const tapSpeedSelect = document.getElementById("engine-tap-speed");
const reactionTimeSelect = document.getElementById("engine-reaction-time");

export function EngineAnalysisManager(board) {
  this.board = board;
}

EngineAnalysisManager.prototype.makeRequest = function () {
  // Compile arguments
  const encodedBoard = this.board.map((row) => row.join("")).join("");
  console.log(curPieceSelect, curPieceSelect.value);
  const curPiece = curPieceSelect.value;
  const nextPiece = nextPieceSelect.value;
  this.reactionTime = reactionTimeSelect.value;
  const tapSpeed = tapSpeedSelect.value;
  const url = `http://localhost:3000/engine/${encodedBoard}/${curPiece}/${
    nextPiece || null
  }/${GetLevel()}/${GetLines()}/0/0/0/0/${this.reactionTime}/${tapSpeed}/false`;

  // Make request
  fetch(url, { mode: "cors" })
    .then(function (response) {
      console.log(response);
      return response.json();
    })
    .then(
      function (text) {
        console.log(text.length, text);
        console.log("Request successful", text);
        this.loadResponse(text);
      }.bind(this)
    )
    .catch(function (error) {
      console.log("Request failed", error);
    });
};

/** Runs an animation to clear the lines passed in in an array.
 * Doesn't affect the actual board, those updates come at the end of the animation. */
EngineAnalysisManager.prototype.loadResponse = function (moveList) {
  engineTable.innerHTML = "";
  for (let i = 0; i < moveList.length; i++) {
    const mainMove = moveList[i];

    // Create a row for the default move
    let spacer = engineTable.insertRow();
    spacer.classList.add("table-spacer");
    let row = engineTable.insertRow();
    row.classList.add("default-move");

    // Fill the default placement row
    let ranking = row.insertCell();
    ranking.classList.add("ranking");
    ranking.innerHTML = i + 1 + ")";
    let evalScore = row.insertCell();
    evalScore.classList.add("eval-score");
    evalScore.innerHTML = mainMove.totalValue.toFixed(1);
    let move = row.insertCell();
    move.colSpan = 2;
    move.classList.add("notated-move");
    move.innerHTML = getNotatedMove(
      mainMove.piece,
      mainMove.inputSequence,
      mainMove.isSpecialMove
    );

    // Fill in the adjustment rows
    for (const adjustment of mainMove.adjustments) {
      let adjRow = engineTable.insertRow();
      adjRow.classList.add("adjustment");

      // Fill the default placement row
      adjRow.insertCell();
      let adjScore = adjRow.insertCell();
      adjScore.classList.add("eval-score-adj");
      adjScore.innerHTML = adjustment.totalValue.toFixed(1);
      let adjMove = adjRow.insertCell();
      adjMove.classList.add("notated-adj");
      if (
        mainMove.inputSequence.slice(this.reactionTime) ===
        adjustment.inputSequence
      ) {
        adjMove.innerHTML = "(no adj.)";
      } else {
        adjMove.innerHTML = getNotatedMove(
          adjustment.piece,
          mainMove.inputSequence.slice(0, this.reactionTime) +
            adjustment.inputSequence,
          adjustment.isSpecialMove
        );
      }

      let nextMove = adjRow.insertCell();
      nextMove.classList.add("notated-next");
      nextMove.innerHTML = getNotatedMove(
        adjustment.followUp.piece,
        adjustment.followUp.inputSequence,
        adjustment.followUp.isSpecialMove
      );
    }
  }
};

function isAnyOf(testChar, candidates) {
  for (const loopChar of candidates) {
    if (testChar === loopChar) {
      return true;
    }
  }
  return false;
}

const ROTATION_LETTER_LOOKUP = {
  I: ["", ""],
  O: [""],
  L: ["d", "l", "u", "r"],
  J: ["d", "r", "u", "l"],
  T: ["d", "l", "u", "r"],
  S: ["", ""],
  Z: ["", ""],
};

const PIECE_WIDTH_LOOKUP = {
  I: [4, 1],
  O: [2],
  L: [3, 2, 3, 2],
  J: [3, 2, 3, 2],
  T: [3, 2, 3, 2],
  S: [3, 2],
  Z: [3, 2],
};

const LEFTMOST_COL_LOOKUP = {
  I: [4, 6],
  O: [5],
  L: [5, 5, 5, 6],
  J: [5, 5, 5, 6],
  T: [5, 5, 5, 6],
  S: [5, 6],
  Z: [5, 6],
};

function getNotatedMove(pieceStr, inputSequence, isSpecialMove) {
  let rotationIndex = 0;
  let shiftIndex = 0;
  for (const inputChar of inputSequence) {
    if (isAnyOf(inputChar, "AEI")) {
      rotationIndex++;
    }
    if (isAnyOf(inputChar, "BFG")) {
      rotationIndex--;
    }
    if (isAnyOf(inputChar, "LEF")) {
      shiftIndex--;
    }
    if (isAnyOf(inputChar, "RIG")) {
      shiftIndex++;
    }
  }

  const finalRotation =
    (rotationIndex + 4) % PIECE_WIDTH_LOOKUP[pieceStr].length;
  const rotationLetter = ROTATION_LETTER_LOOKUP[pieceStr][finalRotation];
  const leftMostCol = LEFTMOST_COL_LOOKUP[pieceStr][finalRotation] + shiftIndex;
  let colsStr = "";
  for (let i = 0; i < PIECE_WIDTH_LOOKUP[pieceStr][finalRotation]; i++) {
    colsStr += (leftMostCol + i).toString().slice(-1);
  }
  return `${pieceStr} ${rotationLetter}${colsStr}${isSpecialMove ? "*" : ""}`;
}

console.log(getNotatedMove("T", "E...E...L...L.......********", true));
