#ifndef TYPES
#define TYPES

#define FLOAT_EPSILON 0.000001

// Generic "not applicable" value for unsigned ints
#define NONE 999999999
// Not actually the minimum float but a generic "huge negative value" to use in maximization loops
#define FLOAT_MIN -999999999.0

// No-op used to mark output parameters
#define OUT

#undef max
#undef min

#define INITIAL_X 3

enum RequestType {
  GET_LOCK_VALUE_LOOKUP, // Gets a map of all the values for all possible places where the current piece could lock.
  GET_TOP_MOVES, // Gets a list of the top moves, using full playouts. Supports with or without next box.
  GET_TOP_MOVES_HYBRID, // Gets a list of the top moves *BOTH* with and without next box.
  RATE_MOVE, // Compares a player move to the best move, and gives the score for both, with and without next box.
  GET_MOVE // Gets a single best move for a given scenario, using full playouts. Supports with or without next box.
};

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
  unsigned int board[20];  // See board encoding details below
  int surfaceArray[10];
  float adjustedNumHoles; // A count of how many holes there are, with adjustments for the height of holes.
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

/** Minimal representation of an entire placement, such that the input sequence is deterministic from the data here. */
struct LockPlacement {
  int x;
  int y;
  int rotationIndex;
  int tuckFrame;
  char tuckInput;
  const Piece *piece;
};

const LockPlacement NO_PLACEMENT = {
  NONE, NONE, NONE, NONE, 'x', NULL
};

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
  PLAY_PERFECT
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
 * Considered "global" because the tapping speed does not change within the lifetime of one query to the C++ module
 * (whereas the eval context can change based on the AiMode).
 */
struct PieceRangeContext {
  char const *inputFrameTimeline;
  int yValueOfEachShift[7];
  int max4TapHeight;
  int max5TapHeight;
  int maxAccessibleLeft5Surface[10];
  int maxAccessibleRightSurface[10];
};

/**
 * A collection of meta-information that dictates how boards are evaluated.
 * Notably excludes any context that depends primarily on the tapping speed and level (which would be included in the global context)
 */
struct EvalContext {
  AiMode aiMode;
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
};

/** A data model for one playout within a series of such playouts*/
struct PlayoutData {
  float totalScore;
  std::vector<LockLocation> placements;
  std::string pieceSequence;
  unsigned int resultingBoard[20];
};

/** A data model for a move, as it relates to being part of an API response for the list of top moves */
struct EngineMoveData {
  LockLocation firstPlacement;
  LockLocation secondPlacement;
  float playoutScore;
  float evalScore;
  std::string resultingBoard;
  PlayoutData playout1; // Best case
  PlayoutData playout2; // 83 %ile
  PlayoutData playout3; // 66 %ile
  PlayoutData playout4; // Median case
  PlayoutData playout5; // 33 %ile
  PlayoutData playout6; // 16 %ile
  PlayoutData playout7; // Worst case
};

#endif
