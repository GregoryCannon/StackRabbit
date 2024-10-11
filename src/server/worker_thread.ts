console.time("loading");
import * as process from "process";
import {
  CPP_LIVEGAME_PLAYOUT_COUNT,
  CPP_LIVEGAME_PLAYOUT_LENGTH,
} from "./params";
const cModule = require("../../../build/Release/cRabbit");

console.timeEnd("loading");
process.send({ type: "ready" }); // Let the main process know that it's loaded the ranks file

/**
 * Compute adjustment for the given piece
 */
function performComputationFinesseCpp(args: WorkerDataArgs): Object {
  console.time(args.piece);

  const boardStr = args.newSearchState.board.map((x) => x.join("")).join("");
  const pieceLookup = ["I", "O", "L", "J", "T", "S", "Z"];
  const curPieceIndex = pieceLookup.indexOf(args.newSearchState.currentPieceId);
  const nextPieceIndex = pieceLookup.indexOf(args.newSearchState.nextPieceId);
  const inputFrameTimeline = args.inputFrameTimeline;
  const encodedInputString = `${boardStr}|${args.newSearchState.level}|${args.newSearchState.lines}|${curPieceIndex}|${nextPieceIndex}|${inputFrameTimeline}|${CPP_LIVEGAME_PLAYOUT_COUNT}|${CPP_LIVEGAME_PLAYOUT_LENGTH}`;
  // console.log(args.newSearchState.nextPieceId, encodedInputString);
  const lockPositionValueLookup = JSON.parse(
    cModule.getLockValueLookup(encodedInputString)
  );
  console.timeEnd(args.piece);
  return lockPositionValueLookup;
}

process.on("message", (args: WorkerDataArgs) => {
  const result = performComputationFinesseCpp(args);
  process.send({
    type: "result",
    piece: args.piece,
    result: result,
  });
});
