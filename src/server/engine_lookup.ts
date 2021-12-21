import { getSortedMoveList } from "./main";
import { predictSearchStateAtAdjustmentTime } from "./precompute";

export function engineLookup(
  searchState: SearchState,
  aiParams: AiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string
): EngineMoveListWithAdjustments {
  const baseResult = getMoveData(
    searchState,
    aiParams,
    paramMods,
    inputFrameTimeline
  );
  return formatEngineResultStyleA(
    baseResult,
    searchState.currentPieceId,
    searchState.nextPieceId
  );
}

export function engineLookupTopMovesWithNextBox(
  searchState: SearchState,
  aiParams: AiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string
) {
  const bestNbMoves = getSortedMoveList(
    {
      ...searchState,
    },
    /* shouldLog= */ false,
    aiParams,
    paramMods,
    inputFrameTimeline,
    /* searchDepth= */ 2,
    /* hypotheticalSearchDepth= */ 1
  )[0];

  return bestNbMoves.map((move) => [
    getMinimallyFormattedMove(move),
    getMinimallyFormattedMove(move.innerPossibility),
  ]);
}

export function engineLookupTopMovesListNoNextBox(
  searchState: SearchState,
  aiParams: AiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string
) {
  const bestNnbMoves = getSortedMoveList(
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
  return bestNnbMoves.map(getMinimallyFormattedMove);
}

/* ---------- Move formatting ----------- */

function getMinimallyFormattedMove(
  move: PossibilityChain
): MinimalFormattedMove {
  return {
    placement: move.placement,
    isSpecialMove: move.inputCost !== 0,
    totalValue: move.totalValue.toFixed(2),
    hypotheticalLines: move.hypotheticalLines,
  };
}

/** Formats the output of an engine calculation, in accordance with "Style A" aka. the way engine moves are shown in TetrisTrainer. */
function formatEngineResultStyleA(
  engineResult: EngineResult,
  curPiece: PieceId,
  nextPiece: PieceId
): EngineMoveListWithAdjustments {
  return engineResult.map((mainMove) => {
    const [possibility, adjustmentList] = mainMove;
    if (adjustmentList.length > 0) {
      console.log(
        "LINES",
        possibility.hypotheticalLines.length,
        adjustmentList[0].hypotheticalLines
      );
    }
    return {
      piece: curPiece,
      placement: possibility.placement,
      inputSequence: possibility.inputSequence,
      totalValue: possibility.totalValue,
      isSpecialMove: possibility.inputCost !== 0,
      evalScore: possibility.evalScore,
      evalExplanation: possibility.evalExplanation,
      hypotheticalLines: possibility.hypotheticalLines,
      adjustments: adjustmentList.map((adjPos: PossibilityChain) => ({
        piece: curPiece,
        placement: adjPos.placement,
        inputSequence: adjPos.inputSequence,
        totalValue: adjPos.totalValue,
        evalScore: adjPos.innerPossibility.evalScore,
        evalExplanation: adjPos.innerPossibility.evalExplanation,
        hypotheticalLines: adjPos.innerPossibility.hypotheticalLines,
        isSpecialMove: adjPos.inputCost !== 0,
        followUp: adjPos.innerPossibility
          ? {
              piece: nextPiece,
              placement: adjPos.innerPossibility.placement,
              inputSequence: adjPos.innerPossibility.inputSequence,
              isSpecialMove: adjPos.innerPossibility.inputCost !== 0,
              totalValue: adjPos.innerPossibility.totalValue,
            }
          : {
              piece: nextPiece,
              placement: null,
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
