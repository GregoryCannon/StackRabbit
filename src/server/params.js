const AI_MODE = Object.freeze({
  STANDARD: "standard",
  DIG_WITH_HOLES: "dig_with_holes", // When digging and there are holes left
  NEAR_KILLSCREEN: "near_killscreen",
  KILLSCREEN: "killscreen",
});

const NUM_TO_CONSIDER = 20;

/*--------------------------------
  State-based param modification
---------------------------------*/

function applyModsToParams(aiParams, modObject) {
  const modifiedParams = JSON.parse(JSON.stringify(aiParams));
  for (const key in modObject) {
    modifiedParams[key] = modObject[key];
  }
  return modifiedParams;
}

function modifyParamsForAiMode(aiParams, aiMode) {
  switch (aiMode) {
    case AI_MODE.DIG_WITH_HOLES:
      return applyModsToParams(aiParams, DIG_WITH_HOLES_MODIFICATIONS);
    case AI_MODE.NEAR_KILLSCREEN:
      return applyModsToParams(aiParams, NEAR_KILLSCREEN_MODIFICATIONS);
    case AI_MODE.KILLSCREEN:
      return applyModsToParams(aiParams, KILLSCREEN_MODIFICATIONS);
    default:
      return aiParams;
  }
}

const DIG_WITH_HOLES_MODIFICATIONS = {
  BURN_PENALTY: 0,
  COL_10_PENALTY: -2,
  HOLE_WEIGHT_PENALTY: -3,
  HOLE_PENALTY: -30,
  SURFACE_MULTIPLIER: 0.2,
  HIGH_LEFT_MULTIPLIER: 0,
};

const NEAR_KILLSCREEN_MODIFICATIONS = {
  BURN_PENALTY: -20,
  TETRIS_READY_BONUS: 10,
};

const KILLSCREEN_MODIFICATIONS = {
  COL_10_PENALTY: 0,
  HIGH_LEFT_MULTIPLIER: 10.5,
};

/*--------------------------------
    Raw Param Data
---------------------------------*/

// ... (Previous results are not important now, they're in git history if curious)

// Trained using gradient descent
const V5_TRAINED_PARAMS = {
  AVG_HEIGHT_EXPONENT: 1.1556000000000004,
  AVG_HEIGHT_MULTIPLIER: -10.50624,
  BURN_PENALTY: -2.2,
  COL_10_PENALTY: -4, // changed due to feature changing
  MAX_DIRTY_TETRIS_HEIGHT: 0.2, // (As a multiple of the scare height) Added manually since didn't exist at time of training
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

const AGGRO_MODIFICATIONS = {
  BURN_PENALTY: -15,
  HOLE_PENALTY: -40,
};
const V5_AGGRO = applyModsToParams(V5_TRAINED_PARAMS, AGGRO_MODIFICATIONS);

function getParams() {
  return V5_TRAINED_PARAMS;
}

module.exports = {
  getParams,
  V5_TRAINED_PARAMS,
  NUM_TO_CONSIDER,
  AI_MODE,
  modifyParamsForAiMode,
};
