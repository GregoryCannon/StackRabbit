import { rejects } from "assert/strict";
import { formatResponse } from "./app";
import { getBestMove } from "./main";
import { POSSIBLE_NEXT_PIECES } from "./utils";

const child_process = require("child_process");

/**
 * This class is involved with precomputing adjustments for all possible next pieces, and choosing
 * the initial placement based on the ability to reach those adjustments.
 * */

export function precomputePlacementAndAdjustments(
  searchState: SearchState,
  shouldLog: boolean,
  initialAiParams: InitialAiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string,
  searchDepth: number,
  hypotheticalSearchDepth: number
): string {
  const results = {};

  for (const nextPieceId of POSSIBLE_NEXT_PIECES) {
    const newSearchState = { ...searchState, nextPieceId };
    const bestMoveThisPiece = getBestMove(
      newSearchState,
      shouldLog,
      initialAiParams,
      paramMods,
      inputFrameTimeline,
      searchDepth,
      hypotheticalSearchDepth
    );
    results[nextPieceId] = bestMoveThisPiece;
  }

  // Find the average position of all the possible adjustments
  let totalXOffset = 0;
  let totalRotation = 0;
  for (const nextPieceId of POSSIBLE_NEXT_PIECES) {
    const bestMove: PossibilityChain = results[nextPieceId];
    totalXOffset += bestMove.placement[1];
    totalRotation += bestMove.placement[0] == 3 ? -1 : bestMove.placement[0];
  }
  const averageXOffset = Math.round(totalXOffset / POSSIBLE_NEXT_PIECES.length);
  const averageRotation = Math.round(
    totalRotation / POSSIBLE_NEXT_PIECES.length
  );

  // Sort the possibilities by their distance from the average and choose the closest
  const sortedPossibilities: Array<PossibilityChain> = Object.values(
    results
  ).sort(
    (a: PossibilityChain, b: PossibilityChain) =>
      distanceFromAverage(a.placement, averageXOffset, averageRotation) -
      distanceFromAverage(b.placement, averageXOffset, averageRotation)
  ) as Array<PossibilityChain>;
  const defaultPlacement = sortedPossibilities[0];

  // Format the results
  return formatPrecomputeResult(results, defaultPlacement);
  // return response;
  // return new Promise((resolve, reject) => {
  //   resolve(response);
  // });
}

function distanceFromAverage(placement, averageXOffset, averageRotation) {
  const [rotation, xOffset] = placement;
  const numShiftsNeeded = Math.abs(xOffset - averageXOffset);
  const numRotationsNeeded = Math.abs(rotation - averageRotation);
  return Math.max(numShiftsNeeded, numRotationsNeeded);
}

function formatPrecomputeResult(results, defaultPlacement) {
  let resultString = `Default:${formatResponse(defaultPlacement)}`;
  for (const piece of POSSIBLE_NEXT_PIECES) {
    const singlePieceFormatted = formatResponse(results[piece]);
    resultString += `\n${piece}:${singlePieceFormatted}`;
  }
  return resultString;
}
