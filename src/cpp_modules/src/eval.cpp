#include "eval.hpp"
#include "../data/ranks_base_7.hpp"
#include "../data/ranks_output.hpp"
#include "eval_context.hpp"
#include "move_result.hpp"
#include "utils.hpp"
#include <cstdio>
#include <math.h>
#include <string>
#include <vector>
using namespace std;

/**
 * A crude way to evaluate a surface for when I'm debugging and don't want to load the surfaces
 * every time I run.
 */
static inline float calculateFlatness(const int surfaceArray[10], int wellColumn) {
  float score = 30;
  bool hasFlatSpot = false;
  for (int i = 0; i < 9; i++) {
    if (i == wellColumn || i + 1 == wellColumn) {
      continue;
    }
    int diff = surfaceArray[i + 1] - surfaceArray[i];
    if (diff == 0) {
      hasFlatSpot = true;
    }
    // Correct for double wells
    if (USE_RIGHT_WELL_FEATURES && i == 7 && wellColumn == 9 && diff < -2) {
      diff = -2;
    }
    // Punish based on the absolute value of the column differences
    if (diff != 0) {
      score -= pow(abs(diff), 1.5);
    }
    // Line dependency
    if (diff >= 3 && (i == 0 || (surfaceArray[i] - surfaceArray[i - 1]) <= -3)) {
      score -= 25;
    }
  }
  // Not having a single flat spot can be disastrous if you get a square or L/J
  if (!hasFlatSpot) {
    score -= 12;
  }
  return score;
}

/** Gets the value of a surface. */
static inline float rateSurface(const int surfaceArray[10], const EvalContext *evalContext) {
  int wellColumn = evalContext->wellColumn;

  if (USE_BASE_7_RANKS) {
    // Convert the surface array into the custom base-9 encoding
    int b7index = 0;
    int excessGap = 0;
    for (int i = 0; i < 7; i++) {
      int diff = surfaceArray[i + 1] - surfaceArray[i];
      if (abs(diff) > 3) {
        excessGap += abs(diff) - 3;
        diff = diff > 0 ? 3 : -3;
      }
      b7index = b7index * 7 + (diff + 3);
    }
    int diff7 = surfaceArray[8] - surfaceArray[7];
    if (wellColumn == 9 && diff7 < -2) {
      diff7 = -2;
    } else if (abs(diff7) > 3) {
      excessGap += abs(diff7) - 3;
      diff7 = diff7 > 0 ? 3 : -3;
    }
    b7index = b7index * 7 + (diff7 + 3);
    unsigned long long chunk = surfaceRanksChunked[b7index / 8];
    unsigned int subIndex = b7index & 0b111;
    int numShifts = (7 - subIndex) * 8;
    unsigned int byte = (chunk >> numShifts) & 0xFF;
    float scaledTo30 = ((float)byte) / 255.0 * 33.8;
    // Make lower ranks more punishing
    float rawScore = scaledTo30 + (excessGap * evalContext->weights.extremeGapCoef);
    return rawScore + 2 - (70 / max(3.0f, rawScore));
  }

  // If the ranks aren't loaded, use the flatness score
  return calculateFlatness(surfaceArray, wellColumn);
}

static inline float getAverageHeight(const int surfaceArray[10], int wellColumn) {
  float avgHeight = 0;
  float weight = wellColumn == -1 ? 0.1 : 0.111111;
  for (int i = 0; i < 10; i++) {
    if (i == wellColumn) {
      continue;
    }
    avgHeight += surfaceArray[i] * weight;
  }
  return avgHeight;
}

static inline float getAverageHeightFactor(float avgHeight, float scareHeight) {
  float diff = max(0.0f, avgHeight - scareHeight);
  return diff * diff;
}

