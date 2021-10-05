#include "eval.hpp"
#include "move_result.hpp"
#include "utils.hpp"
#include "../data/ranks_output.hpp"
#include <math.h>
#include <vector>
using namespace std;

/**
 * A crude way to evaluate a surface for when I'm debugging and don't want to load the surfaces every time I
 * run.
 */
float calculateFlatness(int surfaceArray[10], int wellColumn) {
  float score = 30;
  for (int i = 0; i < 9; i++) {
    if (i == wellColumn || i+1 == wellColumn) {
      continue;
    }
    int diff = surfaceArray[i + 1] - surfaceArray[i];
    // Correct for double wells
    if (i == 7 && wellColumn == 9 && diff < -2) {
      diff = -2;
    }
    // Punish based on the absolute value of the column differences
    if (diff != 0) {
      score -= pow(abs(diff), 1.5);
    }
    // Line dependency
    if (diff >= 3 && (i == 0 || surfaceArray[i-1] + surfaceArray[i] >= 3)) {
      score -= 20;
    }
  }
  return score;
}

/** Gets the value of a surface. */
float rateSurface(int surfaceArray[10], const EvalContext *evalContext) {
  int wellColumn = evalContext->wellColumn;
  if (USE_RANKS) {
    // Convert the surface array into the custom base-9 encoding
    int index = 0;
    int excessGap = 0;
    for (int i = 0; i < 8; i++) {
      int diff = surfaceArray[i + 1] - surfaceArray[i];
      int absDiff = abs(diff);
      // Correct for double wells
      if (i == 7 && wellColumn == 9 && absDiff > 2) {
        absDiff = 2;
      } else if (absDiff > 4) {
        excessGap += absDiff - 4;
        diff = diff > 0 ? 4 : -4;
      }
      index *= 9;
      index += diff + 4;
    }
    // Make lower ranks more punishing
    float rawScore = surfaceRanksRaw[index] * 0.1 + (excessGap * evalContext->weights.extremeGapCoef);
    return rawScore - (70 / max(3.0f, rawScore));
  }
  // If the ranks aren't loaded, use the flatness score
  return calculateFlatness(surfaceArray, wellColumn);
}

float getAverageHeight(int surfaceArray[10], int wellColumn) {
  float avgHeight = 0;
  float weight = wellColumn >= 0 ? 0.1 : 0.111111;
  for (int i = 0; i < 10; i++) {
    if (i == wellColumn) {
      continue;
    }
    avgHeight += surfaceArray[i] * weight;
  }
  return avgHeight;
}

float getAverageHeightFactor(int avgHeight, float scareHeight) {
  float diff = max(0.0f, avgHeight - scareHeight);
  return diff * diff;
}

float getBuiltOutLeftFactor(int surfaceArray[10], int board[20], float avgHeight, float scareHeight) {
  float heightRatio = avgHeight / max(3.0f, scareHeight);
  float heightDiff = surfaceArray[0] - avgHeight;
  // Handle low left cases first
  if (heightDiff < 0) {
    float softenedHeightRatio = 0.5f * (heightRatio + 1); // Average it with 1 to make it less extreme (faster than sqrt operation)
    return -0.5 * heightDiff * heightDiff * softenedHeightRatio; // Approximate (heightDiff ^ 1.5) as (heightDiff * heightDiff * 0.5)
  }
  // Check for holes (don't reward building out the left over holes)
  int r = 21 - surfaceArray[0];
  while (r < 20) {
    if (!(board[r] & (1 << 9))) {
      return 0;
    }
    r++;
  }
  // Reward built out left
  return heightRatio * heightDiff;
}

float getLeftSurfaceFactor(int board[20], int surfaceArray[10], int max5TapHeight){
  max5TapHeight = max(0, max5TapHeight);
  for (int r = 20 - surfaceArray[0]; r < 20; r++) {
    if (board[r] & HOLE_BIT(0)) {
      return -3;
    }
  }
  if (surfaceArray[1] > max5TapHeight && surfaceArray[1] > surfaceArray[0]) {
    return surfaceArray[0] - surfaceArray[1];
  }
  return 0;
}

float getCol9Factor(int col9Height, float maxSafeCol9Height){
  if (col9Height <= maxSafeCol9Height) {
    return 0;
  }
  float diff = col9Height - maxSafeCol9Height;
  return diff * diff;
}

float getCoveredWellFactor(int board[20], int wellColumn, float scareHeight) {
  if (wellColumn == -1) {
    return 0;
  }
  int mask = (1 << (9 - wellColumn));
  for (int r = 0; r < 20; r++) {
    if (board[r] & mask) {
      int difficultyMultiplier = (board[r] & (ALL_HOLE_BITS | ALL_TUCK_SETUP_BITS)) > 0 ? 10 : 1;
      float heightRatio = (20.0f - r) / max(3.0f, scareHeight);
      return heightRatio * heightRatio * heightRatio * difficultyMultiplier;
    }
  }
  return 0;
}

