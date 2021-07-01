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
    totalValue: 84.08143923062248,
    isSpecialMove: true,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".........L************",
        totalValue: 64.88571919154421,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 64.88571919154421,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "R.R.R.R.......**************",
    totalValue: 19.507274064433354,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".L.L.....L************",
        totalValue: 64.88571919154421,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 64.88571919154421,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "L.........****************",
    totalValue: -3.4830204706027224,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: "R.R......L************",
        totalValue: 64.88571919154421,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 64.88571919154421,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "R..............************",
    totalValue: -5.296564748843279,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".........L************",
        totalValue: 64.88571919154421,
        isSpecialMove: true,
        followUp: {
          piece: "T",
          inputSequence: "E..........**************",
          isSpecialMove: false,
          totalValue: 64.88571919154421,
        },
      },
    ],
  },
  {
    piece: "O",
    inputSequence: "L.L.L.....****************",
    totalValue: -10.346661578783154,
    isSpecialMove: false,
    adjustments: [
      {
        piece: "O",
        inputSequence: ".R.R.****************",
        totalValue: 1.5019318318881372,
        isSpecialMove: false,
        followUp: {
          piece: "T",
          inputSequence: "I.A...........L.************",
          isSpecialMove: true,
          totalValue: 1.5019318318881372,
        },
      },
      {
        piece: "O",
        inputSequence: ".....****************",
        totalValue: -2.78064232737919,
        isSpecialMove: false,
        followUp: {
          piece: "T",
          inputSequence: "I.A...........L.************",
          isSpecialMove: true,
          totalValue: -2.78064232737919,
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
      if (mainMove.inputSequence.slice(this.reactionTime) === adjustment.inputSequence){
        adjMove.innerHTML = "(no adj.)"
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
}


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
  const rotationLetter = ROTATION_LETTER_LOOKUP[pieceStr][finalRotation];
  const leftMostCol = (pieceStr === "I" ? 4 : 5) + shiftIndex;
  let colsStr = "";
  for (let i = 0; i < PIECE_WIDTH_LOOKUP[pieceStr][finalRotation]; i++) {
    colsStr += (leftMostCol + i).toString().slice(-1);
  }
  return `${pieceStr} ${rotationLetter}${colsStr}${isSpecialMove ? "*" : ""}`;
}

console.log(getNotatedMove("T", "E...E...L...L.......********", true));
