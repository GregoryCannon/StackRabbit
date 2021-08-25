#include "utils.h"

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
  
  return context;
}
