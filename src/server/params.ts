export const SEARCH_BREADTH = {
  1: 34,
  2: 20,
  3: 10,
};

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

export function modifyParamsForAiMode(aiParams, aiMode, paramMods) {
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

export const DEFAULT_PARAM_MODS = {
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
    TETRIS_BONUS: 200, // has to be more than INACCESSIBLE_RIGHT, so that it'll take a tetris on 229 lines
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
    HIGH_COL_9_COEF: 0,
    SPIRE_HEIGHT_COEF: -1.2,
    LEFT_SURFACE_COEF: 0,
  },
};

/*--------------------------------
    Raw Param Data
---------------------------------*/

// ... (Previous results are not important now, they're in git history if curious)

// Trained using gradient descent
export const DEFAULT_PARAMS = {
  AVG_HEIGHT_EXPONENT: 1.356000000000004,
  AVG_HEIGHT_COEF: -5.50624,
  BURN_COEF: -4,
  COL_10_COEF: -1, // changed due to feature changing
  MAX_DIRTY_TETRIS_HEIGHT: 0.2, // (As a multiple of the scare height) Added manually since didn't exist at time of training
  EXTREME_GAP_COEF: -1.6416000000000004,
  BUILT_OUT_LEFT_COEF: 0.5, // changed due to feature changing
  BUILT_OUT_RIGHT_COEF: 0, // Added manually since didn't exist at time of training
  HOLE_COEF: -30,
  HOLE_WEIGHT_COEF: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999, // changed due to feature changing
  SPIRE_HEIGHT_COEF: -1.1556000000000002, // changed due to feature changing
  SCARE_HEIGHT_18: 10.032000000000002,
  SCARE_HEIGHT_19: 5.58,
  SCARE_HEIGHT_29: 0,
  HIGH_COL_9_COEF: -1, // changed due to feature changing
  SURFACE_COEF: 0.2739200000000001,
  LEFT_SURFACE_COEF: 0,
  TETRIS_BONUS: 28.248,
  TETRIS_READY_BONUS: 5.909760000000001,
  TETRIS_READY_BONUS_BAR_NEXT: 15.36,
  INACCESSIBLE_LEFT_COEF: -30, // Added manually since didn't exist at time of training
  INACCESSIBLE_RIGHT_COEF: -100, // Added manually since didn't exist at time of training
  TAP_ARR: 4,
  FIRST_TAP_DELAY: 2,
};

const NO_DIRTIES_MODIFICATIONS = {
  MAX_DIRTY_TETRIS_HEIGHT: 0, // (As a multiple of the scare height)
};
export const V5_NO_DIRTIES = applyModsToParams(
  DEFAULT_PARAMS,
  NO_DIRTIES_MODIFICATIONS
);

const AGGRO_MODIFICATIONS = {
  BURN_COEF: -10,
  HIGH_COL_9_COEF: -0.3,
};
const V5_AGGRO = applyModsToParams(DEFAULT_PARAMS, AGGRO_MODIFICATIONS);

export function getParams() {
  // Uncomment when using the AI to practice digging
  // return V5_NO_DIRTIES;
  return V5_AGGRO;
}

const V3_CUSTOM = {
  DIG: {
    BURN_COEF: -0.4611586002571638,
    COL_10_COEF: -4.8800000000000001,
    HOLE_WEIGHT_COEF: -5.338196782641227,
    HOLE_COEF: -100,
    SURFACE_COEF: 0.7444800000000001,
    BUILT_OUT_LEFT_COEF: 0.6664592319119086,
  },
  NEAR_KILLSCREEN: DEFAULT_PARAM_MODS.NEAR_KILLSCREEN,
  KILLSCREEN: DEFAULT_PARAM_MODS.KILLSCREEN,
};

export function getParamMods() {
  return V3_CUSTOM;
}
