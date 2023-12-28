import { PIECE_LOOKUP } from "../../docs/tetrominoes";
import { getBoardAndLinesClearedAfterPlacement } from "./board_helper";
import { engineLookup, engineLookupTopMoves } from "./engine_lookup";
import { rateSurface } from "./evaluator";
import { getSearchStateAfter } from "./main";
import { getPossibleMoves } from "./move_search";
import { DISABLE_LOGGING, LINE_CAP, SHOULD_LOG, USE_CPP } from "./params";
import { PreComputeManager } from "./precompute";
import {
  boardEquals,
  formatPossibility,
  getSurfaceArrayAndHoles,
  logBoard,
  parseBoard,
} from "./utils";
import { CPP_APP_VERSION, JS_APP_VERSION } from "./versions";
const cModule = require("../../../build/Release/cRabbit");
const mainApp = require("./main");
const params = require("./params");

export class RequestHandler {
  preComputeManager: PreComputeManager;
  asyncCallInProgress: boolean;
  asyncResult: string;
  partialResult: any;
  partialResultsUsed: number;
  computationsFinished: number;

  constructor(precomputeManager) {
    this.preComputeManager = precomputeManager;
    this.asyncCallInProgress = false;
    this.asyncResult = null;
    this.partialResult = null;
    this.partialResultsUsed = 0;
    this.computationsFinished = 0;

    this.routeRequest = this.routeRequest.bind(this);
    this._wrapAsync = this._wrapAsync.bind(this);
  }

  routeRequest(req): [string, number] {
    let requestArgsFull = req.url.slice(1); // Remove initial slash
    requestArgsFull = decodeURI(requestArgsFull); // Decode URL artifacts, e.g. %20
    requestArgsFull = requestArgsFull.replace(/\s/g, ""); // Remove spaces
    const [requestType, requestArgs] = requestArgsFull.split("?");
    const urlArgs =
      requestType !== "ping" &&
      requestType !== "version" &&
      requestType !== "async-result" &&
      this._parseArguments(requestArgs);

    switch (requestType) {
      case "ping":
        return ["pong", 200];

      case "async-result":
        console.log("FAILED:", this.partialResultsUsed);
        // If a previous async request has now completed, send that.
        if (this.asyncResult !== null) {
          return [this.asyncResult, 200];
        } else if (this.partialResult !== null) {
          for (let i = 0; i < 10; i++) {
            console.log(
              "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
            );
          }
          this.partialResultsUsed += 1;
          return [this.partialResult, 200];
        } else if (this.asyncCallInProgress) {
          return ["Still calculating", 504]; // Gateway timeout
        } else {
          return ["No previous async request has been made", 404]; // Not found
        }

      case "rank-lookup":
        return [this.handleRankLookup(urlArgs), 200];

      case "engine":
        return [this.handleEngineLookup(urlArgs), 200];

      case "engine-movelist":
        return [this.handleEngineLookupTopMoves(urlArgs), 200];

      case "get-move":
        return [this.handleRequestSync(urlArgs), 200];

      case "get-move-async":
        return this._wrapAsync(() => this.handleRequestSync(urlArgs));

      case "eval":
        return [this.handleRequestEvalNoNextBox(urlArgs), 200];

      case "rate-move":
        return [this.handleRequestRateMove(urlArgs), 200];

      case "precompute":
        if (!this.preComputeManager) {
          return [
            "Precompute requests are not supported on this server instance.",
            200,
          ];
        }
        return this._wrapAsync(() => this.handlePrecomputeRequest(urlArgs));

      case "version":
        return [
          JSON.stringify({
            jsVersion: JS_APP_VERSION,
            cppVersion: CPP_APP_VERSION
          }),
          200,
        ]
      default:
        return [
          "Please specify the request type, e.g. 'get-move' or 'rate-move'. Received: " +
            requestType,
          200,
        ];
    }
  }

  _getSearchStateFromUrlArguments(urlArgs): SearchState {
    return {
      board: urlArgs.board,
      currentPieceId: urlArgs.currentPiece,
      nextPieceId: urlArgs.nextPiece,
      level: urlArgs.level,
      lines: urlArgs.lines,
      existingXOffset: urlArgs.existingXOffset,
      existingYOffset: urlArgs.existingYOffset,
      existingRotation: urlArgs.existingRotation,
      reactionTime: urlArgs.reactionTime,
      framesAlreadyElapsed: urlArgs.existingFramesElapsed,
      canFirstFrameShift: urlArgs.arrWasReset,
    };
  }

