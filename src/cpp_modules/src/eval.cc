#include "../include/eval.h"
#include "../include/move_result.h"
#include "../include/utils.h"

#include <math.h>
#include <vector>
//#include "data/ranksOutput.cc"
using namespace std;

const int USE_RANKS = 0;

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
    if (diff != 0) {
      score -= pow(abs(diff), 2.5);
    }
  }
  return score;
}

/** Gets the value of a surface. */
float rateSurface(int surfaceArray[10], int wellColumn) {
  // Backup option in case ranks aren't loaded
  if (!USE_RANKS) {
    return calculateFlatness(surfaceArray, wellColumn);
  }
  // Convert the surface array into the custom base-9 encoding
  int index = 0;
  for (int i = 0; i < 8; i++) {
    int diff = surfaceArray[i + 1] - surfaceArray[i];
    index *= 9;
    index += diff + 3;
  }
  return 1.0; // So it compiles without the ranks
  // return surfaceRanksRaw[index] * 0.1;
}

float getAverageHeightFactor(int surfaceArray[10], int wellColumn, int scareHeight, FastEvalWeights weights) {
  float totalHeight = 0;
  float weight = wellColumn >= 0 ? 0.1 : 0.111111;
  for (int i = 0; i < 10; i++) {
    if (i == wellColumn) {
      continue;
    }
    totalHeight += surfaceArray[i] * weight;
  }
  
  int scaledPenalty = scareHeight < 3 ? totalHeight : totalHeight / max(3, scareHeight);
  return weights.avgHeightCoef * pow(scaledPenalty, weights.avgHeightExponent);
}

float getCol9Factor(int col9Height, float maxSafeCol9Height){
  if (col9Height <= maxSafeCol9Height){
    return 0;
  }
  float diff = maxSafeCol9Height - col9Height;
  return diff * diff;
}

float getCoveredWellFactor(int board[20], int wellColumn, int scareHeight, FastEvalWeights weights) {
  if (wellColumn == -1) {
    return 0;
  }
  int mask = (1 << (9 - wellColumn));
  for (int r = 0; r < 20; r++) {
    if (board[r] & mask) {
      float heightRatio = (20.0f - r) / max(3, scareHeight);
      return weights.coveredWellCoef * heightRatio * heightRatio * heightRatio;
    }
  }
  return 0;
}

float getGuaranteedBurnsFactor(int board[20], int wellColumn, FastEvalWeights weights) {
  if (wellColumn == -1) {
    return 0;
  }
  int mask = (1 << (9 - wellColumn));
  int wellCells = 0;
  for (int r = 0; r < 20; r++) {
    if (board[r] & mask) {
      wellCells++;
    }
  }
  return weights.burnCoef * wellCells;
}

float getLineClearFactor(int numLinesCleared, FastEvalWeights weights){
  return numLinesCleared == 4 ? weights.tetrisCoef : weights.burnCoef * numLinesCleared;
}

float fastEval(GameState gameState,
               GameState newState,
               SimState lockPlacement,
               EvalContext evalContext,
               FastEvalWeights weights) {
  // Calculate all the factors
  float surfaceFactor = weights.surfaceCoef * rateSurface(newState.surfaceArray, evalContext.wellColumn);
  float avgHeightFactor = getAverageHeightFactor(newState.surfaceArray, evalContext.wellColumn, evalContext.scareHeight, weights);
  float lineClearFactor = getLineClearFactor(newState.lines - gameState.lines, weights);
  float holeFactor = weights.holeCoef * newState.adjustedNumHoles;
  float coveredWellFactor = getCoveredWellFactor(newState.board, evalContext.wellColumn, evalContext.scareHeight, weights);
  float guaranteedBurnsFactor = getGuaranteedBurnsFactor(newState.board, evalContext.wellColumn, weights);
  float highCol9Factor = weights.col9Coef * getCol9Factor(newState.surfaceArray[8], evalContext.maxSafeCol9);

  float total = surfaceFactor + avgHeightFactor + lineClearFactor + holeFactor + guaranteedBurnsFactor + coveredWellFactor + highCol9Factor;

  // Logging
  if (LOGGING_ENABLED) {
    maybePrint("\nEvaluating possibility %d %d %d\n",
               lockPlacement.rotationIndex,
               lockPlacement.x - SPAWN_X,
               lockPlacement.y);
    printBoard(newState.board);
    for (int i = 0; i < 9; i++) {
      maybePrint("%d ", newState.surfaceArray[i]);
    }
    maybePrint("%d\n", newState.surfaceArray[9]);
    maybePrint(
      "Surface %01f, AvgHeight %01f, LineClear %01f, Hole %01f, GuaranteedBurns %01f, CoveredWell %01f, HighCol9 %01f\t Total: %01f\n",
      surfaceFactor,
      avgHeightFactor,
      lineClearFactor,
      holeFactor,
      guaranteedBurnsFactor,
      coveredWellFactor,
      highCol9Factor,
      total);
  }

  return total;
}
