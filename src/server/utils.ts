export const NUM_ROW = 20;
export const NUM_COLUMN = 10;
export const SquareState = Object.freeze({
  EMPTY: 0,
  FULL: 1,
});

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

export function getSurfaceArrayAndHoleCount(
  board: Board
): [Array<number>, number] {
  const heights = [];
  let numHoles = 0;
  for (let col = 0; col < NUM_COLUMN; col++) {
    let row = 0;
    while (row < NUM_ROW && board[row][col] == 0) {
      row++;
    }
    heights.push(20 - row);
    while (row < NUM_ROW - 1) {
      row++;
      if (board[row][col] === SquareState.EMPTY && col < NUM_COLUMN - 1) {
        // Add a hole if it's anywhere other than column 10
        numHoles++;
        row++;
        while (row < NUM_ROW && board[row][col] === SquareState.EMPTY) {
          numHoles += 0.2;
          row++;
        }
      }
    }
  }
  return [heights, numHoles];
}

/**
 * Gets a list of the heights of columns 0-9.
 * Used for rating the goodness of boards only, will not be used to reconstruct
 * boards (since it omits holes and the state of column 10).
 * @param {Array<Array<number>>} board
 */
export function getSurfaceArray(board) {
  return getSurfaceArrayAndHoleCount(board)[0].slice(0, 9);
}

export function hasInvalidHeightDifferences(surfaceArray) {
  for (let i = 1; i < surfaceArray.length; i++) {
    if (Math.abs(surfaceArray[i] - surfaceArray[i - 1]) > 4) {
      return true;
    }
  }
  return false;
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
  return getSurfaceArrayAndHoleCount(board)[1];
}

export function getMaxColumnHeight(board) {
  const heights = getSurfaceArrayAndHoleCount(board)[0];
  return Math.max(...heights.slice(1));
}

export function getAverageColumnHeight(board) {
  const heights = getSurfaceArrayAndHoleCount(board)[0];
  let totalHeight = 0;
  for (let height of heights) {
    totalHeight += height;
  }
  return totalHeight / 8;
}

/** Checks if clearing a certain number of lines will increase the level, and if so what that level is. */
export function getLevelAfterLineClears(level, lines, numLinesCleared) {
  if (level === 18 && lines + numLinesCleared >= 130) {
    return 19;
  }
  if (level === 28 && lines + numLinesCleared >= 230) {
    return 29;
  }
  return level;
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

// module.exports = {
//   NUM_ROW,
//   NUM_COLUMN,
//   SquareState,
//   GetGravity,
//   getSurfaceArray,
//   generateDigPracticeBoard,
//   getHoleCount: getHoleCount,
//   hasValidHeightDifferences: hasInvalidHeightDifferences,
//   getMaxColumnHeight,
//   getAverageColumnHeight,
//   logBoard,
//   correctSurfaceForExtremeGaps,
//   getLevelAfterLineClears,
// };
