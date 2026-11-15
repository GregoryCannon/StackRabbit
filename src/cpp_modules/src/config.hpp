#ifndef CONFIG
#define CONFIG

// ROM configuration
#define DOUBLE_KILLSCREEN_ENABLED 0
#define DEBUG_DOUBLE_KS_ALWAYS_ENABLED 0

// Logging
#define LOGGING_ENABLED 0 // Refers to movesearch/eval logging
#define PLAYOUT_LOGGING_ENABLED 0
#define TMT_LOGGING_ENABLED                                                                        \
  0 // Round-by-round tournament candidate logging (see tournament_search.cpp)
#define SIMULATION_LOGGING_ENABLED 0
#define PLAYOUT_RESULT_LOGGING_ENABLED 0
#define MOVE_SEARCH_DEBUG_LOGGING 0
#define VARIABLE_RANGE_CHECKS_ENABLED 1

// How the agent should play
#define USE_BASE_7_RANKS 1
#define CAN_TUCK 1
#define WELL_COLUMN 9
#define USE_RIGHT_WELL_FEATURES 1
#define ALWAYS_LINEOUT 0
#define ALWAYS_LINEOUT_29 0
#define SHOULD_PLAY_PERFECT 0

// Search Depth
#define DEFAULT_PLAYOUT_COUNT 300
#define DEFAULT_PLAYOUT_LENGTH 6
#define DEFAULT_PRUNING_BREADTH 12

// Maximum DAS charge (only reachable via a wall charge; holding caps at 15).
// See docs/DAS_explainer.md. Used as the "full charge" first-piece
// starting charge for the DAS bucket searches.
#define DAS_CHARGE_MAX 16

// Engine API params
#define NUM_ENGINE_MOVES 8
#define NUM_ENGINE_MOVES_ADJUSTMENTS 3
// Playout-volume cap for a single request. In tournament mode (pruningBreadth == -1 or -2) the
// wire count field isn't multiplied by playoutLength (budget count or milliseconds respectively).
#define MAX_CPP_PLAYOUT_MOVES 99999999LL
// Conservative fallback speed (playout moves / ms) used for -2 timed mode when no startup
// calibration is provided by the host (e.g., direct CLI/addon calls or unit tests).
#define DEFAULT_CALIBRATED_MOVES_PER_MS 180.0
// Safety reserve (ms) subtracted from precompute deadlines for the final collapse/formatting +
// HTTP margin after the concurrent lock value lookups join.
#define PRECOMPUTE_COLLAPSE_RESERVE_MS 2

// Whether precompute responses apply the push-down input rewrite to per-piece lines
// (only ever in combination with level 18; see the precompute pipeline).
#define SHOULD_PUSHDOWN 0

// Logistics of move search and pruning
#define LOCK_POSITION_REPEAT_CAP                                                                   \
  3 // Only used for current+next piece search. Refers to the limit on the percent of positions
    // considered that can have the same first move. This increases the diversity of moves
    // considered.
#define SEMI_HOLE_PROPORTION                                                                       \
  0.6f // Value used for things that are sort of like holes but not fully, e.g. unfilled wells while
       // digging

// Piece sequences
#define SEQUENCE_LENGTH 20
#define EXHAUSTIVE_SEQUENCE_LENGTH 4
#define EXHAUSTIVE_SEQUENCE_COUNT                                                                  \
  2401 // All 7^4 piece sequences of length 4, lexicographically ordered (their prefixes serve as
       // the exhaustive lists for playout lengths 1-4).
#define CANONICAL_SEQUENCE_BATCH_SIZE                                                              \
  8000 // Rows per canonical piece-sequence batch (see canonical_sequences.hpp). Playout counts get
       // clamped to this.

#endif
