import * as process from "process";

console.time("loading");
const mainApp = require("../../../built/src/server/main.js");
console.timeEnd("loading");
process.send({ type: "ready" }); // Let the main process know that it's loaded the ranks file

/**
 * Compute adjustment for the given piece
 */
function performComputation(args): PossibilityChain {
  console.time(args.piece);
  const result: PossibilityChain = mainApp.getBestMove(
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

process.on("message", (args: WorkerDataArgs) => {
  process.send({
    type: "result",
    piece: args.piece,
    result: performComputation(args),
  });
});
