const evaluator = require("./evaluator");
const aiModeManager = require("./ai_mode_manager");
const boardHelper = require("./board_helper");
const { SEARCH_BREADTH, modifyParamsForAiMode } = require("./params");
import { getPossibleMoves } from "./move_search";
import { EVALUATION_BREADTH, IS_DROUGHT_MODE } from "./params";
import { predictSearchStateAtAdjustmentTime } from "./precompute";
import * as utils from "./utils";
import { POSSIBLE_NEXT_PIECES } from "./utils";

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

export function getBestMove(
  searchState: SearchState,
  shouldLog: boolean,
  initialAiParams: InitialAiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string,
  searchDepth: number,
  hypotheticalSearchDepth: number
): PossibilityChain {
  return (
    getSortedMoveList(
      searchState,
      shouldLog,
      initialAiParams,
      paramMods,
      inputFrameTimeline,
      searchDepth,
      hypotheticalSearchDepth
    )[0][0] || null
  );
}

export function getSortedMoveList(
  searchState: SearchState,
  shouldLog: boolean,
  initialAiParams: InitialAiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string,
  searchDepth: number,
  hypotheticalSearchDepth: number
): MoveSearchResult {
  // Add additional info to the base params (tap speed, dig/scoring mode, etc.)
  let aiParams = addTapInfoToAiParams(
    initialAiParams,
    searchState.level,
    inputFrameTimeline
  );
  const aiMode = aiModeManager.getAiMode(
    searchState.board,
    searchState.lines,
    searchState.level,
    searchState.currentPieceId,
    aiParams
  );
  aiParams = modifyParamsForAiMode(aiParams, aiMode, paramMods);

  let [bestConcrete, prunedConcrete] = searchConcretely(
    searchState,
    shouldLog,
    aiParams,
    aiMode,
    searchDepth
  );

  if (hypotheticalSearchDepth == 0) {
    return [bestConcrete, prunedConcrete];
  } else {
    // Prune the concrete search if it hasn't been pruned already
    if (bestConcrete.length > SEARCH_BREADTH[3]) {
      prunedConcrete = prunedConcrete.concat(
        bestConcrete.slice(SEARCH_BREADTH[3])
      );
      bestConcrete = bestConcrete.slice(0, SEARCH_BREADTH[3]);
    }
    const bestHypothetical = searchHypothetically(
      bestConcrete,
      aiParams,
      aiMode,
      hypotheticalSearchDepth,
      shouldLog
    );
    return [bestHypothetical, prunedConcrete];
  }
}

/** Does a full search of all possible placements given an board and maybe a next piece.
 * This concrete search is opposed to hypothetical search which looks over all possible next pieces.
 */
function searchConcretely(
  searchState: SearchState,
  shouldLog: boolean,
  aiParams: AiParams,
  aiMode: AiMode,
  searchDepth: number
): MoveSearchResult | null {
  if (searchDepth > 2 || searchDepth < 0) {
    throw new Error("Parameter out of bounds: searchDepth = " + searchDepth);
  }

  const [depth1Possibilities, prunedD1]: MoveSearchResult = searchDepth1(
    searchState,
    aiParams,
    aiMode,
    EVALUATION_BREADTH[1]
  );
  if (prunedD1.length > 0) {
    throw new Error("Pruned at depth 1");
  }

  // If the search depth was 1, we're done
  if (searchDepth == 1) {
    if (shouldLog) {
      depth1Possibilities.forEach((x) => {
        console.log(x.placement);
        console.log(x.inputSequence);
        console.log(x.evalExplanation);
      });
    }
    return [depth1Possibilities, []];
  }

  if (shouldLog) {
    console.log("Num promising moves:", depth1Possibilities.length);
    console.log("Promising moves");
    console.log(
      depth1Possibilities.map((x) => [
        x.placement,
        x.inputSequence,
        x.fastEvalScore || "no fast eval",
        x.evalExplanation,
      ])
    );
    console.log("\n\n--------------------------------------------");
  }

  /* ---------- Explore at depth 2 for promising moves ------------ */

  const [depth2Possibilities, prunedD2] = searchDepth2(
    depth1Possibilities.slice(0, SEARCH_BREADTH[2]),
    aiParams,
    aiMode
  );

  if (shouldLog) {
    console.log(
      depth2Possibilities.map((x) => [
        x.placement,
        x.totalValue,
        x.innerPossibility.placement,
        x.innerPossibility.evalExplanation,
      ])
    );
  }
  return [depth2Possibilities, prunedD2];
}

