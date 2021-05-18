const process = require("process");

console.time("loading");
const mainApp = require("../../../built/src/server/main.js");
console.timeEnd("loading");
process.send({ type: "ready" });

function heavyComputation(data) {
  console.time(data.toString());
  let sum = 0;
  for (let i = 0; i < 10000000; i++) {
    sum = Math.pow(sum, 1.02) % 100000;
  }
  console.timeEnd(data.toString());
  return data + 1000;
}

// function performComputation(data) {
//   const {
//     newSearchState,
//     shouldLog,
//     initialAiParams,
//     paramMods,
//     inputFrameTimeline,
//   } = data;
//   console.time(data.newSearchState.nextPieceId);
//   const res = mainApp.getBestMove(
//     newSearchState,
//     shouldLog,
//     initialAiParams,
//     paramMods,
//     inputFrameTimeline,
//     /* searchDepth= */ 2,
//     /* hypotheticalSearchDepth= */ 1
//   );
//   console.timeEnd(data.newSearchState.nextPieceId);
//   return res;
// }

process.on("message", (message) => {
  process.send({
    type: "result",
    piece: "null",
    result: heavyComputation(1),
  });
});
