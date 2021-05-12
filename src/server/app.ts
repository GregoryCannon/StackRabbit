import { PIECE_LOOKUP } from "../tetrominoes";
import { getBoardAndLinesClearedAfterPlacement } from "./board_helper";
import { rateSurface } from "./evaluator";
import { getSurfaceArrayAndHoles, logBoard } from "./utils";

const http = require("http");
const hostname = "127.0.0.1";
const port = 3000;

const mainApp = require("./main");
const params = require("./params");

let asyncCallInProgress = false;
let asyncResult = null;

const SHOULD_LOG_ALL = false;

const SHOULD_LOG_NB = SHOULD_LOG_ALL;
const SHOULD_LOG_NO_NB = SHOULD_LOG_ALL;

/**
 * Parses and validates the inputs
 * @returns {Object} an object with all the parsed arguments
 */
function parseArguments(requestArgs): [SearchState, Array<number>] {
  // Parse and validate inputs
  let [
    boardStr,
    currentPieceId,
    nextPieceId,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    existingRotation,
    framesAlreadyElapsed,
    inputFrameTimeline,
    canFirstFrameShift,
  ] = requestArgs;
  level = parseInt(level);
  lines = parseInt(lines);
  existingXOffset = parseInt(existingXOffset) || 0;
  existingYOffset = parseInt(existingYOffset) || 0;
  framesAlreadyElapsed = parseInt(framesAlreadyElapsed) || 0;
  existingRotation = parseInt(existingRotation) || 0;
  canFirstFrameShift = canFirstFrameShift.toLowerCase() === "true";

  console.log({
    boardStr,
    currentPieceId,
    nextPieceId,
    level,
    lines,
    existingXOffset,
    existingYOffset,
    existingRotation,
    framesAlreadyElapsed,
    inputFrameTimeline,
    canFirstFrameShift,
  });

  // Validate pieces
  currentPieceId = currentPieceId.toUpperCase();
  nextPieceId = nextPieceId.toUpperCase();
  if (!["I", "O", "L", "J", "T", "S", "Z"].includes(currentPieceId)) {
    throw new Error("Unknown current piece:" + currentPieceId);
  }
  if (!["I", "O", "L", "J", "T", "S", "Z", "NULL"].includes(nextPieceId)) {
    throw new Error("Unknown next piece: '" + nextPieceId + "'");
  }
  if (level < 0) {
    throw new Error("Illegal level: " + level);
  }
  if (lines === undefined || lines < 0) {
    throw new Error("Illegal line count: " + lines);
  }
  if (existingRotation < 0 || existingRotation > 3) {
    throw new Error("Illegal existing rotation: " + existingRotation);
  }
  if (level < 18 || level > 30) {
    console.log("WARNING - Unusual level:", level);
  }
  for (const char of inputFrameTimeline) {
    if (char !== "X" && char !== ".") {
      throw new Error("Invalid input frame timeline: " + inputFrameTimeline);
    }
  }
  if (nextPieceId === "NULL") {
    nextPieceId = null;
  }

  // Decode the board
  const board = boardStr
    .match(/.{1,10}/g) // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split("").map((x) => parseInt(x)));

  // logBoard(board);
  logBoard(
    getBoardAndLinesClearedAfterPlacement(
      board,
      PIECE_LOOKUP[currentPieceId][0][existingRotation],
      existingXOffset + 3,
      existingYOffset + (currentPieceId === "I" ? -2 : -1)
    )[0]
  );

  return [
    {
      board,
      currentPieceId,
      nextPieceId,
      level,
      lines,
      existingXOffset,
      existingYOffset,
      existingRotation,
      framesAlreadyElapsed,
      canFirstFrameShift,
    },
    inputFrameTimeline,
  ];
}

/**
 * Asynchronously chooses the best placement, with next box and 1-depth search.
 * Doesn't block the thread while computing, and returns the API response that should be sent in the meantime.
 * @returns {string} the *initial* API response - i.e. whether the request was accepted and started
 */
