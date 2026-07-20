import { getPossibleMoves, getSearchStateAfter } from "./move_search";
import { LOSS_DAS_PENALTY, SHOULD_PUSHDOWN } from "./params";
import { INITIAL_PLACEMENT, PhantomPlacement, PieceId, Possibility, PossibilityChain, SearchState, WorkerDataArgs, WorkerResponse } from "./types";
import {
  formatDefaultPossibility,
  formatPossibility,
  POSSIBLE_NEXT_PIECES,
  toPossibilityChain
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
  onResultCallback: Function | null;
  onReadyCallback: Function | null;
  results: {};
  defaultPlacement: PossibilityChain | null;
  phantomPlacements: Array<PhantomPlacement> | null;
  minSafeDasChargeLookup: Map<string, number>;
  inputFrameTimeline: string | null;
  reactionTime: number | null;
  useDAS: boolean | null;
  lastSeenPiece: PieceId;

  constructor() {
    this.workers = [];
    this.pendingResults = 0;
    this.workersStillLoading = 0;
    // Callbacks to notify parent
    this.onResultCallback = null;
    this.onReadyCallback = null;
    // The results from the worker threads
    this.results = {};
    // Helper variables only used with finesse
    this.phantomPlacements = null;
    this.defaultPlacement = null;
    this.minSafeDasChargeLookup = new Map();
    this.inputFrameTimeline = null;
    this.reactionTime = null;
    this.useDAS = null;
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
    inputFrameTimeline: string,
    onPartialResultCallback: Function,
    onResultCallback: Function
  ) {
    console.time("FINESSE PRECOMPUTE");
    this.onResultCallback = onResultCallback;
    this.results = {};
    this.pendingResults = POSSIBLE_NEXT_PIECES.length;
    this.inputFrameTimeline = inputFrameTimeline;
    this.reactionTime = searchState.reactionTime;
    this.lastSeenPiece = searchState.currentPieceId;
    this.useDAS = searchState.dasCharge !== -1
    this.minSafeDasChargeLookup = new Map();

    const possibleMoves = getPossibleMoves(
      searchState.board,
      searchState.currentPieceId,
      searchState.level,
      searchState.existingXOffset,
      searchState.existingYOffset,
      searchState.framesAlreadyElapsed,
      inputFrameTimeline,
      searchState.existingRotation,
      INITIAL_PLACEMENT,
      searchState.dasCharge,
      searchState.reactionTime
    ).map((x) => toPossibilityChain(x, searchState));

    const defaultPlacement = possibleMoves[0] || null;
    if (defaultPlacement === null) {
      onResultCallback("No legal moves A");
      return;
    }

    // Send a response with just the default placement in case the other computation doesn't finish
    const formattedResult = formatPrecomputeResult({}, defaultPlacement, 999);
    console.log("Saving partial result", defaultPlacement.placement);
    onPartialResultCallback(formattedResult);

    // Ping all the workers to start evaluating the next piece values
    console.time("WORKER PHASE");
    for (let i = 0; i < POSSIBLE_NEXT_PIECES.length; i++) {
      const nextPieceId = POSSIBLE_NEXT_PIECES[i];

      const argsData: WorkerDataArgs = {
        piece: nextPieceId,
        newSearchState: { ...searchState, nextPieceId },
        inputFrameTimeline,
      };

      this.workers[THREAD_ASSIGNMENT[nextPieceId]].send(argsData);
    }

    // Calculate all the possible phantom placements (on main thread since it's not doing anything)
    this._calculatePhantomPlacements(
      searchState,
      possibleMoves,
    );
    this._precompileAdjustmentMoves();
  }

  _calculatePhantomPlacements(
    initialSearchState: SearchState,
    possibleMoves: Array<Possibility>,
  ) {
    if (initialSearchState.reactionTime === 0) {
      this.phantomPlacements = [
        {
          inputSequence: "",
          initialPlacement: null,
          adjustmentSearchState: initialSearchState,
          possibleAdjustments: []
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

    if (this.useDAS) {
      console.time("SAFEDAS");
      // Calculate minimum safe DAS charges for each possible lock location
      for (const possibility of possibleMoves) {
        const ssa = getSearchStateAfter(initialSearchState, possibility)
        ssa.currentPieceId = "T" // Generally representative of the hardest placements

        const countMovesAtDasCharge = (dasCharge: number) => {
          return getPossibleMoves(ssa.board, ssa.currentPieceId, ssa.level, 0, 0, 0, this.inputFrameTimeline, 0, INITIAL_PLACEMENT, dasCharge).length
        }

        let minSafeDasCharge = 15;
        const baseline = countMovesAtDasCharge(15);
        const increment = (ssa.level == 18) ? 3 : 2 // The true values tend to increment in multiples of the gravity, so we can increment by that while searching.

        if (countMovesAtDasCharge(0) == baseline) {
          minSafeDasCharge = 0;
        } else {
          for (let dasCharge = 15 - increment + 1; dasCharge >= 0; dasCharge -= increment) {
            if (countMovesAtDasCharge(dasCharge) == baseline) {
              minSafeDasCharge = dasCharge
            } else {
              break;
            }
          }
        }

        this.minSafeDasChargeLookup.set(possibility.lockPositionEncoded, minSafeDasCharge);
      }
      console.timeEnd("SAFEDAS");
    }


    // const baseline = countMovesAtDasCharge(15);

    //   if (countMovesAtDasCharge(0) == baseline) {
    //     this.minSafeDasChargeLookup.set(possibility.lockPositionEncoded, 0);
    //   } else {
    //     // Binary search
    //     let min = 1;
    //     let max = 16;
    //     while (min < max) {
    //       const median = Math.round((min + max) / 2)
    //       if (countMovesAtDasCharge(median) == baseline) {
    //         max = median
    //       } else {
    //         min = median + 1
    //       }
    //     }

    //     this.minSafeDasChargeLookup.set(possibility.lockPositionEncoded, max);
    //   }

    // Add a new phantom placement if it doesn't overlap an existing one
    for (const possibility of possibleMoves) {
      const newInputSequence = possibility.inputSequence.substr(
        0,
        initialSearchState.reactionTime
      );
      if (!seenInputSequences.has(newInputSequence)) {
        let adjSearchState: SearchState | null = null;
        if (possibility.adjTimeSimState) {
          // Convert the adjSimState to a SearchState
          const at = possibility.adjTimeSimState
          const initialX = 3;
          const initialY = (initialSearchState.currentPieceId == "I" ? -2 : -1);
          adjSearchState = {
            ...initialSearchState,
            framesAlreadyElapsed: at.frameIndex,
            existingXOffset: at.x - initialX,
            existingYOffset: at.y - initialY,
            existingRotation: at.rotationIndex,
            dasCharge: at.dasCharge,
            adjustmentState: at.adjustmentState
          }
        }

        // Add a new phantom placement
        phantomPlacements.push({
          inputSequence: newInputSequence,
          initialPlacement: toPossibilityChain(possibility, initialSearchState),
          adjustmentSearchState: adjSearchState,
          possibleAdjustments: []
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
        phantomPlacement.possibleAdjustments = [];
        console.log("Already did tuck, no adjustments allowed")
        continue;
      }

      // If the piece locks in before reaction time, there will be no adjustmentSearchState saved
      if (!phantomPlacement.adjustmentSearchState) {
        console.log("No adj search state, no adjustments possible")
        phantomPlacement.possibleAdjustments = [];
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
        s.adjustmentState,
        s.dasCharge,
      );
      phantomPlacement.possibleAdjustments = possibleAdjs;
    }
    console.timeEnd("Get adjustment moves");
    // console.log("DONE PRECOMPILE ADJ");
  }

  _compileResponseFinesse() {
    // console.log("STARTING COLLAPSE");

    console.log(this.minSafeDasChargeLookup);

    let overallResponse: string = "No legal moves B";

    console.time("COLLAPSE");
    let bestPhantomPlacementValue = Number.MIN_SAFE_INTEGER;
    for (const phantomPlacement of this.phantomPlacements) {
      let totalValue = 0;
      const responseObj = {};
      for (const pieceId of POSSIBLE_NEXT_PIECES) {
        // Figure out what adjustment you'd do for that piece
        let maxValue = Number.MIN_SAFE_INTEGER;
        let maxPossibility: PossibilityChain = null;

        // If there's no possible adjustments (piece locks too quickly), just consider the default placement
        if (phantomPlacement.possibleAdjustments.length == 0) {
          maxValue = this.results[pieceId][phantomPlacement.initialPlacement.lockPositionEncoded]
            + phantomPlacement.initialPlacement.inputCost
          maxPossibility = phantomPlacement.initialPlacement
        }

        else {
          // Otherwise, check all the adjustments to get the max value from this phantom placement
          for (const adjPossibility of phantomPlacement.possibleAdjustments) {
            // Combine the input cost with the placement value
            // console.log("basline", phantomPlacement.initialPlacement.placement, phantomPlacement.initialPlacement.adjTimeSimState, phantomPlacement.initialPlacement.inputSequence)
            const value =
              this._getAdjustmentInputCost(adjPossibility, phantomPlacement.adjustmentSearchState, pieceId)
              + this.results[pieceId][adjPossibility.lockPositionEncoded];
            if (
              !this.results[pieceId].hasOwnProperty(
                adjPossibility.lockPositionEncoded
              )
            ) {
              continue;
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
        }

        // Save the adjustment you'd make if this ends up being the highest
        responseObj[pieceId] = maxPossibility;
        totalValue +=
          maxValue * getPieceProbability(this.lastSeenPiece, pieceId);
      }

      // Check if this is the new best phantom placement
      const phantomPlacementValue = totalValue / 7;
      if (phantomPlacementValue > bestPhantomPlacementValue) {
        overallResponse = formatPrecomputeResult(
          responseObj,
          phantomPlacement.initialPlacement,
          this.reactionTime
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

  _getAdjustmentInputCost(possibility: Possibility, adjSearchState: SearchState, nextPieceId: PieceId) {
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
      // These are just rotations while holding the DAS buttons
      e: SPIN_COST,
      f: SPIN_COST,
      i: SPIN_COST,
      g: SPIN_COST,
    };
    let adjCost = 0;
    for (const inputChar of possibility.inputSequence) {
      adjCost += INPUT_COST_LOOKUP[inputChar] || 0;
    }

    // console.log(possibility.placement, possibility.inputSequence);
    // Penalize losing DAS, unless the partial (or no) DAS charge is enough for the next piece to reach its full range anyway.
    let dasCost = 0
    if (this.useDAS && possibility.dasChargeAfter !== undefined) {
      const minSafeDasCharge = this.minSafeDasChargeLookup.get(possibility.lockPositionEncoded) || 15
      if (minSafeDasCharge === undefined) {
        throw new Error("Failed to find value in min safe das charge: " + possibility.lockPositionEncoded)
      }
      if (possibility.dasChargeAfter < minSafeDasCharge) {
        dasCost -= LOSS_DAS_PENALTY;
      }
    }

    // console.log(possibility.placement, "\t", possibility.dasChargeAfter, "\t", dasCost);
    return adjCost + possibility.inputCost + dasCost;
  }

  // End of class
}

function formatPrecomputeResult(results, defaultPlacement: PossibilityChain, reactionTime: number) {
  let resultString = `Default:${defaultPlacement
    ? formatDefaultPossibility(defaultPlacement, reactionTime)
    : "N/A (0 reaction time)"
    }`;
  for (const piece of POSSIBLE_NEXT_PIECES) {
    if (results == null) {
      throw new Error("Results were null");
    } else if (!results[piece]) {
      // If we have some results but no moves for this piece
      resultString += `\n${piece}:No legal moves C`;
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

/*
The PRNG of NES Tetris is pretty weird. For example, S bursts are twice as likely as Z bursts, for no good reason.
Thanks to Adrien Wu and HydrantDude for providing the following lookup tables. They represent the odds of getting 
each piece, given a particular previous piece. The indexing goes Array[firstPiece][secondPiece]. 
*/
function getPieceProbability(current: PieceId, next: PieceId) {
  const PIECE_INDICES = {
    T: 0,
    J: 1,
    Z: 2,
    O: 3,
    S: 4,
    L: 5,
    I: 6,
  };
  const TRANSITIONS = [
    [2, 10, 12, 10, 10, 10, 10],
    [12, 2, 10, 10, 10, 10, 10],
    [10, 12, 2, 10, 10, 10, 10],
    [10, 10, 10, 4, 10, 10, 10],
    [10, 10, 10, 10, 4, 10, 10],
    [12, 10, 10, 10, 10, 2, 10],
    [10, 10, 10, 10, 12, 10, 2],
  ];
  const index1 = PIECE_INDICES[current];
  const index2 = PIECE_INDICES[next];
  return TRANSITIONS[index1][index2] / 64;
}
