const rankLookup = require("./rank-lookup");
import {
  ranks_12hz,
  ranks_13hz,
  ranks_15hz,
} from "../../docs/killscreen_ranks_v2";
import * as boardHelper from "./board_helper";
import { getPieceRanges } from "./move_search";
import { getParams, IS_NON_RIGHT_WELL, WELL_COLUMN } from "./params";
import * as utils from "./utils";
const SquareState = utils.SquareState;
const NUM_ROW = utils.NUM_ROW;
const NUM_COLUMN = utils.NUM_COLUMN;

/** Adjust the surface to not penalize double wells as if they're long bar dependencies */
function correctSurfaceForDoubleWell(
  surfaceArray,
  maxSafeCol9Height
): Array<number> {
  // If col 9 is 2 or more lower than col 8, and it's high up on the board, pretend col 9 is exactly 1 lower than col 8
  if (
    surfaceArray[8] + 1 < surfaceArray[7] &&
    surfaceArray[7] >= maxSafeCol9Height &&
    surfaceArray[6] >= maxSafeCol9Height
  ) {
    surfaceArray[8] = surfaceArray[7] - 1;
  }
  // Otherwise for lower down double wells, pretend it's 2 lower than col 8 (not great but not disastrous like a bar dep)
  else if (surfaceArray[8] + 2 < surfaceArray[7]) {
    surfaceArray[8] = surfaceArray[7] - 2;
  }
  return surfaceArray;
}

/** Estimates the number of burns needed to bring column 9 up to match column 8.
 * It can also do this with a long bar, so the burn penalty is later discounted a bit. */
function estimateBurnsDueToEarlyDoubleWell(surfaceArray, maxSafeCol9Height) {
  if (isNaN(maxSafeCol9Height)) {
    throw new Error("MaxSafeCol9Height isNan");
  }
  const col9 = surfaceArray[8];
  const col8 = surfaceArray[7];
  const lowestGoodColumn9 = Math.min(maxSafeCol9Height, col8 - 2);
  if (col9 >= lowestGoodColumn9) {
    return 0;
  }
  // Need a burn for every 2 cells below that.
  // E.g. 1 diff => 3 below col8 = 1 burn,
  //      2 diff => 4 below col8 = 1 burn
  //      3 diff => 5 below col8 = 2 burns
  const diff = lowestGoodColumn9 - col9;
  return Math.ceil(diff / 2);
}

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

/** Gets the average height of the columns (excluding col 10, except on killscreen) */
function getAverageHeightAboveScareLine(surfaceArray, scareHeight) {
  if (surfaceArray.length !== 9){
    throw new Error("Surface array length was " + surfaceArray.length);
  }
  let total = 0;
  for (const height of surfaceArray) {
    total += height;
  }
  const averageHeight = total / surfaceArray.length;
  console.log("Surface for avg height", surfaceArray.join(","), averageHeight, scareHeight);
  return Math.max(0, averageHeight - scareHeight);
}

/** Calculates the number of lines that need to be cleared for all the holes to be resolved. */
function getRowsNeedingToBurn(
  board,
  surfaceArray,
  maxDirtyTetrisHeight,
  holeCells: Set<number>
): [Set<number>, Set<number>] {
  const linesNeededToClear: Set<number> = new Set();
  const linesNeededToClearIfTucksFail: Set<number> = new Set();

  holeCells.forEach((x) => {
    const holeRow = Math.floor(x / 10);
    const holeCol = x % 10;
    const [isTuck, _] = utils.isTuckSetup(
      holeRow,
      holeCol,
      board,
      surfaceArray
    );
    // Ignore holes that can be dirty tetrised over
    if (
      NUM_ROW - holeRow <= maxDirtyTetrisHeight &&
      board[holeRow][WELL_COLUMN] == SquareState.FULL
    ) {
      return;
    }
    // Otherwise, check how many filled rows there are above it
    for (let row = holeRow - 1; row >= NUM_ROW - surfaceArray[holeCol]; row--) {
      (isTuck ? linesNeededToClearIfTucksFail : linesNeededToClear).add(row);
    }
  });

  // Don't double count between the two sets
  let noTuckFiltered = new Set(
    [...linesNeededToClearIfTucksFail].filter((x) => !linesNeededToClear.has(x))
  );

  return [linesNeededToClear, noTuckFiltered];
}

