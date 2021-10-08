import { getBestMove, getSearchStateAfter, getSortedMoveList } from "./main";
import { getPossibleMoves } from "./move_search";
import { IS_DROUGHT_MODE, SHOULD_PUSHDOWN } from "./params";
import { getPieceProbability } from "./piece_rng";
import {
  formatPossibility,
  GetGravity,
  POSSIBLE_NEXT_PIECES,
  shouldPerformInputsThisFrame,
} from "./utils";

const child_process = require("child_process");

const NUM_THREADS = 7;
const THREAD_ASSIGNMENT = {
  O: 0,
  I: 6,
  S: 1,
  Z: 2,
  L: 3,
  J: 4,
  T: 5,
};

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
  phantomPlacements: Array<PhantomPlacement>;
  inputFrameTimeline: string;
  aiParams: InitialAiParams;
  lastSeenPiece: PieceId;

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
    // Helper variables only used with finesse
    this.phantomPlacements = null;
    this.inputFrameTimeline = null;
    this.aiParams = null;
    this.lastSeenPiece = null;

    this._onMessage = this._onMessage.bind(this);
    this._calculatePhantomPlacements = this._calculatePhantomPlacements.bind(
      this
    );
    this._compileResponseFinesse = this._compileResponseFinesse.bind(this);
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
    onPartialResultCallback: Function,
    onResultCallback: Function
  ) {
    console.time("FINESSE PRECOMPUTE");
    this.onResultCallback = onResultCallback;
    this.results = {};
    this.pendingResults = POSSIBLE_NEXT_PIECES.length;
    this.inputFrameTimeline = inputFrameTimeline;
    this.aiParams = initialAiParams;
    this.lastSeenPiece = searchState.currentPieceId;

    // Get a backup placement, in case computation is slow
    const [possibleMoves, _] = getSortedMoveList(
      searchState,
      shouldLog,
      initialAiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 0
    );
    this.defaultPlacement = possibleMoves[0] || null;
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

    // Ping all the workers to start evaluating the next piece values
    console.time("WORKER PHASE");
    for (let i = 0; i < POSSIBLE_NEXT_PIECES.length; i++) {
      const nextPieceId = POSSIBLE_NEXT_PIECES[i];

      const argsData: WorkerDataArgs = {
        computationType: "finesse",
        piece: nextPieceId,
        newSearchState: { ...searchState, nextPieceId },
        initialAiParams,
        paramMods,
        inputFrameTimeline,
      };

      this.workers[THREAD_ASSIGNMENT[nextPieceId]].send(argsData);
    }

    // Calculate all the possible phantom placements (on main thread since it's not doing anything)
    this._calculatePhantomPlacements(
      searchState,
      possibleMoves,
      inputFrameTimeline
    );
    this._precompileAdjustmentMoves();
  }

  precompute(
    searchState: SearchState,
    shouldLog: boolean,
    initialAiParams: InitialAiParams,
    paramMods: ParamMods,
    inputFrameTimeline: string,
    onPartialResultCallback: Function,
    onResultCallback: Function
  ) {
    console.time("PRECOMPUTE");
    this.onResultCallback = onResultCallback;
    this.results = {};
    this.pendingResults = POSSIBLE_NEXT_PIECES.length;

    // Get initial NNB placement
    if (searchState.reactionTime === 0) {
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
      searchState.reactionTime > 0
        ? predictSearchStateAtAdjustmentTime(
            searchState,
            this.defaultPlacement.inputSequence,
            inputFrameTimeline
          )
        : searchState;

    for (let i = 0; i < POSSIBLE_NEXT_PIECES.length; i++) {
      const nextPieceId = POSSIBLE_NEXT_PIECES[i];

      const argsData: WorkerDataArgs = {
        computationType: "standard",
        piece: nextPieceId,
        newSearchState: { ...newSearchState, nextPieceId },
        initialAiParams,
        paramMods,
        inputFrameTimeline,
      };
      this.workers[THREAD_ASSIGNMENT[nextPieceId]].send(argsData);
    }
  }

  _calculatePhantomPlacements(
    initialSearchState: SearchState,
    possibleMoves: Array<PossibilityChain>,
    inputFrameTimeline: string
  ) {
    if (initialSearchState.reactionTime === 0) {
      this.phantomPlacements = [
        {
          inputSequence: "",
          initialPlacement: null,
          adjustmentSearchState: initialSearchState,
        },
      ];
      return;
    }

    const seenInputSequences = new Set();
    const phantomPlacements: Array<PhantomPlacement> = [];

    // Sort the possible moves by minimum number of inputs
    possibleMoves.sort(
      (a, b) => countInputs(a.placement) - countInputs(b.placement)
    );

    // Add a new phantom placement if it doesn't overlap an existing one
    for (const possibility of possibleMoves) {
      const newInputSequence = possibility.inputSequence.substr(
        0,
        initialSearchState.reactionTime
      );
      if (!seenInputSequences.has(newInputSequence)) {
        // Predict the state at adjustment time and register the phantom placement
        const adjSearchState = predictSearchStateAtAdjustmentTime(
          initialSearchState,
          newInputSequence,
          inputFrameTimeline
        );

        // Add a new phantom placement
        phantomPlacements.push({
          inputSequence: newInputSequence,
          initialPlacement: possibility,
          adjustmentSearchState: adjSearchState,
        });
        seenInputSequences.add(newInputSequence);
      }
    }
    this.phantomPlacements = phantomPlacements;
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
        this.results[message.piece] = message.result;
        this.pendingResults--;
        // If all results are in, compile them and send back to parent
        if (this.pendingResults == 0) {
          this._compileResponse();
        }
        break;

      case "result-finesse":
        // Save the partial result
        this.results[message.piece] = message.result;
        this.pendingResults--;
        // If all results are in, compile them and send back to parent
        if (this.pendingResults == 0) {
          console.timeEnd("WORKER PHASE");
          this._compileResponseFinesse();
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

  _precompileAdjustmentMoves() {
    console.time("Get adjustment moves");
    for (const phantomPlacement of this.phantomPlacements) {
      // If it's already done a tuck or spin, it can't do any more inputs
      if (
        phantomPlacement.initialPlacement &&
        phantomPlacement.initialPlacement.inputCost !== 0 &&
        !hasInputs(
          phantomPlacement.initialPlacement.inputSequence.slice(
            phantomPlacement.adjustmentSearchState.reactionTime
          )
        )
      ) {
        console.log(
          "NO ADJUSTMENTS, already did tuck",
          phantomPlacement.initialPlacement.inputSequence
        );
        phantomPlacement.possibleAdjustmentsLookup = [];
        continue;
      }

      // Calculate the possible adjustments from the intermediate state
      const s = phantomPlacement.adjustmentSearchState;
      let possibleAdjs = getPossibleMoves(
        s.board,
        s.currentPieceId,
        s.level,
        s.existingXOffset,
        s.existingYOffset,
        s.framesAlreadyElapsed,
        this.inputFrameTimeline,
        s.existingRotation,
        s.canFirstFrameShift,
        /* shouldLog= */ false
      );

      phantomPlacement.possibleAdjustmentsLookup = possibleAdjs;
    }
    console.timeEnd("Get adjustment moves");
    console.log("DONE PRECOMPILE ADJ");
  }

  _compileResponseFinesse() {
    console.log("STARTING COLLAPSE");
    console.time("COLLAPSE");
    let overallResponse: string = formatPrecomputeResult(
      {},
      this.defaultPlacement
    );

    let bestPhantomPlacementValue = Number.MIN_SAFE_INTEGER;
    for (const phantomPlacement of this.phantomPlacements) {
      let totalValue = 0;
      const responseObj = {};
      for (const pieceId of POSSIBLE_NEXT_PIECES) {
        // Figure out what adjustment you'd do for that piece
        let maxValue = phantomPlacement.initialPlacement
          ? this.results[pieceId][
              phantomPlacement.initialPlacement.lockPositionEncoded
            ]
          : Number.MIN_SAFE_INTEGER;
        let maxPossibility: PossibilityChain = null;
        for (const adjPossibility of phantomPlacement.possibleAdjustmentsLookup) {
          // Combine the input cost with the placement value
          const value =
            getAdjustmentInputCost(adjPossibility) +
            this.results[pieceId][adjPossibility.lockPositionEncoded];
          if (
            !this.results[pieceId].hasOwnProperty(
              adjPossibility.lockPositionEncoded
            )
          ) {
            continue; // TEMPORARY FIX TO STOP CRASHING
            console.log({
              ...phantomPlacement.initialPlacement,
              boardAfter: null,
            });
            console.log({ ...adjPossibility, boardAfter: null });
            console.log(pieceId);
            console.log(this.results[pieceId]);
            for (const piece of POSSIBLE_NEXT_PIECES) {
              console.log(piece);
              if (!this.results[piece]) {
                throw new Error("No results for piece:" + piece);
              }
              console.log(
                piece + " " + Object.keys(this.results[piece]).length
              );
            }
            throw new Error(
              "Unknown lock value: " + adjPossibility.lockPositionEncoded
            );
          }
          // Check if this is the best adjustment
          if (value >= maxValue) {
            maxValue = value;
            maxPossibility = {
              ...adjPossibility,
              searchStateAfterMove: getSearchStateAfter(
                phantomPlacement.adjustmentSearchState,
                adjPossibility
              ),
              totalValue: null, // Not used, only converted types so that searchStateAfter property exists
            };
          }
        }

        // Save the adjustment you'd make if this ends up being the highest
        responseObj[pieceId] = maxPossibility;
        totalValue +=
          maxValue *
          getPieceProbability(this.lastSeenPiece, pieceId, IS_DROUGHT_MODE);
      }

      // Check if this is the new best phantom placement
      const phantomPlacementValue = totalValue / 7;
      if (phantomPlacementValue > bestPhantomPlacementValue) {
        overallResponse = formatPrecomputeResult(
          responseObj,
          phantomPlacement.initialPlacement
        );
        bestPhantomPlacementValue = phantomPlacementValue;
      }
    }

    console.timeEnd("COLLAPSE");
    console.timeEnd("FINESSE PRECOMPUTE");
    if (this.onResultCallback === null) {
      throw new Error("No result callback provided");
    }
    this.onResultCallback(overallResponse);
  }

  // End of class
}

function formatPrecomputeResult(results, defaultPlacement) {
  const defaultFormatted = formatPossibility(
    defaultPlacement,
    /* pushDown= */ SHOULD_PUSHDOWN
  );
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
      resultString += `\n${piece}:${formatPossibility(
        results[piece],
        /* pushDown= */ SHOULD_PUSHDOWN
      )}`;
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

// Check if an input string contains a non-zero number of inputs
function hasInputs(inputSequence: string) {
  for (const inputChar of inputSequence) {
    if (isAnyOf(inputChar, "EFIGLRAB")) {
      return true;
    }
  }
  return false;
}

export function countInputs(placement) {
  if (placement[0] == 3) {
    return 1 + Math.abs(placement[1]);
  }
  return placement[0] + Math.abs(placement[1]);
}

export function getAdjustmentInputCost(possibility: Possibility) {
  const SPINTUCK_COST = -0.3;
  const SPIN_COST = -0.2;
  const TUCK_COST = -0.1;
  const INPUT_COST_LOOKUP = {
    E: SPINTUCK_COST,
    F: SPINTUCK_COST,
    I: SPINTUCK_COST,
    G: SPINTUCK_COST,
    L: TUCK_COST,
    R: TUCK_COST,
    A: SPIN_COST,
    B: SPIN_COST,
  };
  let adjCost = 0;
  for (const inputChar of possibility.inputSequence) {
    adjCost += INPUT_COST_LOOKUP[inputChar] || 0;
  }
  return adjCost + possibility.inputCost;
}

export function predictSearchStateAtAdjustmentTime(
  initialState: SearchState,
  inputSequence: string,
  inputFrameTimeline: string
) {
  let inputsPossibleByAdjTime = 0;
  let inputsUsedByAdjTime = 0;
  let offsetXAtAdjustmentTime = 0;
  let rotationAtAdjustmentTime = 0;
  let totalActiveFrames = 0;

  // Loop through the frames until adjustment time
  for (let i = 0; i < initialState.reactionTime; i++) {
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

    // Check for hitting the stack
    if (isAnyOf(thisFrameStr, "*^")) {
      break;
    }

    // Track inputs used
    if (thisFrameStr !== ".") {
      inputsUsedByAdjTime++;
    }

    totalActiveFrames++;
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
    totalActiveFrames / GetGravity(initialState.level)
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
    framesAlreadyElapsed: initialState.reactionTime,
    reactionTime: initialState.reactionTime,
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
        reactionTime: 0,
        existingXOffset: 0,
        existingYOffset: 0,
        existingRotation: 0,
        canFirstFrameShift: false,
      },
      "E....E...L...L",
      "X....X...X...X"
    )
  );
}
