#include "utils.h"
/**
 * Calculates a lookup table for the Y value you'd be at after completing a specified number of inputs, and while being ready for the next input.
 * This is used in the tuck search, since this would be the first Y value where you could perform a tuck after N inputs of a standard placement.
 */
void getYValuesByNumInputs(char const *inputFrameTimeline, int gravity, int initialY, OUT int result[6]){
  int inputsPerformed = 0;
  int y = initialY;
  int frameIndex = 0;
  while (inputsPerformed <= 5) {
    int isInputFrame = shouldPerformInputsThisFrame(frameIndex, inputFrameTimeline);
    int isGravityFrame =
      frameIndex % gravity == gravity - 1;   // Returns true every Nth frame, where N = gravity
    if (isInputFrame) {
      result[inputsPerformed] = y;
      inputsPerformed++;
    }
    if (isGravityFrame) {
      y++;
    }
    frameIndex++;
  }
}