function getColumn9Factor(
  aiParams: AiParams,
  surfaceArray: Array<number>,
  maxSafeCol9Height: number
) {
  const col9 = surfaceArray[8];
  if (
    aiParams.HIGH_COL_9_COEF === 0 ||
    col9 <= maxSafeCol9Height ||
    col9 <= surfaceArray[9]
  ) {
    return 0;
  }

  return (
    Math.pow(col9 - maxSafeCol9Height, aiParams.HIGH_COL_9_EXP) *
    aiParams.HIGH_COL_9_COEF
  );
}

/**
 * A simple heuristic to check if a cell is very hard to fill (for the purposes of clearing a delayed burn, preparing for something, etc.)
 * Checks for:
 * 1) holes
 * 2) line dependencies
 * 3) cells surrounded by 2-high walls
 */
function getDifficultyToFillCell(
  row: number,
  col: number,
  surfaceArray: Array<number>,
  holeCells: Set<number>,
  minReachableX,
  maxReachableX
) {
  // Check if the cell is out of reach (nightmare scenario, probably instant death if this cell needs to be filled)
  if (col < minReachableX || col > maxReachableX) {
    return 200;
  }
  // Check if the cell is a hole
  const cellHeight = NUM_ROW - row;
  if (holeCells.has(row * 10 + col)) {
    return 10 * (surfaceArray[col] - cellHeight); // 10 per row of garbage
  }
  // Check for a line dependency in that column
  const thisCol = surfaceArray[col];
  const prevCol = surfaceArray[col - 1] || 999;
  const nextCol = surfaceArray[col + 1] || 999;
  if (prevCol - thisCol >= 3 && nextCol - thisCol >= 3) {
    return 10;
  }
  // Otherwise check for 2-wide dependencies in that column
  const prevPrevCol = surfaceArray[col - 2] || 999;
  const nextNextCol = surfaceArray[col + 2] || 999;
  if (
    Math.max(prevCol, prevPrevCol) - thisCol >= 2 &&
    Math.max(nextNextCol, nextCol) - thisCol >= 2
  ) {
    return 2;
  }
  return 1;
}

/** Calculate a factor that estimates the number of cells that need to be cleared to uncover the well.
 * Adjusted for a number of factors, including height on the board, and the difficulty of filling some cells,
 * due to dependencies or piece range.
 */
function getCoveredWellFactor(
  board,
  surfaceArray,
  scareHeight,
  aiParams,
  aiMode,
  rowsNeedingToBurn: Set<number>,
  holeCells,
  minReachableX,
  maxReachableX
) {
  let sum = 0;
  for (let row = 0; row < NUM_ROW; row++) {
    // If we're digging and we need to clear the row, don't penalize filling col 10
    if (aiMode == AiMode.DIG && rowsNeedingToBurn.has(row)) {
      continue;
    }
    // If the well is filled, add to the sum
    if (board[row][WELL_COLUMN] == SquareState.FULL) {
      for (let col = 0; col < NUM_COLUMN; col++) {
        // Ignore the well itself and filled columns
        if (col == WELL_COLUMN || board[row][col] == SquareState.FULL) {
          continue;
        }

        const heightMultiplier = Math.pow(
          scareHeight < 2 ? 1 : (NUM_ROW - row) / scareHeight,
          aiParams.COL_10_HEIGHT_MULTIPLIER_EXP
        );

        // Apply a baseline penalty for having a covered well at all
        if (sum === 0) {
          sum += 5 * heightMultiplier;
        }

        // Add more penalty based on how hard it is to resolve
        const difficultyToFill = getDifficultyToFillCell(
          row,
          col,
          surfaceArray,
          holeCells,
          minReachableX,
          maxReachableX
        );
        sum += difficultyToFill * heightMultiplier;
      }
    }
  }
  return sum;
}

