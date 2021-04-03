const { AI_MODE } = require("./params");
const boardHelper = require("./board_helper");
const rankLookup = require("./rank-lookup");

const utils = require("./utils");
const SquareState = utils.SquareState;
const NUM_ROW = utils.NUM_ROW;
const NUM_COLUMN = utils.NUM_COLUMN;

/** If there is a spire in the middle of the board, return its height above the scare line. Otherwise, return 0. */
function getSpireHeight(surfaceArray, scareHeight) {
  let spireHeight = 0;
  // Col 1 and 9 are covered by left/right accessible factors
  const leftBound = 2;
  const rightBound = 8; // The rightmost column to look for spires
  for (let i = leftBound; i < rightBound; i++) {
    // Only consider columns who are higher than the one on their left, so that
    // nice sloping stacks don't count as spires
    if (surfaceArray[i] > surfaceArray[i - 1]) {
      spireHeight = Math.max(spireHeight, surfaceArray[i]);
    }
  }
  return Math.max(0, spireHeight - scareHeight);
}

/**
 * Calculates the number of un-filled cells there are below the column 9 height.
 * These cells indicate cells that would need to be filled in before burning is possible.
 * @param {Array<number>} surfaceArray
 */
function countEmptyBlocksBelowColumn9Height(surfaceArray) {
  let totalBlocks = 0;
  const surfaceWithoutCol9 = surfaceArray.slice(0, 8);
  const col9Height = surfaceArray[8];
  for (const height of surfaceWithoutCol9) {
    if (height < col9Height) {
      totalBlocks += col9Height - height;
    }
  }
  return totalBlocks;
}

function countCol10Holes(board) {
  let count = 0;

  // Go down to the top of the stack in that column
  let row = 0;
  while (row < NUM_ROW && board[row][NUM_COLUMN - 1] == SquareState.EMPTY) {
    row++;
  }
  // Track the full rows we pass through
  while (row < NUM_ROW - 1) {
    row++;
    if (board[row][NUM_COLUMN - 1] === SquareState.EMPTY) {
      count++;
    }
  }
  return count;
}

/** Calculates the number of lines that need to be cleared for all the holes to be resolved. */
function countLinesNeededUntilClean(board) {
  const linesNeededToClear = new Set();
  let highestHoleRow = 9999;
  for (let col = 0; col < NUM_COLUMN; col++) {
    // Go down to the top of the stack in that column
    let row = 0;
    while (row < NUM_ROW && board[row][col] == SquareState.EMPTY) {
      row++;
    }
    // Track the full rows we pass through
    const rowsAboveHole = new Set();
    while (row < NUM_ROW - 1) {
      rowsAboveHole.add(row);
      row++;
      if (board[row][col] === SquareState.EMPTY) {
        // If not on col 10, we found a hole. Add all the full rows we passed through to the set
        // of lines needing to be cleared. Otherwise we ignore tempSet.
        for (const line of rowsAboveHole) {
          linesNeededToClear.add(line);
          highestHoleRow = Math.min(highestHoleRow, row);
        }
      }
    }
  }

  // Any row that has col 10 filled above the highest hole will need to be cleared
  if (linesNeededToClear.size > 0) {
    for (let row = 0; row < highestHoleRow; row++) {
      if (board[row][NUM_COLUMN - 1] == SquareState.FULL) {
        linesNeededToClear.add(row);
      }
    }
  }
  return linesNeededToClear.size;
}

/** Checks if a board has a well that's not on the right. */
function hasNonRightWell(board) {
  let row = NUM_ROW - 1;
  while (row >= 0 && board[row][NUM_COLUMN - 1] == SquareState.FULL) {
    // If col 10 is full and there are any empty cells on other cols, this is true
    for (let col = 0; col < NUM_COLUMN - 1; col++) {
      if (board[row][col] == SquareState.EMPTY) {
        return true;
      }
    }
    row--;
  }
  return false;
}

/** Calculate a factor that penalizes filling column 10. Specifically, it counts the number of cells
 * in the other columns that would needed to be filled to clear away the block on column 10.
 * However, since holes *want* to have column 10 filled, count them oppositely.
 * Additionally, there's a penalty for the column 10 block being high up.
 */
function getColumn10Factor(board, scareHeight) {
  let sum = 0;
  for (let row = 0; row < NUM_ROW; row++) {
    // If column 10 filled, add to the sum
    if (board[row][NUM_COLUMN - 1] == SquareState.FULL) {
      for (let col = 0; col < NUM_COLUMN - 1; col++) {
        if (board[row][col] == SquareState.FULL) {
          continue;
        }

        let isHole = false;
        for (let loopRow = 0; loopRow < row; loopRow++) {
          if (board[loopRow][col] == SquareState.FULL) {
            isHole = true;
          }
        }

        if (isHole) {
          // Filling in col 10 for holes is good
          sum -= 1;
        } else {
          // How bad an exposed col 10 is scales with height
          const heightMultiplier =
            scareHeight < 2 ? 1 : (NUM_ROW - row) / scareHeight;
          sum += heightMultiplier;
        }
        if (board[row][col] == SquareState.EMPTY) {
        }
      }
    }
  }
  return sum;
}

