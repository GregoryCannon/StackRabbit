const process = require("process");
import { getRelativeLeftSurface } from "../board_helper";
import {
  DIG_LINE_CAP,
  getEmptyBoard,
  simulateGame,
} from "../lite_game_simulator";
import { getParamMods, getParams } from "../params";
import { generateDigPracticeBoard } from "../utils";
import { DEATH_RANK } from "./killscreen_training";
import {
  OPENER_TEST_BOARD,
  SIM_INPUT_TIMELINE,
  SIM_MAX_4_TAP_HEIGHT,
  TRAINING_TIME_MINS,
} from "./simulation_testing";

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
function measureSuccessorsInTestGames(): Object {
  const successors: Object = {};

  // After every placement, add a new connection to the successor map
  let previousSurface;
  const afterPlacementCallback = (board, isGameOver) => {
    const surface = isGameOver
      ? DEATH_RANK
      : getRelativeLeftSurface(board, SIM_MAX_4_TAP_HEIGHT);

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
      SIM_INPUT_TIMELINE,
      /* presetSequence= */ null,
      /* shouldAdjust= */ false,
      /* isDig= */ false,
      /* maxLines= */ 230,
      afterPlacementCallback,
      /* shouldLog= */ false
    );
  }

  return successors;
}

/**
 * Runs a series of simulated games and tracks all of the scores
 */
function measureAverageScore() {
  // Note the starting time and keep training until a specified number of minutes after that
  const startTimeMs = Date.now();
  const endTimeMs = startTimeMs + TRAINING_TIME_MINS * MS_PER_MIN;

  let scores = [];
  for (let i = 0; Date.now() < endTimeMs; i++) {
    // Start of iteration
    console.log(`${threadId}: Iteration ${i + 1}`);
    console.log(
      `${threadId}: ${((Date.now() - startTimeMs) / MS_PER_MIN).toFixed(
        2
      )} minutes elapsed, out of ${TRAINING_TIME_MINS}`
    );

    // Play out one game
    const [score, lines, level] = simulateGame(
      18,
      getEmptyBoard(),
      getParams(),
      getParamMods(),
      SIM_INPUT_TIMELINE,
      /* presetSequence= */ null,
      /* shouldAdjust= */ false,
      /* isDig= */ false,
      /* maxLines= */ 230,
      null,
      /* shouldLog= */ false
    );
    scores.push([score, lines, level]);
  }

  scores.forEach((x) => console.log(x));
  return scores;
}

export function simulateDigPractice() {
  // Note the starting time and keep training until a specified number of minutes after that
  const startTimeMs = Date.now();
  const endTimeMs = startTimeMs + TRAINING_TIME_MINS * MS_PER_MIN;

  let results = [];
  for (let i = 0; Date.now() < endTimeMs; i++) {
    // Start of iteration
    console.log(`${threadId}: Iteration ${i + 1}`);
    console.log(
      `${threadId}: ${((Date.now() - startTimeMs) / MS_PER_MIN).toFixed(
        2
      )} minutes elapsed, out of ${TRAINING_TIME_MINS}`
    );

    // Simulate a game with a dirty starting board and capped lines
    results.push(
      simulateGame(
        18,
        generateDigPracticeBoard(5, 6),
        getParams(),
        getParamMods(),
        SIM_INPUT_TIMELINE,
        /* predefinedPieceSequence= */ null,
        /* shouldAdjust= */ false,
        /* isDig= */ true,
        /* maxLines= */ DIG_LINE_CAP,
        /* onPlacementCallback= */ null,
        /* shouldLog= */ i == 0
      )
    );
  }
  // console.log(results);
  return results;
}

export function testOpener(openerBoard) {
  // Note the starting time and keep training until a specified number of minutes after that
  const startTimeMs = Date.now();
  const endTimeMs = startTimeMs + TRAINING_TIME_MINS * MS_PER_MIN;

  let results = [];
  for (let i = 0; Date.now() < endTimeMs; i++) {
    // Start of iteration
    console.log(`${threadId}: Iteration ${i + 1}`);
    console.log(
      `${threadId}: ${((Date.now() - startTimeMs) / MS_PER_MIN).toFixed(
        2
      )} minutes elapsed, out of ${TRAINING_TIME_MINS}`
    );

    // Simulate a game with a dirty starting board and capped lines
    results.push(
      simulateGame(
        18,
        openerBoard,
        getParams(),
        getParamMods(),
        SIM_INPUT_TIMELINE,
        /* predefinedPieceSequence= */ null,
        /* shouldAdjust= */ false,
        /* isDig= */ false,
        /* maxLines= */ 5,
        /* onPlacementCallback= */ null,
        /* shouldLog= */ i == 0
      )
    );
  }
  // console.log(results);
  return results;
}

/**
 * Listener for messages from the main thread
 */
process.on("message", (message) => {
  threadId = message.threadId;
  let result;
  if (message.type === "successors") {
    result = measureSuccessorsInTestGames();
  } else if (message.type === "average") {
    result = measureAverageScore();
  } else if (message.type === "dig") {
    result = simulateDigPractice();
  } else if (message.type === "opener") {
    result = testOpener(OPENER_TEST_BOARD);
  }
  process.send({
    type: "result",
    result,
  });
});

// measureAverageScore();
