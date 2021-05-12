export const SEARCH_BREADTH = {
  1: 34,
  2: 10,
};

export const EVALUATION_BREADTH = {
  1: 34,
  2: 20,
  3: 10 /* This breadth refers to the first hypothetical level, regardless of whether there was 1 or 2 concrete levels before */,
};

/*--------------------------------
  State-based param modification
---------------------------------*/

function applyModsToParams(aiParams, modObject) {
  const modifiedParams = JSON.parse(JSON.stringify(aiParams));
  for (const key in modObject) {
    if (aiParams[key] === undefined) {
      throw new Error("Invalid key on mod object: " + key);
    }
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
    case AiMode.KILLSCREEN_RIGHT_WELL:
      return applyModsToParams(aiParams, paramMods.KILLSCREEN_RIGHT_WELL);
    default:
      return aiParams;
  }
}

export const DEFAULT_PARAM_MODS = {
  DIG: {
    BURN_COEF: -1,
    COL_10_COEF: -1,
    HOLE_WEIGHT_COEF: -4,
    HOLE_COEF: -100,
    BUILT_OUT_LEFT_COEF: 0.6664592319119086,
  },
  NEAR_KILLSCREEN: {
    BURN_COEF: -15,
    TETRIS_READY_COEF: 10,
    TETRIS_COEF: 5000, // has to be more than INACCESSIBLE_RIGHT, so that it'll take a tetris on 229 lines
  },
  KILLSCREEN: {
    COL_10_COEF: 0,
    BUILT_OUT_LEFT_COEF: 0,
    BUILT_OUT_RIGHT_COEF: 1,
    AVG_HEIGHT_COEF: -3,
    HOLE_COEF: -40, // changed
    BURN_COEF: 0,
    UNABLE_TO_BURN_COEF: 0,
    HIGH_COL_9_COEF: 0,
    HIGH_COL_9_EXP: 0,
    INACCESSIBLE_LEFT_COEF: -100,
    INACCESSIBLE_RIGHT_COEF: -50,
    SPIRE_HEIGHT_COEF: -1.2,
    LEFT_SURFACE_COEF: 1,
    TETRIS_COEF: 100,
    SURFACE_COEF: 0.5,
  },
  KILLSCREEN_RIGHT_WELL: {
    SCARE_HEIGHT_OFFSET: -3,
    BURN_COEF: -1,
  },
};

/*--------------------------------
    Raw Param Data
---------------------------------*/

// ... (Previous results are not important now, they're in git history if curious)

// Trained using gradient descent and heavily modified
export const DEFAULT_PARAMS: InitialAiParams = {
  AVG_HEIGHT_EXPONENT: 1.5000000000004,
  AVG_HEIGHT_COEF: -4.50624,
  SCARE_HEIGHT_OFFSET: -3,
  BURN_COEF: -5,
  COL_10_COEF: -3,
  COL_10_HEIGHT_MULTIPLIER_EXP: 3,
  MAX_DIRTY_TETRIS_HEIGHT: 0.15, // (As a multiple of the scare height)
  EXTREME_GAP_COEF: -3,
  BUILT_OUT_LEFT_COEF: 1.5,
  BUILT_OUT_RIGHT_COEF: 0,
  HOLE_COEF: -30,
  HOLE_WEIGHT_COEF: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999,
  SPIRE_HEIGHT_COEF: -1.1556000000000002,
  UNABLE_TO_BURN_COEF: -0.3,
  UNABLE_TO_BURN_DIFF_EXP: 1.5,
  HIGH_COL_9_COEF: -3,
  HIGH_COL_9_EXP: 2,
  SURFACE_COEF: 1,
  LEFT_SURFACE_COEF: 0,
  TETRIS_COEF: 28.248,
  TETRIS_READY_COEF: 5.909760000000001,
  INACCESSIBLE_LEFT_COEF: -30,
  INACCESSIBLE_RIGHT_COEF: -300,
};

const DROUGHT_MODIFICATIONS = {
  BURN_COEF: -3,
  EXTREME_GAP_COEF: -20,
  AVG_HEIGHT_COEF: -10,
  AVG_HEIGHT_EXPONENT: 1.5,
};