/** Count the number of blocks in column 10 */
function countBlocksInColumn10(board) {
  let sum = 0;
  for (let row = 0; row < NUM_ROW; row++) {
    // If column 10 filled, add to the sum
    if (board[row][NUM_COLUMN - 1] == SquareState.FULL) {
      sum += 1;
    }
  }
  return sum;
}

// /**
//  * Gives a bonus if column 1 is higher than the rest (up to 2, since more isn't
//  * too helpful after that), or a penalty if it's lower. However, if the stack is low,
//  * it always returns 0, since this factor isn't too relevant.
//  * @param {*} board
//  * @param {*} surfaceArray
//  * @param {*} scareHeight
//  */
// function getHighLeftFactor(surfaceArray, scareHeight, board) {
//   const maxHeightNonCol1 = Math.max(...surfaceArray.slice(1));
//   const col1Height = surfaceArray[0];
//   // Scale up based on the board height
//   const boardHeightFactor =
//     scareHeight < 2 ? 1 : maxHeightNonCol1 / scareHeight;
//   // Cap at 2 so it doesn't want crazy high col 1
//   return boardHeightFactor * Math.min(2, col1Height - maxHeightNonCol1);
// }

function isTetrisReadyRightWell(board) {
  // Move the imaginary long bar down column 10
  let row = 0;
  while (row < NUM_ROW && board[row][9] == SquareState.EMPTY) {
    row++;
  }

  // Check if the 4 rows above the stopping point of the long bar are filled
  for (let checkRow = row - 4; checkRow <= row - 1; checkRow++) {
    for (let checkCol = 0; checkCol < 9; checkCol++) {
      if (checkRow < 0 || checkRow >= NUM_ROW) {
        return false;
      }
      if (board[checkRow][checkCol] == SquareState.EMPTY) {
        return false;
      }
    }
  }

  return true;
}

function getLineClearValue(numLinesCleared, aiParams) {
  return numLinesCleared == 4
    ? aiParams.TETRIS_BONUS
    : numLinesCleared > 0
    ? aiParams.BURN_PENALTY * numLinesCleared
    : 0;
}

/**
 * Evaluates a given possibility based on a number of factors.
 * NB: @param nextPieceId CAN be null if you want the NNB value of a possiblity.
 * @param {[rotationId, xOffset, resultingSurface, numHoles, numLinesCleared]} possibility
 */
