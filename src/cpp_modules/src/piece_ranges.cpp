#include "piece_ranges.hpp"

xtable getRangeXTable() {
  xtable table = {};
  for (int p = 0; p < 7; p++) {
    for (int rot = 0; rot < 4; rot++) {
      for (int x = -3; x <= 8;
           x++) { // Lowest possible x is -2, highest possible is 7. Then one extra on either side.
        int tableX = x + X_BOUNDS_COLLISION_TABLE_OFFSET;
        table[p][rot][tableX] = 0; // No collision by default
        for (int row = 0; row < 4; row++) {
          if (SHIFTBY(PIECE_LIST[p].rowsByRotation[rot][row], x) >= 1024) {
            table[p][rot][tableX] = 1; // Mark the collision
            break;
          }
          if (SHIFTBY(PIECE_LIST[p].rowsByRotation[rot][row], x - 1) & 1) {
            table[p][rot][tableX] = 1; // Mark the collision
            break;
          }
        }
      }
    }
  }
  return table;
}

const xtable X_BOUNDS_COLLISION_TABLE = getRangeXTable();

/**
 * Calculates a lookup table for the Y value you'd be at while doing shift number N.
 * This is used in the tuck search, since this would be the first Y value where you could perform a tuck after N inputs of a standard placement.
 */
void computeYValueOfEachShift(char const *inputFrameTimeline, int gravity, int initialY, OUT int result[7]){
  int inputsPerformed = 0;
  int y = initialY;
  int frameIndex = 0;
  while (inputsPerformed <= 5) {
    int isInputFrame = shouldPerformInputsThisFrame(frameIndex, inputFrameTimeline);
    int isGravityFrame =
      frameIndex % gravity == gravity - 1;   // Returns true every Nth frame, where N = gravity
    if (isInputFrame) {
      result[inputsPerformed + 1] = y;
      inputsPerformed++;
    }
    if (isGravityFrame) {
      y++;
    }
    frameIndex++;
  }
}

const PieceRangeContext getPieceRangeContext(char const *inputFrameTimeline, int gravity){
  PieceRangeContext context = {};
  
  context.inputFrameTimeline = inputFrameTimeline;
  computeYValueOfEachShift(inputFrameTimeline, gravity, -1, OUT context.yValueOfEachShift);
  context.max4TapHeight = 17 - context.yValueOfEachShift[4]; // 17 is the surface height of a square/long bar when y=0
  context.max5TapHeight = 17 - context.yValueOfEachShift[5];
  
  // Calculate inaccessible left 5-tap surface
  for (int i = 0; i < 10; i++){
    context.maxAccessibleLeft5Surface[i] = 20;
    context.maxAccessibleRightSurface[i] = 20;
  }
  unsigned int const *bottomSurface = PIECE_T.bottomSurfaceByRotation[3];

  for (int tapIndex = 0; tapIndex < 5; tapIndex++){
    // Superimpose the piece's bottom surface on the inaccessible surface
    for (int i = 0; i < 4; i++) {
      if (bottomSurface[i] == NONE){
        continue;
      }
      int xBeforeShift = SPAWN_X - tapIndex;
      int shiftY = context.yValueOfEachShift[tapIndex + 1];
      // printf("tap=%d, i=%d, xAfter=%d, yBefore=%d\n", tapIndex, i, xBeforeShift, shiftY);
      context.maxAccessibleLeft5Surface[xBeforeShift + i] = 19 - bottomSurface[i] - shiftY - PIECE_T.initialY;
    }
  }
  context.maxAccessibleLeft5Surface[0] = context.maxAccessibleLeft5Surface[1]; // The 5th shift is the last, so it just needs to resolve before/after (which is at the same Y value)
  
  bottomSurface = PIECE_I.bottomSurfaceByRotation[1];
  for (int tapIndex = 0; tapIndex < 4; tapIndex++){
    // Superimpose the piece's bottom surface on the inaccessible surface
    for (int i = 0; i < 4; i++) {
      if (bottomSurface[i] == NONE){
        continue;
      }
      int xBeforeShift = SPAWN_X + tapIndex;
      int shiftY = context.yValueOfEachShift[tapIndex + 1];
      // printf("tap=%d, i=%d, xAfter=%d, yBefore=%d\n", tapIndex, i, xBeforeShift, shiftY);
      context.maxAccessibleRightSurface[xBeforeShift + i] = 19 - bottomSurface[i] - shiftY - PIECE_I.initialY;
    }
  }
  context.maxAccessibleRightSurface[9] = context.maxAccessibleRightSurface[8]; // The 4th shift is the last, so it just needs to resolve before/after (which is at the same Y value)
  
  // printArray(context.maxAccessibleRightSurface, 10, "MAX RIGHT SURFACE");
  
  return context;
}
