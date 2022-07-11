#ifndef TYPES
#define TYPES

#define FLOAT_EPSILON 0.000001

// Generic "not applicable" value for unsigned ints
#define NONE 999999999

// No-op used to mark output parameters
#define OUT

#undef max
#undef min



enum RequestType {
  GET_LOCK_VALUE_LOOKUP,
  PLAY_MOVE_NO_NEXT_BOX,
  CHOOSE_MOVE_FROM_LOCK_VALUES
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

const LockPlacement NO_PLACEMENT {
  NONE, NONE, NONE, NONE, 'x', NULL
};

/** Minimal representation of a lock location. */
struct LockLocation {
  int x;
  int y;
  int rotationIndex;
};

enum AiMode {
  STANDARD,
  SAFE,
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
  float evalScore;
  float immediateReward;
};

#endif
