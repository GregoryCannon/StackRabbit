import {
  CAN_TUCK,
  DEBUG_DOUBLE_KS_ALWAYS_ENABLED,
  DOUBLE_KILLSCREEN_ENABLED,
  IS_PAL,
  WELL_COLUMN,
} from "./params";
let performance = null;

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

export function IsGravityDoubled(level) {
  return (
    DEBUG_DOUBLE_KS_ALWAYS_ENABLED || (DOUBLE_KILLSCREEN_ENABLED && level >= 39)
  );
}

export function GetDoubleKillscreenEquivalentInputTimeline(timeline: string) {
  return timeline.split("").join(".") + ".";
}

export function GetGravity(level) {
  if (IS_PAL) {
    return GetGravityPAL(level);
  }
  const NSTC_GRAVITY = {
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
    return NSTC_GRAVITY[level];
  } else if (level < 29) {
    return 2;
  } else {
    return 1;
  }
}

export function GetGravityPAL(level) {
  const PAL_GRAVITY = {
    0: 36,
    1: 32,
    2: 29,
    3: 25,
    4: 22,
    5: 18,
    6: 15,
    7: 11,
    8: 7,
    9: 5,
    10: 4,
    11: 4,
    12: 4,
    13: 3,
    14: 3,
    15: 3,
    16: 2,
    17: 2,
    18: 2,
    19: 1,
    29: 1,
  };
  if (level <= 18) {
    return PAL_GRAVITY[level];
  } else {
    return 1;
  }
}

export function pushDown(inputString: string) {
  //I....R....R....R......................*****^^^^^^^^^^^^^^^^^*****
  let preLock = inputString.split("*")[0];
  let fallenFrames = 0;
  for (let i = preLock.length - 1; i > 0; i--) {
    if (preLock.charAt(i) !== ".") {
      break;
    }
    fallenFrames += 1;
  }
  preLock = preLock.substring(0, preLock.length - fallenFrames);
  for (let i = 0; i < Math.ceil(0.666 * fallenFrames) + 2; i++) {
    preLock += "D";
  }
  return preLock;
}

