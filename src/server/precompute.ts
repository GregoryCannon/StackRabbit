import { rejects } from "assert/strict";
import { getAiMode } from "./ai_mode_manager";
import { formatResponse } from "./app";
import {
  addTapInfoToAiParams,
  evaluateFirstPlacements,
  getBestMove,
} from "./main";
import { modifyParamsForAiMode } from "./params";
import {
  GetGravity,
  POSSIBLE_NEXT_PIECES,
  shouldPerformInputsThisFrame,
} from "./utils";

const child_process = require("child_process");

/**
 * This class is involved with precomputing adjustments for all possible next pieces, and choosing
 * the initial placement based on the ability to reach those adjustments.
 * */

export function computePlacementAndAdjustments(
  searchState: SearchState,
  shouldLog: boolean,
  initialAiParams: InitialAiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string,
  reactionTimeFrames: number
): string {
  console.time("PRECOMPUTE");
  // Get initial NNB placement
  const defaultPlacement = getBestMove(
    searchState,
    shouldLog,
    initialAiParams,
    paramMods,
    inputFrameTimeline,
    /* searchDepth= */ 1,
    /* hypotheticalSearchDepth= */ 1
  );

  // Get all the possible adjustments
  const results = {};
  for (const nextPieceId of POSSIBLE_NEXT_PIECES) {
    let newSearchState = { ...searchState, nextPieceId };
    newSearchState = predictSearchStateAtAdjustmentTime(
      newSearchState,
      defaultPlacement.inputSequence,
      inputFrameTimeline,
      reactionTimeFrames
    );
    const bestMoveThisPiece = getBestMove(
      newSearchState,
      shouldLog,
      initialAiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 2,
      /* hypotheticalSearchDepth= */ 1
    );
    results[nextPieceId] = bestMoveThisPiece;
  }
  console.timeEnd("PRECOMPUTE");
  return formatPrecomputeResult(results, defaultPlacement);
}

function formatPrecomputeResult(results, defaultPlacement) {
  let resultString = `Default:${formatResponse(defaultPlacement)}`;
  for (const piece of POSSIBLE_NEXT_PIECES) {
    const singlePieceFormatted = formatResponse(results[piece]);
    resultString += `\n${piece}:${singlePieceFormatted}`;
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
  searchState: SearchState,
  inputSequence: string,
  inputFrameTimeline: string,
  reactionTimeFrames
) {
  const { board, currentPieceId, level } = searchState;

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
  if (currentPieceId === "O") {
    numOrientations = 1;
  } else if (isAnyOf(currentPieceId, "ISZ")) {
    numOrientations = 2;
  } else {
    numOrientations = 2;
  }
  rotationAtAdjustmentTime =
    (rotationAtAdjustmentTime + numOrientations) % numOrientations;

  // Calculate the y value from gravity
  let offsetYAtAdjustmentTime = Math.floor(
    reactionTimeFrames / GetGravity(level)
  );

  return {
    board,
    currentPieceId,
    nextPieceId: searchState.nextPieceId,
    level,
    lines: searchState.lines,
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

// export function futureRecomputePlacementAndAdjustments(
//   searchState: SearchState,
//   shouldLog: boolean,
//   initialAiParams: InitialAiParams,
//   paramMods: ParamMods,
//   inputFrameTimeline: string,
//   searchDepth: number,
//   hypotheticalSearchDepth: number
// ): string {
//   const results = {};

//   // Update input objects
//   let aiParams = addTapInfoToAiParams(
//     initialAiParams,
//     searchState.level,
//     inputFrameTimeline
//   );
//   const aiMode = getAiMode(
//     searchState.board,
//     searchState.lines,
//     searchState.level,
//     aiParams
//   );
//   aiParams = modifyParamsForAiMode(aiParams, aiMode, paramMods);

//   for (const nextPieceId of POSSIBLE_NEXT_PIECES) {
//     const newSearchState = { ...searchState, nextPieceId };
//     const bestMoveThisPiece = getBestMove(
//       newSearchState,
//       shouldLog,
//       initialAiParams,
//       paramMods,
//       inputFrameTimeline,
//       searchDepth,
//       hypotheticalSearchDepth
//     );
//     results[nextPieceId] = bestMoveThisPiece;
//   }

//   // Find the average position of all the possible adjustments
//   let totalXOffset = 0;
//   let totalRotation = 0;
//   for (const nextPieceId of POSSIBLE_NEXT_PIECES) {
//     const bestMove: PossibilityChain = results[nextPieceId];
//     totalXOffset += bestMove.placement[1];
//     totalRotation += bestMove.placement[0] == 3 ? -1 : bestMove.placement[0];
//   }
//   const averageXOffset = Math.round(totalXOffset / POSSIBLE_NEXT_PIECES.length);
//   const averageRotation = Math.round(
//     totalRotation / POSSIBLE_NEXT_PIECES.length
//   );

//   // Sort the possibilities by their distance from the average and choose the closest
//   const sortedPossibilities: Array<PossibilityChain> = Object.values(
//     results
//   ).sort(
//     (a: PossibilityChain, b: PossibilityChain) =>
//       distanceFromAverage(a.placement, averageXOffset, averageRotation) -
//       distanceFromAverage(b.placement, averageXOffset, averageRotation)
//   ) as Array<PossibilityChain>;
//   const defaultPlacement = sortedPossibilities[0];

//   // Format the results
//   return formatPrecomputeResult(results, defaultPlacement);
//   // return response;
//   // return new Promise((resolve, reject) => {
//   //   resolve(response);
//   // });
// }

// function distanceFromAverage(placement, averageXOffset, averageRotation) {
//   const [rotation, xOffset] = placement;
//   const numShiftsNeeded = Math.abs(xOffset - averageXOffset);
//   const numRotationsNeeded = Math.abs(rotation - averageRotation);
//   return Math.max(numShiftsNeeded, numRotationsNeeded);
// }