/** Normalizes a weight vector to an average value of 1 per cell (in-place). */
function normalize(weightVector) {
  const normalizedWeights = [];
  const len = weightVector.length;
  let total = weightVector.reduce((x, y) => x + y);
  for (let i = 0; i < len; i++) {
    normalizedWeights.push((weightVector[i] * len) / total);
  }
  return normalizedWeights;
}

function searchHypothetically(
  possibilityChains: Array<PossibilityChain>,
  aiParams: AiParams,
  aiMode: AiMode,
  hypotheticalSearchDepth: number,
  shouldLog: boolean
): Array<PossibilityChain> {
  if (hypotheticalSearchDepth < 1 || hypotheticalSearchDepth > 4) {
    throw new Error("Unsupported hypothetical search depth");
  }

  // Evaluate the weighted EV of each possibility chain
  let hypotheticalResults: Array<HypotheticalResult> = [];
  for (const [i,chain] of possibilityChains.entries()) {
    if (hypotheticalSearchDepth > 1){
      console.log(`Checking 2-chain ${i} of ${possibilityChains.length}`);
    }

    const bestMovesList = getBestMovesForAllPossibleSequences(
      chain,
      hypotheticalSearchDepth,
      aiParams,
      aiMode
    );

    const expectedValue = getExpectedValue(bestMovesList);

    // Attach the EV to the original possibility
    chain.expectedValue = expectedValue;
    if (chain.innerPossibility) {
      chain.innerPossibility.expectedValue = expectedValue;
    }

    hypotheticalResults.push({
      possibilityChain: chain,
      expectedValue,
      bestMoves: bestMovesList,
    });
  }

  // Sort by EV
  hypotheticalResults.sort((a, b) => b.expectedValue - a.expectedValue);

  // Maybe log info about the EV results
  if (shouldLog) {
    logExpectedValueResults(hypotheticalResults, hypotheticalSearchDepth, aiParams);
  }

  return hypotheticalResults.map((x) => x.possibilityChain);
}

/**
 * Finds the N highest valued moves, without doing any search into placements of the next piece.
 * Can be called with or without a next piece, and will function accordingly.
 */
function searchDepth1(
  searchState: SearchState,
  aiParams: AiParams,
  aiMode: AiMode,
  evalBreadth: number
): MoveSearchResult {
  // Get the possible moves
  let possibilityList = getPossibleMoves(
    searchState.board,
    searchState.currentPieceId,
    searchState.level,
    searchState.existingXOffset,
    searchState.existingYOffset,
    searchState.framesAlreadyElapsed,
    aiParams.INPUT_FRAME_TIMELINE,
    searchState.existingRotation,
    searchState.canFirstFrameShift,
    false
  );
  let pruned = [];

  // If there are more moves than we plan on evaluating, do a fast-eval and prune based on that
  if (possibilityList.length > evalBreadth) {
    for (const possibility of possibilityList) {
      const [value, _] = evaluator.fastEval(
        possibility,
        searchState.nextPieceId,
        searchState.level,
        searchState.lines,
        aiMode,
        aiParams
      );
      possibility.fastEvalScore = value;
    }

    // Prune the bad possibilities
    possibilityList.sort((a, b) => b.fastEvalScore - a.fastEvalScore);
    possibilityList = possibilityList.slice(0, evalBreadth);
    pruned = possibilityList.slice(evalBreadth);
  }

  // Evaluate each promising possibility and convert it to a 1-chain
  for (const possibility of possibilityList as Array<PossibilityChain>) {
    // Evaluate
    const [value, explanation] = evaluator.getValueOfPossibility(
      possibility,
      searchState.level,
      searchState.lines,
      aiMode,
      /* shouldLog= */ false,
      aiParams
    );
    possibility.evalScore = value as number;
    possibility.evalExplanation = explanation as string;

    // Convert to a 1-chain
    possibility.totalValue = value as number;
    possibility.partialValue = evaluator.getPartialValue(possibility, aiParams);
    possibility.searchStateAfterMove = getSearchStateAfter(
      searchState,
      possibility
    );
  }
  // Sort by value
  return [
    possibilityList.sort((x, y) => y.evalScore - x.evalScore) as Array<
      PossibilityChain
    >,
    pruned,
  ];
}

