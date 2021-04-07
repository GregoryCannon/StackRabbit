const evaluator = require("./evaluator");
const aiModeManager = require("./ai_mode_manager");
const BoardHelper = require("./board_helper");
const {
  POSSIBILITIES_TO_CONSIDER,
  CHAIN_POSSIBILITIES_TO_CONSIDER,
  modifyParamsForAiMode,
} = require("./params");
import * as utils from "./utils";

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
    possibility.evalScore = value as number;
    possibility.evalExplanation = explanation as string;
  }
  // Sort by value
  return possibilityList.sort((a, b) => b.evalScore - a.evalScore);
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
  aiMode
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

  // Get the top contenders, sorted best -> worst
  return pickBestNMoves(
    possibilityList,
    nextPieceId,
    level,
    lines,
    aiMode,
    aiParams
  );
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
  // Get the AI mode (e.g. digging, scoring)
  const aiMode = aiModeManager.getAiMode(
    startingBoard,
    lines,
    level,
    initialAiParams
  );
  const aiParams = modifyParamsForAiMode(initialAiParams, aiMode, paramMods);

  const sortedPossibilityList = getMostPromisingMoves(
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
  );

  if (shouldLog) {
    sortedPossibilityList.forEach((x) => {
      console.log(
        `${x.placement} : surface ${x.surfaceArray}, holes ${x.numHoles}, score ${x.evalScore}`
      );
      console.log(x.explanation);
    });
  }
  return sortedPossibilityList ? sortedPossibilityList[0] : null;
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

  // Get the AI mode (e.g. digging, scoring)
  const aiMode = aiModeManager.getAiMode(
    startingBoard,
    lines,
    level,
    initialAiParams
  );
  const aiParams = modifyParamsForAiMode(initialAiParams, aiMode, paramMods);

  /* ---------- Initial top-level search ------------ */

  const topN = getMostPromisingMoves(
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
  ).slice(0, POSSIBILITIES_TO_CONSIDER);

  const time2 = Date.now();
  if (shouldLog) {
    console.log("\tElapsed to get N most promising moves:", time2 - startTime);
    console.log("Num promising moves:", topN.length);
    console.log(
      "Promising moves",
      topN.map((x) => x.placement)
    );
    console.log("\n\n---------");
  }

  /* ---------- Explore at depth 2 for promising moves ------------ */

  let chainPossibilityList: Array<ChainPossibility> = [];
  for (const outerPossibility of topN) {
    // Place the next piece in each possibility
    const levelAfter = utils.getLevelAfterLineClears(
      level,
      lines,
      outerPossibility.numLinesCleared
    );
    const innerTopN = getMostPromisingMoves(
      outerPossibility.boardAfter,
      nextPieceId,
      /* nextPieceId= */ null,
      levelAfter,
      lines + outerPossibility.numLinesCleared,
      /* existingXOffset= */ 0,
      /* existingYOffset= */ 0,
      aiParams.FIRST_TAP_DELAY,
      /* existingRotation= */ 0,
      /* shouldLog= */ false,
      aiParams,
      aiMode
    ).slice(0, CHAIN_POSSIBILITIES_TO_CONSIDER);

    // Wrap each inner possibility with its outer move
    const topNChains = innerTopN.map((x: Possibility) => {
      const outerMovePartialValue = evaluator.getLineClearValue(
        outerPossibility.numLinesCleared,
        aiParams
      );
      const innerPlacement = x.placement;
      return {
        ...x,
        innerPlacement,
        placement: outerPossibility.placement,
        parentPlacementPartialValue: outerMovePartialValue,
        totalValue: x.evalScore + outerMovePartialValue,
      };
    });

    if (topNChains.length == 0) {
      continue;
    }

    // Merge with the current best list, leaving the highest N chain possibilities seen so far
    chainPossibilityList = utils
      .mergeSortedArrays(
        chainPossibilityList,
        topNChains,
        (x, y) => y.totalValue - x.totalValue
      )
      .slice(0, CHAIN_POSSIBILITIES_TO_CONSIDER);
  }

  return chainPossibilityList[0] || null;
}

module.exports = { getBestMoveWithSearch, getBestMoveNoSearch };
