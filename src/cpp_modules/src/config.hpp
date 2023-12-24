#ifndef CONFIG
#define CONFIG

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
#define CAN_TUCK 1
#define WELL_COLUMN 9
#define USE_RIGHT_WELL_FEATURES 1
#define PLAY_SAFE_PRE_KILLSCREEN 1
#define PLAY_SAFE_ON_KILLSCREEN 0
#define ALWAYS_LINEOUT 0
#define ALWAYS_LINEOUT_29 0
#define SHOULD_PLAY_PERFECT 0

// Engine API params
#define NUM_TOP_ENGINE_MOVES 8
#define DEFAULT_PLAYOUT_COUNT 49
#define DEFAULT_PLAYOUT_LENGTH 2

// Logistics of move search and pruning
#define DEPTH_2_PRUNING_BREADTH 12
#define DEPTH_1_PRUNING_BREADTH 8
#define LOCK_POSITION_REPEAT_CAP 3
#define SEMI_HOLE_PROPORTION 0.81f // Value used for things that are sort of like holes but not fully (tuck setups, unfilled wells while digging)
#define SEQUENCE_LENGTH 20

#endif
