const evaluator = require("./evaluator");
const aiModeManager = require("./ai_mode_manager");
const BoardHelper = require("./board_helper");
const { NUM_TO_CONSIDER, modifyParamsForAiMode } = require("./params");

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
    const [value, explanation] = evaluator.getValueOfPossibility(
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
 * Finds the N highest valued moves, without doing any search into placements of the next piece.
 * Can be called with or without a next piece, and will function accordingly.
 */
function getMostPromisingMoves(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  existingXOffset,
  existingYOffset,
  firstShiftDelay,
  existingRotation,
  shouldLog,
  aiParams,
  paramMods
) {
  // Get the possible moves
  const possibilityList = BoardHelper.getPossibleMoves(
    startingBoard,
    currentPieceId,
    level,
    existingXOffset,
    existingYOffset,
    aiParams.TAP_ARR,
    firstShiftDelay,
    existingRotation,
    /* shouldLog= */ false && shouldLog
  );

  // Get the AI mode (e.g. digging, scoring)
  const aiMode = aiModeManager.getAiMode(startingBoard, lines, level, aiParams);
  aiParams = modifyParamsForAiMode(aiParams, aiMode, paramMods);

  // Get the top contenders, sorted best -> worst
  const topN = pickBestNMoves(
    possibilityList,
    nextPieceId,
    level,
    lines,
    aiMode,
    NUM_TO_CONSIDER,
    aiParams
  );
  return { topN, aiMode, aiParams };
}

function getBestMoveNoSearch(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  existingXOffset,
  existingYOffset,
  firstShiftDelay,
  existingRotation,
  shouldLog,
  initialAiParams,
  paramMods
) {
  let { topN } = getMostPromisingMoves(
    startingBoard,
    currentPieceId,
    nextPieceId,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
    shouldLog,
    initialAiParams,
    paramMods
  );
  if (shouldLog) {
    topN.forEach((x) => {
      console.log(x.slice(0, 4));
      console.log(x[7]);
    });
  }
  return topN ? topN[0] : null;
}

function getBestMoveWithSearch(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  existingXOffset,
  existingYOffset,
  firstShiftDelay,
  existingRotation,
  shouldLog,
  initialAiParams,
  paramMods
) {
  const startTime = Date.now();

  const { topN, aiMode, aiParams } = getMostPromisingMoves(
    startingBoard,
    currentPieceId,
    nextPieceId,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
    shouldLog,
    initialAiParams,
    paramMods
  );

  const time2 = Date.now();
  if (shouldLog) {
    console.log("\tElapsed to get N most promising moves:", time2 - startTime);
    console.log("Num promising moves:", topN.length);
    console.log(
      "Promising moves",
      topN.map((x) => x.slice(0, 2))
    );
    console.log("\n\n---------");
  }

  // For each contender, place the next piece and maximize the resulting value
  let bestPossibilityAfterNextPiece = null;
  let bestValueAfterNextPiece = Number.MIN_SAFE_INTEGER;
  let bestIndex = 0; // The rank of the best placement (in terms of the original 'promising-ness' sort)
  let i = 0;
  for (const possibility of topN) {
    i++;
    // Place the next piece in each possibility
    const boardAfterOuterMove = possibility[5];
    const linesClearedOuterMove = possibility[4];
    const innerPossibilityList = BoardHelper.getPossibleMoves(
      boardAfterOuterMove,
      nextPieceId,
      level,
      /* existingXOffset= */ 0,
      /* existingYOffset= */ 0,
      aiParams.TAP_ARR,
      aiParams.FIRST_TAP_DELAY,
      /* existingRotation= */ 0,
      /* shouldLog= */ false && shouldLog
    );
    const innerTopN = pickBestNMoves(
      innerPossibilityList,
      null,
      level,
      lines,
      aiMode,
      1,
      aiParams
    );
    if (innerTopN.length == 0) {
      continue;
    }

    // Get a total score for this possibility (including line clears from the outer placement)
    const innerBestMove = innerTopN[0];
    const originalMovePartialValue = evaluator.getLineClearValue(
      linesClearedOuterMove,
      aiParams
    );
    const totalValue = innerBestMove[6] + originalMovePartialValue;

    // If new best, update local vars
    if (totalValue > bestValueAfterNextPiece) {
      bestValueAfterNextPiece = totalValue;
      bestPossibilityAfterNextPiece = possibility;
      bestIndex = i;
    }

    // Log details about the top-level possibility
    if (shouldLog) {
      console.log(
        `\nCurrent move: ${possibility[0]}, ${possibility[1]}. Next move: ${innerBestMove[0]}, ${innerBestMove[1]}.`
      );
      console.log("Final state eval:", innerBestMove[7], "mode:", aiMode); // Log inner explanation
      console.log(
        `\nSurface: ${innerBestMove[2]}, inner value: ${innerBestMove[6]}, original partial value: ${originalMovePartialValue}, \nFINAL TOTAL: ${totalValue}`
      );
      console.log("---------------------------------------------");
    }
  }

  if (shouldLog) {
    // Log performance info
    const msElapsedMoves = Date.now() - time2;
    console.log("\tElapsed per possibility:", msElapsedMoves / topN.length);
    console.log("\tElapsed on all moves:", msElapsedMoves);
  }

  if (shouldLog && bestPossibilityAfterNextPiece) {
    console.log(
      `\nSelected: ${bestPossibilityAfterNextPiece[0]}, ${bestPossibilityAfterNextPiece[1]}`
    );
    console.log("# Candidates:", topN.length, "Selected rank:", bestIndex);
  }

  // Send back the highest value move after the next piece is placed
  return bestPossibilityAfterNextPiece;
}

module.exports = { getBestMoveWithSearch, getBestMoveNoSearch };
