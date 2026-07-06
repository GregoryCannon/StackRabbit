import { getPossibleMoves, getSearchStateAfter } from "./move_search";
import { IS_DAS, SHOULD_LOG } from "./params";
import { PreComputeManager } from "./precompute";
import { formatPossibility } from "./utils";
import {
  parseUrlArguments,
  getSearchStateFromUrlArguments,
  getCppEncodedInputString,
} from "./request_parser";
const cModule = require("../../../build/Release/cRabbit");

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

      case "engine-movelist-cpp":
        return [this.handleCppLookupTopMoves(searchState, urlArgs), 200];

      case "engine-movelist-cpp-hybrid":
        return [this.handleCppLookupTopMovesHybrid(searchState, urlArgs), 200];

      case "get-move-cpp":
        return [this.getMoveSync(searchState, urlArgs), 200];

      case "get-move-async-cpp":
        return this._wrapAsync(() => this.getMoveSync(searchState, urlArgs));

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
        const result = await this.handlePrecomputeRequestSync(
          searchState,
          urlArgs
        );
        return [result, 200];

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
  getMoveSync(searchState: SearchState, urlArgs: UrlArguments) {
    console.time("GetMove");
    let bestMove;

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
      searchState.dasCharge,
      searchState.dasButtonHeld,
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
  async handlePrecomputeRequestSync(
    searchState: SearchState,
    urlArgs: UrlArguments
  ) {
    if (!this.preComputeManager) {
      return;
    }
    // Manually set these variables in lieu of _wrapAsync()
    this.asyncCallInProgress = true;
    this.asyncResult = null;
    this.partialResult = null;

    this.handlePrecomputeRequest(searchState, urlArgs);
    // Check on the async task for completion every 10 ms
    while (this.asyncCallInProgress) {
      await sleep(10);
    }
    return this.asyncResult;
  }
}