/**
 * Calculates the number of un-filled cells there are below the column 9 height.
 * These cells indicate cells that would need to be filled in before burning is possible.
 * @param {Array<number>} surfaceArray
 */
function getUnableToBurnFactor(
  board,
  surfaceArray,
  scareHeight,
  aiParams: AiParams,
  holeCells,
  minReachableX,
  maxReachableX
) {
  let totalAdjustedCells = 0;
  let col9Height = surfaceArray[8];
  // If col 10 is also filled in that row, it's not bad to have c9 filled
  while (
    col9Height > 0 &&
    board[NUM_ROW - col9Height][9] === SquareState.FULL
  ) {
    col9Height--;
  }
  for (let col = 0; col <= 7; col++) {
    for (
      let row = NUM_ROW - col9Height;
      row < NUM_ROW - surfaceArray[col];
      row++
    ) {
      const diffic = getDifficultyToFillCell(
        row,
        col,
        surfaceArray,
        holeCells,
        minReachableX,
        maxReachableX
      );
      totalAdjustedCells += diffic;
    }
  }

  // Scale the importance based on the height relative to scare height
  const heightMultiplier = Math.pow(
    surfaceArray[8] / Math.max(scareHeight, 2),
    aiParams.UNABLE_TO_BURN_HEIGHT_EXP
  );
  return totalAdjustedCells * heightMultiplier;
}

