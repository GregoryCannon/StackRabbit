import * as child_process from "child_process";
import { DIG_LINE_CAP } from "../lite_game_simulator";

const IS_DIG_TESTING = false;

/* ------------------------------
    Worker thread configuration 
--------------------------------- */

let workers = [];
const NUM_THREADS = 7;
let pendingResults = NUM_THREADS;
let results = [];

function start() {
  // Initialize workers
  for (let i = 0; i < NUM_THREADS; i++) {
    workers[i] = child_process.fork(
      "built/src/server/research/killscreen_evo_worker.js"
    );
    workers[i].addListener("message", onMessage);
  }

  // Ping the workers to start
  console.time("worker phase");
  for (let i = 0; i < NUM_THREADS; i++) {
    workers[i].send({ type: IS_DIG_TESTING ? "dig" : "average", threadId: i });
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
      if (numHoles > 5){
        deathCount++;
      }
    }
    numGames += result.length;
  }
  console.log("\n----\n", numGames)
  console.log("Deaths:", deathCount);
  console.log("Avg lines:", totalLines / numGames);
  return [totalLines / numGames, deathCount / numGames];
}

function defaultFitnessFunction(threadResults: Array<Array<SimulatedGameResult>>){
  let totalScore = 0;
  let totalLines = 0;
  let totalEarlyTopouts = 0;
  let numGames = 0;
  for (const result of threadResults) {
    for (const [score, lines] of result) {
      totalScore += score;
      totalLines += lines;
      if (score <= 300000){
        totalEarlyTopouts++;
      }
    }
    numGames += result.length;
  }
  console.log("\n\nAVERAGE SCORE:", totalScore / numGames);
  console.log("AVERAGE LINES:", totalLines / numGames);
  console.log("SAMPLE SIZE:", numGames);
  console.log("Early topouts", totalEarlyTopouts)
  return [totalScore / numGames, totalEarlyTopouts/numGames];
}

function processResults() {
  const fitness = IS_DIG_TESTING ? digFitnessFunction(results) : defaultFitnessFunction(results);
  console.log("\nFITNESS:", fitness.map(x => x.toFixed(4)));
}

start();
