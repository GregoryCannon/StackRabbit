/**
 * A grid search / gradient descent algorithm for choosing the best parameter set.
 */

const liteGameSimulator = require("./lite_game_simulator");
const { getParams, getParamMods } = require("./params");

// Hyperparameters
const DELTA = 0.1; // Small step used to calculate slope (Represents a % of the existing number)
// const STEP_SIZE = 1; // Size of step taken after determining the slope (As a multiple of delta)
// const NUM_ITERATIONS = 20;
const GAMES_PER_TEST = 100;

let startParams = getParams();
// const KEY_LIST = Object.keys(startParams);
const KEY_LIST = [
  "BUILT_OUT_LEFT_COEF",
  "SURFACE_COEF",
  "AVG_HEIGHT_COEF",
  "SPIRE_HEIGHT_COEF",
];

/**
 * Fitness function: the median score
 * @param {Array of [score, lines, level] subarrays} - simulationResult
 */
function fitnessFunctionMedian(simulationResult) {
  console.log(simulationResult);
  const sortedScores = simulationResult
    .map(([score, lines, level]) => parseInt(score))
    .sort((a, b) => a - b);
  return sortedScores[Math.floor(simulationResult.length / 2)];
}

/**
 * Fitness function: the mean score
 * @param {Array of [score, lines, level] subarrays} - simulationResult
 */
function fitnessFunctionMean(simulationResult) {
  console.log(simulationResult);
  let total = 0;
  for (const [score, lines, level] of simulationResult) {
    total += score;
  }
  return total / GAMES_PER_TEST;
}

/**
 * Runs a simulation suite on a theta value
 * @param {Array<number>} multiplierArray - scaling factors for the DEFAUL_PARAMS values
 */
function testThetaValue(theta) {
  const modifiedParameters = getCustomParams(theta, startParams);
  return fitnessFunctionMean(
    liteGameSimulator.simulateManyGames(
      GAMES_PER_TEST,
      18,
      modifiedParameters,
      getParamMods()
    )
  );
}

function getCustomParams(theta, existingParams) {
  const modifiedParameters = JSON.parse(JSON.stringify(existingParams));
  for (const [index, key] of KEY_LIST.entries()) {
    modifiedParameters[key] = existingParams[key] * theta[index];
  }
  return Object.freeze(modifiedParameters);
}

/**
 * Theta is defined here as an array of scaling factors for the default params.
 * They are listed in the order of the key names, alphabetically.
 */
function getInitialTheta() {
  let multiplierArray = [];
  for (const _ in KEY_LIST) {
    multiplierArray.push(1);
  }
  return multiplierArray;
}

function gridSearch() {
  let theta = getInitialTheta();
  let currentFitness = testThetaValue(theta);
  let fitnessHistory = [currentFitness];

  console.log(`Initial fitness: ${currentFitness}, Initial theta: ${theta}`);

  for (let i = 0; i < KEY_LIST.length; i++) {
    const keyBeingOptimized = KEY_LIST[i];
    console.log(
      `\n\n\n -------- Optimizing ${keyBeingOptimized}, key ${i} of ${KEY_LIST.length} ----------`
    );
    let localMaxFitness = Number.MIN_SAFE_INTEGER;
    let localMaxTheta = null;
    let localMaxDelta = null;

    // Small grid search
    for (const individualDelta of [
      -5 * DELTA,
      -1 * DELTA,
      0,
      DELTA,
      5 * DELTA,
    ]) {
      const testTheta = JSON.parse(JSON.stringify(theta));
      testTheta[i] = 1 + individualDelta;
      console.log(testTheta);

      const testFitness = testThetaValue(testTheta);

      console.log(
        `Individual delta of ${individualDelta} has fitness ${testFitness}`
      );
      if (testFitness > localMaxFitness) {
        localMaxFitness = testFitness;
        localMaxTheta = testTheta;
        localMaxDelta = individualDelta;
      }
    }

    // Update the global search with the best local search option
    console.log(
      `Best individual delta: ${localMaxDelta}, with fitness ${localMaxFitness} and theta:\n${localMaxTheta}`
    );

    // By design, the local max fitness is always >= existing fitness
    currentFitness = localMaxFitness;
    theta = localMaxTheta;
    fitnessHistory.push(currentFitness);
    console.log(`Fitness history: ${fitnessHistory}`);
    console.log("params:", getCustomParams(theta, startParams));
  }

  console.log("\n\n\n\n\nBEST:");
  console.log(theta, "fitness:", currentFitness);
  console.log("params:", getCustomParams(theta, startParams));
}

gridSearch();
