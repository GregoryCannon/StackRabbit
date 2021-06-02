export const NUM_ROW = 20;
export const NUM_COLUMN = 10;
export const SquareState = Object.freeze({
  EMPTY: 0,
  FULL: 1,
});

export const POSSIBLE_NEXT_PIECES: Array<PieceId> = [
  "I",
  "O",
  "L",
  "J",
  "T",
  "S",
  "Z",
];

export function GetGravity(level) {
  const GRAVITY = {
    0: 48,
    1: 43,
    2: 38,
    3: 33,
    4: 28,
    5: 23,
    6: 18,
    7: 13,
    8: 8,
    9: 6,
    10: 5,
    11: 5,
    12: 5,
    13: 4,
    14: 4,
    15: 4,
    16: 3,
    17: 3,
    18: 3,
    19: 2,
    29: 1,
  };
  if (level <= 18) {
    return GRAVITY[level];
  } else if (level < 29) {
    return 2;
  } else {
    return 1;
  }
}

export function formatPossibility(possibility: PossibilityChain) {
  if (possibility == null) {
    return "No legal moves";
  }
  return (
    possibility.placement[0] +
    "," +
    possibility.placement[1] +
    "|" +
    (possibility.inputSequence || "none") +
    "|" +
    possibility.boardAfter.map((row) => row.join("")).join("") +
    "|" +
    possibility.searchStateAfterMove.level +
    "|" +
    possibility.searchStateAfterMove.lines
  );
}

const FRAME_WITH_INPUT = "X";
const FRAME_WAITING = ".";

/** Check whether a given frame is an input frame in the frame timeline.
 * @param frameNum - the index of the current frame (0-INDEXED)!
 */
export function shouldPerformInputsThisFrame(
  inputFrameTimeline: string,
  frameNum: number
) {
  const len = inputFrameTimeline.length;
  const index = frameNum % len;
  return inputFrameTimeline[index] === FRAME_WITH_INPUT;
}

/** Given an array specifying the number of frames to wait between shifts, generate a string of which frames will contain shifts and which ones are waiting in between.
 * The string, like itself, will be assumed to be repeated. So "X..X." indicates that the behavior will be "X..X.X..X.X.." and so on.
 * e.g. [2,1] -> X..X.
 */
export function generateInputFrameTimeline(delaySequence: Array<number>) {
  if (delaySequence.length == 0) {
    throw new Error("Empty delay sequence");
  }
  let inputFrameTimeline = "";
  for (const delayLength of delaySequence) {
    inputFrameTimeline += FRAME_WITH_INPUT;
    for (let i = 0; i < delayLength; i++) {
      inputFrameTimeline += FRAME_WAITING;
    }
  }
  return inputFrameTimeline;
}

export function generateDigPracticeBoard(garbageHeight, numHoles) {
  const randomIntLessThan = (n) => Math.floor(Math.random() * n);

  // Create board
  const board = [];
  for (let row = 0; row < NUM_ROW; row++) {
    board.push([]);
    for (let col = 0; col < NUM_COLUMN; col++) {
      const height = NUM_ROW - row;
      board[row][col] =
        height < garbageHeight ||
        (height == garbageHeight && randomIntLessThan(2))
          ? SquareState.FULL
          : SquareState.EMPTY;
    }
  }

  // Clear col 10
  for (let row = NUM_ROW - garbageHeight; row < NUM_ROW; row++) {
    board[row][NUM_COLUMN - 1] = SquareState.EMPTY;
  }

  // Add holes
  for (let i = 0; i < numHoles; i++) {
    const holeRow = NUM_ROW - 1 - randomIntLessThan(garbageHeight - 1);
    const holeCol = randomIntLessThan(NUM_COLUMN - 1);
    board[holeRow][holeCol] = SquareState.EMPTY;
  }
  return board;
}

/** Returns true if the specified cell 1) exists and 2) is empty */
function isEmptyCell(row, col, board) {
  if (col < 0 || col >= NUM_COLUMN || row < 0 || row >= NUM_ROW) {
    return false;
  }
  return board[row][col] == SquareState.EMPTY;
}

/** Detects holes that could potentially be tucked into */
export function isTuckSetup(row, col, board, heights) {
  if (isEmptyCell(row, col + 1, board) && isEmptyCell(row, col + 2, board)) {
    return true;
  }
  if (isEmptyCell(row, col - 1, board) && isEmptyCell(row, col - 2, board)) {
    return true;
  }
  return false;
}

