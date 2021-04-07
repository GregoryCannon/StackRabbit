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
) {}

/**
 * Finds the N highest valued moves, without doing any search into placements of the next piece.
 * Can be called with or without a next piece, and will function accordingly.
 */
function getSortedPossibilityList(
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

function getBestMove(
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
  paramMods,
  searchDepth
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

  const depth1Possibilities: Array<Possibility> = getSortedPossibilityList(
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

  // If the search depth was 1, we're done
  if (searchDepth == 1) {
    if (shouldLog) {
      depth1Possibilities.forEach((x) => {
        console.log(
          `${x.placement} : surface ${x.surfaceArray}, holes ${x.numHoles}, score ${x.evalScore}`
        );
        console.log(x.evalExplanation);
      });
    }
    return depth1Possibilities ? depth1Possibilities[0] : null;
  }

  const time2 = Date.now();
  if (shouldLog) {
    console.log("\tElapsed to get N most promising moves:", time2 - startTime);
    console.log("Num promising moves:", depth1Possibilities.length);
    console.log(
      "Promising moves",
      depth1Possibilities.map((x) => x.placement)
    );
    console.log("\n\n---------");
  }

  /* ---------- Explore at depth 2 for promising moves ------------ */

  let depth2Possibilities: Array<ChainPossibility> = [];
  for (const outerPossibility of depth1Possibilities) {
    // Place the next piece in each possibility
    const levelAfter = utils.getLevelAfterLineClears(
      level,
      lines,
      outerPossibility.numLinesCleared
    );
    const innerPossibilities = getSortedPossibilityList(
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
    const newD2Possibilities = innerPossibilities.map((x: Possibility) => {
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

    // Merge with the current best list, leaving the highest N chain possibilities seen so far
    if (newD2Possibilities.length > 0) {
      depth2Possibilities = utils
        .mergeSortedArrays(
          depth2Possibilities,
          newD2Possibilities,
          (x, y) => y.totalValue - x.totalValue
        )
        .slice(0, CHAIN_POSSIBILITIES_TO_CONSIDER);
    }
  }

  return depth2Possibilities[0] || null;
}

module.exports = { getBestMove };