static inline float getBuiltOutLeftFactor(const int surfaceArray[10], const unsigned int board[20], float avgHeight,
                                          float scareHeight, bool hasHoles) {
  if (!USE_RIGHT_WELL_FEATURES) {
    return 0;
  }
  float heightRatio = max(1.0f, avgHeight) / max(2.0f, scareHeight);
  // float heightDiff = 0.5 * (surfaceArray[0] - avgHeight) + 0.5 * (surfaceArray[0] -
  // surfaceArray[1]);
  float heightDiff = (surfaceArray[0] - avgHeight);

  // Handle low left cases first
  if (heightDiff < 0) {
    float softenedHeightRatio =
        0.5f
        * (heightRatio
           + 1); // Average it with 1 to make it less extreme (faster than sqrt operation)
    return -0.5
           * heightDiff
           * heightDiff
           * softenedHeightRatio; // Approximate (heightDiff ^ 1.5) as (heightDiff * heightDiff *
                                  // 0.5)
  }
  // Check for holes (don't reward building out the left over holes)
  if (hasHoles) {
    int r = 21 - surfaceArray[0];
    while (r < 20) {
      if (!(board[r] & (1 << 9))) {
        return 0;
      }
      r++;
    }
  }
  // Reward built out left
  return heightRatio * heightDiff;
}

