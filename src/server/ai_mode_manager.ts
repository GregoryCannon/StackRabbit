import { IS_PAL, WELL_COLUMN } from "./params";
import * as utils from "./utils";
const SquareState = utils.SquareState;
const NUM_ROW = utils.NUM_ROW;
const NUM_COLUMN = utils.NUM_COLUMN;

export function getAiMode(
  board,
  lines,
  level,
  currentPieceId: PieceId,
  aiParams
) {
  const killscreenLevel = IS_PAL ? 19 : 29;
  if (level >= killscreenLevel && aiParams.MAX_5_TAP_LOOKUP[level] <= 4) {
    return AiMode.KILLSCREEN;
  }
  // Dig, unless it's too close to the killscreen to dig
  if (shouldUseDigMode(board, level, currentPieceId, aiParams)) {
    return AiMode.DIG;
  }
  if (level >= killscreenLevel) {
    // This is checked after dig mode so that right well killscreen AI can still dig
    return AiMode.KILLSCREEN_RIGHT_WELL;
  }
  if (lines >= 220 && level === killscreenLevel - 1) {
    return AiMode.NEAR_KILLSCREEN;
  }

  return AiMode.STANDARD;
}

/** The logic here is quite complex, it's a shame it has to be manually coded instead of trained.
 * Current logic:
 *  - If you have a high-up hole that is blocking the well, go dig it out immediately
 *  - If the hole is in the tetris zone, go dig it
 *  - If the hole doesn't have many filled lines above it, go dig it
 *  - Otherwise, play on and plan to play a few rows off the bottom
 */
function shouldUseDigMode(
  board,
  level,
  currentPieceId: PieceId,
  aiParams: AiParams
) {
  // Never dig while playing perfect
  if (aiParams.BURN_COEF < -500) {
    return false;
  }
  // Calculate where the next Tetris will be built
  let row = 0;
  while (row < NUM_ROW && board[row][WELL_COLUMN] == SquareState.EMPTY) {
    row++;
  }
  // Both inclusive
  const tetrisZoneStart = row - 4;
  const tetrisZoneEnd = row - 1;

  const surfaceArrayWithCol10 = utils.getSurfaceArrayAndHoles(board)[0];

  const scareHeight = utils.getScareHeight(level, aiParams);
  // TODO: when 'eventual board after line clear' implemented, check that the hole can ever
  // be under the max dirty tetris line
  const maxDirtyTetrisHeight = Math.round(
    aiParams.MAX_DIRTY_TETRIS_HEIGHT * scareHeight
  );

  function holeWarrantsDigging(row, col, firstFullRow, surfaceArray) {
    // If it's a tuck setup, don't dig
    if (utils.isTuckSetup(row, col, board, surfaceArray)[0]) {
      return false;
    }
    // If it's perfectly set up to play dirty, don't dig
    const blockingWell = board[row][NUM_COLUMN - 1] === SquareState.FULL;
    const numRowsOfGarbage = row - firstFullRow;
    if (
      blockingWell &&
      NUM_ROW - row <= maxDirtyTetrisHeight &&
      numRowsOfGarbage > 3
    ) {
      return false;
    }
    return (
      (currentPieceId === "I" &&
        row >= tetrisZoneStart &&
        row <= tetrisZoneEnd) ||
      numRowsOfGarbage <= 3 ||
      maxDirtyTetrisHeight == 0
    );
  }

  // Check for holes that are either in the Tetris zone or have <= 3 full lines above them (are reasonably accessible with burns)
  for (let col = 0; col < NUM_COLUMN; col++) {
    if (col == WELL_COLUMN) {
      continue;
    }
    // Navigate past the empty space above each column
    let row = 0;
    while (row < NUM_ROW && board[row][col] === SquareState.EMPTY) {
      row++;
    }
    const firstFullRow = row;

    // Now that we're in the stack, if there are empty cells, they're holes
    while (row < NUM_ROW - 1) {
      row++;
      if (board[row][col] === SquareState.EMPTY) {
        // Found hole
        if (
          holeWarrantsDigging(row, col, firstFullRow, surfaceArrayWithCol10)
        ) {
          return true;
        }
      }
    }
  }
  return false;
}
