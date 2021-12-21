import { PIECE_LOOKUP } from "../../docs/tetrominoes";
import { getBoardAndLinesClearedAfterPlacement } from "./board_helper";
import {
  engineLookup,
  engineLookupTopMovesListNoNextBox,
  engineLookupTopMovesWithNextBox,
} from "./engine_lookup";
import { rateSurface } from "./evaluator";
import { DISABLE_LOGGING, LINE_CAP, SHOULD_LOG } from "./params";
import { PreComputeManager } from "./precompute";
import {
  boardEquals,
  formatPossibility,
  getSurfaceArrayAndHoles,
  logBoard,
  parseBoard,
} from "./utils";

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

      case "lookup":
        return [this.handleRankLookup(requestArgs), 200];

      case "engine":
        return [this.handleEngineLookup(requestArgs), 200];

      case "engine-movelist-nnb":
        return [this.handleEngineLookupTopMovesNoNextBox(requestArgs), 200];

      case "engine-movelist-nb":
        return [this.handleEngineLookupTopMovesWithNextBox(requestArgs), 200];

      case "async-nb":
        return this._wrapAsync(() =>
          this.handleRequestSyncWithNextBox(requestArgs, 1)
        );

      case "async-nnb":
        return this._wrapAsync(() =>
          this.handleRequestSyncNoNextBox(requestArgs)
        );

      case "research-nb":
        return [this.handleRequestSyncWithNextBox(requestArgs, 3), 200];

      case "sync-nb":
        return [this.handleRequestSyncWithNextBox(requestArgs, 1), 200];

      case "sync-nnb":
        return [this.handleRequestSyncNoNextBox(requestArgs), 200];

      case "eval":
        return [this.handleRequestEvalNoNextBox(requestArgs), 200];

      case "rate-move-nnb":
        return [this.handleRequestRateMoveNoNextBox(requestArgs, 0), 200];

      case "rate-move-nnb-3":
        return [this.handleRequestRateMoveNoNextBox(requestArgs, 1), 200];

      case "rate-move-nb":
        return [this.handleRequestRateMoveWithNextBox(requestArgs, 0), 200];

      case "rate-move-nb-3":
        return [this.handleRequestRateMoveWithNextBox(requestArgs, 1), 200];

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
  _parseArguments(reqString: string): [SearchState, string, Board] {
    // Get default values in the SearchState
    // Anything set to 'undefined' is required to be provided in the URL args
    const resultSearchState = {
      board: undefined,
      currentPieceId: undefined,
      nextPieceId: null,
      level: undefined,
      lines: undefined,
      existingXOffset: 0,
      existingYOffset: 0,
      existingRotation: 0,
      reactionTime: 0,
      framesAlreadyElapsed: 0,
      canFirstFrameShift: false,
    };
    let resultInputFrameTimeline;
    let resultSecondBoard = null;

    // Query for non-default values
    const pairs = reqString.split("&").map((x) => x.split("="));

    for (const [argName, value] of pairs) {
      switch (argName) {
        case "board":
          resultSearchState.board = parseBoard(value);
          break;

        case "secondBoard":
          resultSecondBoard = parseBoard(value);
          break;

        case "currentPiece":
          const curPieceId = value.toUpperCase();
          if (!["I", "O", "L", "J", "T", "S", "Z"].includes(curPieceId)) {
            throw new Error("Unknown current piece:" + curPieceId);
          }
          resultSearchState.currentPieceId = curPieceId as PieceId;
          break;

        case "nextPiece":
          const nextPieceId = value.toUpperCase();
          if (!["I", "O", "L", "J", "T", "S", "Z"].includes(nextPieceId)) {
            throw new Error("Unknown next piece:" + nextPieceId);
          }
          resultSearchState.nextPieceId = nextPieceId as PieceId;
          break;

        case "level":
          const level = parseInt(value);
          if (isNaN(level) || level < 0) {
            throw new Error("Illegal level: " + value);
          }
          if (level < 18) {
            throw new Error(
              "Currently only 18, 19, and 29 starts are supported by StackRabbit. Requested: " +
                value
            );
          }
          resultSearchState.level = level;
          break;

        case "lines":
          const lines = parseInt(value);
          if (isNaN(lines) || lines < 0) {
            throw new Error("Illegal line count: " + value);
          }
          resultSearchState.lines = lines;
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
          resultSearchState.reactionTime = reactionTime;
          break;

        case "inputFrameTimeline":
          for (const char of value) {
            if (char !== "X" && char !== ".") {
              throw new Error("Invalid input frame timeline: " + value);
            }
          }
          resultInputFrameTimeline = value;
          break;

        // These properties are pretty advanced, if you're using them you should know what you're doing
        case "existingXOffset":
          resultSearchState.existingXOffset = parseInt(value);
          break;
        case "existingYOffset":
          resultSearchState.existingYOffset = parseInt(value);
          break;
        case "existingRotation":
          resultSearchState.existingRotation = parseInt(value);
          break;
        case "existingFramesElapsed":
          resultSearchState.framesAlreadyElapsed = parseInt(value);
          break;
        case "arrWasReset":
          resultSearchState.canFirstFrameShift =
            value === "true" || value === "TRUE" || value === "1";
          break;
      }
    }

    if (!DISABLE_LOGGING) {
      logBoard(
        getBoardAndLinesClearedAfterPlacement(
          resultSearchState.board,
          PIECE_LOOKUP[resultSearchState.currentPieceId][0][
            resultSearchState.existingRotation
          ],
          resultSearchState.existingXOffset + 3,
          resultSearchState.existingYOffset +
            (resultSearchState.currentPieceId === "I" ? -2 : -1)
        )[0]
      );
    }

    // Manually top out if past line cap
    if (resultSearchState.lines >= LINE_CAP) {
      resultInputFrameTimeline = "."; // Manually top out
    }

    return [resultSearchState, resultInputFrameTimeline, resultSecondBoard];
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
  handleRequestSyncNoNextBox(requestArgs) {
    console.time("NoNextBox");
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    // Get the best move
    const bestMove = mainApp.getBestMove(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 0
    );

    console.timeEnd("NoNextBox");
    if (!bestMove) {
      return "No legal moves";
    }
    return formatPossibility(bestMove);
  }

  /**
   * Synchronously evaluate a board, with no next box and no search.
   * @returns {string} the API response
   */
  handleRequestEvalNoNextBox(requestArgs) {
    console.time("EvalNoNextBox");
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    // Get the best move
    const score = mainApp.getBoardEvaluation(
      searchState,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline
    );

    console.timeEnd("EvalNoNextBox");
    return score.toFixed(2) + "";
  }

  /**
   * Synchronously rate a move compared to the best move, with no next box.
   * @returns {string} the API response
   */
  handleRequestRateMoveNoNextBox(requestArgs, hypotheticalSearchDepth) {
    console.time("RateMoveNoNextBox");
    let [searchState, inputFrameTimeline, secondBoard] = this._parseArguments(
      requestArgs
    );

    // Get the best non-adjustment move
    let prunedNnbMoves, bestNnbMoves: Array<PossibilityChain>;
    [bestNnbMoves, prunedNnbMoves] = mainApp.getSortedMoveList(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ hypotheticalSearchDepth
    );
    if (!bestNnbMoves) {
      return "No legal moves";
    }

    // Find the value of the second board passed in
    let playerScore = this.lookupMoveValue(
      secondBoard,
      bestNnbMoves,
      prunedNnbMoves
    );

    let playerScoreFormatted =
      playerScore == null ? "Unknown score" : playerScore.toFixed(2);

    console.timeEnd("RateMoveNoNextBox");
    return JSON.stringify({
      playerMoveNoAdjustment: playerScoreFormatted,
      bestMoveNoAdjustment: bestNnbMoves[0].totalValue.toFixed(2),
    });
  }

  /**
   * Synchronously rate a move compared to the best move, with no next box.
   * @returns {string} the API response
   */
  handleRequestRateMoveWithNextBox(requestArgs, hypotheticalSearchDepth) {
    console.time("RateMoveWithNextBox");
    let [searchState, inputFrameTimeline, secondBoard] = this._parseArguments(
      requestArgs
    );

    // Get the best non-adjustment move
    let prunedNnbMoves, bestNnbMoves: Array<PossibilityChain>;
    [bestNnbMoves, prunedNnbMoves] = mainApp.getSortedMoveList(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ hypotheticalSearchDepth
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

    // Get the best adjustment move
    let prunedNbMoves, bestNbMoves: Array<PossibilityChain>;
    [bestNbMoves, prunedNbMoves] = mainApp.getSortedMoveList(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 2,
      /* hypotheticalSearchDepth= */ hypotheticalSearchDepth
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

    let formatScore = (score) =>
      score == null ? "Unknown score" : score.toFixed(2);

    console.timeEnd("RateMoveWithNextBox");
    return JSON.stringify({
      playerMoveNoAdjustment: formatScore(playerScoreNoAdj),
      playerMoveAfterAdjustment: formatScore(playerScoreAfterAdj),
      bestMoveNoAdjustment: bestNnbMoves[0].totalValue.toFixed(2),
      bestMoveAfterAdjustment: bestScoreAfterAdj.toFixed(2),
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
   * Synchronously choose the best placement, with next piece & 1-depth search.
   * @returns {string} the API response
   */
  handleRequestSyncWithNextBox(requestArgs, hypotheticalSearchDepth) {
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    // Get the best move
    const bestMove = mainApp.getBestMove(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      /* searchDepth= */ 2,
      hypotheticalSearchDepth
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
    if (!this.preComputeManager) {
      return;
    }
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    this.preComputeManager.finessePrecompute(
      searchState,
      SHOULD_LOG,
      params.getParams(),
      params.getParamMods(),
      inputFrameTimeline,
      function (result) {
        this.partialResult = result;
      }.bind(this),
      function (result) {
        this.asyncResult = result;
        this.asyncCallInProgress = false;
      }.bind(this)
    );
  }

  handleRankLookup(requestArgs) {
    let [searchState] = this._parseArguments(requestArgs);
    logBoard(searchState.board);
    const surfaceArray = getSurfaceArrayAndHoles(searchState.board)[0];
    console.log(surfaceArray);
    return rateSurface(surfaceArray);
  }

  handleEngineLookup(requestArgs) {
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    return JSON.stringify(
      engineLookup(
        searchState,
        params.getParams(),
        params.getParamMods(),
        inputFrameTimeline
      )
    );
  }

  handleEngineLookupTopMovesNoNextBox(requestArgs) {
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    return JSON.stringify(
      engineLookupTopMovesListNoNextBox(
        searchState,
        params.getParams(),
        params.getParamMods(),
        inputFrameTimeline
      )
    );
  }

  handleEngineLookupTopMovesWithNextBox(requestArgs) {
    let [searchState, inputFrameTimeline] = this._parseArguments(requestArgs);

    return JSON.stringify(
      engineLookupTopMovesWithNextBox(
        searchState,
        params.getParams(),
        params.getParamMods(),
        inputFrameTimeline
      )
    );
  }
}
