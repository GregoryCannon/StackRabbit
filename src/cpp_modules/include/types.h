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

typedef struct {
  int x;
  int y;
  int rotationIndex;
  int frameIndex;
  Piece piece;
} SimState;

/**
 * The relative weights of all the eval factors.
 */
typedef struct {
  float avgHeightCoef;
  float burnCoef;
  float coveredWellCoef;
  float highCol9Coef;
  float holeCoef;
  float tetrisCoef;
  float spireHeightCoef;
  float surfaceCoef;
} FastEvalWeights;

/**
 * Any meta-information that might affect how things are evaluated.
 * e.g. how fast the agent can tap
 */
struct EvalContext {
  float scareHeight;
};

#endif