static inline float getLeftSurfaceFactor(const unsigned int board[20], const int surfaceArray[10], int max5TapHeight) {
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

static inline float getCol9Factor(int col9Height, float maxSafeCol9Height) {
  if (!USE_RIGHT_WELL_FEATURES) {
    return 0;
  }
  if (col9Height <= maxSafeCol9Height) {
    return 0;
  }
  float diff = col9Height - maxSafeCol9Height;
  return diff * diff;
}

static inline float getCoveredWellFactor(const unsigned int board[20], const int surfaceArray[10], int wellColumn, float scareHeight) {
  if (wellColumn == -1 || surfaceArray[wellColumn] <= 0 || surfaceArray[wellColumn] > 20) {
    return 0;
  }
  int r = 20 - surfaceArray[wellColumn];
  int difficultyMultiplier = (board[r] & (ALL_TUCK_SETUP_BITS)) > 0 ? 10 : 1;
  float heightRatio = (float)surfaceArray[wellColumn] / max(3.0f, scareHeight);
  return heightRatio * heightRatio * heightRatio * difficultyMultiplier;
}

struct BurnAndHoleWeightCounts {
  int guaranteedBurns;
  int holeWeight;
};

/**
 * Counts guaranteed burns (covered well cells and rows with holes) and hole weight
 * in a single pass over the board.
 */
static inline BurnAndHoleWeightCounts getGuaranteedBurnsAndHoleWeight(
    const unsigned int board[20], const int surfaceArray[10], int wellColumn,
    bool hasHoles, bool countHoleWeight) {
  if (wellColumn == -1) {
    return {0, 0};
  }
  int wellHeight = surfaceArray[wellColumn];

  if (!hasHoles) {
    if (wellHeight == 0) {
      return {0, 0};
    }
    int wellMask = (1 << (9 - wellColumn));
    int guaranteedBurns = 0;
    for (int r = 20 - wellHeight; r < 20; r++) {
      if (board[r] & wellMask) {
        guaranteedBurns++;
      }
    }
    return {guaranteedBurns, 0};
  }

  int wellMask = (1 << (9 - wellColumn));
  int guaranteedBurns = 0;
  int holeWeight = 0;
  for (int r = 0; r < 20; r++) {
    unsigned int row = board[r];
    int hasHoleWeight = (row & HOLE_WEIGHT_BIT) != 0;
    int hasWell = (row & wellMask) != 0;
    holeWeight += hasHoleWeight;
    guaranteedBurns += (hasWell | hasHoleWeight);
  }
  return {guaranteedBurns, countHoleWeight ? holeWeight : 0};
}

static inline float getLikelyBurnsFactor(const int surfaceArray[10], int wellColumn, int maxSafeCol9) {
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
 * @returns the multiple of the accessible left penalty that should be applied. That is, 0 if 5 taps
 * are possible, or a float around 1.0 or higher (depending on how many lines would need to clear
 * for the left to be accessible).
 */
static inline float getInaccessibleLeftFactor(const unsigned int board[20], const int surfaceArray[10],
                                              int const maxAccessibleLeft5Surface[10],
                                              int const maxAccessibleLeftLPieceSurface[10], int wellColumn,
                                              AiMode aiMode, bool hasHoles) {
  // Check if the agent even needs to get a piece left first.
  int highestRowOfCol1 = 19 - surfaceArray[0];
  int needs5TapForDig = (hasHoles && highestRowOfCol1 >= 0 && highestRowOfCol1 < 20)
                            ? (board[highestRowOfCol1] & HOLE_WEIGHT_BIT)
                            : 0;
  int needs5TapForBurn = aiMode != DIG && wellColumn == 9 && surfaceArray[0] < surfaceArray[8];
  int needs5Tap = needs5TapForDig || needs5TapForBurn;
  //  int needs5TapOnKillscreen = (surfaceArray[1] - surfaceArray[0]) > maxAccessibleLeftSurface[0];
  //  int needs5Tap = needs5TapForDig || needs5TapForBurn || needs5TapOnKillscreen;

  // This factor is added to stop the malicious compliance of tossing some random piece left while
  // making a hole to technically have a "built out left"
  int hasHoleInLeft = false;
  if (hasHoles) {
    for (int r = max(0, highestRowOfCol1 + 1); r < min(20, highestRowOfCol1 + 4); r++) {
      if (board[r] & (HOLE_BIT(0) | HOLE_BIT(1) | HOLE_BIT(2))) {
        hasHoleInLeft = true;
        break;
      }
    }
  }

  if (surfaceArray[0] == surfaceArray[1] - 1 && surfaceArray[1] == surfaceArray[2]) {
    bool canTapLPieceLeft = true;
    for (int i = 0; i < 7; i++) { // col 7 is the furthest right the L piece ever is on the board
                                  // when tapped left flat
      if (surfaceArray[i] > maxAccessibleLeftLPieceSurface[i]) {
        canTapLPieceLeft = false;
        break;
      }
    }
    if (canTapLPieceLeft) {
      return 0;
    }
  }

  // If the left is built out higher than the max 5 tap height, and there's no pressing need to get
  // a piece there anyway, this factor doesn't matter
  if (surfaceArray[0] > maxAccessibleLeft5Surface[0] && !needs5Tap && !hasHoleInLeft) {
    return 0;
  }

  int highestAbove = 0;
  for (int i = 0; i < 7;
       i++) { // col 7 is the furthest right a 5 tap piece is on the board when tapped left
    if (surfaceArray[i] > maxAccessibleLeft5Surface[i]) {
      highestAbove = std::max(highestAbove, surfaceArray[i] - maxAccessibleLeft5Surface[i]);
    }
  }
  return highestAbove == 0 ? 0 : (1.0 + 0.2 * highestAbove * highestAbove);
}

static inline float getInaccessibleRightFactor(const int surfaceArray[10], int const maxAccessibleRightSurface[10]) {
  // Check if the agent even needs to get a piece right first.
  // If column 10 is higher than column 9, this feature doesn't matter.
  int needsRightTap = surfaceArray[9] < surfaceArray[8];
  if (surfaceArray[9] > maxAccessibleRightSurface[9] && !needsRightTap) {
    return 0;
  }

  int highestAbove = 0;
  for (int i = 5; i < 10;
       i++) { // col 7 is the furthest right a 5 tap piece is on the board when tapped left
    if (surfaceArray[i] > maxAccessibleRightSurface[i]) {
      highestAbove = std::max(highestAbove, surfaceArray[i] - maxAccessibleRightSurface[i]);
    }
  }
  return highestAbove == 0 ? 0 : 1.0 + 0.2 * highestAbove * highestAbove;
}


/** Calculate how hard it will be to fill in the middle of the board enough to burn. */
static inline float getUnableToBurnFactor(const unsigned int board[20], const int surfaceArray[10], float scareHeight, bool hasHoles) {
  if (!USE_RIGHT_WELL_FEATURES) {
    return 0;
  }
  float totalPenalty = 0;
  int col9Height = surfaceArray[8];

  // If col 10 is also filled, having col 9 filled isn't bad
  if (col9Height <= surfaceArray[9]) {
    while (col9Height > 0 && (board[20 - col9Height] & 1)) {
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

  if (hasHoles) {
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
  }

  float scareRatio = (float)col9Height / max(2.0f, scareHeight);
  float heightMultiplier = scareRatio * scareRatio * scareRatio;

  return totalPenalty * heightMultiplier;
}

static inline int isTetrisReady(const unsigned int board[20], const int surfaceArray[10], int wellColumn) {
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
static inline float rateSurfaceForPerfectPlay(const int surfaceArray[10], int wellColumn) {
  float score = 0;
  for (int i = 0; i < 9; i++) {
    if (i == wellColumn || i + 1 == wellColumn) {
      continue;
    }
    int diff = surfaceArray[i + 1] - surfaceArray[i];

    // Line dependencies
    if (diff >= 7 && (i == 0 || surfaceArray[i - 1] - surfaceArray[i] >= 7)) {
      score += 40;
    } else if (diff >= 3 && (i == 0 || surfaceArray[i - 1] - surfaceArray[i] >= 3)) {
      score += 20;
    } else if (diff <= -7 && i == wellColumn - 2) {
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

/** Custom evaluation function designed for perfect play. The score it returns is aimed to emulate
 * the percent chance of maintaining a perfect board throughout all the playouts. */
static inline float evalForPerfectPlay(const GameState &gameState, const GameState &newState, const LockPlacement &lockPlacement,
                                       const EvalContext *evalContext) {
  // Check for burns
  if (newState.lines != gameState.lines && newState.lines - gameState.lines != 4) {
    return -1; // 0% chance of survival since it's over
  }

  // Check for holes or covered well
  float tuckSetupCells = newState.numPartialHoles;
  bool hasCoveredWell = newState.surfaceArray[evalContext->wellColumn] > 0;
  bool inaccessibleRight = false;
  for (int i = 5; i < 10; i++) {
    if (newState.surfaceArray[i] > evalContext->pieceRangeContext.maxAccessibleRightSurface[i]) {
      inaccessibleRight = true;
    }
  }
  if (newState.numTrueHoles > 0 || hasCoveredWell || inaccessibleRight) {
    return 0;
  }

  // We'll track the overall badness of a position, such that as badness increases, the eval
  // approaches 0.
  float badness = 0;

  // Rate the surface
  badness += rateSurfaceForPerfectPlay(newState.surfaceArray, evalContext->wellColumn);

  // Evaluate tuck setups
  badness += tuckSetupCells * 20;

  // Evaluate board height
  badness += 8 * max(0.0f, getAverageHeight(newState.surfaceArray, evalContext->wellColumn) - 4);

  // Use a decaying exponential with the following key points:
  // 0 badness -> 100% survival
  // 50 badness -> 50% survival
  // 100 badness -> 25% survival
  // infinite badness -> 0% survival
  float evalScore = 100.0f * pow(0.5f, badness / 50.0f);

  // TODO: reward tetris, and tetrisReady

  // Logging
  if (LOGGING_ENABLED) {
    maybePrint("\nEvaluating possibility %d %d %d\n", lockPlacement.rotationIndex,
               lockPlacement.x - SPAWN_X, lockPlacement.y);
    printBoard(newState.board);
    printf("Numholes %d\n", newState.numTrueHoles);
    maybePrint("badness %01f, evalScore %01f\n", badness, evalScore);
  }

  return evalScore;
}

// Short-circuits factor calculation to 0.0f when its weight coefficient is zero.
#define EVAL_FACTOR(coef, expr) ((coef) != 0.0f ? ((coef) * (expr)) : 0.0f)

float fastEval(const GameState &gameState, const GameState &newState, const LockPlacement &lockPlacement,
               const EvalContext *evalContext, std::string *outExplanation) {
  if (SHOULD_PLAY_PERFECT) {
    return evalForPerfectPlay(gameState, newState, lockPlacement, evalContext);
  }

  const FastEvalWeights &weights = evalContext->weights;
  bool hasHoles = newState.numTrueHoles > 0;
  bool hasHolesOrTucks = hasHoles || (newState.numPartialHoles > 0);
  // Preliminary helper work
  float avgHeight = getAverageHeight(newState.surfaceArray, evalContext->wellColumn);
  int isKillscreenLineout = gameState.level >= 29 && evalContext->aiMode == LINEOUT;

  // Calculate all the factors
  float avgHeightFactor = EVAL_FACTOR(weights.avgHeightCoef,
                                      getAverageHeightFactor(avgHeight, evalContext->scareHeight));
  float builtOutLeftFactor = EVAL_FACTOR(weights.builtOutLeftCoef,
                                         getBuiltOutLeftFactor(newState.surfaceArray, newState.board, avgHeight,
                                                               evalContext->scareHeight, hasHolesOrTucks));
  float coveredWellFactor = EVAL_FACTOR(weights.coveredWellCoef,
                                        getCoveredWellFactor(newState.board, newState.surfaceArray,
                                                             evalContext->wellColumn, evalContext->scareHeight));

  BurnAndHoleWeightCounts burnAndHole =
      getGuaranteedBurnsAndHoleWeight(newState.board, newState.surfaceArray, evalContext->wellColumn,
                                     hasHoles, std::abs(weights.holeWeightCoef) > FLOAT_EPSILON);
  float guaranteedBurnsFactor = weights.burnCoef * burnAndHole.guaranteedBurns;
  float holeWeightFactor = weights.holeWeightCoef * burnAndHole.holeWeight;

  float likelyBurnsFactor = EVAL_FACTOR(weights.burnCoef,
                                        getLikelyBurnsFactor(newState.surfaceArray, evalContext->wellColumn,
                                                             evalContext->maxSafeCol9));
  float highCol9Factor = EVAL_FACTOR(weights.col9Coef,
                                     getCol9Factor(newState.surfaceArray[8], evalContext->maxSafeCol9));
  float holeFactor = weights.holeCoef * (newState.numTrueHoles + newState.numPartialHoles);

  float inaccessibleLeftFactor = isKillscreenLineout ? 0.0f : EVAL_FACTOR(weights.inaccessibleLeftCoef,
      getInaccessibleLeftFactor(newState.board, newState.surfaceArray,
                                evalContext->pieceRangeContext.maxAccessibleLeft5Surface,
                                evalContext->pieceRangeContext.maxAccessibleLeftLPieceSurface,
                                evalContext->wellColumn, evalContext->aiMode, hasHoles));

  float inaccessibleRightFactor = isKillscreenLineout ? 0.0f : EVAL_FACTOR(weights.inaccessibleRightCoef,
      getInaccessibleRightFactor(newState.surfaceArray, evalContext->pieceRangeContext.maxAccessibleRightSurface));

  float lineClearFactor = getLineClearFactor(newState.lines - gameState.lines, weights,
                                             evalContext->shouldRewardLineClears);
  float surfaceFactor = weights.surfaceCoef * rateSurface(newState.surfaceArray, evalContext);

  float surfaceLeftFactor = isKillscreenLineout ? EVAL_FACTOR(weights.surfaceLeftCoef,
      getLeftSurfaceFactor(newState.board, newState.surfaceArray, evalContext->pieceRangeContext.max5TapHeight)) : 0.0f;

  float tetrisReadyFactor = (evalContext->wellColumn >= 0 && weights.tetrisReadyCoef != 0.0f
                             && isTetrisReady(newState.board, newState.surfaceArray, evalContext->wellColumn))
                                ? weights.tetrisReadyCoef
                                : 0.0f;

  float unableToBurnFactor = EVAL_FACTOR(weights.unableToBurnCoef,
      getUnableToBurnFactor(newState.board, newState.surfaceArray, evalContext->scareHeight, hasHoles));

  float total = surfaceFactor
                + surfaceLeftFactor
                + avgHeightFactor
                + lineClearFactor
                + holeFactor
                + holeWeightFactor
                + guaranteedBurnsFactor
                + likelyBurnsFactor
                + inaccessibleLeftFactor
                + inaccessibleRightFactor
                + coveredWellFactor
                + highCol9Factor
                + tetrisReadyFactor
                + builtOutLeftFactor
                + unableToBurnFactor;
  total = max(weights.deathCoef, total); // Can't be worse than death

  // Build a human-readable explanation of the non-zero eval factors.
  // Only runs when the caller passes a non-null outExplanation pointer, so
  // other request types avoid the std::string allocation overhead.
  if (outExplanation != nullptr) {
    std::string explanation;
    // Helper that appends "name=value;" for factors whose magnitude is significant.
    auto addFactor = [&](const char *name, float value) {
      if (fabs(value) > FLOAT_EPSILON) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s=%.2f;", name, value);
        explanation += buf;
      }
    };
    addFactor("surface", surfaceFactor);
    addFactor("surfaceLeft", surfaceLeftFactor);
    addFactor("avgHeight", avgHeightFactor);
    addFactor("lineClear", lineClearFactor);
    addFactor("hole", holeFactor);
    addFactor("holeWeight", holeWeightFactor);
    addFactor("guaranteedBurns", guaranteedBurnsFactor);
    addFactor("likelyBurns", likelyBurnsFactor);
    addFactor("inaccessibleLeft", inaccessibleLeftFactor);
    addFactor("inaccessibleRight", inaccessibleRightFactor);
    addFactor("coveredWell", coveredWellFactor);
    addFactor("highCol9", highCol9Factor);
    addFactor("tetrisReady", tetrisReadyFactor);
    addFactor("builtOutLeft", builtOutLeftFactor);
    addFactor("unableToBurn", unableToBurnFactor);
    if (!explanation.empty() && explanation.back() == ';') {
      explanation.pop_back(); // Remove trailing separator
    }
    *outExplanation = explanation;
  }

  // Logging
  if (LOGGING_ENABLED) {
    maybePrint("\nEvaluating possibility %d %d %d\n", lockPlacement.rotationIndex,
               lockPlacement.x - SPAWN_X, lockPlacement.y);
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

    printf("Numholes %d\n", newState.numTrueHoles);
    maybePrint("AiMode %d\n", evalContext->aiMode);
    maybePrint("Surface %01f, LeftSurface %01f, AvgHeight %01f, LineClear %01f, Hole %01f, "
               "HoleWeight %01f, "
               "GuaranteedBurns %01f, LikelyBurns %01f, InaccLeft %01f, CoveredWell %01f, HighCol9 "
               "%01f, TetrisReady "
               "%01f, BuiltLeft %01f, UnableToBrn %01f,\t Total: %01f\n",
               surfaceFactor, surfaceLeftFactor, avgHeightFactor, lineClearFactor, holeFactor,
               holeWeightFactor, guaranteedBurnsFactor, likelyBurnsFactor, inaccessibleLeftFactor,
               coveredWellFactor, highCol9Factor, tetrisReadyFactor, builtOutLeftFactor,
               unableToBurnFactor, total);
  }

  return total;
}

#undef EVAL_FACTOR