  /**
   * Parses and validates the inputs
   * @returns {Object} an object with all the parsed arguments
   */
  _parseArguments(reqString: string): UrlArguments {
    // Get default values in the SearchState
    // Anything set to 'undefined' is required to be provided in the URL args
    const result: UrlArguments = {
      board: undefined,
      secondBoard: null,
      currentPiece: undefined,
      nextPiece: null,
      level: undefined,
      lines: undefined,
      reactionTime: 0,
      inputFrameTimeline: undefined,
      arrWasReset: false,
      lookaheadDepth: 0,
      existingXOffset: 0,
      existingYOffset: 0,
      existingRotation: 0,
      existingFramesElapsed: 0,
    };

    // Query for non-default values
    const pairs = reqString.split("&").map((x) => x.split("="));

    for (const [argName, value] of pairs) {
      switch (argName) {
        case "board":
          result.board = parseBoard(value);
          break;

        case "secondBoard":
          result.secondBoard = parseBoard(value);
          break;

        case "currentPiece":
          const curPieceId = value.toUpperCase();
          if (!["I", "O", "L", "J", "T", "S", "Z"].includes(curPieceId)) {
            throw new Error("Unknown current piece:" + curPieceId);
          }
          result.currentPiece = curPieceId as PieceId;
          break;

        case "nextPiece":
          const nextPieceId = value.toUpperCase();
          if (!["I", "O", "L", "J", "T", "S", "Z"].includes(nextPieceId)) {
            throw new Error("Unknown next piece:" + nextPieceId);
          }
          result.nextPiece = nextPieceId as PieceId;
          break;

        case "level":
          const level = parseInt(value);
          if (isNaN(level) || level < 0) {
            throw new Error("Illegal level: " + value);
          }
          if (level < 18) {
            throw new Error(
              "Currently levels lower than 18 are not supported by StackRabbit. Requested: " +
                value
            );
          }
          result.level = level;
          break;

        case "lines":
          const lines = parseInt(value);
          if (isNaN(lines) || lines < 0) {
            throw new Error("Illegal line count: " + value);
          }
          result.lines = lines;
          break;

        case "reactionTime":
          const reactionTime = parseInt(value);
          if (isNaN(reactionTime) || reactionTime < 0) {
            throw new Error("Illegal reaction time: " + value);
          }
          if (reactionTime > 60) {
            throw new Error(
              "Reaction time exceeds the maximum possible of 60 frames. Did you accidentally send it in millseconds?"
            );
          }
          result.reactionTime = reactionTime;
          break;

        case "inputFrameTimeline":
          for (const char of value) {
            if (char !== "X" && char !== "." && char !== "-") {
              throw new Error("Invalid input frame timeline: " + value);
            }
          }
          // Replace hyphens with dots so that timelines can optionally be represented as X--- instead of X...
          result.inputFrameTimeline = value.replace(/-/g, ".");
          break;

        case "lookaheadDepth":
          const depth = parseInt(value);
          if (depth < 0) {
            throw new Error("Invalid lookahead depth: " + depth);
          }
          if (depth > 1) {
            throw new Error(
              "Maximum supported lookahead depth is currently 1."
            );
          }
          result.lookaheadDepth = depth;
          break;

        // These properties are pretty advanced, if you're using them you should know what you're doing
        case "existingXOffset":
          result.existingXOffset = parseInt(value);
          break;
        case "existingYOffset":
          result.existingYOffset = parseInt(value);
          break;
        case "existingRotation":
          result.existingRotation = parseInt(value);
          break;
        case "existingFramesElapsed":
          result.existingFramesElapsed = parseInt(value);
          break;
        case "arrWasReset":
          result.arrWasReset =
            value === "true" || value === "TRUE" || value === "1";
          break;
      }
    }

    if (!DISABLE_LOGGING) {
      logBoard(
        getBoardAndLinesClearedAfterPlacement(
          result.board,
          PIECE_LOOKUP[result.currentPiece][0][result.existingRotation],
          result.existingXOffset + 3,
          result.existingYOffset + (result.currentPiece === "I" ? -2 : -1)
        )[0]
      );
      if (result.secondBoard) {
        logBoard(result.secondBoard);
      }
    }

    // Manually top out if past line cap
    if (result.lines >= LINE_CAP) {
      result.inputFrameTimeline = "."; // Manually top out
    }

    return result;
  }