function searchDepth2(
  depth1Possibilities: Array<PossibilityChain>,
  aiParams: AiParams,
  aiMode: AiMode
): MoveSearchResult {
  // Best chains of two moves
  let best2Chains: Array<PossibilityChain> = [];
  let pruned: Array<PossibilityChain> = [];
  for (const outerPossibility of depth1Possibilities) {
    // Search one level deeper from the result of the first move
    const [innerPossibilities, prunedD1] = searchDepth1(
      outerPossibility.searchStateAfterMove,
      aiParams,
      aiMode,
      EVALUATION_BREADTH[2]
    );
    if (prunedD1.length > 0) {
      throw new Error("Pruned at depth 1");
    }

    // Generate 2-chains by combining the outer and inner moves
    const new2Chains = innerPossibilities.map(
      (innerPossibility: Possibility) => ({
        ...outerPossibility,
        innerPossibility: innerPossibility,
        totalValue: innerPossibility.evalScore + outerPossibility.partialValue,
      })
    );

    // Merge with the current best list
    if (new2Chains.length > 0) {
      const newList: Array<PossibilityChain> = utils.mergeSortedArrays(
        best2Chains,
        new2Chains,
        (x, y) =>
          y.totalValue - x.totalValue + 0.001 * (y.evalScore - x.evalScore) // Tiebreak by which one has the best value after the first placement
      );
      // Save the best N chains we've seen so far. Keep the rest in case they're needed later.
      best2Chains = newList.slice(0, SEARCH_BREADTH[3]);
      pruned = pruned.concat(newList.slice(SEARCH_BREADTH[3]));
    }
  }
  return [best2Chains, pruned];
}


/**
 * Iterates over all possible future sequences of length N, and plays each one out no-next-box.
 * @param chain - the chain of existing moves
 */
function getBestMovesForAllPossibleSequences(
  chain: PossibilityChain,
  hypotheticalSearchDepth: number,
  aiParams: AiParams,
  aiMode: AiMode
): Array<HypotheticalBestMove> {
  let bestMovesList: Array<HypotheticalBestMove> = []; // Map from piece ID to 1-chain

  // Get the search state and total partial value (the only things needed from the possibility chain)
  let searchState, concretePartialValue;
  if (chain.innerPossibility) {
    searchState = chain.innerPossibility.searchStateAfterMove;
    concretePartialValue =
      chain.partialValue + chain.innerPossibility.partialValue;
  } else {
    searchState = chain.searchStateAfterMove;
    concretePartialValue = chain.partialValue;
  }

  // Get the best placement for each hypothetical piece
  const hypotheticalSequences = getHypotheticalSequences(hypotheticalSearchDepth);
  for (const [i,sequence] of hypotheticalSequences.entries()) {
    // if (i % 500 == 0){
    //   console.log(`Checking hypothetical sequence ${i} of ${hypotheticalSequences.length}`);
    // }

    // If drought mode is on, don't anticipate getting long bars
    if (IS_DROUGHT_MODE && sequence.includes("I")) {
      continue;
    }

    let hypotheticalLine = []; // The string of NNB moves
    let totalValueOfLine = concretePartialValue;
    let loopSearchState = { ...searchState };
    for (let i = 0; i < sequence.length; i++){
      loopSearchState.currentPieceId = sequence[i];
      const [moveList, _] = searchDepth1(
        loopSearchState,
        aiParams,
        aiMode,
        EVALUATION_BREADTH[3]
      );
      let bestMove = moveList[0] || null;
      
      if (bestMove === null){
        // Topped out during the hypothetical line
        totalValueOfLine = aiParams.DEAD_COEF
        break;
      }

      if (i == sequence.length - 1){
        totalValueOfLine += bestMove.totalValue;
      } else {
        totalValueOfLine += bestMove.partialValue;
      }

      hypotheticalLine.push(bestMove.placement);
      loopSearchState = getSearchStateAfter(loopSearchState, bestMove);
    }

    bestMovesList.push({
      pieceSequence: sequence,
      resultingValue: totalValueOfLine,
      moveSequence: hypotheticalLine
    });
  }

  // Sort the hypotheticals best to worst
  bestMovesList.sort((a, b) => b.resultingValue - a.resultingValue);

  return bestMovesList;
}

