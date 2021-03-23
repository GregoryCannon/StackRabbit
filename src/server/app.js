const http = require("http");
const hostname = "127.0.0.1";
const port = 3000;

const mainApp = require("./main");
const params = require("./params");
const evolution = require("./evolution");

/**
 * Parses and validates the inputs
 * @returns {Object} an object with all the parsed arguments
 */
function parseArguments(requestArgs) {
  // Parse and validate inputs
  let [boardStr, currentPieceStr, nextPieceStr, level, lines] = requestArgs;

  // Validate pieces
  currentPieceStr = currentPieceStr.toUpperCase();
  nextPieceStr = nextPieceStr.toUpperCase();
  if (!["I", "O", "L", "J", "T", "S", "Z"].includes(currentPieceStr)) {
    throw new Error("Unknown current piece:" + currentPieceStr);
  }
  if (!["I", "O", "L", "J", "T", "S", "Z"].includes(nextPieceStr)) {
    nextPieceStr = "";
  }

  // Decode the board
  const startingBoard = boardStr
    .match(/.{1,10}/g) // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split("").map(x => parseInt(x)));

  return { startingBoard, currentPieceStr, nextPieceStr, level, lines };
}

/**
 * Synchronously choose the best placement, with no next box and no search.
 * @returns {string} the API response
 */
function handleRequestSyncWithNoNextBox(requestArgs) {
  let { startingBoard, currentPieceStr, level, lines } = parseArguments(
    requestArgs
  );

  // Get the best move
  const bestMove = mainApp.getBestMoveNoSearch(
    startingBoard,
    currentPieceStr,
    null,
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
 * Synchronously choose the best placement, with next piece & 1-depth search.
 * @returns {string} the API response
 */
function handleRequestSyncWithNextBox(requestArgs) {
  let {
    startingBoard,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
  } = parseArguments(requestArgs);

  // Get the best move
  const bestMove = mainApp.getBestMoveWithSearch(
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
  } else if (requestType === "async-nb") {
    // Incomplete
  } else if (requestType === "sync-nb") {
    response = handleRequestSyncWithNextBox(requestArgs);
  } else if (requestType === "sync-nnb") {
    response = handleRequestSyncWithNoNextBox(requestArgs);
  } else {
    response =
      "Please specify the request type, e.g. 'sync-nnb' or 'async-nb'. Received: " +
      requestType;
  }

  console.log("\tElapsed for full request (ms):", Date.now() - startTimeMs);
  console.log("Sending response:", response);
  res.end(response);
});

server.listen(port, hostname, () => {
  console.log(`Server running at http://${hostname}:${port}/`);
});
