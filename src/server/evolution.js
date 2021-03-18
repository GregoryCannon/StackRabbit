const liteGameSimulator = require("./lite_game_simulator");
const { DEFAULT_PARAMS, getParams } = require("./params");
const KEY_LIST = Object.keys(DEFAULT_PARAMS).sort();
/**
 * A grid search / gradient descent algorithm for choosing the best parameter set.
 */

// Hyperparameters
const DELTA = 0.07; // Small step used to calculate slope (Represents a % of the existing number)
const STEP_SIZE = 1; // Size of step taken after determining the slope (As a multiple of delta)
const NUM_ITERATIONS = 20;
const GAMES_PER_TEST = 100;

let startParams = DEFAULT_PARAMS;

/**
 * Fitness function: the median score
 * @param {Array of [score, lines, level] subarrays} - simulationResult
 */
function fitnessFunction(simulationResult) {
  console.log(simulationResult);
  const sortedScores = simulationResult
    .map(([score, lines, level]) => parseInt(score))
    .sort((a, b) => a - b);
  return sortedScores[Math.floor(simulationResult.length / 2)];
}

/**
 * Runs a simulation suite on a theta value
 * @param {Array<number>} multiplierArray - scaling factors for the DEFAUL_PARAMS values
 */
function testThetaValue(theta) {
  const modifiedParameters = getCustomParams(theta, startParams);
  return fitnessFunction(
    liteGameSimulator.simulateManyGames(GAMES_PER_TEST, 18, modifiedParameters)
  );
}

function getCustomParams(theta, existingParams) {
  const modifiedParameters = {};
  for (const [index, key] of KEY_LIST.entries()) {
    modifiedParameters[key] = existingParams[key] * theta[index];
  }
  return Object.freeze(modifiedParameters);
}

/**
 * Theta is defined here as an array of scaling factors for the DEFAULT_PARAMS.
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
    let localMaxFitness = -9999;
    let localMaxTheta = null;
    let localMaxDelta = null;

    // Small grid search
    for (const individualDelta of [
      -2 * DELTA,
      -1 * DELTA,
      0,
      DELTA,
      2 * DELTA,
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
  }

  console.log("\n\n\n\n\nBEST:");
  console.log(theta, "fitness:", currentFitness);
  console.log("params:", getCustomParams(theta, startParams));
}

// gradientDescent();
// gridSearch();
