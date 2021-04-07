const http = require("http");
const hostname = "127.0.0.1";
const port = 3000;

const mainApp = require("./main");
const params = require("./params");
const evolution = require("./evolution");

let asyncCallInProgress = false;
let asyncResult = null;

/**
 * Parses and validates the inputs
 * @returns {Object} an object with all the parsed arguments
 */
function parseArguments(requestArgs) {
  // Parse and validate inputs
  let [
    boardStr,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
  ] = requestArgs;
  level = parseInt(level);
  lines = parseInt(lines);
  existingXOffset = parseInt(existingXOffset) || 0;
  existingYOffset = parseInt(existingYOffset) || 0;
  firstShiftDelay = parseInt(firstShiftDelay) || 0;
  existingRotation = parseInt(existingRotation) || 0;

  // Validate pieces
  currentPieceStr = currentPieceStr.toUpperCase();
  nextPieceStr = nextPieceStr.toUpperCase();
  if (!["I", "O", "L", "J", "T", "S", "Z"].includes(currentPieceStr)) {
    throw new Error("Unknown current piece:" + currentPieceStr);
  }
  if (!["I", "O", "L", "J", "T", "S", "Z", "NULL"].includes(nextPieceStr)) {
    throw new Error("Unknown next piece: '" + nextPieceStr + "'");
  }
  if (level < 0) {
    throw new Error("Illegal level:", level);
  }
  if (lines === undefined || lines < 0) {
    throw new Error("Illegal line count:", lines);
  }
  if (existingRotation < 0 || existingRotation > 3) {
    throw new Error("Illegal existing rotation:", existingRotation);
  }
  if (level < 18 || level > 30) {
    console.log("WARNING - Unusual level:", level);
  }

  // Decode the board
  const startingBoard = boardStr
    .match(/.{1,10}/g) // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split("").map((x) => parseInt(x)));

  return {
    startingBoard,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
  };
}

/**
 * Asynchronously chooses the best placement, with next box and 1-depth search.
 * Doesn't block the thread while computing, and returns the API response that should be sent in the meantime.
 * @param {function} callbackFunction called on completion
 * @returns {string} the *initial* API response - i.e. whether the request was accepted and started
 */
function handleRequestAsyncWithNextBox(requestArgs) {
  if (asyncCallInProgress) {
    return ["Error - already handling an async call", 500];
  }

  // Async wrapper around the synchronous handler
  async function processRequest(requestArgs) {
    // Wait 1ms to ensure that this is called async
    await new Promise((resolve) => setTimeout(resolve, 1));
    asyncResult = handleRequestSyncWithNextBox(requestArgs);
    asyncCallInProgress = false;
  }

  asyncCallInProgress = true;
  asyncResult = null;
  processRequest(requestArgs);
  return ["Request accepted.", 200];
}

/**
 * Synchronously choose the best placement, with no next box and no search.
 * @returns {string} the API response
 */
function handleRequestSyncNoNextBox(requestArgs) {
  let {
    startingBoard,
    currentPieceStr,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
  } = parseArguments(requestArgs);

  // Get the best move
  const bestMove = mainApp.getBestMove(
    startingBoard,
    currentPieceStr,
    null,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
    /* shouldLog= */ false,
    params.getParams(),
    params.getParamMods(),
    /* searchDepth= */ 1
  );

  if (!bestMove) {
    return "No legal moves";
  }
  return bestMove.placement[0] + "," + bestMove.placement[1];
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
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
  } = parseArguments(requestArgs);

  // Get the best move
  const bestMove = mainApp.getBestMove(
    startingBoard,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    firstShiftDelay,
    existingRotation,
    /* shouldLog= */ true,
    params.getParams(),
    params.getParamMods(),
    /* searchDepth= */ 2
  );

  if (!bestMove) {
    return "No legal moves";
  }
  return bestMove.placement[0] + "," + bestMove.placement[1];
}

/**
 * Create HTTP server for the frontend to request
 */
const server = http.createServer((req, res) => {
  res.statusCode = 200;
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Content-Type", "text/plain");

  console.log("\n-------------------------\n" + req.url);

  let response,
    responseCode = 200;
  const [_, requestType, ...requestArgs] = req.url.split("/");
  const startTimeMs = Date.now(); // Save the start time for performance tracking

  // Route the request
  if (requestType === "ping") {
    response = "pong";
  } else if (requestType === "async-result") {
    // If a previous async request has now completed, send that.
    if (asyncResult !== null) {
      response = asyncResult;
    } else if (asyncCallInProgress) {
      responseCode = 504; // Gateway timeout
      response = "Still calculating...";
    } else {
      responseCode = 404; // Not found
      response = "No previous async request has been made";
    }
  } else if (requestType === "async-nb") {
    [response, responseCode] = handleRequestAsyncWithNextBox(requestArgs);
  } else if (requestType === "sync-nb") {
    response = handleRequestSyncWithNextBox(requestArgs);
  } else if (requestType === "sync-nnb") {
    response = handleRequestSyncNoNextBox(requestArgs);
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
