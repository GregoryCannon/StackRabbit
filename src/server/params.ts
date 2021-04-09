const POSSIBILITIES_TO_CONSIDER = 20;
const CHAIN_POSSIBILITIES_TO_CONSIDER = 1;

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

function modifyParamsForAiMode(aiParams, aiMode, paramMods) {
  switch (aiMode) {
    case AiMode.DIG:
      return applyModsToParams(aiParams, paramMods.DIG);
    case AiMode.NEAR_KILLSCREEN:
      return applyModsToParams(aiParams, paramMods.NEAR_KILLSCREEN);
    case AiMode.KILLSCREEN:
      return applyModsToParams(aiParams, paramMods.KILLSCREEN);
    default:
      return aiParams;
  }
}

const DEFAULT_PARAM_MODS = {
  DIG: {
    BURN_COEF: 0,
    COL_10_COEF: -2,
    HOLE_WEIGHT_COEF: -3,
    HOLE_COEF: -100, // changed
    SURFACE_COEF: 0.2,
  },
  NEAR_KILLSCREEN: {
    BURN_COEF: -15,
    TETRIS_READY_BONUS: 10,
  },
  KILLSCREEN: {
    COL_10_COEF: 0,
    BUILT_OUT_LEFT_COEF: 6,
    BUILT_OUT_RIGHT_COEF: 2,
    AVG_HEIGHT_COEF: -8,
    HOLE_COEF: -20, // changed
    BURN_COEF: 0,
    INACCESSIBLE_LEFT_COEF: -100,
    INACCESSIBLE_RIGHT_COEF: -50,
    HIGH_COL_9_COEF_COEF: 0,
  },
};

/*--------------------------------
    Raw Param Data
---------------------------------*/

// ... (Previous results are not important now, they're in git history if curious)

// Trained using gradient descent
const V5_TRAINED_PARAMS = {
  AVG_HEIGHT_EXPONENT: 1.1556000000000004,
  AVG_HEIGHT_COEF: -10.50624,
  BURN_COEF: -2.2,
  COL_10_COEF: -4, // changed due to feature changing
  MAX_DIRTY_TETRIS_HEIGHT: 0.2, // (As a multiple of the scare height) Added manually since didn't exist at time of training
  EXTREME_GAP_COEF: -1.6416000000000004,
  BUILT_OUT_LEFT_COEF: 0.5, // changed due to feature changing
  BUILT_OUT_RIGHT_COEF: 0, // Added manually since didn't exist at time of training
  HOLE_COEF: -19.8,
  HOLE_WEIGHT_COEF: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999, // changed due to feature changing
  SPIRE_HEIGHT_COEF: -1.1556000000000002, // changed due to feature changing
  SCARE_HEIGHT_18: 10.032000000000002,
  SCARE_HEIGHT_19: 5.58,
  SCARE_HEIGHT_29: 0,
  HIGH_COL_9_COEF_COEF: -0.24974400000000005, // changed due to feature changing
  SURFACE_COEF: 0.2739200000000001,
  TETRIS_BONUS: 28.248,
  TETRIS_READY_BONUS: 5.909760000000001,
  TETRIS_READY_BONUS_BAR_NEXT: 15.36,
  INACCESSIBLE_LEFT_COEF: -30, // Added manually since didn't exist at time of training
  INACCESSIBLE_RIGHT_COEF: -200, // Added manually since didn't exist at time of training
  TAP_ARR: 4,
  FIRST_TAP_DELAY: 2,
};

const NO_DIRTIES_MODIFICATIONS = {
  MAX_DIRTY_TETRIS_HEIGHT: 0, // (As a multiple of the scare height)
};
const V5_NO_DIRTIES = applyModsToParams(
  V5_TRAINED_PARAMS,
  NO_DIRTIES_MODIFICATIONS
);

const AGGRO_MODIFICATIONS = {
  BURN_COEF: -15,
  HOLE_COEF: -40,
};
const V5_AGGRO = applyModsToParams(V5_TRAINED_PARAMS, AGGRO_MODIFICATIONS);

function getParams() {
  // Uncomment when using the AI to practice digging
  // return V5_NO_DIRTIES;
  return V5_TRAINED_PARAMS;
}

function getParamMods() {
  return DEFAULT_PARAM_MODS;
}

module.exports = {
  getParams,
  getParamMods,
  V5_TRAINED_PARAMS,
  V5_NO_DIRTIES,
  POSSIBILITIES_TO_CONSIDER,
  CHAIN_POSSIBILITIES_TO_CONSIDER,
  DEFAULT_PARAM_MODS,
  modifyParamsForAiMode,
};
