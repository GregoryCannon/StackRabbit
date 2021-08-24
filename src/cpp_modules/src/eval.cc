#include "../include/eval.h"
#include "../include/move_result.h"
#include "../include/utils.h"
#include "../include/ranks_output.h"
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
    if (i == 7 && wellColumn == 9 && diff < -2){
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
float rateSurface(int surfaceArray[10], int wellColumn, FastEvalWeights weights) {
  if (USE_RANKS) {
    // Convert the surface array into the custom base-9 encoding
    int index = 0;
    int excessGap = 0;
    for (int i = 0; i < 8; i++) {
      int diff = surfaceArray[i + 1] - surfaceArray[i];
      int absDiff = abs(diff);
      // Correct for double wells
      if (i == 7 && wellColumn == 9 && absDiff > 2){
        absDiff = 2;
      } else if (absDiff > 4) {
        excessGap += absDiff - 4;
        diff = diff > 0 ? 4 : -4;
      }
      index *= 9;
      index += diff + 4;
    }
    // Make lower ranks more punishing
    float rawScore = surfaceRanksRaw[index] * 0.1 + (excessGap * weights.extremeGapCoef);
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

float getCol9Factor(int col9Height, float maxSafeCol9Height){
  if (col9Height <= maxSafeCol9Height) {
    return 0;
  }
  float diff = col9Height - maxSafeCol9Height;
  return diff * diff;
}

float getCoveredWellFactor(int board[20], int wellColumn, float scareHeight, FastEvalWeights weights) {
  if (wellColumn == -1) {
    return 0;
  }
  int mask = (1 << (9 - wellColumn));
  for (int r = 0; r < 20; r++) {
    if (board[r] & mask) {
      float heightRatio = (20.0f - r) / max(3.0f, scareHeight);
      return heightRatio * heightRatio * heightRatio;
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
    if ((board[r] & wellMask) || (board[r] & NEED_TO_CLEAR_BIT)) {
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

float getLineClearFactor(int numLinesCleared, FastEvalWeights weights){
  return numLinesCleared == 4 ? weights.tetrisCoef : weights.burnCoef * numLinesCleared;
}

int isTetrisReady(int board[20], int col10Height){
  if (col10Height > 16) {
    return 0;
  }
  // Check that the four rows where a right well tetris would happen are all full except col 10
  for (int r = 0; r <= 4; r++) {
    if (board[19 - col10Height - r] != 1022) {
      return false;
    }
  }
  return true;
}

float fastEval(GameState gameState,
               GameState newState,
               SimState lockPlacement,
               EvalContext evalContext,
               FastEvalWeights weights) {
  // Preliminary helper work
  float avgHeight = getAverageHeight(newState.surfaceArray, evalContext.wellColumn);
  // Calculate all the factors
  float avgHeightFactor = weights.avgHeightCoef * getAverageHeightFactor(avgHeight, evalContext.scareHeight);
  float builtOutLeftFactor = weights.builtOutLeftCoef * getBuiltOutLeftFactor(newState.surfaceArray, newState.board, avgHeight, evalContext.scareHeight);
  float coveredWellFactor = weights.coveredWellCoef * getCoveredWellFactor(newState.board, evalContext.wellColumn, evalContext.scareHeight, weights);
  float guaranteedBurnsFactor = weights.burnCoef * getGuaranteedBurnsFactor(newState.board, evalContext.wellColumn);
  float likelyBurnsFactor = weights.burnCoef * getLikelyBurnsFactor(newState.surfaceArray, evalContext.wellColumn, evalContext.maxSafeCol9);
  float highCol9Factor = weights.col9Coef * getCol9Factor(newState.surfaceArray[8], evalContext.maxSafeCol9);
  float holeFactor = weights.holeCoef * newState.adjustedNumHoles;
  float lineClearFactor = getLineClearFactor(newState.lines - gameState.lines, weights);
  float surfaceFactor = weights.surfaceCoef * rateSurface(newState.surfaceArray, evalContext.wellColumn, weights);
  float tetrisReadyFactor = isTetrisReady(newState.board, newState.surfaceArray[9]) ? weights.tetrisReadyCoef : 0;

  float total = surfaceFactor + avgHeightFactor + lineClearFactor + holeFactor + guaranteedBurnsFactor + likelyBurnsFactor + coveredWellFactor + highCol9Factor + tetrisReadyFactor + builtOutLeftFactor;

  // Logging
  if (LOGGING_ENABLED) {
    maybePrint("\nEvaluating possibility %d %d %d\n",
               lockPlacement.rotationIndex,
               lockPlacement.x - SPAWN_X,
               lockPlacement.y);
    printBoard(newState.board);
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
      maybePrint("%d ", (newState.board[i] & NEED_TO_CLEAR_BIT) > 0);
    }
    maybePrint("%d\n", (newState.board[19] & NEED_TO_CLEAR_BIT) > 0);

    printf("Numholes %f\n", newState.adjustedNumHoles);
    maybePrint(
      "Surface %01f, AvgHeight %01f, LineClear %01f, Hole %01f, GuaranteedBurns %01f, LikelyBurns %01f, CoveredWell %01f, HighCol9 %01f, TetrisReady %01f, BuiltLeft %01f\t Total: %01f\n",
      surfaceFactor,
      avgHeightFactor,
      lineClearFactor,
      holeFactor,
      guaranteedBurnsFactor,
      likelyBurnsFactor,
      coveredWellFactor,
      highCol9Factor,
      tetrisReadyFactor,
      builtOutLeftFactor,
      total);
  }

  return total;
}