export function formatPossibility(
  possibility: PossibilityChain,
  shouldPushDown = false
) {
  if (possibility == null) {
    return "No legal moves";
  }
  return (
    possibility.placement[0] +
    "," +
    possibility.placement[1] +
    "," +
    possibility.placement[2] +
    "|" +
    (shouldPushDown && possibility.searchStateAfterMove.level == 18
      ? pushDown(possibility.inputSequence)
      : possibility.inputSequence || "none") +
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
function isEmptyCellAboveStack(row, col, board, surfaceArray) {
  if (col < 0 || col >= NUM_COLUMN || row < 0 || row >= NUM_ROW) {
    return false;
  }
  return (
    board[row][col] == SquareState.EMPTY &&
    NUM_ROW - row - 1 === surfaceArray[col]
  );
}

/** Detects holes that could potentially be tucked into */
export function isTuckSetup(row, col, board, surfaceArray): [boolean, number] {
  if (!CAN_TUCK) {
    return [false, 0];
  }
  if (
    isEmptyCellAboveStack(row, col + 1, board, surfaceArray) &&
    isEmptyCellAboveStack(row, col + 2, board, surfaceArray)
  ) {
    let cellsOfSpace = 2;
    let i = col + 3;
    while (isEmptyCellAboveStack(row, i, board, surfaceArray)) {
      cellsOfSpace++;
      i++;
    }
    return [true, cellsOfSpace];
  }
  if (
    isEmptyCellAboveStack(row, col - 1, board, surfaceArray) &&
    isEmptyCellAboveStack(row, col - 2, board, surfaceArray)
  ) {
    let cellsOfSpace = 2;
    let i = col - 3;
    while (isEmptyCellAboveStack(row, i, board, surfaceArray)) {
      cellsOfSpace++;
      i--;
    }
    return [true, cellsOfSpace];
  }
  // console.log(surfaceArray.join(","), "false")
  return [false, 0];
}

export function countHolesInColumn(
  col: number,
  board: Board,
  surfaceArray: Array<number>,
  holeCells: Set<number>,
  isWell: boolean
) {
  let numHolesSeen = 0;
  let curHoleHeight = 0;
  let numHoles = 0;
  for (let row = NUM_ROW - 1; row >= NUM_ROW - surfaceArray[col]; row--) {
    if (board[row][col] === SquareState.EMPTY) {
      curHoleHeight++;
    } else if (curHoleHeight > 0) {
      // Add a hole for the one we've tracked so far.
      numHolesSeen++;

      // Ignore the first hole in the well column (the well itself)
      if (isWell && numHolesSeen === 1) {
        curHoleHeight = 0;
        continue;
      }

      const [tuckSetup, cellsOfSpace] = isTuckSetup(
        row + curHoleHeight,
        col,
        board,
        surfaceArray
      );
      if (tuckSetup) {
        const spaceMultiplier = Math.max(0.2, 1 - 0.3 * (cellsOfSpace - 2));
        numHoles += spaceMultiplier * curHoleHeight;
        // console.log(col, surfaceArray.join(","), numHoles, curHoleHeight);
      }

      // Penalize taller holes more, except in the well column
      else if (curHoleHeight === 1 || isWell) {
        numHoles += 1;
      } else if (curHoleHeight === 2) {
        numHoles += 2;
      } else {
        numHoles += 4;
      }

      for (let r = 1; r <= curHoleHeight; r++) {
        holeCells.add((row + r) * 10 + col);
      }
      curHoleHeight = 0;
    }
  }
  return numHoles;
}

export function getSurfaceArrayAndHoles(
  board: Board
): [Array<number>, number, Set<number>] {
  const heights = [];
  let numHoles = 0;
  let holeCells: Set<number> = new Set();

  // Get the column heights first
  for (let col = 0; col < NUM_COLUMN; col++) {
    let row = 0;
    while (row < NUM_ROW && board[row][col] == 0) {
      row++;
    }
    heights.push(20 - row);
  }
  // Then look for holes
  // TODO: Handle delayed tuck setups
  for (let col = 0; col < NUM_COLUMN; col++) {
    if (col === WELL_COLUMN) {
      continue;
    }
    numHoles += countHolesInColumn(
      col,
      board,
      heights,
      holeCells,
      /* isWell= */ false
    );
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
  // If it hasn't reached transition, it can't go up in level
  if (level == 18 && lines < 126) {
    return 18;
  }
  if (level == 19 && lines < 136) {
    return 19;
  }
  if (level == 29 && lines < 196) {
    return 29;
  }

  // Otherwise it goes up every time you cross a multiple of 10
  if ((lines % 10) + numLinesCleared >= 10) {
    return level + 1;
  }
  return level;
}

const BOARD_COMPRESSION_SCHEME = {
  a: "00000",
  b: "00001",
  c: "00010",
  d: "00011",
  e: "00100",
  f: "00101",
  g: "00110",
  h: "00111",
  i: "01000",
  j: "01001",
  k: "01010",
  l: "01011",
  m: "01100",
  n: "01101",
  o: "01110",
  p: "01111",
  q: "10000",
  r: "10001",
  s: "10010",
  t: "10011",
  u: "10100",
  v: "10101",
  w: "10110",
  x: "10111",
  y: "11000",
  z: "11001",
  A: "11010",
  B: "11011",
  C: "11100",
  D: "11101",
  E: "11110",
  F: "11111",
};

export function parseBoard(boardStr: string): Board {
  // Handle encoded boards
  if (boardStr.includes(",")) {
    const split = boardStr.split(",");
    const numEmptyRows = parseInt(split[0]);
    const encodedRows = split[1];
    const numFullRightWellRows = parseInt(split[2]);
    const newBoard = [];
    for (let i = 0; i < numEmptyRows; i++) {
      newBoard.push([0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
    }
    for (let i = 0; i < encodedRows.length; i += 2) {
      const chunk1 = BOARD_COMPRESSION_SCHEME[encodedRows.charAt(i)];
      const chunk2 = BOARD_COMPRESSION_SCHEME[encodedRows.charAt(i + 1)];
      const newRow = (chunk1 + chunk2).split("");
      newBoard.push(newRow.map((x) => parseInt(x)));
    }
    for (let i = 0; i < numFullRightWellRows; i++) {
      newBoard.push([1, 1, 1, 1, 1, 1, 1, 1, 1, 0]);
    }
    if (newBoard.length == 20) {
      return newBoard;
    } else {
      throw new Error(
        "Invalid compressed board. Must contain 20 rows, but found: " +
          newBoard.length
      );
    }
  }

  // Otherwise, handle raw boards of 1s and 0s
  if (boardStr.length !== 200) {
    throw new Error(
      "Invalid board, must be 200 characters in length (20 rows of 10, listed top to bottom). e.g. 00000000001000000001..."
    );
  }
  return boardStr
    .match(/.{1,10}/g) // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split("").map((x) => parseInt(x)));
}

export function getMaxSafeCol9(level: number, aiParams: AiParams) {
  const max4TapHeight = aiParams.MAX_4_TAP_LOOKUP[level];
  let offset;
  if (max4TapHeight <= 10) {
    offset = -4;
  } else {
    offset = -5;
  }
  return Math.max(4, max4TapHeight + offset);
}

export function getScareHeight(
  level: number,
  lines: number,
  aiParams: AiParams
) {
  if (!aiParams.MAX_5_TAP_LOOKUP) {
    throw new Error("No tap heights calculated when looking up scare height");
  }
  let max5TapHeight = aiParams.MAX_5_TAP_LOOKUP[level];

  // If near transition, take it into account early
  const preEmptTransitionLines = 6;
  if (
    getLevelAfterLineClears(level, lines, preEmptTransitionLines) > level &&
    level < 28
  ) {
    const newMax5Tap = aiParams.MAX_5_TAP_LOOKUP[level + 1];
    const linesUntilTransition = 10 - (lines % 10);
    const ratioPreToPost = linesUntilTransition / preEmptTransitionLines;
    max5TapHeight =
      ratioPreToPost * max5TapHeight + (1 - ratioPreToPost) * newMax5Tap;
  }

  let offset;
  if (max5TapHeight <= 4) {
    offset = -1;
  } else if (max5TapHeight <= 6) {
    offset = -2;
  } else if (max5TapHeight <= 10) {
    offset = -3;
  } else if (max5TapHeight <= 12) {
    offset = -4;
  } else {
    offset = -5;
  }
  return Math.max(0, max5TapHeight + offset);
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

export function boardEquals(a, b) {
  for (let r = 0; r < NUM_ROW; r++) {
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (a[r][c] !== b[r][c]) {
        return false;
      }
    }
  }
  return true;
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

const performanceCounts = {};
const performanceTotals = {};
const performanceStartTimes = {};
const startupWait = 2;

export function startTiming(id: string) {
  // Load this dynamically so the web UI doesn't need to require it
  if (performance == null) {
    performance = require("perf_hooks").performance;
  }
  performanceStartTimes[id] = performance.now();
  if (!performanceCounts[id]) {
    performanceCounts[id] = 0;
    performanceTotals[id] = 0;
  }
}

export function stopTiming(id, repeats) {
  // Get the time first thing for accuracy
  const endTime = performance.now();

  if (!performanceStartTimes.hasOwnProperty(id)) {
    throw new Error("Tried to stop timer that didn't exist");
  }
  performanceCounts[id] += 1;
  if (performanceCounts[id] > startupWait) {
    performanceTotals[id] += endTime - performanceStartTimes[id];
    const avgTime =
      performanceTotals[id] / (performanceCounts[id] - startupWait) / repeats -
      0.0072 / repeats;
    console.log(id, avgTime.toFixed(4), (avgTime * 34).toFixed(4));
  }
}
