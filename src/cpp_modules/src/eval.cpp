#include "eval.hpp"
#include "move_result.hpp"
#include "eval_context.hpp"
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
    if (USE_RIGHT_WELL_FEATURES && i == 7 && wellColumn == 9 && diff < -2) {
      diff = -2;
    }
    // Punish based on the absolute value of the column differences
    if (diff != 0) {
      score -= pow(abs(diff), 1.5);
    }
    // Line dependency
    if (diff >= 3 && (i == 0 || surfaceArray[i-1] + surfaceArray[i] >= 3)) {
      score -= 25;
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

float getBuiltOutLeftFactor(int surfaceArray[10], unsigned int board[20], float avgHeight, float scareHeight) {
  if (!USE_RIGHT_WELL_FEATURES) {
    return 0;
  }
  float heightRatio = max(1.0f, avgHeight) / max(2.0f, scareHeight);
  float heightDiff = 0.5 * (surfaceArray[0] - avgHeight) + 0.5 * (surfaceArray[0] - surfaceArray[1]);

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

float getLeftSurfaceFactor(unsigned int board[20], int surfaceArray[10], int max5TapHeight){
  max5TapHeight = max(0, max5TapHeight);
  for (int r = 20 - surfaceArray[0]; r < 20; r++) {
    if (board[r] & HOLE_BIT(0)) {
      return -5;
    }
  }
  if (surfaceArray[1] > max5TapHeight && surfaceArray[1] > surfaceArray[0]) {
    return surfaceArray[0] - surfaceArray[1];
  }
  return 0;
}

float getCol9Factor(int col9Height, float maxSafeCol9Height){
  if (!USE_RIGHT_WELL_FEATURES) {
    return 0;
  }
  if (col9Height <= maxSafeCol9Height) {
    return 0;
  }
  float diff = col9Height - maxSafeCol9Height;
  return diff * diff;
}

float getCoveredWellFactor(unsigned int board[20], int wellColumn, float scareHeight) {
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

float getGuaranteedBurnsFactor(unsigned int board[20], int wellColumn) {
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

float getHoleWeightFactor(unsigned int board[20], int wellColumn) {
  // Neither of these measures make sense in lineout mode, so don't calculate this factor
  if (wellColumn == -1) {
    return 0;
  }
  int holeWeight = 0;
  for (int r = 0; r < 20; r++) {
    if ((board[r] & HOLE_WEIGHT_BIT)) {
      holeWeight++;
    }
  }
  return holeWeight;
}


float getLikelyBurnsFactor(int surfaceArray[10], int wellColumn, int maxSafeCol9) {
  if (wellColumn != 9 || !USE_RIGHT_WELL_FEATURES) {
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
float getInaccessibleLeftFactor(unsigned int board[20], int surfaceArray[10], int const maxAccessibleLeftSurface[10], int wellColumn){
  // Check if the agent even needs to get a piece left first.
  int highestRowOfCol1 = 19 - surfaceArray[0];
  int needs5TapForDig = board[highestRowOfCol1] & HOLE_WEIGHT_BIT;
  int needs5TapForBurn = wellColumn == 9 && surfaceArray[0] <= surfaceArray[8];
  int needs5Tap = needs5TapForDig || needs5TapForBurn;
//  int needs5TapOnKillscreen = (surfaceArray[1] - surfaceArray[0]) > maxAccessibleLeftSurface[0];
//  int needs5Tap = needs5TapForDig || needs5TapForBurn || needs5TapOnKillscreen;

  int hasHoleInLeft = false;
  for (int r = highestRowOfCol1 + 1; r < highestRowOfCol1 + 4; r++) {
    if (board[r] & (HOLE_BIT(0) | HOLE_BIT(1) | HOLE_BIT(2))) {
      hasHoleInLeft = true;
      break;
    }
  }

  // If the left is built out higher than the max 5 tap height, and there's no pressing need to get a piece there anyway, this factor doesn't matter
  if (surfaceArray[0] > maxAccessibleLeftSurface[0] && surfaceArray[0] >= surfaceArray[1] && !needs5Tap && !hasHoleInLeft) {
    return 0;
  }

  int highestAbove = 0;
  for (int i = 0; i < 7; i++) {  // col 7 is the furthest right a 5 tap piece is on the board when tapped left
    if (surfaceArray[i] > maxAccessibleLeftSurface[i]) {
      highestAbove = std::max(highestAbove, surfaceArray[i] - maxAccessibleLeftSurface[i]);
    }
  }
  return highestAbove == 0 ? 0 : (1.0 + 0.2 * highestAbove * highestAbove);
}

float getInaccessibleRightFactor(int surfaceArray[10], int const maxAccessibleRightSurface[10]){
  // Check if the agent even needs to get a piece right first.
  // If column 10 is higher than column 9, this feature doesn't matter.
  int needsRightTap = surfaceArray[9] < surfaceArray[8];
  if (surfaceArray[9] > maxAccessibleRightSurface[9] && !needsRightTap) {
    return 0;
  }

  int highestAbove = 0;
  for (int i = 5; i < 10; i++) {  // col 7 is the furthest right a 5 tap piece is on the board when tapped left
    if (surfaceArray[i] > maxAccessibleRightSurface[i]) {
      highestAbove = std::max(highestAbove, surfaceArray[i] - maxAccessibleRightSurface[i]);
    }
  }
  return highestAbove == 0 ? 0 : 1.0 + 0.2 * highestAbove * highestAbove;
}

float getLineClearFactor(int numLinesCleared, FastEvalWeights weights, int shouldRewardLineClears){
  return numLinesCleared == 4
    ? weights.tetrisCoef
    : weights.burnCoef * numLinesCleared * (shouldRewardLineClears ? -0.25 : 1);
}

/** Calculate how hard it will be to fill in the middle of the board enough to burn. */
float getUnableToBurnFactor(unsigned int board[20], int surfaceArray[10], float scareHeight){
  if (!USE_RIGHT_WELL_FEATURES) {
    return 0;
  }
  float totalPenalty = 0;
  int col9Height = surfaceArray[8];

  // If col 10 is also filled, having col 9 filled isn't bad
  if (col9Height <= surfaceArray[9]) {
    while (board[20 - col9Height] & 1) {
      col9Height--;
    }
  }
  if (col9Height <= 0) {
    return 0;
  }

  // Check for columns in the middle of the board lower than col 9
  for (int c = 0; c < 8; c++) {
    if (surfaceArray[c] < col9Height) {
      // Check for a line dependency in that column
      int thisCol = surfaceArray[c];
      int prevCol = c == 0 ? 99 : surfaceArray[c - 1];
      int nextCol = c == 9 ? 99 : surfaceArray[c + 1];
      if (prevCol - thisCol >= 3 && nextCol - thisCol >= 3) {
        totalPenalty += 100;
        break;
      }

      // Otherwise penalize for unfilled cells, scales with d^2 as the distance from col 9 increases
      int diff = col9Height - surfaceArray[c];
      totalPenalty += diff * diff;
    }
  }

  int col9Row = 20 - col9Height;
  // Check for holes in the must-use-to-burn zone
  for (int r = max(0, col9Row - 1); r <= col9Row; r++) {
    if (board[r] & ALL_HOLE_BITS) {
      totalPenalty += 50;
    }
  }
  // Check for holes in the maybe-used-to-burn zone
  for (int r = max(0, col9Row - 2); r <= min(19, col9Row + 2); r++) {
    if (board[r] & ALL_HOLE_BITS) {
      totalPenalty += 50;
    }
  }

  float scareRatio = (float) col9Height / max(2.0f, scareHeight);
  float heightMultiplier = scareRatio * scareRatio * scareRatio;

  return totalPenalty * heightMultiplier;
}

int isTetrisReady(unsigned int board[20], int surfaceArray[10], int wellColumn){
  int wellColHeight = surfaceArray[wellColumn];
  if (wellColHeight > 16) {
    return 0;
  }
  int wellMask = (1 << (9 - wellColumn));
  int idealRowMask = FULL_ROW & ~wellMask;
  // Check that the four rows where a right well tetris would happen are all full except col 10
  for (int r = 0; r < 4; r++) {
    if ((board[19 - wellColHeight - r] & FULL_ROW) != idealRowMask) {
      return false;
    }
  }
  return true;
}

/** Rate the "badness" of a surface, where more points is worse. */
float rateSurfaceForPerfectPlay(int surfaceArray[10], int wellColumn) {
  float score = 0;
  for (int i = 0; i < 9; i++) {
    if (i == wellColumn || i+1 == wellColumn) {
      continue;
    }
    int diff = surfaceArray[i + 1] - surfaceArray[i];

    // Line dependencies
    if (diff >= 7 && (i == 0 || surfaceArray[i-1] - surfaceArray[i] >= 7)) {
      score += 40;
    } else if (diff >= 3 && (i == 0 || surfaceArray[i-1] - surfaceArray[i] >= 3)) {
      score += 20;
    }  else if (diff <= -7 && i == wellColumn - 2) {
      score += 40;
    } else if (diff <= -3 && i == wellColumn - 2) {
      score += 20;
    } else if (diff != 0) {
      // Otherwise, punish based on the absolute value of the column differences
      score += pow(abs(diff), 1.5);
    }
  }
  return score;
}

/** Custom evaluation function designed for perfect play. The score it returns is aimed to emulate the percent chance of maintaining a perfect board throughout all the playouts. */
float evalForPerfectPlay(GameState gameState,
                         GameState newState,
                         LockPlacement lockPlacement,
                         const EvalContext *evalContext) {
  // Check for holes or covered well
  float trueHoles = getNumTrueHoles(newState.adjustedNumHoles);
  float tuckSetupCells = (newState.adjustedNumHoles - trueHoles) / TUCK_SETUP_HOLE_PROPORTION;
  bool hasCoveredWell = newState.surfaceArray[evalContext->wellColumn] > 0;
  bool inaccessibleRight = false;
  for (int i = 5; i < 10; i++) {  // col 7 is the furthest right a 5 tap piece is on the board when tapped left
    if (newState.surfaceArray[i] > evalContext->pieceRangeContext.maxAccessibleRightSurface[i]) {
      inaccessibleRight = true;
    }
  }

  if (trueHoles > 0 || hasCoveredWell || inaccessibleRight) {
    if (LOGGING_ENABLED) {
      maybePrint("\nEvaluating possibility %d %d %d\n",
                 lockPlacement.rotationIndex,
                 lockPlacement.x - SPAWN_X,
                 lockPlacement.y);
      printBoard(newState.board);
      printf("RUN FAILED\n");
    }
    return 0;
  }

  // We'll track the overall badness of a position, such that as badness increases, the eval approaches 0.
  float badness = 0;

  // Rate the surface
  badness += rateSurfaceForPerfectPlay(newState.surfaceArray, evalContext->wellColumn);

  // Evaluate tuck setups
  badness += tuckSetupCells * 20;

  // Evaluate board height
  badness += 20 * max(0.0f, getAverageHeight(newState.surfaceArray, evalContext->wellColumn) - 4);

  // Use a decaying exponential with the following key points:
  // 0 badness -> 100% survival
  // 50 badness -> 50% survival
  // 100 badness -> 25% survival
  // infinite badness -> 0% survival
  float evalScore = 100.0f * pow(0.5f, badness / 50.0f);

  // TODO: reward tetris, and tetrisReady

  // Logging
  if (LOGGING_ENABLED) {
    maybePrint("\nEvaluating possibility %d %d %d\n",
               lockPlacement.rotationIndex,
               lockPlacement.x - SPAWN_X,
               lockPlacement.y);
    printBoard(newState.board);
    printf("Numholes %f\n", newState.adjustedNumHoles);
    maybePrint(
      "badness %01f, evalScore %01f\n",
      badness,
      evalScore);
  }


  return evalScore;
}



float fastEval(GameState gameState,
               GameState newState,
               LockPlacement lockPlacement,
               const EvalContext *evalContext) {
  if (SHOULD_PLAY_PERFECT) {
    return evalForPerfectPlay(gameState, newState, lockPlacement, evalContext);
  }

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
  float holeWeightFactor = abs(weights.holeWeightCoef) > FLOAT_EPSILON ? weights.holeWeightCoef * getHoleWeightFactor(newState.board, evalContext->wellColumn) : 0;
  float inaccessibleLeftFactor = isKillscreenLineout
              ? 0
              : (weights.inaccessibleLeftCoef * getInaccessibleLeftFactor(newState.board, newState.surfaceArray, evalContext->pieceRangeContext.maxAccessibleLeft5Surface, evalContext->wellColumn));
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
    (evalContext->wellColumn >= 0 && isTetrisReady(newState.board, newState.surfaceArray, evalContext->wellColumn))
      ? weights.tetrisReadyCoef
      : 0;
  float unableToBurnFactor = weights.unableToBurnCoef * getUnableToBurnFactor(newState.board, newState.surfaceArray, evalContext->scareHeight);

  float total = surfaceFactor + surfaceLeftFactor + avgHeightFactor + lineClearFactor + holeFactor + holeWeightFactor + guaranteedBurnsFactor + likelyBurnsFactor + inaccessibleLeftFactor + inaccessibleRightFactor + coveredWellFactor + highCol9Factor + tetrisReadyFactor + builtOutLeftFactor + unableToBurnFactor;
  total = max(weights.deathCoef, total); // Can't be worse than death

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
      "Surface %01f, LeftSurface %01f, AvgHeight %01f, LineClear %01f, Hole %01f, HoleWeight %01f, GuaranteedBurns %01f, LikelyBurns %01f, InaccLeft %01f, CoveredWell %01f, HighCol9 %01f, TetrisReady %01f, BuiltLeft %01f, UnableToBrn %01f,\t Total: %01f\n",
      surfaceFactor,
      surfaceLeftFactor,
      avgHeightFactor,
      lineClearFactor,
      holeFactor,
      holeWeightFactor,
      guaranteedBurnsFactor,
      likelyBurnsFactor,
      inaccessibleLeftFactor,
      coveredWellFactor,
      highCol9Factor,
      tetrisReadyFactor,
      builtOutLeftFactor,
      unableToBurnFactor,
      total);
  }

  return total;
}
