#include "eval_context.hpp"
#include <math.h>

// Unused
//const EvalContext DEBUG_CONTEXT = {
//  /* aiMode= */ STANDARD,
//  /* fastEvalWeights= */ MAIN_WEIGHTS,
//  /* pieceRangeContext= */ {},
//  /* countWellHoles= */ false,
//  /* maxDirtyTetrisHeight= */ 1,
//  /* maxSafeCol9Height= */ 6,
//  /* scareHeight= */ 5,
//  /* shouldRewardLineClears= */ false,
//  /* wellColumn= */ 9,
//};

int hasHoleBlockingTetrisReady(unsigned int board[20], int col10Height){
  if (col10Height > 16) {
    return 0;
  }
  // Check that the four rows where a right well tetris would happen have no holes
  for (int r = 0; r <= 4; r++) {
    if (board[19 - col10Height - r] & ALL_HOLE_BITS) {
      return true;
    }
  }
  return false;
}

/** Gets the number of holes that are holes and not tuck setups*/
int getNumTrueHoles(float adjustedNumHoles){
  while (std::abs(adjustedNumHoles - round(adjustedNumHoles)) > FLOAT_EPSILON) {
    adjustedNumHoles -= TUCK_SETUP_HOLE_PROPORTION;
  }
  return (int) adjustedNumHoles;
}

AiMode getAiMode(GameState gameState, int currentMax5TapHeight, int max5TapHeight29) {
  if (gameState.lines > 226 || currentMax5TapHeight < 4 || ALWAYS_LINEOUT) {
    return LINEOUT;
  }
  if (max5TapHeight29 < 2 && gameState.lines > 220 && gameState.level < 29) {
    if (hasHoleBlockingTetrisReady(gameState.board, gameState.surfaceArray[9])) {
      return DIRTY_NEAR_KILLSCREEN;
    }
    return NEAR_KILLSCREEN;
  }
  if (getNumTrueHoles(gameState.adjustedNumHoles) >= 1) {
    return DIG;
  }
  // Optionally play very safe on killscreen
  if (PLAY_SAFE_ON_KILLSCREEN && gameState.level >= 29) {
    return SAFE;
  }
  return STANDARD;
}

const EvalContext getEvalContext(GameState gameState, const PieceRangeContext pieceRangeContextLookup[]){
  EvalContext context = {};

  // Copy the piece range context from the global lookup
  context.pieceRangeContext = pieceRangeContextLookup[getGravity(gameState.level) - 1];

  // Set the mode
  AiMode aiMode = getAiMode(gameState, context.pieceRangeContext.max5TapHeight, pieceRangeContextLookup[0].max5TapHeight);
  context.aiMode = aiMode;
  context.weights = getWeights(context.aiMode);

  // Set the scare heights
  if (aiMode == LINEOUT) {
    context.scareHeight = 0;
    context.maxSafeCol9 = -1;
  } else {
    int lowerScareHeight = (PLAY_SAFE_PRE_KILLSCREEN && gameState.level < 29) || (PLAY_SAFE_ON_KILLSCREEN && gameState.level >= 29);
    context.scareHeight = context.pieceRangeContext.max5TapHeight - (lowerScareHeight ? 4 : 3);
    context.maxSafeCol9 = context.pieceRangeContext.max4TapHeight - (lowerScareHeight ? 6 : 5);

    context.scareHeight = context.scareHeight * 0.7 + 6 * 0.3;
    context.maxSafeCol9 = context.maxSafeCol9 * 0.7 + 8 * 0.3;
  }

  // Set the well column
  if (aiMode == LINEOUT) {
    context.wellColumn = -1;
  } else {
    context.wellColumn = WELL_COLUMN;
  }

  // Misc other properties
  context.maxDirtyTetrisHeight = 0;
  // context.countWellHoles = context.aiMode == DIG;
  context.countWellHoles = SHOULD_PLAY_PERFECT;
  context.shouldRewardLineClears = (aiMode == LINEOUT || aiMode == DIRTY_NEAR_KILLSCREEN);

  return context;
}

