#include "types.h"

int isLineout(GameState gameState){
  return false;
  // return gameState.level >= 29;
}

int hasHoleBlockingTetrisReady(int board[20], int col10Height){
  if (col10Height > 16) {
    return 0;
  }
  // Check that the four rows where a right well tetris would happen are all full except col 10
  for (int r = 0; r <= 4; r++) {
    if (board[19 - col10Height - r] & ALL_HOLE_BITS) {
      return true;
    }
  }
  return false;
}

AiMode getAiMode(GameState gameState) {
  if (isLineout(gameState)) {
    return LINEOUT;
  }
  if (gameState.lines > 220 && gameState.level < 29){
    if (hasHoleBlockingTetrisReady(gameState.board, gameState.surfaceArray[9])){
      return DIRTY_NEAR_KILLSCREEN;
    }
    return NEAR_KILLSCREEN;
  }
  if (gameState.adjustedNumHoles >= 1) {
    return DIG;
  }
  return STANDARD;
}

const EvalContext getEvalContext(GameState gameState, char const *inputFrameTimeline){
  EvalContext context = DEBUG_CONTEXT;

  // Set the mode
  context.aiMode = getAiMode(gameState);
  context.inputFrameTimeline = inputFrameTimeline;
  context.weights = getWeights(context.aiMode);

  // Set the scare heights
  if (context.aiMode == LINEOUT) {
    context.scareHeight = 0;
    context.maxSafeCol9 = -1;
  } else if (gameState.level <= 18) {
    context.scareHeight = 8;
    context.maxSafeCol9 = 9;
  } else if (gameState.level < 29) {
    context.scareHeight = 5;
    context.maxSafeCol9 = 6;
  } else {
    context.scareHeight = 3;
    context.maxSafeCol9 = 4;
  }

  // Set the well column
  if (context.aiMode == LINEOUT) {
    context.wellColumn = -1;
  }
  return context;
}
