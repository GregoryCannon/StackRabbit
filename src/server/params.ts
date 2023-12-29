export const NO_LIMIT = Number.MAX_SAFE_INTEGER;

// Each index corresponds to the depth you're starting search at while doing the pruning.
export const SEARCH_BREADTH = {
  2: 999, // Don't prune before the second stage ("bad" placements could be a floating burn setup)
  3: 8 /* This breadth refers to the first hypothetical level, regardless of whether there was 1 or 2 concrete levels before */,
};

export const EVALUATION_BREADTH = {
  1: 999, // Same as search breadth
  2: 20,
  3: 10 /* This breadth refers to the first hypothetical level, regardless of whether there was 1 or 2 concrete levels before */,
};

/*--------------------------------
      Global configuration
---------------------------------*/

export const SHOULD_LOG = false;

export const IS_DROUGHT_MODE = false;
export const LINE_CAP = NO_LIMIT;
export const DOUBLE_KILLSCREEN_ENABLED = true;
export const DEBUG_DOUBLE_KS_ALWAYS_ENABLED = false;
export const CPP_LIVEGAME_PLAYOUT_COUNT = 50;
export const CPP_LIVEGAME_PLAYOUT_LENGTH = 2;

// Rarely changed
export const IS_PAL = false;
export const WELL_COLUMN = 9; // 0-indexed
export const CAN_TUCK = true;
export const SHOULD_PUSHDOWN = false;
export const DISABLE_LOGGING = true;
export const MAX_CPP_PLAYOUT_MOVES = 1000;

// Calculated automatically
export const USE_RANKS = true;
export const IS_NON_RIGHT_WELL = WELL_COLUMN !== 9;
export const KILLSCREEN_LINES = IS_PAL ? 130 : 230;
export const KILLSCREEN_LEVEL = IS_PAL ? 19 : 29;

/*--------------------------------
  State-based param modification
---------------------------------*/

export function applyModsToParams(aiParams, modObject) {
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
    case AiMode.DIG_INTO_KILLSCREEN:
      return applyModsToParams(aiParams, paramMods.DIG_INTO_KILLSCREEN);
    case AiMode.NEAR_KILLSCREEN:
      return applyModsToParams(aiParams, paramMods.NEAR_KILLSCREEN);
    case AiMode.KILLSCREEN:
      return applyModsToParams(aiParams, paramMods.KILLSCREEN);
    case AiMode.KILLSCREEN_FOR_TETRISES:
      return applyModsToParams(aiParams, paramMods.KILLSCREEN_FOR_TETRISES);
    case AiMode.IMMINENT_DEATH:
      return applyModsToParams(aiParams, paramMods.IMMINENT_DEATH);
    default:
      return aiParams;
  }
}

/*
Backup of what the dig param mods were before some tweaking on 6/27/2021
DIG: {
    BURN_COEF: -1,
    COL_10_COEF: -0.25,
    HOLE_WEIGHT_COEF: -8,
    HOLE_COEF: -50,
    AVG_HEIGHT_COEF: -8,
    HIGH_COL_9_COEF: -1,
    UNABLE_TO_BURN_COEF: 0,
  },
*/

