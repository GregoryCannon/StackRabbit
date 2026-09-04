export const NO_LIMIT = Number.MAX_SAFE_INTEGER;
/*--------------------------------
      Global configuration
---------------------------------*/
export const LINE_CAP = NO_LIMIT;
export const DOUBLE_KILLSCREEN_ENABLED = false;
export const DEBUG_DOUBLE_KS_ALWAYS_ENABLED = false;
export const CPP_LIVEGAME_PLAYOUT_COUNT = 300;
export const CPP_LIVEGAME_PLAYOUT_LENGTH = 5;
export const CPP_LIVEGAME_PRUNING_BREADTH = 10;

// Rarely changed
export const CAN_TUCK = true;
export const DAS_SLOW_TAP_TIMELINE = "X......." // 7.5 Hz slowtapping
export const IS_PAL = false;
export const WELL_COLUMN = 9; // 0-indexed
export const SHOULD_PUSHDOWN = false;
export const MAX_CPP_PLAYOUT_MOVES = 9604; // 4 * 7^4, i.e. a fully exhastive depth 4 search

// Calculated automatically
export const IS_NON_RIGHT_WELL = WELL_COLUMN !== 9;
export const KILLSCREEN_LINES = IS_PAL ? 130 : 230;
export const KILLSCREEN_LEVEL = IS_PAL ? 19 : 29;

export const LOSS_DAS_PENALTY = 20;

// var tempLoggingOn = false;
// export function isTempLoggingOn() {
//       return tempLoggingOn;
// }
// export function setTempLoggingOn(value: boolean) {
//       tempLoggingOn = value;
// }