/**
 * A grid search / gradient descent algorithm for choosing the best parameter set.
 */

const liteGameSimulator = require("./lite_game_simulator");
const params = require("./params");
const { V5_TRAINED_PARAMS, getParams } = require("./params");

const NOISE_THRESHOLD = 1000;
const DOMAIN = "DIG";
const ITERATIONS_PER_TEST = 100;
const SPOT_CHECK_ITERATIONS = 10;
const NUM_PASSES = 2;
const FEATURES_TO_OPTIMIZE = [
  "BURN_PENALTY",
  "COL_10_PENALTY",
  "HOLE_WEIGHT_PENALTY",
  "HOLE_PENALTY",
  "SURFACE_MULTIPLIER",
  "HIGH_LEFT_MULTIPLIER",
];

/**
 * Fitness function: the median score
 * @param {Array of [score, lines, level, numHoles] subarrays} - simulationResult
 */
function fitnessFunction(simulationResult) {
  if (simulationResult.length === 0){
    return 0;
  }
  function scoreForOneGame(score, lines, numHoles) {
    if (numHoles > 0) {
      return -40000 * numHoles;
    }
    return score + (liteGameSimulator.DIG_LINE_CAP - lines) * 5000;
  }
  const evaluatedScores = simulationResult
    .map(([score, lines, level, numHoles]) =>
      scoreForOneGame(score, lines, numHoles)
    );
  const avgScore = evaluatedScores.reduce((a, b) => a + b, 0) / evaluatedScores.length;
  return avgScore;
}

function getFitness(paramMods, currentFitness) {
  // First do a spot check with a small number of iterations.
  // If it's not even close to the current fitness, don't run more iterations
  console.log("Running spot check...");
  const spotCheckResults = liteGameSimulator.simulateDigPractice(
    SPOT_CHECK_ITERATIONS,
    18,
    V5_TRAINED_PARAMS,
    paramMods
  );
  const spotCheckFitness = fitnessFunction(spotCheckResults);
  const thresholdToBeat = !currentFitness
    ? 0
    : currentFitness > 0
    ? currentFitness * 0.8
    : currentFitness * 1.2;
  console.log(spotCheckFitness, thresholdToBeat);
  if (currentFitness !== 0 && spotCheckFitness < thresholdToBeat) {
    return spotCheckFitness;
  }

  // Run the rest of the simulations and combine the results
  console.log("Continuing to full simulation...");
  return fitnessFunction(
    spotCheckResults.concat(
      liteGameSimulator.simulateDigPractice(
        ITERATIONS_PER_TEST - SPOT_CHECK_ITERATIONS,
        18,
        V5_TRAINED_PARAMS,
        paramMods
      )
    )
  );
}

function modifyParamMods(paramMods, feature, value) {
  const newParamMods = JSON.parse(JSON.stringify(paramMods));
  newParamMods[DOMAIN][feature] = value;
  return newParamMods;
}

function gradientDescend(mainParams, startingParamMods) {
  const fitnessHistory = [];
  const changeHistory = [];

  let currentParamMods = JSON.parse(JSON.stringify(startingParamMods));
  let currentFitness = 0;

  for (let passIndex = 0; passIndex < NUM_PASSES; passIndex++) {
    for (const feature of FEATURES_TO_OPTIMIZE) {
      console.log("\n\nOptimizing feature:", feature);
      let DELTA = 2;
      while (DELTA > 0.1) {
        console.log(`\n\n-----DELTA: ${DELTA} --------`);

        // The null hypothesis
        let bestDirection = 0;
        const currentVal = currentParamMods[DOMAIN][feature];
        currentFitness = getFitness(currentParamMods);
        console.log(`No change: ${currentVal} has fitness ${currentFitness}`);

        // Try growing
        const growVal = currentVal * (1 + DELTA);
        const fitnessGrow = getFitness(
          modifyParamMods(currentParamMods, feature, growVal),
          currentFitness
        );
        console.log(`Grow direction: ${growVal} has fitness ${fitnessGrow}`);
        if (fitnessGrow > currentFitness + NOISE_THRESHOLD) {
          bestDirection = 1;
          currentFitness = fitnessGrow;
          currentParamMods[DOMAIN][feature] = growVal;
        }

        // Try shrinking
        const shrinkVal = currentVal / (1 + DELTA);
        const fitnessShrink = getFitness(
          modifyParamMods(currentParamMods, feature, shrinkVal),
          currentFitness
        );
        console.log(
          `Shrink direction: ${shrinkVal} has fitness ${fitnessShrink}`
        );
        if (
          fitnessShrink > currentFitness + NOISE_THRESHOLD &&
          fitnessShrink > fitnessGrow
        ) {
          bestDirection = -1;
          currentFitness = fitnessShrink;
          currentParamMods[DOMAIN][feature] = shrinkVal;
        }

        fitnessHistory.push(currentFitness);
        changeHistory.push(feature + "=" + parseFloat(currentParamMods[DOMAIN][feature]).toFixed(2))
        console.log(`Best direction: ${bestDirection}`);
        console.log(
          `\nBest params so far: ${JSON.stringify(currentParamMods)}`
        );
        console.log(`Fitness history:`)
        console.log(fitnessHistory);
        console.log("Change history");
        console.log(changeHistory);

        // If neither big step found grow changes, it means we're close: decrease step size
        if (bestDirection == 0) {
          DELTA *= 0.4;
          console.log("Decreasing step size:", DELTA);
        }
      }
    }
  }
}

