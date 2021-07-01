const engineTable = document.getElementById("engine-table");

export function EngineAnalysisManager(board) {
  this.board = board;
  this.reactionTime = 5;
  this.loadResponse(testResponseObj);
}

const testResponseObj = [
  {
    piece: "O",
    inputSequence: "R.............L************",
    totalValue: 69.8837965829672,
    isSpecialMove: true,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".........L************",
        totalValue: 53.62217945138146,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 53.722179451381464,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "R.R.R.R.......**************",
    totalValue: -15.923681102753307,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".L.L.....L************",
        totalValue: 53.62217945138146,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 53.722179451381464,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "L.........****************",
    totalValue: -37.53993578375008,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: "R.R......L************",
        totalValue: 53.62217945138146,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 53.722179451381464,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "R..............************",
    totalValue: 0.02033909547512991,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".........L************",
        totalValue: 53.62217945138146,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 53.722179451381464,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "L.L.L.....****************",
    totalValue: -38.76164272102713,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".R.R.****************",
        totalValue: -16.497850485002274,
        isSpecialMove: false,
        followUp: {
          piece: "T",
          inputSequence: "I.A...........L.************",
          isSpecialMove: true,
          totalValue: -16.497850485002274,
        },
      },
      {
        piece: "O",
        inputSequence: ".....****************",
        totalValue: -19.347708987316523,
        isSpecialMove: false,
        followUp: {
          piece: "T",
          inputSequence: "I.A...........L.************",
          isSpecialMove: true,
          totalValue: -19.347708987316523,
        },
      },
    ],
  },
];

/** Runs an animation to clear the lines passed in in an array.
 * Doesn't affect the actual board, those updates come at the end of the animation. */
EngineAnalysisManager.prototype.loadResponse = function (moveList) {
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
      adjMove.innerHTML = getNotatedMove(
        adjustment.piece,
        mainMove.inputSequence.slice(0, this.reactionTime) +
          adjustment.inputSequence,
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
