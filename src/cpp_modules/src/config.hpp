#ifndef CONFIG
#define CONFIG

// ROM configuration
#define DOUBLE_KILLSCREEN_ENABLED 0
#define DEBUG_DOUBLE_KS_ALWAYS_ENABLED 0

// Logging
#define LOGGING_ENABLED 0 // Refers to movesearch/eval logging
#define PLAYOUT_LOGGING_ENABLED 0
#define SIMULATION_LOGGING_ENABLED 0
#define PLAYOUT_RESULT_LOGGING_ENABLED 0
#define MOVE_SEARCH_DEBUG_LOGGING 0
#define VARIABLE_RANGE_CHECKS_ENABLED 1

// Game simulation
#define NUM_SIM_GAMES 1

// How the agent should play
#define USE_RANKS 0
#define USE_BASE_7_RANKS 1
#define CAN_TUCK 1
#define WELL_COLUMN 9
#define USE_RIGHT_WELL_FEATURES 1
#define PLAY_SAFE_PRE_KILLSCREEN 0
#define PLAY_SAFE_ON_KILLSCREEN 0
#define ALWAYS_LINEOUT 0
#define ALWAYS_LINEOUT_29 0
#define SHOULD_PLAY_PERFECT 0

// Engine API params
#define NUM_TOP_ENGINE_MOVES 5
#define DEFAULT_PLAYOUT_COUNT 49
#define DEFAULT_PLAYOUT_LENGTH 2
#define DEFAULT_PRUNING_BREADTH 20
#define TRACK_PLAYOUT_DETAILS true // Can disable for performance reasons

// Logistics of move search and pruning
#define LOCK_POSITION_REPEAT_CAP_PROPORTION .25 // Only used for current+next piece search. Refers to the limit on the percent of positions considered that can have the same first move. This increases the diversity of moves considered.
#define SEMI_HOLE_PROPORTION 0.6f // Value used for things that are sort of like holes but not fully, e.g. unfilled wells while digging
#define SEQUENCE_LENGTH 20
#define EXHAUSTIVE_SEQUENCE_LENGTH 4

#endif
