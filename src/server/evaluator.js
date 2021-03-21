const fs = require("fs");
const { AI_MODE } = require("./params");

const ranks_NoNextBox_NoBars = fs.readFileSync(
  "docs/condensed_NoNextBox_NoBars.txt",
  "utf8"
);
const ranks_NextBox_NoBars_1 = fs.readFileSync("2byte_NextBox_1.txt", "utf8");
const ranks_NextBox_NoBars_2 = fs.readFileSync("2byte_NextBox_2.txt", "utf8");
const CUTOFF = 200000000;

const utils = require("./utils");
const SquareState = utils.SquareState;
const NUM_ROW = utils.NUM_ROW;
const NUM_COLUMN = utils.NUM_COLUMN;

function getAiMode(board, lines) {
  if (lines > 225) {
    return AI_MODE.NEAR_KILLSCREEN;
  }
  if (hasHolesInRowRange(board, 0, NUM_ROW - 1)) {
    return AI_MODE.DIG;
  }
  return AI_MODE.STANDARD;
}

/**
 * Converts a list of board heights into the index into the ranks array.
 * Uses fractal's proprietary hashing method which involves base 9 and other shenanigans.
 * e.g
 *     3  2  2  2  1  1  2  1  0
 *  ->   -1  0  0 -1  0  1 -1 -1    make diff array
 *  ->    3  4  4  3  4  5  3  3    add 4
 *  ->    parse that number as base 10
 * @param {Array<number>} surfaceHeightsArray - DOES NOT INCLUDE COL 10
 */
function surfaceHeightsToNoNBIndex(surfaceHeightsArray) {
  let diffs = [];
  for (let i = 0; i < surfaceHeightsArray.length - 1; i++) {
    diffs.push(
      parseInt(surfaceHeightsArray[i + 1]) -
        parseInt(surfaceHeightsArray[i]) +
        4
    );
  }
  const diffString = diffs.join("");
  return parseInt(diffString, 9);
}

/**
 * Converts a list of board heights, along with the next piece ID into the index into the ranks array.
 * Is encoded such that the index / 7 = the standard NNB index, and index % 7 = the next piece index,
 * in the array [TJZOSLI].
 * @param {Array<number>} surfaceArray - DOES NOT INCLUDE COL 10
 */
function surfaceHeightsToNBIndex(surfaceArray, nextPieceId) {
  const noNBIndex = surfaceHeightsToNoNBIndex(surfaceArray);
  const pieceIndex = ["T", "J", "Z", "O", "S", "L", "I"].findIndex(
    (x) => x == nextPieceId
  );
  return 7 * noNBIndex + pieceIndex;
}
/**
 * Looks up the rank at a given index in a rank string, and decodes it from the
 * custom space-saving string encoding.
 * @param {string} rankStr
 * @param {number} index
 */
function lookUpRankInString(lookupStr, index) {
  const rankStr = lookupStr.substr(index * 2, 2);
  return parseInt(rankStr, 36) / 10;
}

function getValueOfBoardSurfaceNoNextBox(surfaceArray) {
  const index = surfaceHeightsToNoNBIndex(surfaceArray);
  return lookUpRankInString(ranks_NoNextBox_NoBars, index);
}

