const child_process = require("child_process");

let workers = [];
const NUM_THREADS = 6;
let pendingResults = NUM_THREADS;
let numThreadsLoading = NUM_THREADS;
let numRepeats = 1;

function start() {
  console.time("async");
  for (let i = 0; i < NUM_THREADS; i++) {
    workers[i].send({ data: 1 });
  }
}

function onMessage(message) {
  if (message.type === "ready") {
    numThreadsLoading--;
    if (numThreadsLoading === 0) {
      console.log("All threads ready");
      start();
    }
    return;
  }

  if ((message.type = "result")) {
    // console.log("Received response message:", message.result);
    pendingResults--;
    if (pendingResults == 0) {
      console.timeEnd("async");
      // workers.forEach(x => x.kill());

      // Go again
      numRepeats--;
      if (numRepeats > 0) {
        pendingResults = NUM_THREADS;
        start();
      }
    }
  }
}

for (let i = 0; i < NUM_THREADS; i++) {
  workers[i] = child_process.fork(
    "src/server/research/worker_thread_test_only.js"
  );
  workers[i].addListener("message", onMessage);
}

// const DEFAULT_PARAM_MODS = {
//   DIG: {
//     BURN_COEF: -1,
//     COL_10_COEF: -1,
//     HOLE_WEIGHT_COEF: -4,
//     HOLE_COEF: -100,
//     BUILT_OUT_LEFT_COEF: 0.6664592319119086,
//   },
//   NEAR_KILLSCREEN: {
//     BURN_COEF: -15,
//     TETRIS_READY_COEF: 10,
//     TETRIS_COEF: 500, // has to be more than INACCESSIBLE_RIGHT, so that it'll take a tetris on 229 lines
//   },
//   KILLSCREEN: {
//     COL_10_COEF: 0,
//     BUILT_OUT_LEFT_COEF: 0,
//     BUILT_OUT_RIGHT_COEF: 1,
//     AVG_HEIGHT_COEF: -3,
//     HOLE_COEF: -40, // changed
//     BURN_COEF: 0,
//     UNABLE_TO_BURN_COEF: 0,
//     HIGH_COL_9_COEF: 0,
//     HIGH_COL_9_EXP: 0,
//     INACCESSIBLE_LEFT_COEF: -100,
//     INACCESSIBLE_RIGHT_COEF: -50,
//     SPIRE_HEIGHT_COEF: -1.2,
//     LEFT_SURFACE_COEF: 1,
//     TETRIS_COEF: 70,
//     SURFACE_COEF: 0.5,
//   },
//   KILLSCREEN_RIGHT_WELL: {
//     SCARE_HEIGHT_OFFSET: -3,
//     BURN_COEF: -1,
//   },
// };

// const DEFAULT_PARAMS = {
//   AVG_HEIGHT_EXPONENT: 1.5000000000004,
//   AVG_HEIGHT_COEF: -4.50624,
//   SCARE_HEIGHT_OFFSET: -3,
//   BURN_COEF: -5,
//   COL_10_COEF: -3,
//   COL_10_HEIGHT_MULTIPLIER_EXP: 3,
//   MAX_DIRTY_TETRIS_HEIGHT: 0.15, // (As a multiple of the scare height)
//   EXTREME_GAP_COEF: -3,
//   BUILT_OUT_LEFT_COEF: 1.5,
//   BUILT_OUT_RIGHT_COEF: 0,
//   HOLE_COEF: -30,
//   HOLE_WEIGHT_COEF: 0,
//   SPIRE_HEIGHT_EXPONENT: 1.215999999999999,
//   SPIRE_HEIGHT_COEF: -1.1556000000000002,
//   UNABLE_TO_BURN_COEF: -0.3,
//   UNABLE_TO_BURN_DIFF_EXP: 1.5,
//   HIGH_COL_9_COEF: -3,
//   HIGH_COL_9_EXP: 2,
//   SURFACE_COEF: 1,
//   LEFT_SURFACE_COEF: 0,
//   TETRIS_COEF: 40,
//   TETRIS_READY_COEF: 0,
//   INACCESSIBLE_LEFT_COEF: -30,
//   INACCESSIBLE_RIGHT_COEF: -300,
// };

// const boardStr =
//     "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
//   const board = boardStr
//     .match(/.{1,10}/g) // Select groups of 10 characters
//     .map((rowSerialized) => rowSerialized.split("").map((x) => parseInt(x)));
//   const searchState =
//       {
//         board,
//         currentPieceId: "J",
//         nextPieceId: "I",
//         level: 18,
//         lines: 0,
//         framesAlreadyElapsed: 0,
//         existingXOffset: 0,
//         existingYOffset: 0,
//         existingRotation: 0,
//         canFirstFrameShift: false,
//       };
//   const inputFrameTimeline = "X....X...X...X";

//   const argsData = {
//     newSearchState: searchState,
//     shouldLog: false,
//     initialAiParams: DEFAULT_PARAMS,
//     paramMods: DEFAULT_PARAM_MODS,
//     inputFrameTimeline
//   };
