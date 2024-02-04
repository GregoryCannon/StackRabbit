import { engineLookup, engineLookupTopMoves } from "./engine_lookup";
import { rateSurface } from "./evaluator";
import { getSearchStateAfter } from "./main";
import { getPossibleMoves } from "./move_search";
import { SHOULD_LOG } from "./params";
import { PreComputeManager } from "./precompute";
import {
  boardEquals,
  formatPossibility,
  getSurfaceArrayAndHoles,
  logBoard,
} from "./utils";
import { CPP_APP_VERSION, JS_APP_VERSION } from "./versions";
import {
  parseUrlArguments,
  getSearchStateFromUrlArguments,
  getCppEncodedInputString,
} from "./request_parser";
const cModule = require("../../../build/Release/cRabbit");
const mainApp = require("./main");
const params = require("./params");

function sleep(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

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

  // Returns [string (result data), number (http code)]
  async routeRequest(req) {
    let requestArgsFull = req.url.slice(1); // Remove initial slash
    requestArgsFull = decodeURI(requestArgsFull); // Decode URL artifacts, e.g. %20
    requestArgsFull = requestArgsFull.replace(/\s/g, ""); // Remove spaces
    const [requestType, requestArgs] = requestArgsFull.split("?");
    const urlArgs =
      requestType !== "ping" &&
      requestType !== "version" &&
      requestType !== "async-result" &&
      parseUrlArguments(requestArgs, requestType);
    const searchState = getSearchStateFromUrlArguments(urlArgs);

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
        return [this.handleEngineLookup(searchState, urlArgs), 200];

      case "engine-movelist":
        return [this.handleEngineLookupTopMoves(searchState, urlArgs), 200];

      case "engine-movelist-cpp":
        return [this.handleCppLookupTopMoves(searchState, urlArgs), 200];

      case "engine-movelist-cpp-hybrid":
        return [this.handleCppLookupTopMovesHybrid(searchState, urlArgs), 200];

      case "get-move":
        return [
          this.getMoveSync(searchState, urlArgs, /* isCpp= */ false),
          200,
        ];

      case "get-move-cpp":
        return [this.getMoveSync(searchState, urlArgs, /* isCpp= */ true), 200];

      case "get-move-async":
        return this._wrapAsync(() =>
          this.getMoveSync(searchState, urlArgs, /* isCpp= */ false)
        );

      case "get-move-async-cpp":
        return this._wrapAsync(() =>
          this.getMoveSync(searchState, urlArgs, /* isCpp= */ true)
        );

      case "eval":
        return [this.handleRequestEvalNoNextBox(searchState, urlArgs), 200];

      case "rate-move":
        return [this.handleRequestRateMove(searchState, urlArgs), 200];

      case "rate-move-cpp":
        return [this.handleCppRateMove(searchState, urlArgs), 200];

      case "precompute":
        if (!this.preComputeManager) {
          return [
            "Precompute requests are not supported on this server instance.",
            200,
          ];
        }
        return this._wrapAsync(() =>
          this.handlePrecomputeRequest(searchState, urlArgs)
        );

      case "precompute-sync":
        if (!this.preComputeManager) {
          return [
            "Precompute requests are not supported on this server instance.",
            200,
          ];
        }
        const result = await this.handlePrecomputeRequestSync(searchState, urlArgs);
        return [result, 200];

      case "version":
        return [
          JSON.stringify({
            jsVersion: JS_APP_VERSION,
            cppVersion: CPP_APP_VERSION,
          }),
          200,
        ];
      default:
        return [
          "Please specify the request type, e.g. 'get-move' or 'rate-move'. Received: " +
            requestType,
          200,
        ];
    }
  }

  _wrapAsync(func): [string, number] {
    const execute = async function () {
      // Wait 1ms to ensure that this is called async
      await sleep(1);
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
  getMoveSync(searchState: SearchState, urlArgs: UrlArguments, isCpp: boolean) {
    console.time("GetMove");

    let bestMove;

    if (isCpp) {
      // Ping the CPP backend
      const encodedInputString = getCppEncodedInputString(searchState, urlArgs);
      const result = JSON.parse(cModule.getMove(encodedInputString));
      const [rotation, xOffset, yOffset] = result;
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
      for (const possibility of possibilityList) {
        if (
          possibility.placement[0] === rotation &&
          possibility.placement[1] === xOffset && 
          possibility.placement[2] === yOffset
        ) {
          const possibilityChain: PossibilityChain = {
            totalValue: -1,
            searchStateAfterMove: getSearchStateAfter(searchState, possibility),
            ...possibility,
          };
          bestMove = possibilityChain;
          break;
        }
      }
    } else {
      // Get the best move with StackRabbit 1.0 Javascript code
      bestMove = mainApp.getBestMove(
        searchState,
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

  handleCppLookupTopMoves(searchState: SearchState, urlArgs: UrlArguments) {
    const encodedInputString = getCppEncodedInputString(searchState, urlArgs);
    return cModule.getTopMoves(encodedInputString);
  }

  handleCppLookupTopMovesHybrid(
    searchState: SearchState,
    urlArgs: UrlArguments
  ) {
    const encodedInputString = getCppEncodedInputString(searchState, urlArgs);
    if (!urlArgs.nextPiece) {
      return "Error: engine-movelist-cpp-hybrid request requires the next piece as a URL argument.";
    }
    return cModule.getTopMovesHybrid(encodedInputString);
  }

  handleCppRateMove(searchState: SearchState, urlArgs: UrlArguments) {
    const encodedInputString = getCppEncodedInputString(searchState, urlArgs);
    return cModule.rateMove(encodedInputString);
  }

  /**
   * Synchronously evaluate a board, with no next box and no search.
   * @returns {string} the API response
   */
  handleRequestEvalNoNextBox(searchState: SearchState, urlArgs: UrlArguments) {
    console.time("EvalNoNextBox");

    // Get the best move
    const score = mainApp.getBoardEvaluation(
      searchState,
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
  handleRequestRateMove(searchState: SearchState, urlArgs: UrlArguments) {
    console.time("RateMove");

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
      bestMoveNoAdjustment: formatScore(bestNnbMoves[0].totalValue),
      playerMoveAfterAdjustment: formatScore(playerScoreAfterAdj),
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
  handlePrecomputeRequest(searchState: SearchState, urlArgs: UrlArguments) {
    if (!this.preComputeManager) {
      return;
    }

    this.preComputeManager.finessePrecompute(
      searchState,
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

  /**
   * Runs a standard precompute request (as for live-games), but returns the result synchronously.
   * @param searchState 
   * @param urlArgs 
   * @returns 
   */
  async handlePrecomputeRequestSync(searchState: SearchState, urlArgs: UrlArguments) {
    if (!this.preComputeManager) {
      return;
    }
    // Manually set these variables in lieu of _wrapAsync()
    this.asyncCallInProgress = true;
    this.asyncResult = null;
    this.partialResult = null;

    this.handlePrecomputeRequest(searchState, urlArgs);
    // Check on the async task for completion every 10 ms
    while(this.asyncCallInProgress){
      await sleep(10);
    }
    return this.asyncResult;
  }

  handleRankLookup(urlArgs) {
    logBoard(urlArgs.board);
    const surfaceArray = getSurfaceArrayAndHoles(urlArgs.board)[0];
    console.log(surfaceArray);
    return rateSurface(surfaceArray);
  }

  handleEngineLookup(searchState: SearchState, urlArgs: UrlArguments) {
    return JSON.stringify(
      engineLookup(
        searchState,
        params.getParams(),
        params.getParamMods(),
        urlArgs.inputFrameTimeline
      )
    );
  }

  handleEngineLookupTopMoves(searchState: SearchState, urlArgs: UrlArguments) {
    return JSON.stringify(
      engineLookupTopMoves(
        searchState,
        params.getParams(),
        params.getParamMods(),
        urlArgs.inputFrameTimeline,
        urlArgs.lookaheadDepth
      )
    );
  }
}