export const DEFAULT_PARAM_MODS = {
  DIG: {
    BURN_COEF: -1,
    COL_10_COEF: -0.25,
    HOLE_WEIGHT_COEF: -8,
    HOLE_COEF: -50,
    AVG_HEIGHT_COEF: -8,
    HIGH_COL_9_COEF: IS_DROUGHT_MODE ? -3 : -1,
    UNABLE_TO_BURN_COEF: 0,
  },
  DIG_INTO_KILLSCREEN: {
    BURN_COEF: -1,
    TETRIS_READY_COEF: 10,
    TETRIS_COEF: 400, // has to be more than INACCESSIBLE_RIGHT, so that it'll take a tetris on 229 lines
  },
  NEAR_KILLSCREEN: {
    TETRIS_READY_COEF: 15,
    MAX_DIRTY_TETRIS_HEIGHT: 0.5,
    TETRIS_COEF: 400, // has to be more than INACCESSIBLE_RIGHT, so that it'll take a tetris on 229 lines
  },
  IMMINENT_DEATH: {
    AVG_HEIGHT_COEF: 0,
    SPIRE_HEIGHT_COEF: 0,
    INACCESSIBLE_LEFT_COEF: 0,
    INACCESSIBLE_RIGHT_COEF: -30,
    TETRIS_COEF: 400, // In case of a crazy center well or something
  },
  KILLSCREEN: {
    COL_10_COEF: 0,
    BUILT_OUT_LEFT_COEF: 6,
    BUILT_OUT_RIGHT_COEF: 1.5,
    AVG_HEIGHT_COEF: -5,
    AVG_HEIGHT_EXPONENT: 1.2000000000004,
    HOLE_COEF: -60,
    BURN_COEF: 0,
    UNABLE_TO_BURN_COEF: 0,
    HIGH_COL_9_COEF: 0,
    HIGH_COL_9_EXP: 0,
    INACCESSIBLE_LEFT_COEF: -100,
    INACCESSIBLE_RIGHT_COEF: -100,
    SPIRE_HEIGHT_COEF: -0.5,
    LEFT_SURFACE_COEF: 0.5,
    TETRIS_COEF: 40,
    SURFACE_COEF: 0.5,
  },
  KILLSCREEN_FOR_TETRISES: {
    BURN_COEF: -0, // No need to be aggro when there's no line cap
    // HIGH_COL_9_COEF: -2,
    // HIGH_COL_9_EXP: 1.6,
  },
};

/*--------------------------------
    Raw Param Data
---------------------------------*/

// Trained using gradient descent and heavily modified
export const DEFAULT_PARAMS: InitialAiParams = {
  AVG_HEIGHT_EXPONENT: 1.5000000000004,
  AVG_HEIGHT_COEF: -5,
  BURN_COEF: -12,
  BURN_COEF_POST: -10,
  COL_10_COEF: -1,
  COL_10_HEIGHT_MULTIPLIER_EXP: 3,
  DEAD_COEF: -10000,
  MAX_DIRTY_TETRIS_HEIGHT: 0.25, // (As a multiple of the scare height)
  EXTREME_GAP_COEF: -2,
  BUILT_OUT_LEFT_COEF: 2,
  BUILT_OUT_RIGHT_COEF: 0,
  LOW_LEFT_EXP: 1.5,
  HOLE_COEF: -30,
  HOLE_WEIGHT_COEF: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999,
  SPIRE_HEIGHT_COEF: -1.1556000000000002,
  UNABLE_TO_BURN_COEF: -0.3,
  UNABLE_TO_BURN_HEIGHT_EXP: 3,
  HIGH_COL_9_COEF: -3,
  HIGH_COL_9_EXP: 2,
  SURFACE_COEF: 1,
  LEFT_SURFACE_COEF: 0,
  TETRIS_COEF: 30,
  TETRIS_READY_COEF: 6,
  INACCESSIBLE_LEFT_COEF: -50,
  INACCESSIBLE_RIGHT_COEF: -300,
};

const CENTER_WELL_MODIFICATIONS = {
  UNABLE_TO_BURN_COEF: 0,
  UNABLE_TO_BURN_HEIGHT_EXP: 0,
  HIGH_COL_9_COEF: 0,
  HIGH_COL_9_EXP: 0,
  LEFT_SURFACE_COEF: 0,
  INACCESSIBLE_LEFT_COEF: -200,
  INACCESSIBLE_RIGHT_COEF: -200,
};

