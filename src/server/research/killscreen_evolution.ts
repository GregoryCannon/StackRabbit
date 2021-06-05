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

function registerSuccessor(successors, fromSurface, toSurface) {
  if (!successors.has(fromSurface)) {
    successors.set(fromSurface, new Map());
  }
  // console.log(fromSurface + " -> " + toSurface);
  const existingConnections = successors.get(fromSurface).get(toSurface) || 0;
  successors.get(fromSurface).set(toSurface, existingConnections + 1);
}

function simulateKillscreenTraining2(numIterations) {
  const successors: SuccessorMap = new Map();

  // After every placement, add a new connection to the successor map
  let previousSurface;
  const afterPlacementCallback = (board, isGameOver) => {
    const surface = isGameOver
      ? DEATH_RANK
      : getLeftSurface(board, MAX_4_TAP_HEIGHT + 3);

    if (surface !== previousSurface && surface !== null) {
      registerSuccessor(successors, previousSurface, surface);
      previousSurface = surface;
    }
  };

  for (let i = 0; i < numIterations; i++) {
    // Start of iteration
    console.log(`Iteration ${i + 1} of ${numIterations}`);
    previousSurface = "000";

    // Play out one game
    simulateGame(
      29,
      getEmptyBoard(),
      getParams(),
      getParamMods(),
      INPUT_TIMELINE,
      /* presetSequence= */ null,
      /* shouldAdjust= */ false,
      /* isDig= */ false,
      afterPlacementCallback,
      /* shouldLog= */ false
    );
  }

  console.log(successors);

  const ranks: Object = valueIterate(successors);

  console.log(ranks);
}

// /** Runs trials on the killscreen and logs down how good various surfaces are for the left-most 3 columns. */
// export function simulateKillscreenTraining(numIterations) {
//   const ranks: Map<string, Array<number>> = new Map();

//   let aiParams = addTapInfoToAiParams(getParams(), 29, INPUT_TIMELINE);
//   const MAX_4_TAP_HEIGHT = calculateTapHeight(29, INPUT_TIMELINE, 4);
//   console.log("height: ", MAX_4_TAP_HEIGHT);

//   for (let i = 0; i < numIterations; i++) {
//     console.log(`Iteration ${i + 1} of ${numIterations}`);
//     const history = [];
//     const afterPlacementCallback = (board, isGameOver) => {
//       const leftSurface = getLeftSurface(board, MAX_4_TAP_HEIGHT + 3);
//       if (leftSurface !== null) {
//         history.push(leftSurface);
//       }
//     };

//     // Play out one game
//     simulateGame(
//       29,
//       getEmptyBoard(),
//       getParams(),
//       getParamMods(),
//       INPUT_TIMELINE,
//       /* presetSequence= */ null,
//       /* shouldAdjust= */ false,
//       /* isDig= */ false,
//       afterPlacementCallback,
//       /* shouldLog= */ true
//     );

//     // Process the history
//     console.log(history);
//     if (history.length === 0) {
//       continue;
//     }
//     history.reverse(); // We'll work back to front
//     let lastAdded = null;
//     // Score each surface based on how many clean surfaces succeeded it
//     for (let t = 0; t < history.length; t++) {
//       const surface = history[t];
//       // Don't repeatedly add for long sequences of the same surface
//       if (surface === lastAdded) {
//         continue;
//       }
//       if (!ranks.has(surface)) {
//         ranks.set(surface, []);
//       }
//       // Add the score to the list of scores for that surface
//       ranks.get(surface).push(t);
//       lastAdded = surface;
//     }
//   }
//   console.log(ranks);
//   const avgRanks = {};
//   ranks.forEach((scoreList, surface) => {
//     if (scoreList.length == 0) {
//       return;
//     }
//     let total = scoreList.reduce((a, b) => a + b);
//     const avgScore = total / scoreList.length;
//     avgRanks[surface] = avgScore;
//   });
//   console.log(avgRanks);
// }

simulateKillscreenTraining2(NUM_SAMPLE_GAMES);
