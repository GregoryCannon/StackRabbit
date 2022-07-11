#ifndef PARAMS
#define PARAMS

#include "config.hpp"
#include "types.hpp"

const FastEvalWeights MAIN_WEIGHTS = {
  /* avgHeightCoef= */ PLAY_SAFE_PRE_KILLSCREEN ? -8 : -7,
  /* builtOutLeftCoef= */ 2,
  /* burnCoef= */ PLAY_SAFE_PRE_KILLSCREEN ? -9 : -14,
  /* coveredWellCoef= */ -5,
  /* col9Coef= */ -3,
  /* deathCoef= */ -3000,
  /* extremeGapCoef= */ -3,
  /* holeCoef= */ -50,
  /* holeWeightCoef= */ 0,
  /* inaccessibleLeftCoef= */ -100,
  /* inaccessibleRightCoef= */ -200,
  /* tetrisCoef= */ 40,
  /* tetrisReadyCoef= */ 6,
  /* surfaceCoef= */ 1,
  /* surfaceLeft= */ 0,
  /* unableToBurnCoef= */ -0.5
};

// No weights apply here, all are automatically set to 0
const FastEvalWeights PLAY_PERFECT_WEIGHTS = {
  /* avgHeightCoef= */ 0,
  /* builtOutLeftCoef= */ 0,
  /* burnCoef= */ -50,
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

const FastEvalWeights SAFE_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.builtOutLeftCoef,
  /* burnCoef= */ -6,
  MAIN_WEIGHTS.coveredWellCoef,
  MAIN_WEIGHTS.col9Coef,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  MAIN_WEIGHTS.holeWeightCoef,
  MAIN_WEIGHTS.inaccessibleLeftCoef,
  MAIN_WEIGHTS.inaccessibleRightCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  MAIN_WEIGHTS.surfaceLeftCoef,
  MAIN_WEIGHTS.unableToBurnCoef
};

const FastEvalWeights DIG_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.builtOutLeftCoef,
  /* burnCoef= */ -1,
  /* coveredWellCoef= */ -2.5,
  /* col9Coef= */ -1,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  /* holeWeightCoef= */ -8,
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
  /* tetrisCoef= */ 500,
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
  /* tetrisCoef= */ 500,
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
  MAIN_WEIGHTS.unableToBurnCoef
};

FastEvalWeights getWeights(AiMode mode){
  if (SHOULD_PLAY_PERFECT){
    return PLAY_PERFECT_WEIGHTS;
  }
  switch (mode) {
    case DIG:
      return DIG_WEIGHTS;
    case NEAR_KILLSCREEN:
      return NEAR_KILLSCREEN_WEIGHTS;
    case DIRTY_NEAR_KILLSCREEN:
      return DIRTY_NEAR_KILLSCREEN_WEIGHTS;
    case LINEOUT:
      return LINEOUT_WEIGHTS;
    case SAFE:
      return SAFE_WEIGHTS;
    case PLAY_PERFECT:
      return PLAY_PERFECT_WEIGHTS;
    case STANDARD:
      return MAIN_WEIGHTS;
    default:
      printf("Unknown AI Mode");
      return {};
  }
}


#endif
