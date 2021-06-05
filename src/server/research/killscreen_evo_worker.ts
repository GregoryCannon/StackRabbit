const process = require("process");
import { calculateTapHeight, getRelativeLeftSurface } from "../board_helper";
import { getEmptyBoard, simulateGame } from "../lite_game_simulator";
import { getParamMods, getParams } from "../params";

type SuccessorMap = Map<string, Map<string, number>>;

const INPUT_TIMELINE = "X....";
const MAX_4_TAP_HEIGHT = calculateTapHeight(29, INPUT_TIMELINE, 4);
const DEATH_RANK = "xxx";

const TRAINING_TIME_MINS = 15;
const MS_PER_MIN = 60000;

let threadId = -1;

/**
 * Adds or increments the count of a given surface transition (e.g. surface A to surface B)
 */
export function registerSuccessor(successors, fromSurface, toSurface) {
  if (!successors.hasOwnProperty(fromSurface)) {
    successors[fromSurface] = {};
  }
  // console.log("\n", fromSurface + " -> " + toSurface);
  const existingConnections = successors[fromSurface][toSurface] || 0;
  successors[fromSurface][toSurface] = existingConnections + 1;
}

/**
 * Runs a series of simulated killscreen games and stores how often each left surface
 * transitions to each other one.
 * This successor map is used to train the surface values using value iteration.
 */
function measureSuccessorsInTestGames(numIterations): Object {
  const successors: Object = {};

  // After every placement, add a new connection to the successor map
  let previousSurface;
  const afterPlacementCallback = (board, isGameOver) => {
    const surface = isGameOver
      ? DEATH_RANK
      : getRelativeLeftSurface(board, MAX_4_TAP_HEIGHT);

    if (surface !== previousSurface && surface !== null) {
      registerSuccessor(successors, previousSurface, surface);
      previousSurface = surface;
    }
  };

  // Note the starting time and keep training until a specified number of minutes after that
  const startTimeMs = Date.now();
  const endTimeMs = startTimeMs + TRAINING_TIME_MINS * MS_PER_MIN;
  for (let i = 0; Date.now() < endTimeMs; i++) {
    // Start of iteration
    console.log(`${threadId}: Iteration ${i + 1}`);
    console.log(
      `${threadId}: ${((Date.now() - startTimeMs) / MS_PER_MIN).toFixed(
        2
      )} minutes elapsed, out of ${TRAINING_TIME_MINS}`
    );
    previousSurface = "000|0";

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

  return successors;
}

/**
 * Listener for messages from the main thread
 */
process.on("message", (message) => {
  threadId = message.threadId;
  process.send({
    type: "result",
    result: measureSuccessorsInTestGames(1),
  });
});