const PLAY_PERFECT_PARAMS: InitialAiParams = {
  AVG_HEIGHT_EXPONENT: 1,
  AVG_HEIGHT_COEF: -300,
  BURN_COEF: -1000,
  BURN_COEF_POST: -1000,
  COL_10_COEF: 0,
  COL_10_HEIGHT_MULTIPLIER_EXP: 0,
  DEAD_COEF: -100000,
  MAX_DIRTY_TETRIS_HEIGHT: 0,
  EXTREME_GAP_COEF: -5,
  BUILT_OUT_LEFT_COEF: 0,
  BUILT_OUT_RIGHT_COEF: 0,
  LOW_LEFT_EXP: 0,
  HOLE_COEF: 0,
  HOLE_WEIGHT_COEF: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999,
  SPIRE_HEIGHT_COEF: -1.1556000000000002,
  UNABLE_TO_BURN_COEF: 0,
  UNABLE_TO_BURN_HEIGHT_EXP: 0,
  HIGH_COL_9_COEF: 0,
  HIGH_COL_9_EXP: 0,
  SURFACE_COEF: 0.02,
  LEFT_SURFACE_COEF: 0,
  TETRIS_COEF: 50,
  TETRIS_READY_COEF: 5.909760000000001,
  INACCESSIBLE_LEFT_COEF: -30,
  INACCESSIBLE_RIGHT_COEF: -1000,
};

const DROUGHT_CODE_PATCH = {
  BURN_COEF: -10,
  EXTREME_GAP_COEF: -5,
  AVG_HEIGHT_COEF: -8,
  UNABLE_TO_BURN_COEF: -0.7,
  HIGH_COL_9_COEF: -10,
  TETRIS_READY_COEF: 25,
  TETRIS_COEF: 100,
};

const NO_DIRTY_TETRIS_PATCH = {
  MAX_DIRTY_TETRIS_HEIGHT: 0,
};

const EXHIBITION_PATCH = {
  BURN_COEF: -12,
  BURN_COEF_POST: -12,
  HIGH_COL_9_COEF: -5,
  UNABLE_TO_BURN_COEF: -0.5,
};

const SAFE_PATCH = {
  AVG_HEIGHT_COEF: -7,
  HIGH_COL_9_COEF: -5,
  UNABLE_TO_BURN_COEF: -0.5,
};

const EXHIBITION_AGGRO_PATCH = {
  BURN_COEF: -15,
  BURN_COEF_POST: -15,
  HIGH_COL_9_COEF: -5,
  UNABLE_TO_BURN_COEF: -0.5,
  AVG_HEIGHT_COEF: -6,
};

const EXHIBITION_20HZ_KILLSCREEN_PATCH = {
  BUILT_OUT_LEFT_COEF: 3,
  HOLE_COEF: -50,
  BURN_COEF: -6,
  BURN_COEF_POST: -6,
  AVG_HEIGHT_COEF: -5,
  AVG_HEIGHT_EXPONENT: 1.7,
  SPIRE_HEIGHT_COEF: -1.5,
  INACCESSIBLE_LEFT_COEF: -200,
  MAX_DIRTY_TETRIS_HEIGHT: 0,
};

const AGGRO_PATCH = {
  BURN_COEF: -14,
  HIGH_COL_9_COEF: -1.5,
  UNABLE_TO_BURN_COEF: -0.1,
};

const FULL_AGGRO_PATCH = {
  BURN_COEF: -40,
  MAX_DIRTY_TETRIS_HEIGHT: 0.25,
  UNABLE_TO_BURN_COEF: -0.1,
  HIGH_COL_9_COEF: -1.5,
  AVG_HEIGHT_COEF: -10,
  INACCESSIBLE_RIGHT_COEF: -1000,
};

const LOW_TAP_SPEED_PATCH = {
  UNABLE_TO_BURN_COEF: -0.7,
  BURN_COEF: -5,
  INACCESSIBLE_LEFT_COEF: -200,
};

const MEDIUM_LOW_TAP_SPEED_MODIFICATIONS = {
  UNABLE_TO_BURN_COEF: -0.7,
  INACCESSIBLE_LEFT_COEF: -200,
};

export const NO_DIRTIES_PARAMS = applyModsToParams(
  DEFAULT_PARAMS,
  NO_DIRTY_TETRIS_PATCH
);

export function getParams(): InitialAiParams {
  return applyModsToParams(DEFAULT_PARAMS, SAFE_PATCH);
  return;
}

export function getParamMods(): ParamMods {
  return DEFAULT_PARAM_MODS;
}
