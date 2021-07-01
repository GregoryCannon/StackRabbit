const engineTable = document.getElementById("engine-table");

export function EngineAnalysisManager(board) {
  this.board = board;
  this.reactionTime = 21;
  this.loadResponse(testResponseObj);
}

const testResponseObj = [
  {
    piece: "T",
    inputSequence: "I...R...R...R....................********",
    isSpecialMove: false,
    totalValue: 31.424,
    adjustments: [
      {
        piece: "T",
        inputSequence: "..B..........***********",
        totalValue: 33.71,
        isSpecialMove: true,
        followUp: {
          piece: "L",
          inputSequence: "A...A...........**********",
          totalValue: 29.411,
        },
      },
      {
        piece: "T",
        inputSequence: ".............***********",
        totalValue: 32.589,
        isSpecialMove: false,
        followUp: {
          piece: "L",
          inputSequence: "A...A...........**********",
          totalValue: 28.395,
        },
      },
    ],
  },
  {
    piece: "T",
    inputSequence: "E...E...L...L......................********",
    isSpecialMove: false,
    totalValue: 26.335,
    adjustments: [
      {
        piece: "T",
        inputSequence: ".............***********",
        isSpecialMove: false,
        totalValue: 30.689,
        followUp: {
          piece: "L",
          inputSequence: "A...A...........**********",
          totalValue: 28.395,
        },
      },
    ],
  },
];

/** Runs an animation to clear the lines passed in in an array.
 * Doesn't affect the actual board, those updates come at the end of the animation. */
EngineAnalysisManager.prototype.loadResponse = function (moveList) {
  for (let i = 0; i < moveList.length; i++){
    const mainMove = moveList[i];

    // Create a row for the default move
    let spacer = engineTable.insertRow();
    spacer.classList.add("table-spacer")
    let row = engineTable.insertRow();
    row.classList.add("default-move");

    // Fill the default placement row
    let ranking = row.insertCell();
    ranking.classList.add("ranking");
    ranking.innerHTML = (i+1) + ")";
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
    for (const adjustment of mainMove.adjustments){
      let adjRow = engineTable.insertRow();
      adjRow.classList.add("adjustment");
  
      // Fill the default placement row
      adjRow.insertCell();
      let adjScore = adjRow.insertCell();
      adjScore.classList.add("eval-score-adj");
      adjScore.innerHTML = adjustment.totalValue.toFixed(1);
      let adjMove = adjRow.insertCell();
      adjMove.classList.add("notated-adj");
      adjMove.innerHTML = getNotatedMove(
        adjustment.piece,
        mainMove.inputSequence.slice(0, this.reactionTime) + adjustment.inputSequence,
        adjustment.isSpecialMove
      );
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
  0: "",
  1: "r",
  2: "d",
  3: "l",
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
  const rotationLetter = ROTATION_LETTER_LOOKUP[finalRotation];
  const leftMostCol = (pieceStr === "I" ? 4 : 5) + shiftIndex;
  let colsStr = "";
  for (let i = 0; i < PIECE_WIDTH_LOOKUP[pieceStr][finalRotation]; i++) {
    colsStr += (leftMostCol + i).toString().slice(-1);
  }
  return `${pieceStr} ${rotationLetter}${colsStr}${isSpecialMove ? "*" : ""}`;
}

console.log(getNotatedMove("T", "E...E...L...L.......********", true));
