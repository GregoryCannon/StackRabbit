const evaluator = require("./evaluator");
const aiModeManager = require("./ai_mode_manager");
const boardHelper = require("./board_helper");
const {
  POSSIBILITIES_TO_CONSIDER,
  CHAIN_POSSIBILITIES_TO_CONSIDER,
  modifyParamsForAiMode,
} = require("./params");
import * as utils from "./utils";

/**
 * Finds the N highest valued moves, without doing any search into placements of the next piece.
 * Can be called with or without a next piece, and will function accordingly.
 */
function searchDepth1(
  searchState: SearchState,
  shouldLog: boolean,
  aiParams: AiParams,
  aiMode: AiMode
) {
  // Get the possible moves
  const possibilityList = boardHelper.getPossibleMoves(
    searchState.board,
    searchState.currentPieceId,
    searchState.level,
    searchState.existingXOffset,
    searchState.existingYOffset,
    searchState.firstShiftDelay,
    searchState.existingRotation,
    aiParams.TAP_ARR,
    /* shouldLog= */ false && shouldLog
  );

  // Get the top contenders, sorted best -> worst
  for (const possibility of possibilityList) {
    const [value, explanation] = evaluator.getValueOfPossibility(
      possibility,
      searchState.nextPieceId,
      searchState.level,
      searchState.lines,
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
 * Main function called externally.
 * Finds the best move for a given scenario (with or without a next box). Behavior changes based on the search depth:
 *   - If searchDepth is 1, it looks at all the possible placements of the current
 *        piece, then evaluates the resulting states.
 *   - If searchDepth is 2, it takes the most promising of those 1-states and calculates
 *        the 2-states that can occur from them.
 *   - If searchDepth is 3, it takes a small number of the best 2-states and calculates
 *        placements for all 7 theoretical next pieces that could occur.
 */

function getBestMove(
  searchState: SearchState,
  shouldLog: boolean,
  initialAiParams: AiParams,
  paramMods: ParamMods,
  searchDepth: number
) {
  const startTime = Date.now();

  // Get the AI mode (e.g. digging, scoring)
  const aiMode = aiModeManager.getAiMode(
    searchState.board,
    searchState.lines,
    searchState.level,
    initialAiParams
  );
  const aiParams = modifyParamsForAiMode(initialAiParams, aiMode, paramMods);
  aiParams.MAX_5_TAP_HEIGHT = boardHelper.calculateTapHeight(
    searchState.level,
    aiParams.TAP_ARR,
    aiParams.FIRST_TAP_DELAY,
    5
  );
  aiParams.MAX_4_TAP_HEIGHT = boardHelper.calculateTapHeight(
    searchState.level,
    aiParams.TAP_ARR,
    aiParams.FIRST_TAP_DELAY,
    5
  );

  /* ---------- Initial top-level search ------------ */

  const depth1Possibilities: Array<Possibility> = searchDepth1(
    searchState,
    shouldLog,
    aiParams,
    aiMode
  );

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
      depth1Possibilities.map((x) => [x.placement, x.evalExplanation])
    );
    console.log("\n\n---------");
  }

  /* ---------- Explore at depth 2 for promising moves ------------ */

  const depth2Possibilities = searchDepth2(
    depth1Possibilities.slice(0, POSSIBILITIES_TO_CONSIDER),
    searchState,
    aiParams,
    aiMode
  );

  return depth2Possibilities[0] || null;
}

function searchDepth2(
  depth1Possibilities: Array<Possibility>,
  prevSearchState: SearchState,
  aiParams: AiParams,
  aiMode: AiMode
): Array<PossibilityDepth2> {
  let depth2Possibilities: Array<PossibilityDepth2> = [];
  for (const outerPossibility of depth1Possibilities) {
    // Place the next piece in each possibility
    const levelAfter = utils.getLevelAfterLineClears(
      prevSearchState.level,
      prevSearchState.lines,
      outerPossibility.numLinesCleared
    );
    const newSearchState = {
      board: outerPossibility.boardAfter,
      currentPieceId: prevSearchState.nextPieceId,
      nextPieceId: null,
      level: levelAfter,
      lines: prevSearchState.lines + outerPossibility.numLinesCleared,
      existingXOffset: 0,
      existingYOffset: 0,
      firstShiftDelay: aiParams.FIRST_TAP_DELAY,
      existingRotation: 0,
    };
    const innerPossibilities = searchDepth1(
      newSearchState,
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

    // console.log(newD2Possibilities);
    // console.log(
    //   "current best:",
    //   depth2Possibilities[0] ? depth2Possibilities[0].totalValue : 0
    // );

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
  return depth2Possibilities;
}

module.exports = { getBestMove };