const NO_DIRTIES_MODIFICATIONS = {
  MAX_DIRTY_TETRIS_HEIGHT: 0, // (As a multiple of the scare height)
};
export const V5_NO_DIRTIES = applyModsToParams(
  DEFAULT_PARAMS,
  NO_DIRTIES_MODIFICATIONS
);

const SEMI_AGGRO_MODIFICATIONS = {
  BURN_COEF: -14,
  HIGH_COL_9_COEF: -1.5,
  UNABLE_TO_BURN_COEF: -0.1,
};

const AGGRO_MODIFICATIONS = {
  BURN_COEF: -25,
  MAX_DIRTY_TETRIS_HEIGHT: 0.25,
  UNABLE_TO_BURN_COEF: -0.1,
  HIGH_COL_9_COEF: -1.5,
  AVG_HEIGHT_COEF: -10,
};

const LOW_TAP_SPEED_MODIFICATIONS = {
  UNABLE_TO_BURN_COEF: -0.7,
  BUILT_OUT_LEFT_COEF: 3,
  SCARE_HEIGHT_OFFSET: -1,
  BURN_COEF: -8,
  INACCESSIBLE_LEFT_COEF: -200,
};

const AGGRO_PARAMS = applyModsToParams(DEFAULT_PARAMS, AGGRO_MODIFICATIONS);
const SEMI_AGGRO_PARAMS = applyModsToParams(
  DEFAULT_PARAMS,
  SEMI_AGGRO_MODIFICATIONS
);
const DROUGHT_CODE_PARAMS = applyModsToParams(
  DEFAULT_PARAMS,
  DROUGHT_MODIFICATIONS
);
const LOW_TAP_SPEED_PARAMS = applyModsToParams(
  DEFAULT_PARAMS,
  LOW_TAP_SPEED_MODIFICATIONS
);

const PLAY_PERFECT_PARAMS = {
  AVG_HEIGHT_EXPONENT: 1.36000000000004,
  AVG_HEIGHT_COEF: -4.5,
  SCARE_HEIGHT_OFFSET: -3,
  BURN_COEF: -1000,
  COL_10_COEF: 0, // changed due to feature changing
  COL_10_HEIGHT_MULTIPLIER_EXP: 0,
  MAX_DIRTY_TETRIS_HEIGHT: 0, // (As a multiple of the scare height) Added manually since didn't exist at time of training
  EXTREME_GAP_COEF: -2,
  BUILT_OUT_LEFT_COEF: 0, // changed due to feature changing
  BUILT_OUT_RIGHT_COEF: 0, // Added manually since didn't exist at time of training
  HOLE_COEF: 0,
  HOLE_WEIGHT_COEF: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999, // changed due to feature changing
  SPIRE_HEIGHT_COEF: -1.1556000000000002, // changed due to feature changing
  UNABLE_TO_BURN_COEF: 0, // changed due to feature changing
  UNABLE_TO_BURN_DIFF_EXP: 0,
  HIGH_COL_9_COEF: 0,
  HIGH_COL_9_EXP: 0,
  SURFACE_COEF: 1,
  LEFT_SURFACE_COEF: 0,
  TETRIS_COEF: 500,
  TETRIS_READY_COEF: 5.909760000000001,
  INACCESSIBLE_LEFT_COEF: -30, // Added manually since didn't exist at time of training
  INACCESSIBLE_RIGHT_COEF: -10000, // The only thing it cares about more than not burning is keeping the right well open
};

export function getParams(): InitialAiParams {
  return AGGRO_PARAMS;
}

const V3_CUSTOM = {
  DIG: {
    BURN_COEF: -1,
    COL_10_COEF: 0,
    HOLE_WEIGHT_COEF: -5.338196782641227,
    HOLE_COEF: -100,
    BUILT_OUT_LEFT_COEF: 0.6664592319119086,
  },
  NEAR_KILLSCREEN: DEFAULT_PARAM_MODS.NEAR_KILLSCREEN,
  KILLSCREEN: DEFAULT_PARAM_MODS.KILLSCREEN,
  KILLSCREEN_RIGHT_WELL: DEFAULT_PARAM_MODS.NEAR_KILLSCREEN,
};

export function getParamMods(): ParamMods {
  return DEFAULT_PARAM_MODS;
}
