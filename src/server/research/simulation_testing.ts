import * as child_process from "child_process";
import { calculateTapHeight } from "../board_helper";
import { parseBoard } from "../utils";
import { processKillscreenResults } from "./killscreen_training";

// Specifies to the worker thread what type of simulation needs to be done
const TestType = Object.freeze({
  DIG: "dig",
  STANDARD: "average",
  KILLSCREEN: "successors",
  OPENER: "opener",
});

/* ------------------------------
    Configuration
--------------------------------- */

export const SIM_INPUT_TIMELINE = "X....X...";
export const SIM_MAX_4_TAP_HEIGHT = calculateTapHeight(
  29,
  SIM_INPUT_TIMELINE,
  4
);
export const OPENER_TEST_BOARD = parseBoard(
  "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000011100000011110"
);

export const TRAINING_TIME_MINS = 2;
const NUM_THREADS = 8;

const curTestType = TestType.OPENER;

/* ------------------------------
    Worker thread setup
--------------------------------- */

let workers = [];
let pendingResults = NUM_THREADS;
let results = [];

function start() {
  // Initialize workers
  for (let i = 0; i < NUM_THREADS; i++) {
    workers[i] = child_process.fork(
      "built/src/server/research/simulation_worker.js"
    );
    workers[i].addListener("message", onMessage);
  }

  // Ping the workers to start
  console.time("worker phase");
  for (let i = 0; i < NUM_THREADS; i++) {
    workers[i].send({ type: curTestType, threadId: i });
  }
}

function onMessage(message) {
  if ((message.type = "result")) {
    // console.log("Received response message:", message.result);
    results.push(message.result);
    pendingResults--;
    if (pendingResults == 0) {
      console.timeEnd("worker phase");
      workers.forEach((x) => x.kill());
      processResults();
    }
  }
}

/**
 * Fitness function: the average score
 * @param {Array of [score, lines, level, numHoles] subarrays} - simulationResult
 */
function digFitnessFunction(threadResults: Array<Array<SimulatedGameResult>>) {
  let deathCount = 0;
  let totalLines = 0;
  let numGames = 0;
  for (const result of threadResults) {
    for (const [score, lines, _, numHoles] of result) {
      // totalScore += scoreForOneGame(score, lines, numHoles);
      totalLines += lines;
      console.log(lines);
      if (numHoles > 5) {
        deathCount++;
      }
    }
    numGames += result.length;
  }
  console.log("\n----\n", numGames);
  console.log("Deaths:", deathCount);
  console.log("Avg lines:", totalLines / numGames);
  return [totalLines / numGames, deathCount / numGames].map((x) =>
    x.toFixed(4)
  );
}

function defaultFitnessFunction(
  threadResults: Array<Array<SimulatedGameResult>>
) {
  let totalScore = 0;
  let totalLines = 0;
  let totalEarlyTopouts = 0;
  let numGames = 0;
  for (const result of threadResults) {
    for (const [score, lines] of result) {
      totalScore += score;
      totalLines += lines;
      if (score <= 500000) {
        totalEarlyTopouts++;
      }
    }
    numGames += result.length;
  }
  console.log("\n\nAVERAGE SCORE:", totalScore / numGames);
  console.log("AVERAGE LINES:", totalLines / numGames);
  console.log("SAMPLE SIZE:", numGames);
  console.log("Early topouts", totalEarlyTopouts);
  return [totalScore / numGames, totalEarlyTopouts / numGames].map((x) =>
    x.toFixed(4)
  );
}

function openerFitnessFunction(
  threadResults: Array<Array<SimulatedGameResult>>
) {
  let gamesPerfect = 0;
  let totalEarlyTopouts = 0;
  let numGames = 0;
  for (const result of threadResults) {
    for (const [score, lines, level, numHoles] of result) {
      if (lines === 8 && numHoles === 0){
        gamesPerfect++;
      }
      if (lines < 5) {
        totalEarlyTopouts++;
      }
    }
    numGames += result.length;
  }
  console.log("\n\n% TWO PERFECT TETRISES:", gamesPerfect / numGames * 100);
  console.log("NUM PERFECT, SAMPLE SIZE:")
  console.log(gamesPerfect);
  console.log(numGames);
  console.log("Early topouts", totalEarlyTopouts);
  return [gamesPerfect / numGames, totalEarlyTopouts / numGames].map((x) =>
    x.toFixed(4)
  );
}

function processResults() {
  switch (curTestType) {
    case TestType.OPENER:
      console.log("\n\nFitness:", openerFitnessFunction(results));
      break;

    case TestType.STANDARD:
      console.log("\n\nFitness:", defaultFitnessFunction(results));
      break;

    case TestType.DIG:
      console.log("\n\nFitness:", digFitnessFunction(results));
      break;

    case TestType.KILLSCREEN:
      processKillscreenResults(results);
  }
}

if (require.main === module) {
  start();
}