float getGuaranteedBurnsFactor(int board[20], int wellColumn) {
  // Neither of these measures make sense in lineout mode, so don't calculate this factor
  if (wellColumn == -1) {
    return 0;
  }
  int wellMask = (1 << (9 - wellColumn));
  int guaranteedBurns = 0;
  for (int r = 0; r < 20; r++) {
    if ((board[r] & wellMask) || (board[r] & HOLE_WEIGHT_BIT)) {
      guaranteedBurns++;
    }
  }
  return guaranteedBurns;
}

float getLikelyBurnsFactor(int surfaceArray[10], int wellColumn, int maxSafeCol9) {
  if (wellColumn != 9) {
    return 0;
  }
  int col9 = surfaceArray[8];
  int col8 = surfaceArray[7];
  int lowestGoodColumn9 = min(maxSafeCol9, col8 - 2);
  if (col9 >= lowestGoodColumn9) {
    return 0;
  }
  // Need a burn for every 2 cells below that.
  // E.g. 1 diff => 3 below col8 = 1 burn,
  //      2 diff => 4 below col8 = 1 burn
  //      3 diff => 5 below col8 = 2 burns
  int diff = lowestGoodColumn9 - col9;
  return ceil(diff / 2.0f) * 0.6;
}

/**
 * Assesses whether the surface allows for 5 taps.
 * @returns the multiple of the accessible left penalty that should be applied. That is, 0 if 5 taps are possible, or a float around 1.0 or higher (depending on how many lines would need to clear for the left to be accessible).
 */
float getInaccessibleLeftFactor(int surfaceArray[10], int const maxAccessibleLeftSurface[10], int wellColumn){
  // Check if the agent even needs to get a piece left first.
  // If the left is built out higher than the max 5 tap height and also higher than col 9, then it's chilling.
  int needs5Tap = wellColumn == 9 ? surfaceArray[0] < surfaceArray[8] : surfaceArray[0] < surfaceArray[1];
  if (surfaceArray[0] > maxAccessibleLeftSurface[0] && !needs5Tap) {
    return 0;
  }

  int highestAbove = 0;
  for (int i = 0; i < 7; i++) {  // col 7 is the furthest right a 5 tap piece is on the board when tapped left
    if (surfaceArray[i] > maxAccessibleLeftSurface[i]) {
      highestAbove = std::max(highestAbove, surfaceArray[i] - maxAccessibleLeftSurface[i]);
    }
  }
  return highestAbove == 0 ? 0 : 1.0 + 0.5 * highestAbove;
}

float getInaccessibleRightFactor(int surfaceArray[10], int const maxAccessibleRightSurface[10]){
  // Check if the agent even needs to get a piece left first.
  // If the left is built out higher than the max 5 tap height and also higher than col 9, then it's chilling.
  int needsRightTap = surfaceArray[9] < surfaceArray[8];
  if (surfaceArray[0] > maxAccessibleRightSurface[0] && !needsRightTap) {
    return 0;
  }

  int highestAbove = 0;
  for (int i = 5; i < 10; i++) {  // col 7 is the furthest right a 5 tap piece is on the board when tapped left
    if (surfaceArray[i] > maxAccessibleRightSurface[i]) {
      highestAbove = std::max(highestAbove, surfaceArray[i] - maxAccessibleRightSurface[i]);
    }
  }
  return highestAbove == 0 ? 0 : 1.0 + 0.5 * highestAbove;
}

float getLineClearFactor(int numLinesCleared, FastEvalWeights weights, int shouldRewardLineClears){
  return numLinesCleared == 4
    ? weights.tetrisCoef
    : weights.burnCoef * numLinesCleared * (shouldRewardLineClears ? -0.25 : 1);
}

int isTetrisReady(int board[20], int col10Height){
  if (col10Height > 16) {
    return 0;
  }
  // Check that the four rows where a right well tetris would happen are all full except col 10
  for (int r = 0; r < 4; r++) {
    if ((board[19 - col10Height - r] & FULL_ROW) != 1022) {
      return false;
    }
  }
  return true;
}