  _wrapAsync(func): [string, number] {
    const execute = async function () {
      // Wait 1ms to ensure that this is called async
      await new Promise((resolve) => setTimeout(resolve, 1));
      const result = func();
      if (result !== undefined) {
        this.asyncResult = result;
        this.asyncCallInProgress = false;
      }
    }.bind(this);

    this.asyncCallInProgress = true;
    this.asyncResult = null;
    this.partialResult = null;
    execute();
    return ["Request accepted.", 200];
  }

  /**
   * Synchronously choose the best placement, with no next box and no search.
   * @returns {string} the API response
   */
  handleRequestSync(urlArgs) {
    console.time("GetMove");

    const searchState = this._getSearchStateFromUrlArguments(urlArgs);

    let bestMove;

    if (USE_CPP) {
      // Ping the CPP backend
      const boardStr = searchState.board.map((x) => x.join("")).join("");
      const pieceLookup = ["I", "O", "L", "J", "T", "S", "Z"];
      const curPieceIndex = pieceLookup.indexOf(searchState.currentPieceId);
      const nextPieceIndex = pieceLookup.indexOf(searchState.nextPieceId);
      const encodedInputString = `${boardStr}|${searchState.level}|${searchState.lines}|${curPieceIndex}|${nextPieceIndex}|${urlArgs.inputFrameTimeline}|`;
      const result = JSON.parse(cModule.playMoveNoNextBox(encodedInputString));
      const [rotation, xOffset] = result;
      console.log("RESULT: ", result);

      // Format it using Javascript stuff
      let possibilityList = getPossibleMoves(
        searchState.board,
        searchState.currentPieceId,
        searchState.level,
        searchState.existingXOffset,
        searchState.existingYOffset,
        searchState.framesAlreadyElapsed,
        urlArgs.inputFrameTimeline,
        searchState.existingRotation,
        searchState.canFirstFrameShift,
        false
      );
      for (const possibility of possibilityList){
        if (possibility.placement[0] === rotation && possibility.placement[1] === xOffset){
          const possibilityChain : PossibilityChain = {
            totalValue: -1,
            searchStateAfterMove: getSearchStateAfter(searchState, possibility),
            ...possibility
          };
          bestMove = possibilityChain;
          break;
        }
      }
    } else {
      // Get the best move with StackRabbit 1.0 Javascript code
      bestMove = mainApp.getBestMove(
        this._getSearchStateFromUrlArguments(urlArgs),
        SHOULD_LOG,
        params.getParams(),
        params.getParamMods(),
        urlArgs.inputFrameTimeline,
        /* searchDepth= */ urlArgs.nextPiece !== null ? 2 : 1,
        /* hypotheticalSearchDepth= */ urlArgs.lookaheadDepth
      );
    }

    

    console.timeEnd("GetMove");
    if (!bestMove) {
      return "No legal moves";
    }
    return formatPossibility(bestMove);
  }

  /**
   * Synchronously evaluate a board, with no next box and no search.
   * @returns {string} the API response
   */
  handleRequestEvalNoNextBox(urlArgs) {
    console.time("EvalNoNextBox");

    // Get the best move
    const score = mainApp.getBoardEvaluation(
      this._getSearchStateFromUrlArguments(urlArgs),
      params.getParams(),
      params.getParamMods(),
      urlArgs.inputFrameTimeline
    );

    console.timeEnd("EvalNoNextBox");
    return score.toFixed(2) + "";
  }

