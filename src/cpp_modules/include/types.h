#ifndef TYPES
#define TYPES

struct Piece {
  char id;
  int index;
  int rowsByRotation[4][4];
  int topSurfaceByRotation[4][4];
  int bottomSurfaceByRotation[4][4];
  int maxYByRotation[4];
  int initialY;
};

/**
 * A representation of the overall state of the game, like a freeze frame before each piece spawns.
 */
struct GameState {
  int board[20];  // See board encoding details below
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
  Piece piece;
};

/** Minimal representation of a lock location. */
struct LockLocation {
  int x;
  int y;
  int rotationIndex;
};

enum AiMode {
  STANDARD,
  DIG,
  LINEOUT,
  NEAR_KILLSCREEN,
  DIRTY_NEAR_KILLSCREEN
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
  float tetrisCoef;
  float tetrisReadyCoef;
  float surfaceCoef;
};

/**
 * Any meta-information that might affect how things are evaluated.
 */
struct EvalContext {
  AiMode aiMode;
  int countWellHoles;
  FastEvalWeights weights;
  char const *inputFrameTimeline;
  float maxDirtyTetrisHeight;
  float maxSafeCol9;
  float scareHeight;
  int wellColumn; // Equals -1 if lining out
};


struct Depth2Possibility {
  LockLocation firstPlacement;
  LockLocation secondPlacement;
  GameState resultingState;
  float evalScore;
  float immediateReward;
};

#endif