function getValueOfPossibility(
  possibility,
  nextPieceId,
  level,
  lines,
  aiMode,
  shouldLog,
  aiParams
) {
  const [
    _,
    __,
    surfaceArray,
    numHoles,
    numLinesCleared,
    boardAfter,
  ] = possibility;

  if (!aiParams) {
    throw new Error("No AI Params provided: ", aiParams);
  }

  // Preliminary calculations
  const [
    correctedSurface,
    totalHeightCorrected,
  ] = utils.correctSurfaceForExtremeGaps(surfaceArray);
  const adjustedNumHoles =
    numHoles + (aiMode === AI_MODE.KILLSCREEN && countCol10Holes(boardAfter));
  const scareHeight =
    level >= 29
      ? aiParams.SCARE_HEIGHT_29
      : level >= 19
      ? aiParams.SCARE_HEIGHT_19
      : aiParams.SCARE_HEIGHT_18;
  const spireHeight = getSpireHeight(surfaceArray, scareHeight);
  const averageHeight = utils.getAverageColumnHeight(boardAfter);
  const avgHeightAboveScareLine = Math.max(0, averageHeight - scareHeight);
  const tetrisReady = isTetrisReadyRightWell(boardAfter);
  const levelAfterPlacement = utils.getLevelAfterLineClears(
    level,
    lines,
    numLinesCleared
  );
  const leftIsInaccessible = boardHelper.boardHasInaccessibileLeft(
    boardAfter,
    levelAfterPlacement,
    averageHeight
  );
  const rightIsInaccessible = boardHelper.boardHasInaccessibileRight(
    boardAfter,
    levelAfterPlacement,
    averageHeight
  );
  const col1Height = surfaceArray[0];
  const col10Height = boardHelper.getBoardHeightAtColumn(
    boardAfter,
    NUM_COLUMN - 1
  );

  let extremeGapFactor = totalHeightCorrected * aiParams.EXTREME_GAP_PENALTY;
  let surfaceFactor =
    aiParams.SURFACE_MULTIPLIER *
    rankLookup.getValueOfBoardSurface(correctedSurface, nextPieceId);
  const tetrisReadyFactor =
    tetrisReady *
    (nextPieceId == "I"
      ? aiParams.TETRIS_READY_BONUS_BAR_NEXT
      : aiParams.TETRIS_READY_BONUS);
  const holeFactor = adjustedNumHoles * aiParams.HOLE_PENALTY;
  const holeWeightFactor =
    countLinesNeededUntilClean(boardAfter) * aiParams.HOLE_WEIGHT_PENALTY;
  const lineClearFactor = getLineClearValue(numLinesCleared, aiParams);
  const spireHeightFactor =
    aiParams.SPIRE_HEIGHT_MULTIPLIER *
    Math.pow(spireHeight, aiParams.SPIRE_HEIGHT_EXPONENT);
  const avgHeightFactor =
    aiParams.AVG_HEIGHT_MULTIPLIER *
    Math.pow(avgHeightAboveScareLine, aiParams.AVG_HEIGHT_EXPONENT);
  const col10Factor =
    getColumn10Factor(boardAfter, scareHeight) * aiParams.COL_10_PENALTY;
  const col10BurnFactor =
    countBlocksInColumn10(boardAfter) * aiParams.BURN_PENALTY; // Any blocks on col 10 will result in a burn
  const col9Factor =
    aiParams.HIGH_COL_9_PENALTY_MULTIPLIER *
    countEmptyBlocksBelowColumn9Height(surfaceArray);
  const builtOutLeftFactor =
    aiParams.BUILT_OUT_LEFT_MULTIPLIER * Math.max(0, col1Height - scareHeight);
  const builtOutRightFactor =
    aiParams.BUILT_OUT_RIGHT_MULTIPLIER *
    Math.max(0, col10Height - scareHeight);
  const inaccessibleLeftFactor = leftIsInaccessible
    ? aiParams.INACCESSIBLE_LEFT_PENALTY
    : 0;
  const inaccessibleRightFactor = rightIsInaccessible
    ? aiParams.INACCESSIBLE_RIGHT_PENALTY
    : 0;

  const factors = {
    surfaceFactor,
    extremeGapFactor,
    holeFactor,
    holeWeightFactor,
    lineClearFactor,
    spireHeightFactor,
    avgHeightFactor,
    col10Factor,
    col10BurnFactor,
    col9Factor,
    tetrisReadyFactor,
    builtOutLeftFactor,
    builtOutRightFactor,
    inaccessibleLeftFactor,
    inaccessibleRightFactor,
  };

  let totalValue = 0;
  let explanation = "";
  for (const key in factors) {
    const val = factors[key];

    // Crash instantly if any of the factors are NaN (it's better than still running and making bad placements)
    if (isNaN(val)) {
      throw new Error(`NaN detected for factor: ${key}`);
    }

    totalValue += val;
    const shortKeyName = key.substr(0, key.length - 6);
    explanation += `${shortKeyName}: ${val.toFixed(2)}, `;
  }
  explanation += `SUBTOTAL: ${totalValue}`;

  if (shouldLog) {
    console.log(
      `---- Evaluated possiblity: ${possibility[0]} ${possibility[1]}, mode: ${aiMode}\n`,
      explanation
    );
  }
  return [totalValue, explanation];
}

/**
 * Iterates over the list of possiblities and return the one with the highest value.
 * @param {Array<possibility obj>} possibilityList
 */
function pickBestNMoves(
  possibilityList,
  nextPieceId,
  level,
  lines,
  aiMode,
  numMovesToConsider,
  aiParams
) {
  for (const possibility of possibilityList) {
    const [value, explanation] = getValueOfPossibility(
      possibility,
      nextPieceId,
      level,
      lines,
      aiMode,
      /* shouldLog= */ false,
      aiParams
    );
    possibility.push(value);
    possibility.push(explanation);
  }
  // Sort by value
  possibilityList.sort((a, b) => b[6] - a[6]);

  return possibilityList.slice(0, numMovesToConsider);
}

/**
 * Iterates over the list of possiblities and return the one with the highest value.
 * @param {Array<possibility obj>} possibilityList
 */
function pickBestMoveNoNextBox(
  possibilityList,
  level,
  lines,
  aiMode,
  shouldLog,
  aiParams
) {
  let bestMove = null;
  let bestValue = Number.MIN_SAFE_INTEGER;
  for (const possibility of possibilityList) {
    // Get the total value of the proposed placement + the next placement afterwards
    const [value, explanation] = getValueOfPossibility(
      possibility,
      null,
      level,
      lines,
      aiMode,
      shouldLog,
      aiParams
    );
    if (value > bestValue) {
      possibility.push(value);
      possibility.push(explanation);
      bestValue = value;
      bestMove = possibility;
    }
  }
  return bestMove;
}

module.exports = {
  pickBestNMoves,
  pickBestMoveNoNextBox,
  getValueOfPossibility,
  getLineClearValue,
};
