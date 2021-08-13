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
  int board[20];  // Each int represent a row, where the last 10 bits of the int represent the cells.
  int surfaceArray[10];
  int adjustedNumHoles; // A count of how many holes there are, with adjustments for the height of holes.
  int lines;
  int level;
};

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
  DIG
};

/**
 * The relative weights of all the eval factors.
 */
struct FastEvalWeights {
  float avgHeightCoef;
  float avgHeightExponent;
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
 * e.g. how fast the agent can tap
 */
struct EvalContext {
  AiMode aiMode;
  int countWellHoles;
  char const * inputFrameTimeline;
  float maxDirtyTetrisHeight;
  float maxSafeCol9;
  float scareHeight;
  int wellColumn;
};

struct FastEvalResult {
  float evalScore;
  GameState resultingState;
};

struct Depth2Possibility {
  LockLocation firstPlacement;
  LockLocation secondPlacement;
  GameState resultingState;
  float evalScore;
  float immediateReward;
};

#endif
