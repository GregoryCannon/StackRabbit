import { PIECE_LOOKUP } from "../tetrominoes";
import { getBoardAndLinesClearedAfterPlacement } from "./board_helper";
import { rateSurface } from "./evaluator";
import { PreComputeManager } from "./precompute";
import { formatPossibility, getSurfaceArrayAndHoles, logBoard } from "./utils";

const mainApp = require("./main");
const params = require("./params");

const REQUEST_ACCEPTED_STR = "Request accepted.";

const SHOULD_LOG_ALL = false;

export class RequestHandler {
  preComputeManager: PreComputeManager;
  asyncCallInProgress: boolean;
  asyncResult: string;

  constructor(precomputeManager) {
    this.preComputeManager = precomputeManager;
    this.asyncCallInProgress = false;
    this.asyncResult = null;

    this.routeRequest = this.routeRequest.bind(this);
    this._wrapAsync = this._wrapAsync.bind(this);
  }

  routeRequest(req): [string, number] {
    const [_, requestType, ...requestArgs] = req.url.split("/");

    switch (requestType) {
      case "ping":
        return ["pong", 200];

      case "async-result":
        // If a previous async request has now completed, send that.
        if (this.asyncResult !== null) {
          return [this.asyncResult, 200];
        } else if (this.asyncCallInProgress) {
          return ["Still calculating", 504]; // Gateway timeout
        } else {
          return ["No previous async request has been made", 404]; // Not found
        }

      case "lookup":
        return [this.handleRankLookup(requestArgs), 200];

      case "async-nb":
        return this._wrapAsync(() =>
          this.handleRequestSyncWithNextBox(requestArgs)
        );

      case "async-nnb":
        return this._wrapAsync(() =>
          this.handleRequestSyncNoNextBox(requestArgs)
        );

      case "sync-nb":
        return [this.handleRequestSyncWithNextBox(requestArgs), 200];

      case "sync-nnb":
        return [this.handleRequestSyncNoNextBox(requestArgs), 200];

      case "precompute":
        return this._wrapAsync(() => this.handlePrecomputeRequest(requestArgs));

      default:
        return [
          "Please specify the request type, e.g. 'sync-nnb' or 'async-nb'. Received: " +
            requestType,
          200,
        ];
    }
  }

  /**
   * Parses and validates the inputs
   * @returns {Object} an object with all the parsed arguments
   */
  _parseArguments(requestArgs): [SearchState, string] {
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
  handleRequestAsyncWithNextBox(requestArgs): [string, number] {
    if (this.asyncCallInProgress) {
      return ["Error - already handling an async call", 500];
    }

    // Async wrapper around the synchronous handler
    const processRequest = async function (requestArgs) {
      // Wait 1ms to ensure that this is called async
      await new Promise((resolve) => setTimeout(resolve, 1));
      this.asyncResult = this.handleRequestSyncWithNextBox(requestArgs);
      this.asyncCallInProgress = false;
    }.bind(this);

    this.asyncCallInProgress = true;
    this.asyncResult = null;
    processRequest(requestArgs);
    return [REQUEST_ACCEPTED_STR, 200];
  }

  _wrapAsync(func): [string, number] {
    const execute = async function () {
      // Wait 1ms to ensure that this is called async
      await new Promise((resolve) => setTimeout(resolve, 1));
      this.asyncResult = func();
      this.asyncCallInProgress = false;
    }.bind(this);

    this.asyncCallInProgress = true;
    this.asyncResult = null;
    execute();
    return ["Request accepted.", 200];
  }

  /**
   * Asynchronously chooses the best placement, with no next box and 1-depth search.
   * Doesn't block the thread while computing, and returns the API response that should be sent in the meantime.
   * @returns {string} the *initial* API response - i.e. whether the request was accepted and started
   */
  handleRequestAsyncNoNextBox(requestArgs): [string, number] {
    if (this.asyncCallInProgress) {
      throw new Error(
        "Async call made when previous calculation wasn't complete"
      );
      // return ["Error - already handling an async call", 500];
    }

    // Async wrapper around the synchronous handler
    const processRequest = async function (requestArgs) {
      // Wait 1ms to ensure that this is called async
      await new Promise((resolve) => setTimeout(resolve, 1));
      this.asyncResult = this.handleRequestSyncNoNextBox(requestArgs);
      this.asyncCallInProgress = false;
    }.bind(this);

    this.asyncCallInProgress = true;
    this.asyncResult = null;
    processRequest(requestArgs);
    return [REQUEST_ACCEPTED_STR, 200];
  }

  /**
   * Synchronously choose the best placement, with no next box and no search.
   * @returns {string} the API response
   */
  handleRequestSyncNoNextBox(requestArgs) {
    console.time("NoNextBox");
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    // Get the best move
    const bestMove = mainApp.getBestMove(
      searchState,
      SHOULD_LOG_ALL,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 1
    );

    console.timeEnd("NoNextBox");
    if (!bestMove) {
      return "No legal moves";
    }
    return formatPossibility(bestMove);
  }

  /**
   * Synchronously choose the best placement, with next piece & 1-depth search.
   * @returns {string} the API response
   */
  handleRequestSyncWithNextBox(requestArgs) {
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    // Get the best move
    const bestMove = mainApp.getBestMove(
      searchState,
      SHOULD_LOG_ALL,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 2,
      /* hypotheticalSearchDepth= */ 1
    );

    if (!bestMove) {
      return "No legal moves";
    }
    return formatPossibility(bestMove);
  }

  /**
   * Pre-compute both an initial placement and all possible adjustments for the upcoming piece.
   * @returns {string} the API response
   */
  handlePrecomputeRequest(requestArgs) {
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    this.preComputeManager.precompute(
      searchState,
      SHOULD_LOG_ALL,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      searchState.framesAlreadyElapsed,
      function (result) {
        this.asyncResult = result;
        this.asyncCallInProgress = false;
      }.bind(this)
    );
    return REQUEST_ACCEPTED_STR;
  }

  handleRankLookup(requestArgs: Array<string>) {
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
}
