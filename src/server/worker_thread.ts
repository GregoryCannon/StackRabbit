console.time("loading");
import * as process from "process";
import { getBestMove, getSortedMoveList } from "./main";

console.timeEnd("loading");
process.send({ type: "ready" }); // Let the main process know that it's loaded the ranks file

/**
 * Compute adjustment for the given piece
 */
function performComputation(args): PossibilityChain {
  console.time(args.piece);
  const result: PossibilityChain = getBestMove(
    args.newSearchState,
    /* shouldLog= */ false,
    args.initialAiParams,
    args.paramMods,
    args.inputFrameTimeline,
    /* searchDepth= */ 2,
    /* hypotheticalSearchDepth= */ 1
  );
  console.timeEnd(args.piece);
  return result;
}

/**
 * Compute adjustment for the given piece
 */
function performComputationFinesse(args): Object {
  console.time(args.piece);

  // Query for the possible moves for the previous piece
  const originalPieceSearchState = args.newSearchState;
  const moveList: Array<PossibilityChain> = getSortedMoveList(
    originalPieceSearchState,
    /* shouldLog= */ false,
    args.initialAiParams,
    args.paramMods,
    args.inputFrameTimeline,
    /* searchDepth= */ 1,
    /* hypotheticalSearchDepth= */ 0
  );

  const lockPositionValueLookup = {};
  for (const originalPiecePossibility of moveList) {
    const bestMoveAfter = getBestMove(
      originalPiecePossibility.searchStateAfterMove,
      /* shouldLog= */ false,
      args.initialAiParams,
      args.paramMods,
      args.inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 0
    );
    lockPositionValueLookup[originalPiecePossibility.lockPositionEncoded] =
      bestMoveAfter.totalValue || Number.MIN_SAFE_INTEGER;
    // console.log(`${args.piece}: Best move after ${originalPiecePossibility.placement} is ${bestMoveAfter.placement}, with value ${bestMoveAfter.totalValue}\n`);
  }

  console.timeEnd(args.piece);
  // console.log("LOOKUP", lockPositionValueLookup);
  return lockPositionValueLookup;
}

process.on("message", (args: WorkerDataArgs) => {
  const result =
    args.computationType == "finesse"
      ? performComputationFinesse(args)
      : performComputation(args);
  process.send({
    type: args.computationType == "finesse" ? "result-finesse" : "result",
    piece: args.piece,
    result: result,
  });
});