  /**
   * Synchronously rate a move compared to the best move.
   * @returns {string} the API response
   */
  handleRequestRateMove(urlArgs) {
    console.time("RateMove");

    const searchState = this._getSearchStateFromUrlArguments(urlArgs);
    const inputFrameTimeline = urlArgs.inputFrameTimeline;
    const secondBoard = urlArgs.secondBoard;

    let formatScore = (score) =>
      score == null ? "Unknown score" : score.toFixed(2);

    // Get the best non-adjustment move
    let prunedNnbMoves, bestNnbMoves: Array<PossibilityChain>;
    [bestNnbMoves, prunedNnbMoves] = mainApp.getSortedMoveList(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 1,
      urlArgs.lookaheadDepth
    );
    if (!bestNnbMoves) {
      return "No legal moves";
    }

    // Find the value of the second board passed in
    let playerScoreNoAdj = this.lookupMoveValue(
      secondBoard,
      bestNnbMoves,
      prunedNnbMoves
    );

    // If there's no next box, we're done
    if (urlArgs.nextPiece == null) {
      console.timeEnd("RateMove");
      return JSON.stringify({
        playerMoveNoAdjustment: formatScore(playerScoreNoAdj),
        bestMoveNoAdjustment: bestNnbMoves[0].totalValue.toFixed(2),
      });
    }

    // Otherwise, get the best adjustment move
    let prunedNbMoves, bestNbMoves: Array<PossibilityChain>;
    [bestNbMoves, prunedNbMoves] = mainApp.getSortedMoveList(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 2,
      urlArgs.lookaheadDepth
    );
    if (!bestNbMoves) {
      return "No legal moves";
    }
    const bestScoreAfterAdj = bestNbMoves[0].totalValue;

    // Find the value of the second board passed in
    let playerScoreAfterAdj = this.lookupMoveValue(
      secondBoard,
      bestNbMoves,
      prunedNbMoves
    );

    console.timeEnd("RateMove");
    return JSON.stringify({
      playerMoveNoAdjustment: formatScore(playerScoreNoAdj),
      playerMoveAfterAdjustment: formatScore(playerScoreAfterAdj),
      bestMoveNoAdjustment: formatScore(bestNnbMoves[0].totalValue),
      bestMoveAfterAdjustment: formatScore(bestScoreAfterAdj),
    });
  }

  /** Gets the value of a move using two lists of possible options.
   * @param targetBoard - the board of the player's actual move
   * @param bestMoves - a list of the top N moves sorted best to worst
   * @param prunedMoves - all moves rank N+1 or higher, in no particular order
   */
  lookupMoveValue(
    targetBoard: Board,
    bestMoves: Array<PossibilityChain>,
    prunedMoves: Array<PossibilityChain>
  ) {
    let foundPlayerMove = false;
    // If it appears in the best moves list, we know we're done
    for (const move of bestMoves) {
      if (boardEquals(move.boardAfter, targetBoard)) {
        return move.totalValue;
      }
    }
    // If we haven't found it so far, look through the whole list of pruned moves
    if (!foundPlayerMove) {
      let moveValue = Number.MIN_SAFE_INTEGER;
      for (const move of prunedMoves) {
        if (boardEquals(move.boardAfter, targetBoard)) {
          // We do a max() and keep looping because we don't know if it will reappear later with better eval
          moveValue = Math.max(move.totalValue, moveValue);
          foundPlayerMove = true;
        }
      }
      if (foundPlayerMove) {
        return moveValue;
      }
    }
    // If it doesn't appear, return null so the callers know to format the response differently
    return null;
  }

  /**
   * Pre-compute both an initial placement and all possible adjustments for the upcoming piece.
   * @returns {string} the API response
   */
  handlePrecomputeRequest(urlArgs) {
    if (!this.preComputeManager) {
      return;
    }

    this.preComputeManager.finessePrecompute(
      this._getSearchStateFromUrlArguments(urlArgs),
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      urlArgs.inputFrameTimeline,
      function (result) {
        this.partialResult = result;
      }.bind(this),
      function (result) {
        this.asyncResult = result;
        this.asyncCallInProgress = false;
      }.bind(this)
    );
  }

  handleRankLookup(urlArgs) {
    logBoard(urlArgs.board);
    const surfaceArray = getSurfaceArrayAndHoles(urlArgs.board)[0];
    console.log(surfaceArray);
    return rateSurface(surfaceArray);
  }

  handleEngineLookup(urlArgs) {
    return JSON.stringify(
      engineLookup(
        this._getSearchStateFromUrlArguments(urlArgs),
        params.getParams(),
        params.getParamMods(),
        urlArgs.inputFrameTimeline
      )
    );
  }

  handleEngineLookupTopMoves(urlArgs: UrlArguments) {
    return JSON.stringify(
      engineLookupTopMoves(
        this._getSearchStateFromUrlArguments(urlArgs),
        params.getParams(),
        params.getParamMods(),
        urlArgs.inputFrameTimeline,
        urlArgs.lookaheadDepth
      )
    );
  }
}
