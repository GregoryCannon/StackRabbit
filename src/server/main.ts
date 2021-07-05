const evaluator = require("./evaluator");
const aiModeManager = require("./ai_mode_manager");
const boardHelper = require("./board_helper");
const { SEARCH_BREADTH, modifyParamsForAiMode } = require("./params");
import { getPossibleMoves } from "./move_search";
import { EVALUATION_BREADTH, IS_DROUGHT_MODE, LINE_CAP } from "./params";
import { getPieceProbability, getSequenceProbability } from "./piece_rng";
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
  const [aiParams, aiMode] = preProcessAiParams(
    initialAiParams,
    searchState,
    inputFrameTimeline,
    paramMods
  );

  let [bestConcrete, prunedConcrete] = searchConcretely(
    searchState,
    shouldLog,
    aiParams,
    aiMode,
    searchDepth
  );
  const lastSeenPiece =
    searchDepth === 1 ? searchState.currentPieceId : searchState.nextPieceId;

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
      lastSeenPiece,
      aiParams,
      aiMode,
      hypotheticalSearchDepth,
      shouldLog
    );
    return [bestHypothetical, prunedConcrete];
  }
}

export function preProcessAiParams(
  initialAiParams: InitialAiParams,
  searchState: SearchState,
  inputFrameTimeline: string,
  paramMods: ParamMods
): [AiParams, AiMode] {
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
    inputFrameTimeline,
    aiParams
  );
  // Update the params based on level, AI mode, etc.
  if (searchState.level >= 19 && aiParams.BURN_COEF_POST !== undefined) {
    aiParams.BURN_COEF = aiParams.BURN_COEF_POST;
  }
  aiParams = modifyParamsForAiMode(aiParams, aiMode, paramMods);
  return [aiParams, aiMode];
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
  lastSeenPiece: PieceId,
  aiParams: AiParams,
  aiMode: AiMode,
  hypotheticalSearchDepth: number,
  shouldLog: boolean
): Array<PossibilityChain> {
  if (hypotheticalSearchDepth < 1 || hypotheticalSearchDepth > 4) {
    throw new Error("Unsupported hypothetical search depth");
  }

  // Evaluate the weighted EV of each possibility chain
  for (const [i, chain] of possibilityChains.entries()) {
    if (hypotheticalSearchDepth > 2) {
      console.log(`Checking 2-chain ${i} of ${possibilityChains.length}`);
    }

    const hypotheticalLines = getBestMovesForAllPossibleSequences(
      chain,
      lastSeenPiece,
      hypotheticalSearchDepth,
      aiParams,
      aiMode
    );

    const expectedValue = getExpectedValue(hypotheticalLines);

    // Attach the EV to the original possibility
    chain.expectedValue = expectedValue;
    chain.totalValue = expectedValue;
    if (chain.innerPossibility) {
      chain.innerPossibility.expectedValue = expectedValue;
      chain.innerPossibility.totalValue = expectedValue;
      chain.innerPossibility.hypotheticalLines = hypotheticalLines;
    } else {
      chain.hypotheticalLines = hypotheticalLines;
    }
  }

  // Sort by EV
  possibilityChains.sort((a, b) => b.expectedValue - a.expectedValue);

  // Maybe log info about the EV results
  if (shouldLog) {
    logExpectedValueResults(
      possibilityChains,
      hypotheticalSearchDepth,
      aiParams
    );
  }

  return possibilityChains;
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
      aiParams
    );
    possibility.evalScore = value as number;
    possibility.evalExplanation = explanation as string;

    // Convert to a 1-chain
    possibility.totalValue = value as number;
    possibility.partialValue = evaluator.getPartialValue(
      possibility,
      searchState.lines,
      aiParams,
      aiMode
    );
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
      throw new Error("Pruned at depth 1 - this should never happen");
    }

    let new2Chains;
    if (innerPossibilities.length > 0) {
      // Generate 2-chains by combining the outer and inner moves
      new2Chains = innerPossibilities.map((innerPossibility: Possibility) => ({
        ...outerPossibility,
        innerPossibility: innerPossibility,
        totalValue: innerPossibility.evalScore + outerPossibility.partialValue,
      }));
    } else {
      new2Chains = [
        {
          ...outerPossibility,
          innerPossibility: null,
          totalValue: aiParams.DEAD_COEF,
        },
      ];
    }

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
  lastSeenPiece: PieceId,
  hypotheticalSearchDepth: number,
  aiParams: AiParams,
  aiMode: AiMode
): Array<HypotheticalLine> {
  let hypotheticalLines: Array<HypotheticalLine> = [];

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
  const allSequences = getAllPieceSequences(hypotheticalSearchDepth);
  for (const sequence of allSequences) {
    let hypotheticalLine = []; // The string of NNB moves
    let hypotheticalLineAsInputs = [];
    let totalValueOfLine = concretePartialValue;
    let loopSearchState = { ...searchState };
    for (let i = 0; i < sequence.length; i++) {
      loopSearchState.currentPieceId = sequence[i];
      const [moveList, _] = searchDepth1(
        loopSearchState,
        aiParams,
        aiMode,
        EVALUATION_BREADTH[3]
      );
      let bestMove = moveList[0] || null;

      if (bestMove === null) {
        // Topped out during the hypothetical line
        totalValueOfLine = aiParams.DEAD_COEF;
        break;
      }

      if (i == sequence.length - 1) {
        totalValueOfLine += bestMove.totalValue;
      } else {
        totalValueOfLine += bestMove.partialValue;
      }

      hypotheticalLine.push(bestMove.placement);
      hypotheticalLineAsInputs.push(bestMove.inputSequence);
      loopSearchState = getSearchStateAfter(loopSearchState, bestMove);
    }

    hypotheticalLines.push({
      pieceSequence: sequence,
      probability: getSequenceProbability(
        sequence,
        lastSeenPiece,
        IS_DROUGHT_MODE
      ),
      resultingValue: totalValueOfLine,
      moveSequence: hypotheticalLine,
      moveSequenceAsInputs: hypotheticalLineAsInputs,
    });
  }

  // Sort the hypotheticals best to worst
  hypotheticalLines.sort((a, b) => b.resultingValue - a.resultingValue);

  return hypotheticalLines;
}

