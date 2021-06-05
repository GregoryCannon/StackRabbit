const process = require("process");
import {
  calculateTapHeight,
  getLeftSurface,
  getRelativeLeftSurface,
} from "../board_helper";
import { getEmptyBoard, simulateGame } from "../lite_game_simulator";
import { getParamMods, getParams } from "../params";

type SuccessorMap = Map<string, Map<string, number>>;

const INPUT_TIMELINE = "X....X...X...";
const MAX_4_TAP_HEIGHT = calculateTapHeight(29, INPUT_TIMELINE, 4);
const DEATH_RANK = "xxx";

const NUM_TRAINING_GAMES = 10;

function registerSuccessor(successors, fromSurface, toSurface) {
  if (!successors.has(fromSurface)) {
    successors.set(fromSurface, new Map());
  }
  console.log(fromSurface + " -> " + toSurface);
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
    console.log(getRelativeLeftSurface(board, MAX_4_TAP_HEIGHT));

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

  return successors;
}

simulateKillscreenTraining2(1);
