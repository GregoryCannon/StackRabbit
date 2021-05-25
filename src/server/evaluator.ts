const rankLookup = require("./rank-lookup");
const killscreenRanks = require("../../docs/killscreen_ranks");
import {
  RANKS_15_HZ,
  RANKS_12HZ_5K,
  RANKS_13_5_HZ,
  VALUE_ITERATED_RANKS_13_5_HZ,
} from "../../docs/killscreen_ranks";
import * as boardHelper from "./board_helper";
import { getParams } from "./params";
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
    surfaceArray[7] > maxSafeCol9Height &&
    surfaceArray[6] > maxSafeCol9Height
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

/** Get the rank of the left-3-column surface (killscreen-only) */
function getLeftSurfaceValue(board, aiParams, level) {
  const max4Tap = aiParams.MAX_4_TAP_LOOKUP[level];
  const max5Tap = aiParams.MAX_5_TAP_LOOKUP[level];

  const leftSurface = boardHelper.getLeftSurface(board, max4Tap + 3);

  if (max4Tap == 3 && max5Tap == -1) {
    return RANKS_12HZ_5K[leftSurface] || 0;
  } else if (max4Tap == 5 && max5Tap == 0) {
    return VALUE_ITERATED_RANKS_13_5_HZ[leftSurface] || 0;
  } else if (max4Tap == 6 && max5Tap == 2) {
    return RANKS_15_HZ[leftSurface] || 0;
  }
  return 0;
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
  let total = 0;
  for (const height of surfaceArray) {
    total += height;
  }
  const averageHeight = total / surfaceArray.length;
  return Math.max(0, averageHeight - scareHeight);
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
  aiParams: AiParams
) {
  let totalBlocks = 0;
  const surfaceWithoutCol9 = surfaceArray.slice(0, 8);
  let col9Height = surfaceArray[8];
  // If col 10 is also filled in that row, it's not bad to have c9 filled
  while (
    col9Height > 0 &&
    board[NUM_ROW - col9Height][9] === SquareState.FULL
  ) {
    col9Height--;
  }
  for (const height of surfaceWithoutCol9) {
    if (height < col9Height) {
      totalBlocks += Math.pow(
        col9Height - height,
        aiParams.UNABLE_TO_BURN_DIFF_EXP
      );
    }
  }
  // Scale the importance based on the height relative to scare height
  const heightMultiplier = surfaceArray[8] / Math.max(scareHeight, 2);
  return totalBlocks * heightMultiplier;
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
function getRowsNeedingToBurn(
  board,
  surfaceArray,
  maxDirtyTetrisHeight,
  aiMode
): [Set<number>, Set<number>] {
  const linesNeededToClear: Set<number> = new Set();
  const linesNeededToClearIfTucksFail: Set<number> = new Set();
  const rightColBoundary =
    aiMode === AiMode.DIG ? NUM_COLUMN - 1 : NUM_COLUMN - 2;
  for (let col = 0; col <= rightColBoundary; col++) {
    // Go down to the top of the stack in that column
    let row = 0;
    while (row < NUM_ROW && board[row][col] == SquareState.EMPTY) {
      row++;
    }
    // Track the full rows we pass through
    const rowsInStackPassedThrough: Set<number> = new Set();
    while (row < NUM_ROW - 1) {
      rowsInStackPassedThrough.add(row);
      row++;
      if (board[row][col] === SquareState.EMPTY) {
        if (utils.isTuckSetup(row, col, board, surfaceArray)) {
          for (const line of rowsInStackPassedThrough) {
            linesNeededToClearIfTucksFail.add(line);
          }
        }

        // Ignore holes that are under the dirty tetris line and not in the Tetris zone
        const canBeDirtyTetrisedOver =
          NUM_ROW - row <= maxDirtyTetrisHeight &&
          board[row][NUM_COLUMN - 1] == SquareState.FULL;
        if (canBeDirtyTetrisedOver) {
          break;
        }
        // Otherwise, We found a hole. All the full rows above it will need to be cleared
        for (const line of rowsInStackPassedThrough) {
          linesNeededToClear.add(line);
        }
      }
    }
  }

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
  if (aiParams.HIGH_COL_9_COEF === 0 || surfaceArray[8] <= 4) {
    return 0;
  }
  const col9HeightAboveComfort = Math.max(
    0,
    surfaceArray[8] - maxSafeCol9Height
  );
  if (col9HeightAboveComfort === 0) {
    return 0;
  }
  return (
    aiParams.HIGH_COL_9_COEF &&
    Math.pow(col9HeightAboveComfort, aiParams.HIGH_COL_9_EXP) *
      aiParams.HIGH_COL_9_COEF
  );
}

/** Calculate a factor that penalizes filling column 10. Specifically, it counts the number of cells
 * in the other columns that would needed to be filled to clear away the block on column 10.
 * However, since holes *want* to have column 10 filled, count them oppositely.
 * Additionally, there's a penalty for the column 10 block being high up.
 */
function getColumn10Factor(
  board,
  scareHeight,
  aiParams,
  rowsNeedingToBurn: Set<number>
) {
  let sum = 0;
  for (let row = 0; row < NUM_ROW; row++) {
    // If column 10 filled, add to the sum
    if (board[row][NUM_COLUMN - 1] == SquareState.FULL) {
      for (let col = 0; col < NUM_COLUMN - 1; col++) {
        if (board[row][col] == SquareState.FULL) {
          continue;
        }

        if (!rowsNeedingToBurn.has(row)) {
          // How bad an exposed col 10 is scales exponentially with height
          const heightMultiplier =
            scareHeight < 2 ? 1 : (NUM_ROW - row) / scareHeight;
          sum += Math.pow(
            heightMultiplier,
            aiParams.COL_10_HEIGHT_MULTIPLIER_EXP
          );
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

function hasHoleInTetrisZone(board, holeCells) {
  // Calculate where the next Tetris will be built
  let row = 0;
  while (row < NUM_ROW && board[row][9] == SquareState.EMPTY) {
    row++;
  }
  // Both inclusive
  const tetrisZoneStart = row - 4;
  const tetrisZoneEnd = row - 1;

  for (const [row, col] of holeCells) {
    if (row <= tetrisZoneEnd && row >= tetrisZoneStart) {
      return true;
    }
  }
  return false;
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

function getSurfaceValue(
  surfaceArray: Array<number>,
  totalHeightCorrected: number,
  nextPieceId: PieceId,
  aiParams: AiParams
) {
  let rawValue = rankLookup.getValueOfBoardSurface(surfaceArray, nextPieceId);
  // Add in the extreme gap penalty prior to transforming the rank
  rawValue += totalHeightCorrected * aiParams.EXTREME_GAP_COEF;
  const A = 150;
  const B = A / 30;
  // console.log(surfaceArray, nextPieceId, rawValue);
  return rawValue + B - A / Math.max(1, rawValue);
}

export function getLineClearValue(numLinesCleared, aiParams) {
  return numLinesCleared == 4
    ? aiParams.TETRIS_COEF
    : numLinesCleared > 0
    ? aiParams.BURN_COEF * numLinesCleared
    : 0;
}

function getLowLeftFactor(surfaceArray: Array<number>, averageHeight: number) {
  return Math.min(0, surfaceArray[0] - averageHeight);
}

function getBuiltOutLeftFactor(
  boardAfter: Board,
  surfaceArray: Array<number>,
  scareHeight: number,
  aiParams: AiParams,
  aiMode: AiMode
) {
  // Don't build out the left if there's a hole there
  if (
    aiMode === AiMode.DIG ||
    boardHelper.hasHoleInColumn(boardAfter, 0, /* numRowsFromTop= */ 5) ||
    boardHelper.hasHoleInColumn(boardAfter, 1, /* numRowsFromTop= */ 5)
  ) {
    return 0;
  }

  // Handle low left cases first
  const averageHeight = surfaceArray.slice(2, 8).reduce((a, b) => a + b) / 7;
  if (surfaceArray[0] < averageHeight) {
    return (
      -1 *
      aiParams.BUILT_OUT_LEFT_COEF *
      Math.pow(averageHeight - surfaceArray[0], aiParams.LOW_LEFT_EXP)
    );
  }

  // Otherwise reward building out the left
  const col1Height = surfaceArray[0];
  return aiParams.BUILT_OUT_LEFT_COEF * Math.max(0, col1Height - scareHeight);
}

function getBuiltOutRightFactor(boardAfter, scareHeight) {
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
  return Math.max(0, col10Height - scareHeight);
}

export function rateSurface(surfaceArray): string {
  // Correct the inputs for certain conditions
  const aiParams = getParams();
  let [correctedSurface, _] = utils.correctSurfaceForExtremeGaps(
    surfaceArray.slice(0, 9)
  );
  let surfaceFactorNoNextBox = rankLookup.getValueOfBoardSurface(
    correctedSurface,
    null
  );
  let result = "No next box: " + surfaceFactorNoNextBox.toFixed(2);
  for (const pieceId of utils.POSSIBLE_NEXT_PIECES) {
    result +=
      "\n" +
      pieceId +
      ": " +
      rankLookup.getValueOfBoardSurface(correctedSurface, pieceId).toFixed(2);
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
  let { surfaceArray, numHoles, numLinesCleared, boardAfter } = possibility;
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
  const adjustedNumHoles =
    numHoles +
    (aiMode === AiMode.KILLSCREEN && countCol10Holes(boardAfter) * 0.7);
  const scareHeight = utils.getScareHeight(levelAfterPlacement, aiParams);
  const spireHeight = getSpireHeight(surfaceArray, scareHeight);
  const avgHeightAboveScareLine = getAverageHeightAboveScareLine(
    surfaceArray,
    scareHeight
  );

  let surfaceFactor =
    aiParams.SURFACE_COEF *
    getSurfaceValue(
      correctedSurface,
      totalHeightCorrected,
      nextPieceId,
      aiParams
    );
  let killscreenSurfaceLeftFactor =
    aiParams.LEFT_SURFACE_COEF *
    getLeftSurfaceValue(boardAfter, aiParams, level);
  const holeFactor = adjustedNumHoles * aiParams.HOLE_COEF;
  const estimatedHoleWeightBurnFactor =
    adjustedNumHoles * (aiParams.BURN_COEF * 3);
  const lineClearFactor = getLineClearValue(numLinesCleared, aiParams);
  const spireHeightFactor =
    aiParams.SPIRE_HEIGHT_COEF *
    Math.pow(spireHeight, aiParams.SPIRE_HEIGHT_EXPONENT);
  const avgHeightFactor =
    aiParams.AVG_HEIGHT_COEF *
    Math.pow(avgHeightAboveScareLine, aiParams.AVG_HEIGHT_EXPONENT);
  const col10BurnFactor =
    countBlocksInColumn10(boardAfter) * aiParams.BURN_COEF; // Any blocks on col 10 will result in a burn

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
  nextPieceId: PieceId,
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
    correctedSurface,
    maxSafeCol9Height
  );
  if (aiParams.BURN_COEF > -500) {
    correctedSurface = correctSurfaceForDoubleWell(
      correctedSurface,
      maxSafeCol9Height
    );
  }
  const adjustedNumHoles =
    numHoles +
    (aiMode === AiMode.KILLSCREEN && countCol10Holes(boardAfter) * 0.7);

  // Precompute values needed in calculating the factors
  const scareHeight = utils.getScareHeight(levelAfterPlacement, aiParams);
  const spireHeight = getSpireHeight(surfaceArray, scareHeight);
  const avgHeightAboveScareLine = getAverageHeightAboveScareLine(
    aiMode == AiMode.KILLSCREEN ? surfaceArrayWithCol10 : surfaceArray,
    scareHeight
  );
  const tetrisReady = isTetrisReadyRightWell(boardAfter);
  const holeInTetrisZone = hasHoleInTetrisZone(boardAfter, holeCells);
  const [
    rowsNeedingToBurn,
    rowsNeedingToBurnIfTucksFail,
  ] = getRowsNeedingToBurn(
    boardAfter,
    surfaceArrayWithCol10,
    aiParams.MAX_DIRTY_TETRIS_HEIGHT * scareHeight,
    aiMode
  );

  const leftIsInaccessible = boardHelper.boardHasInaccessibileLeft(
    boardAfter,
    surfaceArray,
    levelAfterPlacement,
    aiParams,
    aiMode
  );
  const rightIsInaccessible = boardHelper.boardHasInaccessibileRight(
    boardAfter,
    levelAfterPlacement,
    aiParams,
    aiMode
  );

  const earlyDoubleWellFactor =
    aiParams.BURN_COEF > -500
      ? aiParams.BURN_COEF * estimatedBurnsDueToEarlyDoubleWell * 0.6
      : 0;
  let surfaceFactor =
    aiParams.SURFACE_COEF *
    getSurfaceValue(
      correctedSurface,
      totalHeightCorrected,
      nextPieceId,
      aiParams
    );
  let killscreenSurfaceLeftFactor =
    aiParams.LEFT_SURFACE_COEF *
    getLeftSurfaceValue(boardAfter, aiParams, level);
  const tetrisReadyFactor =
    aiParams.TETRIS_READY_COEF * (holeInTetrisZone ? -1 : tetrisReady ? 1 : 0);
  const holeFactor = adjustedNumHoles * aiParams.HOLE_COEF;
  const holeWeightFactor =
    (rowsNeedingToBurn.size + 0.1 * rowsNeedingToBurnIfTucksFail.size) *
    aiParams.HOLE_WEIGHT_COEF;
  const holeWeightBurnFactor =
    (rowsNeedingToBurn.size + 0.5 * rowsNeedingToBurnIfTucksFail.size) *
    aiParams.BURN_COEF;
  const lineClearFactor = getLineClearValue(numLinesCleared, aiParams);
  const spireHeightFactor =
    aiParams.SPIRE_HEIGHT_COEF *
    Math.pow(spireHeight, aiParams.SPIRE_HEIGHT_EXPONENT);
  const avgHeightFactor =
    aiParams.AVG_HEIGHT_COEF *
    Math.pow(avgHeightAboveScareLine, aiParams.AVG_HEIGHT_EXPONENT);
  const col10Factor =
    getColumn10Factor(boardAfter, scareHeight, aiParams, rowsNeedingToBurn) *
    aiParams.COL_10_COEF;
  const col10BurnFactor =
    countBlocksInColumn10(boardAfter) * aiParams.BURN_COEF; // Any blocks on col 10 will result in a burn
  const col9Factor = getColumn9Factor(
    aiParams,
    surfaceArray,
    maxSafeCol9Height
  );
  const unableToBurnFactor =
    aiParams.UNABLE_TO_BURN_COEF *
    getUnableToBurnFactor(boardAfter, surfaceArray, scareHeight, aiParams);
  const builtOutLeftFactor = getBuiltOutLeftFactor(
    boardAfter,
    surfaceArray,
    scareHeight,
    aiParams,
    aiMode
  );
  const builtOutRightFactor =
    aiParams.BUILT_OUT_RIGHT_COEF *
    getBuiltOutRightFactor(boardAfter, scareHeight);
  const inaccessibleLeftFactor = leftIsInaccessible
    ? aiParams.INACCESSIBLE_LEFT_COEF
    : 0;
  const inaccessibleRightFactor = rightIsInaccessible
    ? aiParams.INACCESSIBLE_RIGHT_COEF
    : 0;
  const inputCostFactor = possibility.inputCost;

  const factors = {
    surfaceFactor,
    killscreenSurfaceLeftFactor,
    earlyDoubleWellFactor,
    holeFactor,
    holeWeightFactor,
    holeWeightBurnFactor,
    lineClearFactor,
    spireHeightFactor,
    avgHeightFactor,
    col10Factor,
    col10BurnFactor,
    unableToBurnFactor,
    col9Factor,
    tetrisReadyFactor,
    builtOutLeftFactor,
    builtOutRightFactor,
    inaccessibleLeftFactor,
    inaccessibleRightFactor,
    inputCostFactor,
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

    totalValue += val;
    const shortKeyName = key.substr(0, key.length - 6);
    explanation += `${shortKeyName}: ${val.toFixed(2)}, `;
  }
  explanation += `SUBTOTAL: ${totalValue}, `;
  explanation += `Mode: ${aiMode}`;

  return [totalValue, explanation];
}