float fastEval(GameState gameState,
               GameState newState,
               SimState lockPlacement,
               const EvalContext *evalContext) {
  FastEvalWeights weights = evalContext->weights;
  // Preliminary helper work
  float avgHeight = getAverageHeight(newState.surfaceArray, evalContext->wellColumn);
  int isKillscreenLineout = gameState.level >= 29 && evalContext->aiMode == LINEOUT;
  // Calculate all the factors
  float avgHeightFactor = weights.avgHeightCoef * getAverageHeightFactor(avgHeight, evalContext->scareHeight);
  float builtOutLeftFactor = weights.builtOutLeftCoef * getBuiltOutLeftFactor(newState.surfaceArray, newState.board, avgHeight, evalContext->scareHeight);
  float coveredWellFactor = weights.coveredWellCoef * getCoveredWellFactor(newState.board, evalContext->wellColumn, evalContext->scareHeight);
  float guaranteedBurnsFactor = weights.burnCoef * getGuaranteedBurnsFactor(newState.board, evalContext->wellColumn);
  float likelyBurnsFactor = weights.burnCoef * getLikelyBurnsFactor(newState.surfaceArray, evalContext->wellColumn, evalContext->maxSafeCol9);
  float highCol9Factor = weights.col9Coef * getCol9Factor(newState.surfaceArray[8], evalContext->maxSafeCol9);
  float holeFactor = weights.holeCoef * newState.adjustedNumHoles;
  float inaccessibleLeftFactor = isKillscreenLineout
              ? 0
              : (weights.inaccessibleLeftCoef * getInaccessibleLeftFactor(newState.surfaceArray, evalContext->pieceRangeContext.maxAccessibleLeft5Surface, evalContext->wellColumn));
  float inaccessibleRightFactor = isKillscreenLineout
              ? 0
              : (weights.inaccessibleRightCoef * getInaccessibleRightFactor(newState.surfaceArray, evalContext->pieceRangeContext.maxAccessibleRightSurface));
  float lineClearFactor = getLineClearFactor(newState.lines - gameState.lines, weights, evalContext->shouldRewardLineClears);
  float surfaceFactor = weights.surfaceCoef * rateSurface(newState.surfaceArray, evalContext);
  float surfaceLeftFactor =
    (isKillscreenLineout)
      ? weights.surfaceLeftCoef * getLeftSurfaceFactor(newState.board, newState.surfaceArray, evalContext->pieceRangeContext.max5TapHeight)
      : 0;
  float tetrisReadyFactor =
    (evalContext->wellColumn >= 0 && isTetrisReady(newState.board, newState.surfaceArray[evalContext->wellColumn]))
      ? weights.tetrisReadyCoef
      : 0;

  float total = surfaceFactor + surfaceLeftFactor + avgHeightFactor + lineClearFactor + holeFactor + guaranteedBurnsFactor + likelyBurnsFactor + inaccessibleLeftFactor + inaccessibleRightFactor + coveredWellFactor + highCol9Factor + tetrisReadyFactor + builtOutLeftFactor;

  // Logging
  if (LOGGING_ENABLED) {
    maybePrint("\nEvaluating possibility %d %d %d\n",
               lockPlacement.rotationIndex,
               lockPlacement.x - SPAWN_X,
               lockPlacement.y);
    printBoard(newState.board);
    printSurface(newState.surfaceArray);
    maybePrint("Tuck setups:\n");
    for (int i = 0; i < 19; i++) {
      maybePrint("%d ", (newState.board[i] & ALL_TUCK_SETUP_BITS) >> 20);
    }
    maybePrint("%d\n", (newState.board[19] & ALL_TUCK_SETUP_BITS) >> 20);
    maybePrint("Holes:\n");
    for (int i = 0; i < 19; i++) {
      maybePrint("%d ", (newState.board[i] & ALL_HOLE_BITS) >> 10);
    }
    maybePrint("%d\n", (newState.board[19] & ALL_HOLE_BITS) >> 10);
    maybePrint("Hole weights:\n");
    for (int i = 0; i < 19; i++) {
      maybePrint("%d ", (newState.board[i] & HOLE_WEIGHT_BIT) > 0);
    }
    maybePrint("%d\n", (newState.board[19] & HOLE_WEIGHT_BIT) > 0);

    printf("Numholes %f\n", newState.adjustedNumHoles);
    maybePrint(
      "Surface %01f, LeftSurface %01f, AvgHeight %01f, LineClear %01f, Hole %01f, GuaranteedBurns %01f, LikelyBurns %01f, InaccLeft %01f, CoveredWell %01f, HighCol9 %01f, TetrisReady %01f, BuiltLeft %01f\t Total: %01f\n",
      surfaceFactor,
      surfaceLeftFactor,
      avgHeightFactor,
      lineClearFactor,
      holeFactor,
      guaranteedBurnsFactor,
      likelyBurnsFactor,
      inaccessibleLeftFactor,
      coveredWellFactor,
      highCol9Factor,
      tetrisReadyFactor,
      builtOutLeftFactor,
      total);
  }

  return total;
}
