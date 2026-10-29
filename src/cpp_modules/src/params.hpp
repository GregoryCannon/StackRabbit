#ifndef PARAMS
#define PARAMS

#include "config.hpp"
#include "types.hpp"

/**
 * The burnCoef from the aggression level input parameter, which overrides
 * the value in MAIN_WEIGHTS-derived weight sets.
 */
float getAggressionBurnCoef(AggressionLevel aggressionLevel){
  switch (aggressionLevel) {
    case SAFE:
      return -9;
    case AGGRO:
      return -15;
    case HYPER:
      return -25;
    default:
      return -9;
  }
}

const FastEvalWeights MAIN_WEIGHTS = {
  /* avgHeightCoef= */ -6,
  /* builtOutLeftCoef= */ 1,
  /* burnCoef= */ -9,
  /* coveredWellCoef= */ -10,
  /* col9Coef= */ -3,
  /* deathCoef= */ -3000,
  /* extremeGapCoef= */ -3,
  /* holeCoef= */ -50,
  /* holeWeightCoef= */ 0,
  /* inaccessibleLeftCoef= */ -30,
  /* inaccessibleRightCoef= */ -200,
  /* tetrisCoef= */ 50,
  /* tetrisReadyCoef= */ 6,
  /* surfaceCoef= */ 1,
  /* surfaceLeft= */ 0,
  /* unableToBurnCoef= */ -0.6
};

const FastEvalWeights TEST_WEIGHTS = {
  /* avgHeightCoef= */ -6,
  /* builtOutLeftCoef= */ 1,
  /* burnCoef= */ -9,
  /* coveredWellCoef= */ -12,
  /* col9Coef= */ -3,
  /* deathCoef= */ -3000,
  /* extremeGapCoef= */ -3,
  /* holeCoef= */ -50,
  /* holeWeightCoef= */ 0,
  /* inaccessibleLeftCoef= */ -30,
  /* inaccessibleRightCoef= */ -200,
  /* tetrisCoef= */ 50,
  /* tetrisReadyCoef= */ 6,
  /* surfaceCoef= */ 1,
  /* surfaceLeft= */ 0,
  /* unableToBurnCoef= */ -0.6
};

// No weights apply here, all are automatically set to 0
const FastEvalWeights PLAY_PERFECT_WEIGHTS = {
  /* avgHeightCoef= */ 0,
  /* builtOutLeftCoef= */ 0,
  /* burnCoef= */ -1000,
  /* coveredWellCoef= */ 0,
  /* col9Coef= */ 0,
  /* deathCoef= */ 0,
  /* extremeGapCoef= */ 0,
  /* holeCoef= */ 0,
  /* holeWeightCoef= */ 0,
  /* inaccessibleLeftCoef= */ 0,
  /* inaccessibleRightCoef= */ 0,
  /* tetrisCoef= */ 0,
  /* tetrisReadyCoef= */ 0,
  /* surfaceCoef= */ 0,
  /* surfaceLeft= */ 0,
  /* unableToBurnCoef= */ 0
};

const FastEvalWeights DIG_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.builtOutLeftCoef,
  /* burnCoef= */ -1,
  /* coveredWellCoef= */ -3,
  /* col9Coef= */ -1,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  /* holeWeightCoef= */ -7,
  MAIN_WEIGHTS.inaccessibleLeftCoef,
  MAIN_WEIGHTS.inaccessibleRightCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  MAIN_WEIGHTS.surfaceLeftCoef,
  /* unableToBurnCoef= */ 0
};

const FastEvalWeights NEAR_KILLSCREEN_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.builtOutLeftCoef,
  MAIN_WEIGHTS.burnCoef,
  MAIN_WEIGHTS.coveredWellCoef,
  MAIN_WEIGHTS.col9Coef,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  MAIN_WEIGHTS.holeWeightCoef,
  MAIN_WEIGHTS.inaccessibleLeftCoef,
  MAIN_WEIGHTS.inaccessibleRightCoef,
  /* tetrisCoef= */ 100,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  MAIN_WEIGHTS.surfaceLeftCoef,
  MAIN_WEIGHTS.unableToBurnCoef
};

const FastEvalWeights DIRTY_NEAR_KILLSCREEN_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.builtOutLeftCoef,
  MAIN_WEIGHTS.burnCoef,
  MAIN_WEIGHTS.coveredWellCoef,
  MAIN_WEIGHTS.col9Coef,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  MAIN_WEIGHTS.holeWeightCoef,
  /* inaccessibleLeftCoef= */ 0,
  /* inaccessibleRightCoef= */ 0,
  /* tetrisCoef= */ 1000,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  MAIN_WEIGHTS.surfaceLeftCoef,
  MAIN_WEIGHTS.unableToBurnCoef
};

// set to avgHeight=-20, builtLeft=8 when testing 20hz killscreen linout

const FastEvalWeights LINEOUT_WEIGHTS = {
  /* avgHeightCoef= */ -10,
  /* builtOutLeftCoef= */ 15,
  /* burnCoef= */ 0,
  /* coveredWellCoef= */ 0,
  /* col9Coef= */ 0,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  MAIN_WEIGHTS.holeWeightCoef,
  MAIN_WEIGHTS.inaccessibleLeftCoef,
  MAIN_WEIGHTS.inaccessibleRightCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  /* surfaceLeft= */ 40,
  /* unableToBurnCoef= */ 0,
};

FastEvalWeights getWeights(AiMode mode, bool useTestWeights, AggressionLevel aggressionLevel){
  if (SHOULD_PLAY_PERFECT){
    return PLAY_PERFECT_WEIGHTS;
  }
  FastEvalWeights weights;
  switch (mode) {
    case DIG:
      weights = DIG_WEIGHTS;
      break;
    case NEAR_KILLSCREEN:
      weights = NEAR_KILLSCREEN_WEIGHTS;
      break;
    case DIRTY_NEAR_KILLSCREEN:
      weights = DIRTY_NEAR_KILLSCREEN_WEIGHTS;
      break;
    case LINEOUT:
      weights = LINEOUT_WEIGHTS;
      break;
    case STANDARD:
      weights = useTestWeights ? TEST_WEIGHTS : MAIN_WEIGHTS;
      break;
    default:
      printf("Unknown AI Mode");
      return {};
  }
  // The aggression level parameter overrides the burnCoef of all MAIN_WEIGHTS-derived sets
  if (mode == STANDARD || mode == NEAR_KILLSCREEN || mode == DIRTY_NEAR_KILLSCREEN){
    weights.burnCoef = getAggressionBurnCoef(aggressionLevel);
  }
  return weights;
}


#endif
