const http = require("http");
const hostname = "127.0.0.1";
const port = 3000;

const mainApp = require("./main");
const params = require("./params");
const evolution = require("./evolution");

/**
 * Synchronously choose the best placement for a scenario, with the next piece known.
 */
function handleRequestSyncWithNextBox(requestArgs) {
  console.log("Starting handler: Sync with args");

  // Parse and validate inputs
  let [boardStr, currentPieceStr, nextPieceStr, level, lines] = requestArgs;
  currentPieceStr = currentPieceStr.toUpperCase();
  if (!["I", "O", "L", "J", "T", "S", "Z"].includes(currentPieceStr)) {
    return "bad next piece:" + currentPieceStr;
  }

  // Decode the board from the URL
  const startingBoard = boardStr
    .match(/.{1,10}/g)  // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split(""));

  // Get the best move
  const bestMove = mainApp.getMove(
    startingBoard,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
    /* shouldLog= */ false,
    params.getParams()
  );

  if (!bestMove) {
    return "No legal moves";
  }
  return bestMove[0] + "," + bestMove[1];
}

/**
 * Create HTTP server for the frontend to request
 */
const server = http.createServer((req, res) => {
  res.statusCode = 200;
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Content-Type", "text/plain");

  console.log("\n-------------------------\n" + req.url);

  let response;
  const [_, requestType, ...requestArgs] = req.url.split("/");
  const startTimeMs = Date.now(); // Save the start time for performance tracking

  // Route the request to a handler function in this class
  if (requestType === "ping") {
    response = "pong";
  } else if (requestType === "async") {
    // Incomplete
  } else if (requestType === "sync") {
    response = handleRequestSyncWithNextBox(requestArgs);
  } else {
    response =
      "Please specify if the request is sync or async, e.g. /sync/00000...000/T/J/18/0";
  }

  console.log("\tElapsed for full request (ms):", Date.now() - startTimeMs);
  console.log("Sending response:", response);
  res.end(response);
});

server.listen(port, hostname, () => {
  console.log(`Server running at http://${hostname}:${port}/`);
});