function getExpectedValue(hypotheticalLines: Array<HypotheticalLine>): number {
  let total = 0;
  const len = hypotheticalLines.length;
  for (let i = 0; i < len; i++) {
    total +=
      hypotheticalLines[i].resultingValue * hypotheticalLines[i].probability;
  }
  return total;
}

export function logExpectedValueResults(
  hypotheticalResults: Array<PossibilityChain>,
  hypotheticalSearchDepth: number,
  aiParams: AiParams
) {
  console.log(
    "\n\n------------------------\nBest Results after Stochastic Analysis"
  );
  hypotheticalResults.slice(0, 5).forEach((result: PossibilityChain, i) => {
    console.log(
      `\n#${i + 1}    Moves: ${result.placement}    ${
        result.innerPossibility ? result.innerPossibility.placement : ""
      }`
    );
    console.log(
      `Expected Value: ${result.expectedValue.toFixed(
        2
      )}, Original Value: ${result.totalValue.toFixed(2)}`
    );
    const numMoves = result.hypotheticalLines.length;
    for (const [
      i,
      hypotheticalBestMove,
    ] of result.hypotheticalLines.entries()) {
      if (i < 4 || i >= numMoves - 4) {
        console.log(
          `If ${
            hypotheticalBestMove.pieceSequence
          }, do ${hypotheticalBestMove.moveSequence.join(" ")}. Value: ${(
            hypotheticalBestMove.resultingValue || aiParams.DEAD_COEF
          ).toFixed(2)}`
        );
      } else if (i == 4) {
        console.log("...");
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

export function getAllPieceSequences(goalLength) {
  let sequences = POSSIBLE_NEXT_PIECES;
  let length = 1;
  while (length < goalLength) {
    const newSequences = [];
    for (const oldSeq of sequences) {
      for (const newPiece of POSSIBLE_NEXT_PIECES) {
        newSequences.push(oldSeq + newPiece);
      }
    }
    sequences = newSequences;
    length++;
  }
  return sequences;
}

export function addTapInfoToAiParams(
  initialAiParams: InitialAiParams,
  level: number,
  inputFrameTimeline: string
): AiParams {
  const newParams = JSON.parse(JSON.stringify(initialAiParams));
  // Save the input frame timeline
  newParams.INPUT_FRAME_TIMELINE = inputFrameTimeline;
  newParams.MAX_5_TAP_LOOKUP = {};
  newParams.MAX_4_TAP_LOOKUP = {};

  const addForLevel = (lvl) => {
    newParams.MAX_5_TAP_LOOKUP[lvl] = boardHelper.calculateTapHeight(
      lvl,
      inputFrameTimeline,
      5
    );
    newParams.MAX_4_TAP_LOOKUP[lvl] = boardHelper.calculateTapHeight(
      lvl,
      inputFrameTimeline,
      4
    );
  };

  addForLevel(level);
  addForLevel(level + 1);
  addForLevel(level + 2);

  return newParams;
}
