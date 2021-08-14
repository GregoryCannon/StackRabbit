#include "types.h"

int isLineout(GameState gameState){
  return gameState.level >= 29;
}

AiMode getAiMode(GameState gameState) {
  if (isLineout(gameState)) {
    return LINEOUT;
  }
  if (gameState.adjustedNumHoles > 0) {
    return DIG;
  }
  return STANDARD;
}

EvalContext getEvalContext(GameState gameState){
  EvalContext context = DEBUG_CONTEXT;

  // Set the mode
  context.aiMode = getAiMode(gameState);

  // Set the scare heights
  if (context.aiMode == LINEOUT) {
    context.scareHeight = 0;
    context.maxSafeCol9 = -1;
  } else if (gameState.level <= 18) {
    context.scareHeight = 8;
    context.maxSafeCol9 = 9;
  } else {
    context.scareHeight = 5;
    context.maxSafeCol9 = 6;
  }

  // Set the well column
  if (context.aiMode == LINEOUT) {
    context.wellColumn = -1;
  }
  return context;
}