function getValueOfBoardSurfaceWithNextBox(surfaceArray, nextPieceId) {
  const index = surfaceHeightsToNBIndex(surfaceArray, nextPieceId);
  if (index < CUTOFF) {
    return lookUpRankInString(ranks_NextBox_NoBars_1, index);
  }
  return lookUpRankInString(ranks_NextBox_NoBars_2, index - CUTOFF);
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

/** Calculates the number of lines that need to be cleared for all the holes to be resolved. */
function countLinesNeededUntilClean(board) {
  const linesNeededToClear = new Set();
  let maxHeightNonCol10 = 0;

  for (let col = 0; col < NUM_COLUMN; col++) {
    // Go down to the top of the stack in that column
    let row = 0;
    while (row < NUM_ROW && board[row][col] == SquareState.EMPTY) {
      row++;
    }
    // Update the highest height we've seen from cols 0-9
    if (col < NUM_COLUMN - 1) {
      maxHeightNonCol10 = Math.max(maxHeightNonCol10, NUM_ROW - 1 - row);
    }
    const tempSet = new Set();
    while (row < NUM_ROW - 1) {
      tempSet.add(row);
      row++;
      if (board[row][col] == 0) {
        // If not on col 10, we found a hole. Add all the full rows we passed through to the set
        // of lines needing to be cleared
        for (const line of tempSet) {
          linesNeededToClear.add(line);
        }
      }
    }
  }

  // Any row that has col 10 filled above the rest of the stack will need to be cleared.
  let foundPiece = false;
  for (let row = 0; row < NUM_ROW - 1 - maxHeightNonCol10; row++) {
    if (board[row][NUM_COLUMN - 1] == SquareState.FULL) {
      foundPiece = true;
    }
    if (foundPiece) {
      linesNeededToClear.add(row);
    }
  }
  return linesNeededToClear.size;
}

function getNumberOfBlocksInColumn10(board) {
  let count = 0;
  for (let row = 0; row < utils.NUM_ROW; row++) {
    if (board[row][9] == SquareState.FULL) {
      const height = 20 - row;
      count += 1 + height / 6; // Count extra for high up blocks
    }
  }
  return count;
}

/**
 * Gives a bonus if column 1 is higher than the rest (up to 2, since more isn't
 * too helpful after that), or a penalty if it's lower. However, if the stack is low,
 * it always returns 0, since this factor isn't too relevant.
 * @param {*} board
 * @param {*} surfaceArray
 * @param {*} scareHeight
 */
function getHighLeftFactor(board, surfaceArray, scareHeight) {
  const maxHeightNonCol1 = utils.getMaxColumnHeight(board);
  const col1Height = surfaceArray[0];
  // Not relevant for low stacks
  if (Math.max(maxHeightNonCol1, col1Height) < scareHeight) {
    return 0;
  }
  // Cap at 2 so it doesn't want crazy high col 1
  return Math.min(2, col1Height - maxHeightNonCol1);
}

/**
 *
 * @param {number} startRow - inclusive
 * @param {number} endRow - inclusive
 */
function hasHolesInRowRange(board, startRow, endRow) {
  for (let col = 0; col < NUM_COLUMN - 1; col++) {
    // Navigate past the empty space above each column
    let row = 0;
    while (row < NUM_ROW && board[row][col] == 0) {
      row++;
    }

    // Now that we're in the stack, if there are empty cells, they're holes
    while (row < endRow) {
      row++;
      if (row >= startRow && board[row][col] == 0) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Returns true iff. there are no holes/overhangs preventing getting Tetris-ready assuming normal play.
 * This prevents the AI from never taking the triple if it has a hole in the bottom 4 rows.
 * @param {Array<Array<number>>} board
 */
function isNotBuildingTowardTetris(board) {
  // Move the imaginary long bar down column 10
  let row = 0;
  while (row < NUM_ROW && board[row][9] == SquareState.EMPTY) {
    row++;
  }

  // If there are any holes in the zone where you'd take the tetris, you're not building towards a tetris.
  return hasHolesInRowRange(board, row - 4, row - 1);
}

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

function getValueOfPossibilityNoNextBox(
  possibility,
  level,
  lines,
  currentMode,
  shouldLog,
  aiParams
) {
  return getValueOfPossibility(
    possibility,
    null,
    level,
    lines,
    currentMode,
    shouldLog,
    aiParams
  );
}

/**
 * Evaluates a given possibility based on a number of factors.
 * @param {[rotationId, xOffset, resultingSurface, numHoles, numLinesCleared]} possibility
 */
function getValueOfPossibility(
  possibility,
  nextPieceId,
  level,
  lines,
  currentMode,
  shouldLog,
  aiParams
) {
  const [
    _,
    __,
    surfaceArray,
    numHoles,
    numLinesCleared,
    trialBoard,
  ] = possibility;

  if (!aiParams) {
    console.log("RED ALERT", level, lines, currentMode, shouldLog, aiParams);
  }

  // Preliminary calculations
  const [
    correctedSurface,
    totalHeightCorrected,
  ] = utils.correctSurfaceForExtremeGaps(surfaceArray);
  const scareHeight =
    level >= 29
      ? aiParams.SCARE_HEIGHT_29
      : level >= 19
      ? aiParams.SCARE_HEIGHT_19
      : aiParams.SCARE_HEIGHT_18;
  const maxHeightAboveScareLine = Math.max(
    0,
    utils.getMaxColumnHeight(trialBoard) - scareHeight
  );
  const avgHeightAboveScareLine = Math.max(
    0,
    utils.getAverageColumnHeight(trialBoard) - scareHeight
  );
  const tetrisReady = isTetrisReadyRightWell(trialBoard);
  const notBuildingTowardsTetris = isNotBuildingTowardTetris(trialBoard);

  let extremeGapFactor = totalHeightCorrected * aiParams.EXTREME_GAP_PENALTY;
  let surfaceFactor;
  if (nextPieceId !== null) {
    surfaceFactor =
      aiParams.SURFACE_MULTIPLIER *
      getValueOfBoardSurfaceWithNextBox(correctedSurface, nextPieceId);
  } else {
    surfaceFactor = getValueOfBoardSurfaceNoNextBox(correctedSurface);
  }
  const tetrisReadyFactor =
    tetrisReady *
    (nextPieceId == "I"
      ? aiParams.TETRIS_READY_BONUS_BAR_NEXT
      : aiParams.TETRIS_READY_BONUS);
  const notBuildingTowardsTetrisFactor = notBuildingTowardsTetris
    ? aiParams.NOT_BUILDING_TOWARD_TETRIS_PENALTY
    : 0;
  const holeFactor = numHoles * aiParams.HOLE_PENALTY;
  const holeWeightFactor =
    countLinesNeededUntilClean(trialBoard) * aiParams.HOLE_WEIGHT_PENALTY;
  const lineClearFactor = getLineClearValue(numLinesCleared, aiParams);
  const maxHeightFactor =
    aiParams.MAX_HEIGHT_MULTIPLIER *
    Math.pow(maxHeightAboveScareLine, aiParams.MAX_HEIGHT_EXPONENT);
  const avgHeightFactor =
    aiParams.AVG_HEIGHT_MULTIPLIER *
    Math.pow(avgHeightAboveScareLine, aiParams.AVG_HEIGHT_EXPONENT);
  const col10Factor =
    getNumberOfBlocksInColumn10(trialBoard) *
    aiParams.COL_10_PENALTY *
    (level < 29); // doesn't apply on 29+
  const slopingFactor =
    aiParams.SLOPE_PENALTY_MULTIPLIER *
    countEmptyBlocksBelowColumn9Height(surfaceArray);
  const highLeftFactor =
    aiParams.HIGH_LEFT_MULTIPLIER *
    getHighLeftFactor(trialBoard, surfaceArray, scareHeight) *
    (level >= 29 ? 3.5 : 1); // more powerful on 29

  const totalValue =
    surfaceFactor +
    extremeGapFactor +
    holeFactor +
    holeWeightFactor +
    lineClearFactor +
    maxHeightFactor +
    avgHeightFactor +
    col10Factor +
    slopingFactor +
    tetrisReadyFactor +
    highLeftFactor +
    notBuildingTowardsTetrisFactor;

  const explanation = `Surf: ${surfaceFactor}, Hole: ${holeFactor}, HoleWeight: ${holeWeightFactor}, ExtremeGap: ${extremeGapFactor}, LineClear: ${lineClearFactor}, MaxHeight: ${maxHeightFactor}, AvgHeight: ${avgHeightFactor}, Col10: ${col10Factor} Slope: ${slopingFactor} HighLeft: ${highLeftFactor}, TetrisReady: ${tetrisReadyFactor} NotBldgTwdTetris: ${notBuildingTowardsTetrisFactor}, Subtotal: ${totalValue}`;
  if (shouldLog) {
    console.log(
      `---- Evaluating possiblity: ${possibility[0]} ${possibility[1]}\n`,
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
  currentMode,
  numMovesToConsider,
  aiParams
) {
  for (const possibility of possibilityList) {
    const [value, explanation] = getValueOfPossibility(
      possibility,
      nextPieceId,
      level,
      lines,
      currentMode,
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
  currentMode,
  shouldLog,
  aiParams
) {
  let bestMove = null;
  let bestValue = -999;
  for (const possibility of possibilityList) {
    // Get the total value of the proposed placement + the next placement afterwards
    const [value, explanation] = getValueOfPossibilityNoNextBox(
      possibility,
      level,
      lines,
      currentMode,
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
  getLineClearValue,
  getAiMode,
};
