export const NO_LIMIT = Number.MAX_SAFE_INTEGER;
/*--------------------------------
      Global configuration
---------------------------------*/
export const IS_DAS = true;

export const SHOULD_LOG = false;

export const LINE_CAP = NO_LIMIT;
export const DOUBLE_KILLSCREEN_ENABLED = false;
export const DEBUG_DOUBLE_KS_ALWAYS_ENABLED = false;
export const CPP_LIVEGAME_PLAYOUT_COUNT = 120;
export const CPP_LIVEGAME_PLAYOUT_LENGTH = 5;
export const CPP_LIVEGAME_PRUNING_BREADTH = 10;

// Rarely changed
export const IS_PAL = false;
export const WELL_COLUMN = 9; // 0-indexed
export const CAN_TUCK = false; // ONLY FOR DEBUGGING DAS
export const SHOULD_PUSHDOWN = false;
export const DISABLE_LOGGING = true;
export const MAX_CPP_PLAYOUT_MOVES = 9604;

// Calculated automatically
export const USE_RANKS = true;
export const IS_NON_RIGHT_WELL = WELL_COLUMN !== 9;
export const KILLSCREEN_LINES = IS_PAL ? 130 : 230;
export const KILLSCREEN_LEVEL = IS_PAL ? 19 : 29;
