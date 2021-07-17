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

struct SimState {
  int x;
  int y;
  int rotationIndex;
  int frameIndex;
  Piece piece;
};

#endif