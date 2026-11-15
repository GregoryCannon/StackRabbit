#ifndef TYPES
#define TYPES

#define FLOAT_EPSILON 0.0001

// Generic "not applicable" value for unsigned ints
#define NONE 999999999
// Not actually the minimum float but a generic "huge negative value" to use in maximization loops
#define FLOAT_MIN -999999999.0

// No-op used to mark output parameters
#define OUT

#undef max
#undef min

#define INITIAL_X 3

#define NO_TUCK_NOTATION '.'

#include <chrono>
#include <string>
#include <vector>

enum RequestType {
  GET_LOCK_VALUE_LOOKUP, // Gets a map of all the values for all possible places where the current
                         // piece could lock.
  GET_LOCK_VALUE_LOOKUP_DAS, // Similar to GET_LOCK_VALUE_LOOKUP, but also returns a map of the DAS
                             // penalties for each placement
  GET_TOP_MOVES, // Gets a list of the top moves, using full playouts. Supports with or without next
                 // box.
  GET_TOP_MOVES_HYBRID, // Gets a list of the top moves *BOTH* with and without next box.
  RATE_MOVE, // Compares a player move to the best move, and gives the score for both, with and
             // without next box.
  GET_MOVE,  // Gets a single best move for a given scenario, using full playouts. Supports with or
             // without next box.
  GET_MOVE_AB_TEST, // Compares the result of GET_MOVE between the set of main weights and test
                    // weights.
  PRECOMPUTE, // Chooses the current piece's placement by how well each reachable mid-air adjustment
              // leads into every possible next piece. Input: the standard 11-field pipe string plus
              // a reactionTime field (field 11); nextPiece is unused (-1).
  PRECOMPUTE_BACKUP // The backup response for the live game server: the same pipe format and move
                    // search as PRECOMPUTE, answered with the best-eval placement instead of the
                    // full pipeline. Served when the full precompute has not finished by poll
                    // time.
};

// Whether a request type speaks the precompute pipe format: the standard 10
// fields plus reactionTime as field 11 (nextPiece is required but unused).
inline bool isPrecomputePipeRequest(RequestType req) {
  return req == PRECOMPUTE || req == PRECOMPUTE_BACKUP;
}

// Whether a request type needs per-move eval explanations.
// Only GET_TOP_MOVES, GET_TOP_MOVES_HYBRID, and GET_MOVE_AB_TEST populate
// evalExplanation; all other request types skip the extra std::string work.
inline bool needExplanation(RequestType req) {
  return req == GET_TOP_MOVES || req == GET_TOP_MOVES_HYBRID || req == GET_MOVE_AB_TEST;
}

struct Piece {
  char id;
  int index;
  unsigned int rowsByRotation[4][4];
  unsigned int topSurfaceByRotation[4][4];
  unsigned int bottomSurfaceByRotation[4][4];
  int maxYByRotation[4];
  int initialY;
};

/**
 * A representation of the overall state of the game, like a freeze frame before each piece spawns.
 */
struct GameState {
  unsigned int board[20]; // See board encoding details below
  int surfaceArray[10];
  int numTrueHoles; // A count of many hole cells there are (not incl. tuck setups, etc).
                    // Triple-high holes do count as 3 holes.
  float
      numPartialHoles; // A count of how many semi-holes there are, e.g. tuck setups, covered wells,
  int lines;
  int level;
};

/* Board encoding:
   Each row is a 32-bit integer, with the encoding as follows:

   c 0 tttttttttt hhhhhhhhhh bbbbbbbbbb

   b = one cell in the row (whether it's filled)
   h = whether each cell is a hole
   t = whether each cell is a tuck setup
   c = whether the row is guaranteed to be burned
 */

/**
 * The internal state of a move search simulation
 * (looping through pretend frames and seeing how the piece can move).
 */
struct SimState {
  int x;
  int y;
  int rotationIndex;
  int frameIndex;
  int arrIndex;
  const Piece *piece;
};

/** Minimal representation of an entire placement, such that the input sequence is deterministic
 * from the data here. */
struct LockPlacement {
  int x;
  int y;
  int rotationIndex;
  int tuckFrame;
  char tuckInput;
  const Piece *piece;
};

const LockPlacement NO_PLACEMENT = {NONE, NONE, NONE, NONE, 'x', NULL};

/** Minimal representation of a lock location. */
struct LockLocation {
  int x;
  int y;
  int rotationIndex;
};

const LockLocation NULL_LOCK_LOCATION = {NONE, NONE, NONE};

enum AiMode {
  STANDARD,
  DIG,
  LINEOUT,
  NEAR_KILLSCREEN,
  DIRTY_NEAR_KILLSCREEN,
};

/**
 * How aggressively the agent values burn potential.
 * Overrides the burnCoef of MAIN_WEIGHTS-derived weight sets.
 */
enum AggressionLevel {
  SAFE,
  AGGRO,
  HYPER,
};

/**
 * The relative weights of all the eval factors.
 */
