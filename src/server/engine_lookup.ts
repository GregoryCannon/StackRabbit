import { getSortedMoveList } from "./main";
import { predictSearchStateAtAdjustmentTime } from "./precompute";

export function engineLookup(
  searchState: SearchState,
  aiParams: AiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string
): EngineResponseJson {
  const baseResult = getMoveData(
    searchState,
    aiParams,
    paramMods,
    inputFrameTimeline
  );
  return formatEngineResult(
    baseResult,
    searchState.currentPieceId,
    searchState.nextPieceId
  );
}

function formatEngineResult(
  engineResult: EngineResult,
  curPiece: PieceId,
  nextPiece: PieceId
): EngineResponseJson {
  return engineResult.map((mainMove) => {
    const [possibility, adjustmentList] = mainMove;
    return {
      piece: curPiece,
      inputSequence: possibility.inputSequence,
      totalValue: possibility.totalValue,
      isSpecialMove: possibility.inputCost !== 0,
      evalScore: possibility.evalScore,
      evalExplanation: possibility.evalExplanation,
      hypotheticalLines: possibility.hypotheticalLines,
      adjustments: adjustmentList.map((adjPos: PossibilityChain) => ({
        piece: curPiece,
        inputSequence: adjPos.inputSequence,
        totalValue: adjPos.totalValue,
        evalScore: adjPos.innerPossibility.evalScore,
        evalExplanation: adjPos.innerPossibility.evalExplanation,
        hypotheticalLines: adjPos.innerPossibility.hypotheticalLines,
        isSpecialMove: adjPos.inputCost !== 0,
        followUp: adjPos.innerPossibility
          ? {
              piece: nextPiece,
              inputSequence: adjPos.innerPossibility.inputSequence,
              isSpecialMove: adjPos.innerPossibility.inputCost !== 0,
              totalValue: adjPos.innerPossibility.totalValue,
            }
          : {
              piece: nextPiece,
              inputSequence: "",
              isSpecialMove: false,
              totalValue: Number.MIN_SAFE_INTEGER,
            },
      })),
    };
  });
}

function getMoveData(
  searchState: SearchState,
  aiParams: AiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string
): EngineResult {
  const result = [];

  let initalMoves = getSortedMoveList(
    {
      ...searchState,
    },
    /* shouldLog= */ false,
    aiParams,
    paramMods,
    inputFrameTimeline,
    /* searchDepth= */ 1,
    /* hypotheticalSearchDepth= */ 1
  )[0];
  if (initalMoves.length === 0) {
    return [];
  }

  initalMoves = initalMoves.slice(0, 5);

  // If this was a NNB query, we're done
  if (searchState.nextPieceId == null) {
    return initalMoves.map((x) => [x, []]);
  }

  // Otherwise search for adjustments from each of the initial placements
  for (const initialMove of initalMoves) {
    const newSearchState = predictSearchStateAtAdjustmentTime(
      searchState,
      initialMove.inputSequence,
      inputFrameTimeline
    );
    newSearchState.nextPieceId = searchState.nextPieceId;
    const adjustments = getSortedMoveList(
      newSearchState,
      /* shouldLog= */ false,
      aiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 2,
      /* hypotheticalSearchDepth= */ 1
    )[0];
    if (adjustments.length === 0) {
      result.push([initialMove, []]);
      continue;
    }

    const sameAsInitial = adjustments.filter(
      (x) => x.lockPositionEncoded === initialMove.lockPositionEncoded
    );
    if (sameAsInitial.length > 0) {
      const valueOfDefault = sameAsInitial[0].expectedValue;
      result.push([
        initialMove,
        adjustments.filter((x) => x.totalValue >= valueOfDefault),
      ]);
    } else {
      // The default placement literally got pruned out, lol
      result.push([initialMove, adjustments.slice(0, 3)]);
    }
  }
  return result;
}