const MEDIUM_PARAM_MODS = {
  DIG: {
    BURN_PENALTY: -2.7,
    COL_10_PENALTY: -5,
    HOLE_WEIGHT_PENALTY: -5,
    HOLE_PENALTY: -50,
    SURFACE_MULTIPLIER: 0.3,
    HIGH_LEFT_MULTIPLIER: 5,
  },
  NEAR_KILLSCREEN: {
    BURN_PENALTY: -15,
    TETRIS_READY_BONUS: 10,
  },
  KILLSCREEN: {
    COL_10_PENALTY: 0,
    HIGH_LEFT_MULTIPLIER: 10.5,
  },
};
const V1 = {
  DIG: {
    BURN_PENALTY: -1,
    COL_10_PENALTY: -1,
    HOLE_WEIGHT_PENALTY: -10,
    HOLE_PENALTY: -100,
    SURFACE_MULTIPLIER: 0.5,
    HIGH_LEFT_MULTIPLIER: 5,
  },
  NEAR_KILLSCREEN: {
    BURN_PENALTY: -15,
    TETRIS_READY_BONUS: 10,
  },
  KILLSCREEN: {
    COL_10_PENALTY: 0,
    HIGH_LEFT_MULTIPLIER: 10.5,
  },
};
const MODIFIED_V5 = {
  AVG_HEIGHT_EXPONENT: 1.1556000000000004,
  AVG_HEIGHT_MULTIPLIER: -10.50624,
  BURN_PENALTY: -2.2,
  COL_10_PENALTY: -4, // changed due to feature changing
  MAX_DIRTY_TETRIS_HEIGHT: 0, // (As a multiple of the scare height) Added manually since didn't exist at time of training
  EXTREME_GAP_PENALTY: -1.6416000000000004,
  HIGH_LEFT_MULTIPLIER: 1.7280000000000004,
  HOLE_PENALTY: -19.8,
  HOLE_WEIGHT_PENALTY: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999, // changed due to feature changing
  SPIRE_HEIGHT_MULTIPLIER: -1.1556000000000002, // changed due to feature changing
  NOT_BUILDING_TOWARD_TETRIS_PENALTY: -6.054400000000001,
  SCARE_HEIGHT_18: 10.032000000000002,
  SCARE_HEIGHT_19: 5.58,
  SCARE_HEIGHT_29: 0,
  HIGH_COL_9_PENALTY_MULTIPLIER: -0.24974400000000005, // changed due to feature changing
  SURFACE_MULTIPLIER: 0.2739200000000001,
  TETRIS_BONUS: 28.248,
  TETRIS_READY_BONUS: 5.909760000000001,
  TETRIS_READY_BONUS_BAR_NEXT: 15.36,
  INACCESSIBLE_LEFT_PENALTY: -20, // Added manually since didn't exist at time of training
  INACCESSIBLE_RIGHT_PENALTY: -200, // Added manually since didn't exist at time of training
};

// gradientDescend(MODIFIED_V5, V1);
console.log(getFitness(V1));
