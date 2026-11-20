#include "eval_context.hpp"
#include "config.hpp"
#include "params.hpp"
#include <algorithm>

AiMode getAiMode(const GameState &gameState, int currentMax5TapHeight) {
  if ((ALWAYS_LINEOUT_29 && gameState.lines > 226) || currentMax5TapHeight < 4 || ALWAYS_LINEOUT) {
    return LINEOUT;
  }
  if (gameState.numTrueHoles >= 1) {
    return DIG;
  }
  return STANDARD;
}

AiMode getAiMode(const GameState &gameState, const PieceRangeContext pieceRangeContextLookup[]) {
  int idx = isGravityDoubled(gameState.level) ? 0 : getGravity(gameState.level);
  return getAiMode(gameState, pieceRangeContextLookup[idx].max5TapHeight);
}

const EvalContext getEvalContext(GameState gameState,
                                 const PieceRangeContext pieceRangeContextLookup[],
                                 bool useTestWeights, AggressionLevel aggressionLevel) {
  EvalContext context = {};

  // Copy the piece range context from the global lookup
  bool gravityDoubled = isGravityDoubled(gameState.level);
  int pieceRangeContextIndex =
      gravityDoubled ? 0 // double killscreen context is at index 0 of the array
                     : getGravity(gameState.level); // The rest are indexed by the gravity value
  context.pieceRangeContext = pieceRangeContextLookup[pieceRangeContextIndex];

  // Set the mode
  AiMode aiMode = getAiMode(gameState, context.pieceRangeContext.max5TapHeight);
  context.aiMode = aiMode;
  context.aggressionLevel = aggressionLevel;
  context.weights = getWeights(context.aiMode, useTestWeights, aggressionLevel);
  // Index 1 holds the level 29 (1-frame-per-row gravity) piece ranges.
  context.lineoutAt29 =
      ALWAYS_LINEOUT_29 || pieceRangeContextLookup[1].max5TapHeight < 4 || ALWAYS_LINEOUT;

  // Set the scare heights
  if (aiMode == LINEOUT) {
    context.scareHeight = 0;
    context.maxSafeCol9 = -1;
  } else {
    float prelimScareHeight =
        context.pieceRangeContext.max5TapHeight - 3;                // formerly 4 if play_safe
    float prelimCol9 = context.pieceRangeContext.max4TapHeight - 5; // formerly 6 if play_safe

    prelimScareHeight = prelimScareHeight * 0.5 + 6 * 0.5;
    prelimCol9 = prelimCol9 * 0.5 + 8 * 0.5;

    // FOR TESTING
    float ratio = 0;

    if (DOUBLE_KILLSCREEN_ENABLED) {
      int cutoffLines = 320;
      int linearInterpolationLines =
          10; // Slowly shift from the killscreen scare height to the double killscreen scare height
      if (gameState.lines > cutoffLines) {
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
  // context.countWellHoles = context.aiMode == DIG   // This turns out to not work in practice, it
  // prevents filling the well to clear holes.
  context.countWellHoles = false;
  context.shouldRewardLineClears = (aiMode == LINEOUT);

  return context;
}