/** Count the number of blocks in column 10 */
function countBlocksInWell(board): Array<number> {
  let blockedRows = [];
  for (let row = 0; row < NUM_ROW; row++) {
    // If column 10 filled, add to the sum
    if (board[row][WELL_COLUMN] == SquareState.FULL) {
      blockedRows.push(row);
    }
  }
  return blockedRows;
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

function transformSurfaceValue(rawValue, A, maxValue) {
  const correction = A / maxValue;
  return rawValue + correction - A / Math.max(1, rawValue);
}

/** Get the rank of the left-3-column surface (killscreen-only) */
function getLeftSurfaceValue(board, aiParams, level) {
  const max4Tap = aiParams.MAX_4_TAP_LOOKUP[level];
  const max5Tap = aiParams.MAX_5_TAP_LOOKUP[level];

  const leftSurface = boardHelper.getRelativeLeftSurface(board, max4Tap);
  if (leftSurface == null) {
    return transformSurfaceValue(0, 350, 100);
  }

  const split = leftSurface.split("|");
  // Transpositions of the surface with different height relative to the rest of the stack
  const lowerAlt = `${split[0]}|${parseInt(split[1]) - 1}`;
  const higherAlt = `${split[0]}|${parseInt(split[1]) + 1}`;

  // Find the right ranks based on tap speed
  let ranks, rawValue;
  if (max4Tap == 3 && max5Tap == -1) {
    ranks = ranks_12hz;
  } else if (max4Tap == 4 && max5Tap == 0) {
    ranks = ranks_13hz;
  } else if (max4Tap === 6 && max5Tap === 2) {
    ranks = ranks_15hz;
  } else {
    // No ranks exist for that tap speed
    return 0;
  }

  // Look up the surface, or extrapolate from a similar surface
  if (ranks.hasOwnProperty(leftSurface)) {
    rawValue = ranks[leftSurface];
  } else if (ranks.hasOwnProperty(lowerAlt)) {
    rawValue = ranks[lowerAlt] * 1.2;
  } else if (ranks.hasOwnProperty(higherAlt)) {
    rawValue = ranks[higherAlt] * 0.8;
  } else {
    rawValue = 0;
  }
  return transformSurfaceValue(rawValue, 350, 100);
}

function getSurfaceValue(
  surfaceArray: Array<number>,
  totalHeightCorrected: number,
  aiParams: AiParams
) {
  if (IS_NON_RIGHT_WELL) {
    return transformSurfaceValue(getFlatnessScore(surfaceArray), 100, 30);
  }
  let rawValue = rankLookup.getValueOfBoardSurface(surfaceArray);
  // Add in the extreme gap penalty prior to transforming the rank
  rawValue += totalHeightCorrected * aiParams.EXTREME_GAP_COEF;
  return transformSurfaceValue(rawValue, 100, 30);
}

function getFlatnessLossForColumnDiff(
  diff: number,
  prevDiff: number,
  colNum: number
): number {
  let loss = 0;
  if (
    (diff >= 3 && prevDiff <= -3) ||
    (diff <= -3 && prevDiff >= 3) ||
    ((colNum == 0 || colNum == 8) && Math.abs(diff) >= 3)
  ) {
    loss += 20; // Bar dep penalty on top of the standard loss
  }
  loss += Math.pow(Math.abs(diff), 1);
  return loss;
}

function getFlatnessScore(surfaceArray: Array<number>) {
  let score = 30;

  let prevDiff = null;
  for (let i = 0; i + 1 < NUM_COLUMN; i++) {
    // Ignore well cols
    if (i == WELL_COLUMN || i + 1 == WELL_COLUMN) {
      prevDiff = null;
      continue;
    }

    const diff = surfaceArray[i + 1] - surfaceArray[i];
    score -= getFlatnessLossForColumnDiff(diff, prevDiff, i);
    prevDiff = diff;
  }
  return score;
}

export function getPartialValue(
  possibility: Possibility,
  aiParams: InitialAiParams
) {
  return (
    getLineClearValue(possibility.numLinesCleared, aiParams) +
    possibility.inputCost
  );
}

export function getLineClearValue(numLinesCleared, aiParams) {
  return numLinesCleared == 4
    ? aiParams.TETRIS_COEF
    : numLinesCleared > 0
    ? aiParams.BURN_COEF * numLinesCleared
    : 0;
}

function getBuiltOutLeftFactor(
  boardAfter: Board,
  surfaceArray: Array<number>,
  aiParams: AiParams,
  aiMode: AiMode,
  scareHeight: number
) {
  // Handle low left cases first
  const averageHeight = surfaceArray.slice(2, 8).reduce((a, b) => a + b) / 7;
  const heightMultiplier = averageHeight / Math.max(scareHeight, 2);
  if (surfaceArray[0] < averageHeight) {
    return (
      -1 *
      aiParams.BUILT_OUT_LEFT_COEF *
      Math.pow(heightMultiplier, 0.5) *
      Math.pow(averageHeight - surfaceArray[0], aiParams.LOW_LEFT_EXP)
    );
  }

  // Don't build out the left if there's a hole there
  if (
    aiMode === AiMode.DIG ||
    boardHelper.hasHoleInColumn(boardAfter, 0, /* numRowsFromTop= */ 5) ||
    boardHelper.hasHoleInColumn(boardAfter, 1, /* numRowsFromTop= */ 5)
  ) {
    return 0;
  }

  // Otherwise reward building out the left
  const col1Height = surfaceArray[0];
  const rawValue =
    aiParams.BUILT_OUT_LEFT_COEF * Math.max(0, col1Height - averageHeight);
  return heightMultiplier * rawValue;
}

function getBuiltOutRightFactor(boardAfter, surfaceArray) {
  if (
    boardHelper.hasHoleInColumn(boardAfter, NUM_COLUMN - 1) ||
    boardHelper.hasHoleInColumn(boardAfter, NUM_COLUMN - 2)
  ) {
    return 0;
  }
  const col10Height = boardHelper.getBoardHeightAtColumn(
    boardAfter,
    NUM_COLUMN - 1
  );
  const averageHeight = surfaceArray.slice(2, 8).reduce((a, b) => a + b) / 7;
  return Math.max(0, col10Height - averageHeight);
}

export function rateSurface(surfaceArray): string {
  // Correct the inputs for certain conditions
  const aiParams = getParams();
  let [correctedSurface, _] = utils.correctSurfaceForExtremeGaps(
    surfaceArray.slice(0, 9)
  );
  let surfaceFactorNoNextBox = rankLookup.getValueOfBoardSurface(
    correctedSurface
  );
  let result = "No next box: " + surfaceFactorNoNextBox.toFixed(2);
  for (const pieceId of utils.POSSIBLE_NEXT_PIECES) {
    result +=
      "\n" +
      pieceId +
      ": " +
      rankLookup.getValueOfBoardSurface(correctedSurface).toFixed(2);
  }
  return result;
}

/** An evaluation function that only includes the factors that are super fast to calculate */
export function fastEval(
  possibility: Possibility,
  nextPieceId: PieceId,
  level: number,
  lines: number,
  aiMode: AiMode,
  aiParams: AiParams
) {
  let {
    surfaceArray,
    numHoles,
    numLinesCleared,
    boardAfter,
    holeCells,
  } = possibility;
  const surfaceArrayWithCol10 = surfaceArray;
  surfaceArray = surfaceArray.slice(0, 9);

  if (!aiParams) {
    throw new Error("No AI Params provided: " + aiParams);
  }

  // Preliminary calculations
  let [
    correctedSurface,
    totalHeightCorrected,
  ] = utils.correctSurfaceForExtremeGaps(surfaceArray);
  const levelAfterPlacement = utils.getLevelAfterLineClears(
    level,
    lines,
    numLinesCleared
  );
  const maxSafeCol9Height = Math.max(
    4,
    aiParams.MAX_4_TAP_LOOKUP[levelAfterPlacement] - 5
  );
  if (aiParams.BURN_COEF > -500) {
    correctedSurface = correctSurfaceForDoubleWell(
      correctedSurface,
      maxSafeCol9Height
    );
  }

  if (aiMode === AiMode.KILLSCREEN || aiMode === AiMode.DIG) {
    numHoles += utils.countHolesInColumn(
      WELL_COLUMN,
      boardAfter,
      surfaceArrayWithCol10,
      holeCells,
      /* isWell= */ aiMode === AiMode.DIG // It's still a well if you're digging, but not on killscreen
    );
  }
  const scareHeight = utils.getScareHeight(levelAfterPlacement, aiParams);
  const spireHeight = getSpireHeight(surfaceArray, scareHeight);
  const avgHeightAboveScareLine = getAverageHeightAboveScareLine(
    surfaceArray,
    scareHeight
  );

  let surfaceFactor =
    aiParams.SURFACE_COEF *
    getSurfaceValue(
      IS_NON_RIGHT_WELL ? surfaceArrayWithCol10 : correctedSurface,
      totalHeightCorrected,
      aiParams
    );
  let killscreenSurfaceLeftFactor =
    aiParams.LEFT_SURFACE_COEF *
    getLeftSurfaceValue(boardAfter, aiParams, level);
  const holeFactor = numHoles * aiParams.HOLE_COEF;
  const estimatedHoleWeightBurnFactor = numHoles * (aiParams.BURN_COEF * 3);
  const lineClearFactor = getLineClearValue(numLinesCleared, aiParams);
  const spireHeightFactor =
    aiParams.SPIRE_HEIGHT_COEF *
    Math.pow(spireHeight, aiParams.SPIRE_HEIGHT_EXPONENT);
  const avgHeightFactor =
    aiParams.AVG_HEIGHT_COEF *
    Math.pow(avgHeightAboveScareLine, aiParams.AVG_HEIGHT_EXPONENT);
  const col10BurnFactor =
    countBlocksInWell(boardAfter).length * aiParams.BURN_COEF; // Any blocks on col 10 will result in a burn

  const factors = {
    surfaceFactor,
    killscreenSurfaceLeftFactor,
    holeFactor,
    estimatedHoleWeightBurnFactor,
    lineClearFactor,
    spireHeightFactor,
    avgHeightFactor,
    col10BurnFactor,
  };

  // Sum without explanation for speed purposes.
  let total = 0;
  for (const key in factors) {
    total += factors[key];
  }
  return [total, ""];
}

/**
 * Evaluates a given possibility based on a number of factors.
 * NB: @param nextPieceId CAN be null if you want the NNB value of a possiblity.
 */
export function getValueOfPossibility(
  possibility: Possibility,
  level,
  lines,
  aiMode,
  shouldLog,
  aiParams
) {
  let {
    surfaceArray,
    numHoles,
    holeCells,
    numLinesCleared,
    boardAfter,
  } = possibility;
  const surfaceArrayWithCol10 = surfaceArray;
  surfaceArray = surfaceArray.slice(0, 9);

  if (!aiParams) {
    throw new Error("No AI Params provided: " + aiParams);
  }

  // Correct the inputs for certain conditions
  let [
    correctedSurface,
    totalHeightCorrected,
  ] = utils.correctSurfaceForExtremeGaps(surfaceArray);
  const levelAfterPlacement = utils.getLevelAfterLineClears(
    level,
    lines,
    numLinesCleared
  );
  const maxSafeCol9Height = Math.max(
    4,
    aiParams.MAX_4_TAP_LOOKUP[levelAfterPlacement] - 5
  );
  const estimatedBurnsDueToEarlyDoubleWell = estimateBurnsDueToEarlyDoubleWell(
    surfaceArray,
    maxSafeCol9Height
  );
  if (aiParams.BURN_COEF > -500) {
    correctedSurface = correctSurfaceForDoubleWell(
      correctedSurface,
      maxSafeCol9Height
    );
  }
  // Sometimes add holes in the designated well column, based on the game state
  if (aiMode === AiMode.KILLSCREEN || aiMode === AiMode.DIG) {
    numHoles += utils.countHolesInColumn(
      WELL_COLUMN,
      boardAfter,
      surfaceArrayWithCol10,
      holeCells,
      /* isWell= */ aiMode === AiMode.DIG // It's still a well if you're digging, but not on killscreen
    );
  }

  // Precompute values needed in calculating the factors
  const scareHeight = utils.getScareHeight(levelAfterPlacement, aiParams);
  const spireHeight = getSpireHeight(surfaceArray, scareHeight);
  const avgHeightAboveScareLine = getAverageHeightAboveScareLine(
    aiMode == AiMode.KILLSCREEN ? surfaceArrayWithCol10 : surfaceArray,
    scareHeight
  );
  const maxDirtyTetrisHeight = aiParams.MAX_DIRTY_TETRIS_HEIGHT * scareHeight;
  const tetrisReady = isTetrisReadyRightWell(boardAfter);
  const [
    rowsNeedingToBurn,
    rowsNeedingToBurnIfTucksFail,
  ] = getRowsNeedingToBurn(
    boardAfter,
    surfaceArrayWithCol10,
    maxDirtyTetrisHeight,
    holeCells
  );
  const [leftRange, rightRange] = getPieceRanges(
    boardAfter,
    level,
    "I",
    1,
    aiParams.INPUT_FRAME_TIMELINE
  );
  const minReachableX = leftRange + 5;
  const maxReachableX = rightRange + 5;
  const baseCoveredWellFactor = getCoveredWellFactor(
    boardAfter,
    surfaceArray,
    scareHeight,
    aiParams,
    aiMode,
    rowsNeedingToBurn,
    holeCells,
    minReachableX,
    maxReachableX
  );
  for (const row of countBlocksInWell(boardAfter)) {
    rowsNeedingToBurn.add(row);
    rowsNeedingToBurnIfTucksFail.delete(row);
  }
  const guaranteedBurns =
    rowsNeedingToBurn.size + 0 * rowsNeedingToBurnIfTucksFail.size;

  const leftIsInaccessible = boardHelper.boardHasInaccessibileLeft(
    boardAfter,
    surfaceArray,
    levelAfterPlacement,
    aiParams,
    aiMode
  );
  const rightIsInaccessible = boardHelper.boardHasInaccessibileRight(
    boardAfter,
    surfaceArray,
    levelAfterPlacement,
    aiParams,
    aiMode
  );

  // TODO: remove logic like this >-500 thing and make method IsPlayingPerfect()
  const earlyDoubleWellFactor =
    aiParams.BURN_COEF > -500
      ? aiParams.BURN_COEF * estimatedBurnsDueToEarlyDoubleWell * 0.6
      : 0;
  let surfaceFactor =
    aiParams.SURFACE_COEF *
    getSurfaceValue(
      IS_NON_RIGHT_WELL ? surfaceArrayWithCol10 : correctedSurface,
      totalHeightCorrected,
      aiParams
    );
  let killscreenSurfaceLeftFactor =
    aiParams.LEFT_SURFACE_COEF *
    getLeftSurfaceValue(boardAfter, aiParams, level);
  const tetrisReadyFactor =
    aiParams.TETRIS_READY_COEF * (false ? -1 : tetrisReady ? 1 : 0);
  const holeFactor = numHoles * aiParams.HOLE_COEF;
  const holeWeightFactor =
    (rowsNeedingToBurn.size + 0 * rowsNeedingToBurnIfTucksFail.size) *
    aiParams.HOLE_WEIGHT_COEF;
  const lineClearFactor = getLineClearValue(numLinesCleared, aiParams);
  const guaranteedBurnsFactor = guaranteedBurns * aiParams.BURN_COEF;
  const spireHeightFactor =
    aiParams.SPIRE_HEIGHT_COEF *
    Math.pow(spireHeight, aiParams.SPIRE_HEIGHT_EXPONENT);
  const avgHeightFactor =
    aiParams.AVG_HEIGHT_COEF *
    Math.pow(avgHeightAboveScareLine, aiParams.AVG_HEIGHT_EXPONENT);
  const coveredWellFactor = baseCoveredWellFactor * aiParams.COL_10_COEF;
  const col9Factor = getColumn9Factor(
    aiParams,
    surfaceArrayWithCol10,
    maxSafeCol9Height
  );
  const unableToBurnFactor =
    aiParams.UNABLE_TO_BURN_COEF *
    getUnableToBurnFactor(
      boardAfter,
      surfaceArray,
      scareHeight,
      aiParams,
      holeCells,
      minReachableX,
      maxReachableX
    );
  const builtOutLeftFactor = getBuiltOutLeftFactor(
    boardAfter,
    surfaceArray,
    aiParams,
    aiMode,
    scareHeight
  );
  const builtOutRightFactor =
    aiParams.BUILT_OUT_RIGHT_COEF *
    getBuiltOutRightFactor(boardAfter, surfaceArray);
  const inaccessibleLeftFactor = leftIsInaccessible
    ? aiParams.INACCESSIBLE_LEFT_COEF
    : 0;
  const inaccessibleRightFactor = rightIsInaccessible
    ? aiParams.INACCESSIBLE_RIGHT_COEF
    : 0;
  const inputCostFactor = possibility.inputCost;
  const levelCorrectionFactor = levelAfterPlacement >= 29 ? 200 : 0;

  const factors = {
    surfaceFactor,
    killscreenSurfaceLeftFactor,
    earlyDoubleWellFactor,
    holeFactor,
    holeWeightFactor,
    lineClearFactor,
    guaranteedBurnsFactor,
    spireHeightFactor,
    avgHeightFactor,
    coveredWellFactor,
    unableToBurnFactor,
    col9Factor,
    tetrisReadyFactor,
    builtOutLeftFactor,
    builtOutRightFactor,
    inaccessibleLeftFactor,
    inaccessibleRightFactor,
    inputCostFactor,
    levelCorrectionFactor,
  };

  let [totalValue, explanation] = compileFactors(factors, aiMode);

  if (aiParams.BURN_COEF < -500) {
    totalValue = Math.max(-200000, totalValue);
  }

  if (shouldLog) {
    console.log(
      `---- Evaluated possiblity: ${possibility.placement}, mode: ${aiMode}\n`,
      explanation
    );
  }

  return [totalValue, explanation];
}

function compileFactors(factors: Object, aiMode: AiMode): [number, string] {
  let totalValue = 0;
  let explanation = "";
  for (const key in factors) {
    const val = factors[key];

    // Crash instantly if any of the factors are NaN (it's better than still running and making bad placements)
    if (isNaN(val)) {
      throw new Error(`NaN detected for factor: ${key}`);
    }

    if (val !== 0) {
      totalValue += val;
      const shortKeyName = key.substr(0, key.length - 6);
      explanation += `${shortKeyName}: ${val.toFixed(2)}, `;
    }
  }
  explanation += `\nSUBTOTAL: ${totalValue}, `;
  explanation += `Mode: ${aiMode}`;

  return [totalValue, explanation];
}
