import { calculateTapHeight, getLeftSurface } from "../board_helper";
import { getEmptyBoard, simulateGame } from "../lite_game_simulator";
import { addTapInfoToAiParams } from "../main";
import { getParamMods, getParams } from "../params";

const INPUT_TIMELINE = "X....X...X...";
const MAX_4_TAP_HEIGHT = calculateTapHeight(29, INPUT_TIMELINE, 4);
const DEATH_RANK = "xxx";

/** Rewards a fully built-out left */
function getReward(surfaceLeft) {
  if (
    surfaceLeft[0] >= MAX_4_TAP_HEIGHT &&
    surfaceLeft[0] >= surfaceLeft[1] &&
    surfaceLeft[0] >= surfaceLeft[2]
  ) {
    return 100;
  }
  return 0;
}

type SuccessorMap = Map<string, Map<string, number>>;
const MAX_VALUE_ITERATIONS = 10000;
const NUM_SAMPLE_GAMES = 10;

function valueIterate(successors: SuccessorMap): Object {
  let values = {};

  // Initialize the starting ranks
  for (const [surface, nextSurfaceFrequencies] of successors.entries()) {
    values[surface] = getReward(surface);
    values[DEATH_RANK] = 0;
  }

  // Converge ranks
  let totalDelta = Number.MAX_SAFE_INTEGER;
  for (
    let iteration = 0;
    iteration < MAX_VALUE_ITERATIONS && totalDelta > 1;
    iteration++
  ) {
    totalDelta = 0;

    const newValues = {};
    newValues[DEATH_RANK] = 0; // The death rank has to be initialized since it's never the source state of any transition

    for (const [surface, nextSurfaceFrequencies] of successors.entries()) {
      const inherentValue = getReward(surface);

      if (inherentValue > 0) {
        // If it's a goal state, use the inherent reward
        newValues[surface] = inherentValue;
      } else {
        // Otherwise get the value from its successors
        let total = 0;
        let numSuccessors = 0;
        for (const [nextSurface, frequency] of nextSurfaceFrequencies) {
          total += frequency * values[nextSurface];
          numSuccessors += frequency;
        }
        newValues[surface] = total / numSuccessors;
      }

      totalDelta += Math.abs(newValues[surface] - values[surface]);
    }

    values = newValues;
    console.log(iteration, totalDelta.toFixed(2));
  }

  return values;
}

function main() {
  const successors = new Map();

  // ....

  const ranks: Object = valueIterate(successors);

  console.log(ranks);
}

main();
