/**
 * A grid search / gradient descent algorithm for choosing the best parameter set.
 */

import * as liteGameSimulator from "../lite_game_simulator";
import { NO_DIRTIES_PARAMS, getParams, DEFAULT_PARAM_MODS } from "../params";

const NOISE_THRESHOLD = 1000;
const DOMAIN = "DIG";
const ITERATIONS_PER_TEST = 100;
const SPOT_CHECK_ITERATIONS = 10;
const NUM_PASSES = 2;
const FEATURES_TO_OPTIMIZE = [
  "BURN_COEF",
  "COL_10_COEF",
  "HOLE_WEIGHT_COEF",
  "HOLE_COEF",
  "SURFACE_COEF",
  "BUILT_OUT_LEFT_COEF",
];

function simulateDigPractice(a, b, c, d) {
  // Has been migrated elsewhere. This file is no longer in use.
  return null;
}

/**
 * Fitness function: the average score
 * @param {Array of [score, lines, level, numHoles] subarrays} - simulationResult
 */
function fitnessFunction(simulationResult) {
  if (simulationResult.length === 0) {
    return 0;
  }
  function scoreForOneGame(score, lines, numHoles) {
    if (numHoles > 0) {
      return -40000 * numHoles;
    }
    return score + (liteGameSimulator.DIG_LINE_CAP - lines) * 5000;
  }
  const evaluatedScores = simulationResult.map(
    ([score, lines, level, numHoles]) => scoreForOneGame(score, lines, numHoles)
  );
  const avgScore =
    evaluatedScores.reduce((a, b) => a + b, 0) / evaluatedScores.length;
  return avgScore;
}

/** Evaluate the fitness of a paramMods object using either a small sample of testing or a full suite of testing. */
function getFitness(paramMods, currentFitness?) {
  // First do a spot check with a small number of iterations.
  // If it's not even close to the current fitness, don't run more iterations
  console.log("Running spot check...");
  const spotCheckResults = simulateDigPractice(
    SPOT_CHECK_ITERATIONS,
    18,
    NO_DIRTIES_PARAMS,
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
      simulateDigPractice(
        ITERATIONS_PER_TEST - SPOT_CHECK_ITERATIONS,
        18,
        NO_DIRTIES_PARAMS,
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

function gradientDescend(startingParamMods) {
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
        changeHistory.push(
          feature +
            "=" +
            parseFloat(currentParamMods[DOMAIN][feature]).toFixed(2)
        );
        console.log(`Best direction: ${bestDirection}`);
        console.log(
          `\nBest params so far: ${JSON.stringify(currentParamMods)}`
        );
        console.log(`Fitness history:`);
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
    BURN_COEF: -2.7,
    COL_10_COEF: -5,
    HOLE_WEIGHT_COEF: -5,
    HOLE_COEF: -50,
    SURFACE_COEF: 0.3,
    BUILT_OUT_LEFT_COEF: 5,
  },
  NEAR_KILLSCREEN: {
    BURN_COEF: -15,
    TETRIS_READY_COEF: 10,
  },
  KILLSCREEN: {
    COL_10_COEF: 0,
    BUILT_OUT_LEFT_COEF: 10.5,
  },
};
const V1_PARAM_MODS = {
  DIG: {
    BURN_COEF: -3.3443763200000003,
    COL_10_COEF: -3.9808917197452227,
    HOLE_WEIGHT_COEF: -5,
    HOLE_COEF: -250,
    SURFACE_COEF: 0.3,
    BUILT_OUT_LEFT_COEF: 5,
  },
  NEAR_KILLSCREEN: { BURN_COEF: -15, TETRIS_READY_COEF: 10 },
  KILLSCREEN: { COL_10_COEF: 0, BUILT_OUT_LEFT_COEF: 10.5 },
};
const V1_5 = {
  DIG: {
    BURN_COEF: -1,
    COL_10_COEF: -1,
    HOLE_WEIGHT_COEF: -10,
    HOLE_COEF: -100,
    SURFACE_COEF: 0.5,
    BUILT_OUT_LEFT_COEF: 5,
  },
  NEAR_KILLSCREEN: {
    BURN_COEF: -15,
    TETRIS_READY_COEF: 10,
  },
  KILLSCREEN: {
    COL_10_COEF: 0,
    BUILT_OUT_LEFT_COEF: 10.5,
  },
};
const V2 = {
  DIG: {
    BURN_COEF: -0.4611586002571638,
    COL_10_COEF: -3.3840000000000003,
    HOLE_WEIGHT_COEF: -2.338196782641227,
    HOLE_COEF: -338.40000000000003,
    SURFACE_COEF: 0.5640000000000001,
    BUILT_OUT_LEFT_COEF: 1.666666666666667,
  },
  NEAR_KILLSCREEN: {
    BURN_COEF: -15,
    TETRIS_READY_COEF: 10,
  },
  KILLSCREEN: {
    COL_10_COEF: 0,
    BUILT_OUT_LEFT_COEF: 10.5,
  },
};

const V3 = {
  DIG: {
    BURN_COEF: -0.4611586002571638,
    COL_10_COEF: -1.8800000000000001,
    HOLE_WEIGHT_COEF: -2.338196782641227,
    HOLE_COEF: -248.16000000000005,
    SURFACE_COEF: 0.7444800000000001,
    BUILT_OUT_LEFT_COEF: 0.6664592319119086,
  },
  NEAR_KILLSCREEN: { BURN_COEF: -15, TETRIS_READY_COEF: 10 },
  KILLSCREEN: { COL_10_COEF: 0, BUILT_OUT_LEFT_COEF: 10.5 },
};

// gradientDescend(MODIFIED_V5, V1);
console.log(getFitness(DEFAULT_PARAM_MODS));
