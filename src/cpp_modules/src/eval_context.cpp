#include "eval_context.hpp"
#include <math.h>
#include <algorithm>
#include "config.hpp"

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

AiMode getAiMode(GameState gameState, int currentMax5TapHeight, int max5TapHeight29) {
  if ((ALWAYS_LINEOUT_29 && gameState.lines > 226) || currentMax5TapHeight < 4 || ALWAYS_LINEOUT) {
    return LINEOUT;
  }
  if (max5TapHeight29 < 2 && gameState.lines > 220 && gameState.level < 29) {
    if (hasHoleBlockingTetrisReady(gameState.board, gameState.surfaceArray[9])) {
      return DIRTY_NEAR_KILLSCREEN;
    }
    return NEAR_KILLSCREEN;
  }
  if (gameState.numTrueHoles >= 1) {
    return DIG;
  }
  // // Optionally play very safe on killscreen
  // if (PLAY_SAFE_ON_KILLSCREEN && gameState.level >= 29) {
  //   return SAFE;
  // }
  return STANDARD;
}

const EvalContext getEvalContext(GameState gameState, const PieceRangeContext pieceRangeContextLookup[]){
  EvalContext context = {};

  // Copy the piece range context from the global lookup
  bool gravityDoubled = isGravityDoubled(gameState.level);
  int pieceRangeContextIndex = gravityDoubled 
          ? 0 // double killscreen context is at index 0 of the array 
          : getGravity(gameState.level); // The rest are indexed by the gravity value
  context.pieceRangeContext = pieceRangeContextLookup[pieceRangeContextIndex];

  // Set the mode
  AiMode aiMode = getAiMode(gameState, context.pieceRangeContext.max5TapHeight, pieceRangeContextLookup[0].max5TapHeight);
  context.aiMode = aiMode;
  context.weights = getWeights(context.aiMode);

  // Set the scare heights
  if (aiMode == LINEOUT) {
    context.scareHeight = 0;
    context.maxSafeCol9 = -1;
  } else {
    bool lowerScareHeight = (PLAY_SAFE_PRE_KILLSCREEN && gameState.level < 29) || (PLAY_SAFE_ON_KILLSCREEN && gameState.level >= 29);
    float prelimScareHeight = context.pieceRangeContext.max5TapHeight - (lowerScareHeight ? 4 : 3);
    float prelimCol9 = context.pieceRangeContext.max4TapHeight - (lowerScareHeight ? 6 : 5);

    prelimScareHeight = prelimScareHeight * 0.5 + 6 * 0.5;
    prelimCol9 = prelimCol9 * 0.5 + 8 * 0.5;

    // FOR TESTING
    float ratio = 0;

    if (DOUBLE_KILLSCREEN_ENABLED){
      int cutoffLines = 320;
      int linearInterpolationLines = 10; // Slowly shift from the killscreen scare height to the double killscreen scare height
      if (gameState.lines > cutoffLines){
        int diff = std::min(linearInterpolationLines, gameState.lines - cutoffLines);
        ratio = (float)diff / (float)linearInterpolationLines;
      }
    }
    
    context.scareHeight = prelimScareHeight * (1.0 - ratio);
    context.maxSafeCol9 = prelimCol9 * (1.0 - ratio);
  }

  // Set the well column
  if (aiMode == LINEOUT) {
    context.wellColumn = -1;
  } else {
    context.wellColumn = WELL_COLUMN;
  }

  // Misc other properties
  context.maxDirtyTetrisHeight = 0;
  // context.countWellHoles = context.aiMode == DIG   // This turns out to not work in practice, it prevents filling the well to clear holes.
  context.countWellHoles = false;
  context.shouldRewardLineClears = (aiMode == LINEOUT || aiMode == DIRTY_NEAR_KILLSCREEN);

  return context;
}

