import * as fs from "fs";
import { SIM_MAX_4_TAP_HEIGHT } from "./simulation_testing";

export const DEATH_RANK = "xxx";

// /* ------------------------------
//     Worker thread configuration
// --------------------------------- */

// let workers = [];
// const NUM_THREADS = 8;
// let pendingResults = NUM_THREADS;
// let results = [];

// function start() {
//   // Initialize workers
//   for (let i = 0; i < NUM_THREADS; i++) {
//     workers[i] = child_process.fork(
//       "built/src/server/research/killscreen_evo_worker.js"
//     );
//     workers[i].addListener("message", onMessage);
//   }

//   // Ping the workers to start
//   console.time("worker phase");
//   for (let i = 0; i < NUM_THREADS; i++) {
//     workers[i].send({ type: "successors", threadId: i });
//   }
// }

// function onMessage(message) {
//   if ((message.type = "result")) {
//     // console.log("Received response message:", message.result);
//     results.push(message.result);
//     pendingResults--;
//     if (pendingResults == 0) {
//       console.timeEnd("worker phase");
//       workers.forEach((x) => x.kill());
//       processResults();
//     }
//   }
// }

/* ------------------------------------------
    Result compilation and Value iteration
  ------------------------------------------- */

export function processKillscreenResults(resultsList) {
  console.log("Processing results");

  // Merge the successor maps
  const successors = {};
  for (const succMap of resultsList) {
    for (const [fromSurface, freqMap] of Object.entries(succMap)) {
      for (const [toSurface, transitionCount] of Object.entries(freqMap)) {
        if (!successors.hasOwnProperty(fromSurface)) {
          successors[fromSurface] = {};
        }
        if (!successors[fromSurface].hasOwnProperty(toSurface)) {
          successors[fromSurface][toSurface] = 0;
        }
        successors[fromSurface][toSurface] += transitionCount;
      }
    }
  }
  console.log(successors);

  fs.writeFileSync("docs/successors.json", JSON.stringify(successors));

  // Value iterate!
  const ranks: Object = valueIterate(successors);

  fs.writeFileSync("docs/15_hz_ranks_v0.json", JSON.stringify(ranks));

  console.log(ranks);
}

/** Rewards a fully built-out left */
function getReward(surfaceLeft) {
  if (surfaceLeft[0] >= surfaceLeft[1] && surfaceLeft[0] >= surfaceLeft[2]) {
    const heightDiff = surfaceLeft.split("|")[1];
    if (heightDiff == 3) {
      return 50;
    } else if (heightDiff == 4) {
      return 75;
    } else if (heightDiff >= 5) {
      return 100;
    }
  }
  return 0;
}

const MAX_VALUE_ITERATIONS = 10000;
const NUM_SAMPLE_GAMES = 10;

function valueIterate(successors: Object): Object {
  let values = {};

  // Initialize the starting ranks
  for (const [surface, nextSurfaceFrequencies] of Object.entries(successors)) {
    values[surface] = getReward(surface);
    values[DEATH_RANK] = 0;
  }

  // Converge ranks
  let totalDelta = Number.MAX_SAFE_INTEGER;
  for (
    let iteration = 0;
    iteration < MAX_VALUE_ITERATIONS && totalDelta > 1;
    iteration++
  ) {
    totalDelta = 0;

    const newValues = {};
    newValues[DEATH_RANK] = 0; // The death rank has to be initialized since it's never the 'from' state of any transition

    for (const [surface, nextSurfaceFrequencies] of Object.entries(
      successors
    )) {
      const inherentValue = getReward(surface);
      const heightDiff = parseInt(surface.split("|")[1]);

      if (heightDiff >= SIM_MAX_4_TAP_HEIGHT) {
        // If it's a goal state, use the inherent reward
        newValues[surface] = inherentValue;
      } else {
        // Otherwise get the value from its successors
        let total = 0;
        let numSuccessors = 0;
        for (const [nextSurface, count] of Object.entries(
          nextSurfaceFrequencies
        )) {
          total += (count as number) * values[nextSurface];
          numSuccessors += count as number;
        }
        newValues[surface] = total / numSuccessors;
      }

      totalDelta += Math.abs(newValues[surface] - values[surface]);
    }

    values = newValues;
    console.log(iteration, totalDelta.toFixed(2));
  }

  return values;
}

// start();