function getExpectedValue(
  bestMovesList: Array<HypotheticalBestMove>,
  weightVector: Array<number> = null
) {
  // Get a weighted EV based on the weight vector
  if (
    weightVector !== null &&
    (weightVector.length !== bestMovesList.length || weightVector.length === 0)
  ) {
    throw new Error(
      "Weight vector length 0 or doesn't match hypotheticals: " +
        weightVector.length +
        " " +
        bestMovesList.length
    );
  }
  let total = 0;
  const len = bestMovesList.length;
  for (let i = 0; i < len; i++) {
    total += bestMovesList[i].resultingValue * (weightVector ? weightVector[i] : 1);
  }
  return total / len;
}

export function logExpectedValueResults(hypotheticalResults: Array<HypotheticalResult>, hypotheticalSearchDepth: number, aiParams: AiParams){
  console.log(
    "\n\n------------------------\nBest Results after Stochastic Analysis"
  );
  hypotheticalResults.slice(0, 5).forEach((result: HypotheticalResult, i) => {
    const chain = result.possibilityChain;
    console.log(
      `\n#${i+1}    Moves: ${chain.placement}    ${
        chain.innerPossibility ? chain.innerPossibility.placement : ""
      }`
    );
    console.log(
      `Expected Value: ${result.expectedValue.toFixed(
        2
      )}, Original Value: ${chain.totalValue.toFixed(2)}`
    );
    const numMoves = result.bestMoves.length;
    for (const [i, hypotheticalBestMove] of result.bestMoves.entries()) {
      if (i < 4 || i >= numMoves - 4){
        console.log(
          `If ${hypotheticalBestMove.pieceSequence}, do ${
            hypotheticalBestMove.moveSequence.join(" ")
          }. Value: ${(hypotheticalBestMove.resultingValue || aiParams.DEAD_COEF).toFixed(2)}`
        );
      } else if (i == 4){
        console.log("...")
      }
      // console.log(hypotheticalBestMove.evalExplanation);
    }
  });
}

export function getSearchStateAfter(
  prevSearchState: SearchState,
  possibility: Possibility
): SearchState {
  const levelAfter = utils.getLevelAfterLineClears(
    prevSearchState.level,
    prevSearchState.lines,
    possibility.numLinesCleared
  );
  return {
    board: possibility.boardAfter,
    currentPieceId: prevSearchState.nextPieceId,
    nextPieceId: null,
    level: levelAfter,
    lines: prevSearchState.lines + possibility.numLinesCleared,
    framesAlreadyElapsed: 0,
    reactionTime: prevSearchState.reactionTime,
    existingXOffset: 0,
    existingYOffset: 0,
    existingRotation: 0,
    canFirstFrameShift: false,
  };
}

export function getHypotheticalSequences(goalLength){
  let sequences = POSSIBLE_NEXT_PIECES;
  let length = 1;
  while (length < goalLength){
    const newSequences = [];
    for (const oldSeq of sequences){
      for (const newPiece of POSSIBLE_NEXT_PIECES){
        newSequences.push(oldSeq + newPiece);
      }
    }
    sequences = newSequences
    length++;
  }
  return sequences
}

export function addTapInfoToAiParams(
  initialAiParams: InitialAiParams,
  level: number,
  inputFrameTimeline: string
): AiParams {
  const newParams = JSON.parse(JSON.stringify(initialAiParams));
  // Save the input frame timeline
  newParams.INPUT_FRAME_TIMELINE = inputFrameTimeline;

  // Look up the 4/5 tap height for the current and maybe next level
  newParams.MAX_5_TAP_LOOKUP = {};
  newParams.MAX_5_TAP_LOOKUP[level] = boardHelper.calculateTapHeight(
    level,
    inputFrameTimeline,
    5
  );
  newParams.MAX_4_TAP_LOOKUP = {};
  newParams.MAX_4_TAP_LOOKUP[level] = boardHelper.calculateTapHeight(
    level,
    inputFrameTimeline,
    4
  );
  const nextLevel = level + 1;
  // Also look up the tap ranges for the next level, in case we evaluate possibilites after the transition
  newParams.MAX_5_TAP_LOOKUP[nextLevel] = boardHelper.calculateTapHeight(
    nextLevel,
    inputFrameTimeline,
    5
  );
  newParams.MAX_4_TAP_LOOKUP[nextLevel] = boardHelper.calculateTapHeight(
    nextLevel,
    inputFrameTimeline,
    4
  );

  // Add burn quota on 28 if not already present
  if (level == 28) {
    //...
  }

  return newParams;
}
