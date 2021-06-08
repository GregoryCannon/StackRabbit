import * as child_process from "child_process";

/* ------------------------------
    Worker thread configuration 
--------------------------------- */

let workers = [];
const NUM_THREADS = 8;
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
    workers[i].send({ type: "average", threadId: i });
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

function processResults() {
  let totalScore = 0;
  let totalLines = 0;
  let numGames = 0;
  for (const result of results) {
    for (const [score, lines] of result) {
      totalScore += score;
      totalLines += lines;
    }
    numGames += result.length;
  }
  console.log("\n\nAVERAGE SCORE:", totalScore / numGames);
  console.log("AVERAGE LINES:", totalLines / numGames);
  console.log("SAMPLE SIZE:", numGames);
}

start();
