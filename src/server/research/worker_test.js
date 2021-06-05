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
