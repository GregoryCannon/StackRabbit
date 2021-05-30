import { getBestMove } from "./main";
import {
  formatPossibility,
  GetGravity,
  POSSIBLE_NEXT_PIECES,
  shouldPerformInputsThisFrame,
} from "./utils";

const child_process = require("child_process");

const NUM_THREADS = 7;

/**
 * This class is involved with precomputing adjustments for all possible next pieces, and choosing
 * the initial placement based on the ability to reach those adjustments.
 * */
export class PreComputeManager {
  workers: any[];
  pendingResults: number;
  workersStillLoading: number;
  onResultCallback: Function;
  onReadyCallback: Function;
  results: {};
  defaultPlacement: PossibilityChain;

  constructor() {
    this.workers = [];
    this.pendingResults = 0;
    this.workersStillLoading = 0;
    // Callbacks to notify parent
    this.onResultCallback = null;
    this.onReadyCallback = null;
    // The results to calculate
    this.defaultPlacement = null;
    this.results = {};

    this._onMessage = this._onMessage.bind(this);
  }

  initialize(callback) {
    this.onReadyCallback = callback;
    this.workersStillLoading = NUM_THREADS;

    // Create the worker threads
    for (let i = 0; i < NUM_THREADS; i++) {
      const newWorker = child_process.fork("built/src/server/worker_thread.js");
      newWorker.addListener("message", this._onMessage);
      this.workers.push(newWorker);
    }
  }

  finessePrecompute(
    searchState: SearchState,
    shouldLog: boolean,
    initialAiParams: InitialAiParams,
    paramMods: ParamMods,
    inputFrameTimeline: string,
    reactionTimeFrames: number,
    onPartialResultCallback: Function,
    onResultCallback: Function
  ) {
    console.time("FINESSE PRECOMPUTE");
    // Get a backup placement, in case computation is slow
    this.defaultPlacement = getBestMove(
      searchState,
      shouldLog,
      initialAiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 1
    );
    if (this.defaultPlacement === null) {
      onResultCallback("No legal moves");
      return;
    }
    // Send a response with just the default placement in case the other computation doesn't finish
    const formattedResult = formatPrecomputeResult(null, this.defaultPlacement);
    console.log(
      "Saving partial result",
      formatPossibility(this.defaultPlacement)
    );
    onPartialResultCallback(formattedResult);
  }

  precompute(
    searchState: SearchState,
    shouldLog: boolean,
    initialAiParams: InitialAiParams,
    paramMods: ParamMods,
    inputFrameTimeline: string,
    reactionTimeFrames: number,
    onPartialResultCallback: Function,
    onResultCallback: Function
  ) {
    console.time("PRECOMPUTE");
    this.onResultCallback = onResultCallback;
    this.results = {};
    this.pendingResults = NUM_THREADS;

    // Get initial NNB placement
    if (reactionTimeFrames === 0) {
      this.defaultPlacement = null;
    } else {
      this.defaultPlacement = getBestMove(
        searchState,
        shouldLog,
        initialAiParams,
        paramMods,
        inputFrameTimeline,
        /* searchDepth= */ 1,
        /* hypotheticalSearchDepth= */ 1
      );
      if (this.defaultPlacement === null) {
        onResultCallback("No legal moves");
        return;
      }
      // Send a response with just the default placement in case the other computation doesn't finish
      const formattedResult = formatPrecomputeResult({}, this.defaultPlacement);
      console.log(
        "Saving partial result",
        formatPossibility(this.defaultPlacement)
      );
      onPartialResultCallback(formattedResult);
    }

    // Ping the worker threads to compute all possible adjustments
    const newSearchState =
      reactionTimeFrames > 0
        ? predictSearchStateAtAdjustmentTime(
            searchState,
            this.defaultPlacement.inputSequence,
            inputFrameTimeline,
            reactionTimeFrames
          )
        : searchState;

    for (let i = 0; i < POSSIBLE_NEXT_PIECES.length; i++) {
      const nextPieceId = POSSIBLE_NEXT_PIECES[i];

      const argsData: WorkerDataArgs = {
        piece: nextPieceId,
        newSearchState: { ...newSearchState, nextPieceId },
        initialAiParams,
        paramMods,
        inputFrameTimeline,
      };
      this.workers[i].send(argsData);
    }
  }

