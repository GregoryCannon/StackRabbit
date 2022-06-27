#ifndef CONFIG
#define CONFIG

// Logging
#define LOGGING_ENABLED 0
#define PLAYOUT_LOGGING_ENABLED 0
#define SIMULATION_LOGGING_ENABLED 0
#define PLAYOUT_RESULT_LOGGING_ENABLED 0
#define MOVE_SEARCH_DEBUG_LOGGING 0
#define VARIABLE_RANGE_CHECKS_ENABLED 0

#define NUM_SIM_GAMES 3

// How the agent should play
#define USE_RANKS 0
#define CAN_TUCK 1
#define WELL_COLUMN 0
#define USE_RIGHT_WELL_FEATURES 0
#define PLAY_SAFE_PRE_KILLSCREEN 1
#define PLAY_SAFE_ON_KILLSCREEN 0
#define ALWAYS_LINEOUT 0

// Logistics of move search and pruning
#define DEPTH_2_PRUNING_BREADTH 12
#define DEPTH_1_PRUNING_BREADTH 8
#define LOCK_POSITION_REPEAT_CAP 3
#define TUCK_SETUP_HOLE_PROPORTION 0.81f
#define SEQUENCE_LENGTH 20

// Playouts
#define NUM_PLAYOUTS_LONG 100
#define PLAYOUT_LENGTH_LONG 6
#define NUM_PLAYOUTS_SHORT 0
#define PLAYOUT_LENGTH_SHORT 3

// Currently not used since the simulations use the exact same call as regular pipeline
//#define SIM_NUM_PLAYOUTS 20
//#define SIM_PLAYOUT_LENGTH 1

#endif
