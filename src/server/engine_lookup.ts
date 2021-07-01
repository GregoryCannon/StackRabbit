import { getBestMove, getSortedMoveList } from "./main";
import { predictSearchStateAtAdjustmentTime } from "./precompute";

function getMoveData(searchState: SearchState,
  aiParams: AiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string,
){
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
    return null;
  }

  initalMoves = initalMoves.slice(0, 5);
  for (const initalMove of initalMoves){
    const newSearchState = predictSearchStateAtAdjustmentTime(
      searchState,
      initalMove.inputSequence,
      inputFrameTimeline
    );
    newSearchState.nextPieceId = searchState.nextPieceId;
    const adjustment = getBestMove(
      newSearchState,
      /* shouldLog= */ false,
      aiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 2,
      /* hypotheticalSearchDepth= */ 1
    );
  }


  
  // console.log("INITIAL MOVE:", initalMove.placement);
  // console.log("ADJUSTMENT:", adjustment.placement);
  return adjustment || initalMove;
}