struct FastEvalWeights {
  float avgHeightCoef;
  float builtOutLeftCoef;
  float burnCoef;
  float coveredWellCoef;
  float col9Coef;
  float deathCoef;
  float extremeGapCoef;
  float holeCoef;
  float holeWeightCoef;
  float inaccessibleLeftCoef;
  float inaccessibleRightCoef;
  float tetrisCoef;
  float tetrisReadyCoef;
  float surfaceCoef;
  float surfaceLeftCoef;
  float unableToBurnCoef;
};

/**
 * Precomputed meta-information related to tapping speed and piece reachability.
 * Considered "global" because the tapping speed does not change within the lifetime of one query to
 * the C++ module (whereas the eval context can change based on the AiMode).
 */
struct PieceRangeContext {
  char const *inputFrameTimeline;
  int yValueOfEachShift[7];
  int max4TapHeight;
  int max5TapHeight;
  int maxAccessibleLeft5Surface[10];
  int maxAccessibleRightSurface[10];
  int maxAccessibleLeftLPieceSurface[10];
};

/**
 * A collection of meta-information that dictates how boards are evaluated.
 * Notably excludes any context that depends primarily on the tapping speed and level (which would
 * be included in the global context)
 */
struct EvalContext {
  AiMode aiMode;
  AggressionLevel aggressionLevel;
  FastEvalWeights weights;
  PieceRangeContext pieceRangeContext;
  int countWellHoles;
  float maxDirtyTetrisHeight;
  float maxSafeCol9;
  float scareHeight;
  int shouldRewardLineClears;
  int wellColumn; // Equals -1 if lining out
};

struct Possibility {
  LockLocation firstPlacement;
  LockLocation secondPlacement; // Can be null if it's actually depth 1
  GameState resultingState;
  float evalScoreInclReward;
  float immediateReward;
  std::string
      evalExplanation; // Non-zero factor breakdown; only populated when explanations are requested
};

/** A data model for one playout within a series of such playouts*/
struct PlayoutData {
  float totalScore;
  std::vector<LockLocation> placements;
  std::string pieceSequence;
  unsigned int resultingBoard[20];
};

/** A data model for a move, as it relates to being part of an API response for the list of top
 * moves */
struct EngineMoveData {
  LockLocation firstPlacement;
  LockLocation secondPlacement;
  float playoutScore;
  float evalScore;
  std::string resultingBoard;
  PlayoutData playout1;        // Best case
  PlayoutData playout2;        // 83 %ile
  PlayoutData playout3;        // 66 %ile
  PlayoutData playout4;        // Median case
  PlayoutData playout5;        // 33 %ile
  PlayoutData playout6;        // 16 %ile
  PlayoutData playout7;        // Worst case
  std::string evalExplanation; // Concatenated non‑zero factor contributions
  bool isPlayerMove; // True when this move matches the player's move (from playerBoardAfter)
};

enum SearchBudgetMode {
  BUDGET_PLAYOUT_COUNT,
  BUDGET_TIME_MS,
};

/**
 * Explicit playout-count vs millisecond wall-clock budget for tournament/lookup searches.
 * Avoids overloading playoutCount inside internal engine functions.
 */
struct SearchBudget {
  SearchBudgetMode mode;
  int pruningBreadth; // >= 0 for legacy, -1 for fixed-playout tournament
  int playoutCount;   // Playouts when mode == BUDGET_PLAYOUT_COUNT
  int timeBudgetMs;   // Millisecond budget when mode == BUDGET_TIME_MS
  bool hasDeadline;
  std::chrono::steady_clock::time_point deadline;
  double calibratedMovesPerMs; // 0 = fall back to built-in conservative default

  static SearchBudget fromPlayoutCount(int pruningBreadth, int playoutCount) {
    SearchBudget b{};
    b.mode = BUDGET_PLAYOUT_COUNT;
    b.pruningBreadth = pruningBreadth;
    b.playoutCount = playoutCount;
    return b;
  }

  static SearchBudget fromTimeMs(int timeBudgetMs, double calibratedMovesPerMs = 0.0) {
    SearchBudget b{};
    b.mode = BUDGET_TIME_MS;
    b.pruningBreadth = -2;
    b.timeBudgetMs = timeBudgetMs;
    b.calibratedMovesPerMs = calibratedMovesPerMs;
    return b;
  }

  static SearchBudget fromDeadline(int timeBudgetMs, std::chrono::steady_clock::time_point deadline,
                                   double calibratedMovesPerMs = 0.0) {
    SearchBudget b = fromTimeMs(timeBudgetMs, calibratedMovesPerMs);
    b.hasDeadline = true;
    b.deadline = deadline;
    return b;
  }

  static SearchBudget fromWire(int pruningBreadth, int playoutCount) {
    if (pruningBreadth == -2) {
      return fromTimeMs(playoutCount);
    }
    return fromPlayoutCount(pruningBreadth, playoutCount);
  }
};

/**
 * Caller-supplied timing context for timed requests (used by live_game_server).
 */
struct EngineTimingContext {
  bool hasRequestReceived = false;
  std::chrono::steady_clock::time_point requestReceived{};
  double calibratedMovesPerMs7T = 0.0; // Baseline rate across 7 concurrent precompute threads
  double calibratedMovesPerMs1T = 0.0; // Baseline rate for single-threaded lookups
};

#endif