  _onMessage(message: WorkerResponse) {
    switch (message.type) {
      case "ready":
        // Update the ready worker count, and notify the parent if all threads are ready
        this.workersStillLoading--;
        if (this.workersStillLoading === 0) {
          console.log("Done loading worker threads");
          if (this.onReadyCallback !== null) {
            this.onReadyCallback();
          }
        }
        break;

      case "result":
        // Save the partial result
        this.results[message.piece] = message.result;
        this.pendingResults--;
        // If all results are in, compile them and send back to parent
        if (this.pendingResults == 0) {
          this._compileResponse();
        }
        break;

      default:
        throw new Error(
          "Unrecognized message type received from worker: " + message.type
        );
    }
  }

  _compileResponse() {
    console.timeEnd("PRECOMPUTE");
    const formattedResult = formatPrecomputeResult(
      this.results,
      this.defaultPlacement
    );
    if (this.onResultCallback === null) {
      throw new Error("No result callback provided");
    }
    // console.log(formattedResult);
    this.onResultCallback(formattedResult);
  }
}

function formatPrecomputeResult(results, defaultPlacement) {
  const defaultFormatted = formatPossibility(defaultPlacement);
  let resultString = `Default:${
    defaultPlacement ? formatPossibility(defaultPlacement) : "N/A"
  }`;
  for (const piece of POSSIBLE_NEXT_PIECES) {
    if (results == null) {
      // If we have no next box info, do the default for everything
      resultString += `\n${piece}:${defaultFormatted}`;
    } else if (!results[piece]) {
      // If we have some results but no moves for this piece
      resultString += `\n${piece}:No legal moves`;
    } else {
      // Otherwise, add the real result
      resultString += `\n${piece}:${formatPossibility(results[piece])}`;
    }
  }
  return resultString;
}

function isAnyOf(str, possible) {
  for (const candidate of possible) {
    if (str === candidate) {
      return true;
    }
  }
  return false;
}

function predictSearchStateAtAdjustmentTime(
  initialState: SearchState,
  inputSequence: string,
  inputFrameTimeline: string,
  reactionTimeFrames
) {
  let inputsPossibleByAdjTime = 0;
  let inputsUsedByAdjTime = 0;
  let offsetXAtAdjustmentTime = 0;
  let rotationAtAdjustmentTime = 0;

  // Loop through the frames until adjustment time
  for (let i = 0; i < reactionTimeFrames; i++) {
    if (shouldPerformInputsThisFrame(inputFrameTimeline, i)) {
      inputsPossibleByAdjTime++;
    }

    // Track shifts
    const thisFrameStr = inputSequence[i];
    if (isAnyOf(thisFrameStr, "LEF")) {
      offsetXAtAdjustmentTime--;
    } else if (isAnyOf(thisFrameStr, "RIG")) {
      offsetXAtAdjustmentTime++;
    }

    // Track rotations
    if (isAnyOf(thisFrameStr, "AEI")) {
      rotationAtAdjustmentTime++;
    } else if (isAnyOf(thisFrameStr, "BFG")) {
      rotationAtAdjustmentTime--;
    }

    // Track inputs used
    if (thisFrameStr !== ".") {
      inputsUsedByAdjTime++;
    }
  }

  // Correct the rotation to be in the modulus
  let numOrientations;
  if (initialState.currentPieceId === "O") {
    numOrientations = 1;
  } else if (isAnyOf(initialState.currentPieceId, "ISZ")) {
    numOrientations = 2;
  } else {
    numOrientations = 4;
  }
  rotationAtAdjustmentTime =
    (rotationAtAdjustmentTime + numOrientations) % numOrientations;

  // Calculate the y value from gravity
  let offsetYAtAdjustmentTime = Math.floor(
    reactionTimeFrames / GetGravity(initialState.level)
  );

  return {
    board: initialState.board,
    currentPieceId: initialState.currentPieceId,
    nextPieceId: initialState.nextPieceId,
    level: initialState.level,
    lines: initialState.lines,
    existingXOffset: offsetXAtAdjustmentTime,
    existingYOffset: offsetYAtAdjustmentTime,
    existingRotation: rotationAtAdjustmentTime,
    framesAlreadyElapsed: reactionTimeFrames,
    canFirstFrameShift: inputsUsedByAdjTime < inputsPossibleByAdjTime,
  };
}

function testPrediction() {
  const boardStr =
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
  const board = boardStr
    .match(/.{1,10}/g) // Select groups of 10 characters
    .map((rowSerialized) => rowSerialized.split("").map((x) => parseInt(x)));
  console.log(
    predictSearchStateAtAdjustmentTime(
      {
        board,
        currentPieceId: "J",
        nextPieceId: "I",
        level: 18,
        lines: 0,
        framesAlreadyElapsed: 0,
        existingXOffset: 0,
        existingYOffset: 0,
        existingRotation: 0,
        canFirstFrameShift: false,
      },
      "E....E...L...L",
      "X....X...X...X",
      15
    )
  );
}