export function getSurfaceArrayAndHoles(
  board: Board
): [Array<number>, number, Array<CellLocation>] {
  const heights = [];
  let numHoles = 0;
  let holeCells: Array<CellLocation> = [];

  // Get the column heights first
  for (let col = 0; col < NUM_COLUMN; col++) {
    let row = 0;
    while (row < NUM_ROW && board[row][col] == 0) {
      row++;
    }
    heights.push(20 - row);
  }
  // Then look for holes
  for (let col = 0; col < NUM_COLUMN; col++) {
    let row = 20 - heights[col];
    while (row < NUM_ROW - 1) {
      row++;
      if (board[row][col] === SquareState.EMPTY && col < NUM_COLUMN - 1) {
        // if (isTuckSetup(row, col, board, heights)) {
        //   numHoles += 0.8;
        //   holeCells.push([row, col]);
        //   while (row < NUM_ROW && board[row][col] === SquareState.EMPTY) {
        //     numHoles += 0.2;
        //     holeCells.push([row, col]);
        //     row++;
        //   }
        //   break;
        // }

        // Add a hole if it's anywhere other than column 10
        numHoles++;
        holeCells.push([row, col]);
        row++;
        // Add fractional holes for subsequent cells within a tall hole
        while (row < NUM_ROW && board[row][col] === SquareState.EMPTY) {
          numHoles += 0.2;
          holeCells.push([row, col]);
          row++;
        }
      }
    }
  }
  return [heights, numHoles, holeCells];
}

/**
 * Makes a copy of a surface that's corrected for height gaps that are to high.
 * e.g. an increase of 7 between two columns would be treated as an increase of 4
 * (for surface rating purposes only)
 * @param {*} surfaceArray
 */
export function correctSurfaceForExtremeGaps(
  initialArray
): [Array<number>, number] {
  const newArray = [];
  for (const elt of initialArray) {
    newArray.push(elt);
  }

  let totalExcessHeight = 0; // Accumulator for the excess height trimmed away. Used for evaluation purposes.

  for (let i = 1; i < newArray.length; i++) {
    const diffFromPrev = newArray[i] - newArray[i - 1];
    if (Math.abs(diffFromPrev) > 4) {
      const correctionFactor = Math.abs(diffFromPrev) - 4; // The amount that it overshot by (always positive)
      for (let j = i; j < newArray.length; j++) {
        newArray[j] +=
          diffFromPrev > 0 ? -1 * correctionFactor : correctionFactor;
      }
      totalExcessHeight += correctionFactor;
    }
  }
  return [newArray, totalExcessHeight];
}

export function getHoleCount(board) {
  return getSurfaceArrayAndHoles(board)[1];
}

export function getLineCountOfFirstTransition(startingLevel) {
  if (startingLevel < 10) {
    return (startingLevel + 1) * 10;
  } else if (startingLevel <= 15) {
    return 100;
  } else {
    return (startingLevel - 5) * 10;
  }
}

/** Checks if clearing a certain number of lines will increase the level, and if so what that level is.
 * NOTE: This assumes level 18, 19, or 29 starts.
 */
export function getLevelAfterLineClears(level, lines, numLinesCleared) {
  // Once you go killscreen you never go back
  if (level >= 29) {
    return 29;
  }
  const newLineCount = lines + numLinesCleared;
  if (newLineCount < 130) {
    return Math.max(level, 18);
  } else if (newLineCount < 230) {
    return 19 + Math.floor((newLineCount - 130) / 10);
  } else {
    return 29;
  }
}

export function parseBoard(boardStr: string): Board {
  return boardStr
    .match(/.{1,10}/g) // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split("").map((x) => parseInt(x)));
}

export function getScareHeight(level: number, aiParams: AiParams) {
  if (!aiParams.MAX_5_TAP_LOOKUP) {
    throw new Error("No tap heights calculated when looking up scare height");
  }
  const max5TapHeight = aiParams.MAX_5_TAP_LOOKUP[level];
  return Math.max(0, max5TapHeight + aiParams.SCARE_HEIGHT_OFFSET);
}

export function logBoard(board) {
  console.log(" -- Board start -- ");
  for (let r = 0; r < NUM_ROW; r++) {
    let rowStr = "";
    for (let c = 0; c < NUM_COLUMN; c++) {
      rowStr += board[r][c];
    }
    console.log(rowStr.replace(/0/g, "."));
  }
}

// O(n) time & O(n) space
export function mergeSortedArrays(arr1, arr2, compareFunc) {
  let merged = [];
  let index1 = 0;
  let index2 = 0;
  let indexMerged = 0;

  while (indexMerged < arr1.length + arr2.length) {
    let isArr1Depleted = index1 >= arr1.length;
    let isArr2Depleted = index2 >= arr2.length;

    if (
      !isArr1Depleted &&
      (isArr2Depleted || compareFunc(arr1[index1], arr2[index2]) < 0)
    ) {
      merged[indexMerged] = arr1[index1];
      index1++;
    } else {
      merged[indexMerged] = arr2[index2];
      index2++;
    }

    indexMerged++;
  }

  return merged;
}

export function cloneBoard(board) {
  const newBoard = [];
  for (let row = 0; row < NUM_ROW; row++) {
    const newRow = [];
    for (let col = 0; col < NUM_COLUMN; col++) {
      newRow.push(board[row][col]);
    }
    newBoard.push(newRow);
  }
  return newBoard;
}