function handleRequestAsyncWithNextBox(requestArgs): [string, number] {
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
 * Asynchronously chooses the best placement, with no next box and 1-depth search.
 * Doesn't block the thread while computing, and returns the API response that should be sent in the meantime.
 * @returns {string} the *initial* API response - i.e. whether the request was accepted and started
 */
function handleRequestAsyncNoNextBox(requestArgs): [string, number] {
  if (asyncCallInProgress) {
    throw new Error(
      "Async call made when previous calculation wasn't complete"
    );
    // return ["Error - already handling an async call", 500];
  }

  // Async wrapper around the synchronous handler
  async function processRequest(requestArgs) {
    // Wait 1ms to ensure that this is called async
    await new Promise((resolve) => setTimeout(resolve, 1));
    asyncResult = handleRequestSyncNoNextBox(requestArgs);
    asyncCallInProgress = false;
  }

  asyncCallInProgress = true;
  asyncResult = null;
  processRequest(requestArgs);
  return ["Request accepted.", 200];
}

function formatResponse(possibility: PossibilityChain) {
  return (
    possibility.placement[0] +
    "," +
    possibility.placement[1] +
    "|" +
    (possibility.inputSequence || "none") +
    "|" +
    possibility.boardAfter.map((row) => row.join("")).join("") +
    "|" +
    possibility.searchStateAfterMove.level +
    "|" +
    possibility.searchStateAfterMove.lines
  );
}

/**
 * Synchronously choose the best placement, with no next box and no search.
 * @returns {string} the API response
 */
function handleRequestSyncNoNextBox(requestArgs) {
  let [searchState, inputDelaySequence] = parseArguments(requestArgs);

  // Get the best move
  const bestMove = mainApp.getBestMove(
    searchState,
    SHOULD_LOG_NO_NB,
    params.getParams(),
    params.getParamMods(),
    inputDelaySequence,
    /* searchDepth= */ 1,
    /* hypotheticalSearchDepth= */ 1
  );

  if (!bestMove) {
    return "No legal moves";
  }
  return formatResponse(bestMove);
}

/**
 * Synchronously choose the best placement, with next piece & 1-depth search.
 * @returns {string} the API response
 */
function handleRequestSyncWithNextBox(requestArgs) {
  let [searchState, inputDelaySequence] = parseArguments(requestArgs);

  // Get the best move
  const bestMove = mainApp.getBestMove(
    searchState,
    SHOULD_LOG_NB,
    params.getParams(),
    params.getParamMods(),
    inputDelaySequence,
    /* searchDepth= */ 2,
    /* hypotheticalSearchDepth= */ 1
  );

  if (!bestMove) {
    return "No legal moves";
  }
  return formatResponse(bestMove);
}

function handleRankLookup(requestArgs: Array<string>) {
  const [boardStr] = requestArgs;
  // Decode the board
  const board = boardStr
    .match(/.{1,10}/g) // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split("").map((x) => parseInt(x)));
  logBoard(board);
  const surfaceArray = getSurfaceArrayAndHoles(board)[0];
  console.log(surfaceArray);
  return rateSurface(surfaceArray);
}

/**
 * Create HTTP server for the frontend to request
 */
const server = http.createServer((req, res) => {
  res.statusCode = 200;
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Content-Type", "text/plain");

  console.log("\n-------------------------\nlocalhost:3000" + req.url);

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
  } else if (requestType === "lookup") {
    response = handleRankLookup(requestArgs);
  } else if (requestType === "async-nb") {
    [response, responseCode] = handleRequestAsyncWithNextBox(requestArgs);
  } else if (requestType === "async-nnb") {
    [response, responseCode] = handleRequestAsyncNoNextBox(requestArgs);
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
  res.statusCode = responseCode;
  res.end(response);
});

server.listen(port, hostname, () => {
  console.log(`Server running at http://${hostname}:${port}/`);
});
