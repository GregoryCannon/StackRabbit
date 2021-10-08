#ifndef PARAMS
#define PARAMS

const FastEvalWeights MAIN_WEIGHTS = {
  /* avgHeightCoef= */ PLAY_SAFE_PRE_KILLSCREEN ? -10 : -5,
  /* builtOutLeftCoef= */ 2,
  /* burnCoef= */ PLAY_SAFE_PRE_KILLSCREEN ? -6 : -12,
  /* coveredWellCoef= */ -5,
  /* col9Coef= */ -2,
  /* deathCoef= */ -2000,
  /* extremeGapCoef= */ -3,
  /* holeCoef= */ -40,
  /* inaccessibleLeftCoef= */ -100,
  /* inaccessibleRightCoef= */ -200,
  /* tetrisCoef= */ 40,
  /* tetrisReadyCoef= */ 6,
  /* surfaceCoef= */ 1,
  /* surfaceLeft= */ 0,
  /* unableToBurnCoef= */ -0.5
};

const FastEvalWeights SAFE_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.builtOutLeftCoef,
  /* burnCoef= */ -5,
  MAIN_WEIGHTS.coveredWellCoef,
  MAIN_WEIGHTS.col9Coef,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
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
  /* holeCoef= */ -60,
  MAIN_WEIGHTS.inaccessibleLeftCoef,
  MAIN_WEIGHTS.inaccessibleRightCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  MAIN_WEIGHTS.surfaceLeftCoef,
  MAIN_WEIGHTS.unableToBurnCoef
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
  /* inaccessibleLeftCoef= */ 0,
  /* inaccessibleRightCoef= */ 0,
  /* tetrisCoef= */ 500,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  MAIN_WEIGHTS.surfaceLeftCoef,
  MAIN_WEIGHTS.unableToBurnCoef
};

const FastEvalWeights LINEOUT_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  /* builtOutLeftCoef= */ 15,
  MAIN_WEIGHTS.burnCoef,
  /* coveredWellCoef= */ 0,
  /* col9Coef= */ 0,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  MAIN_WEIGHTS.inaccessibleLeftCoef,
  MAIN_WEIGHTS.inaccessibleRightCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  /* surfaceLeft= */ 40,
  MAIN_WEIGHTS.unableToBurnCoef
};

FastEvalWeights getWeights(AiMode mode){
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
    case STANDARD:
      return MAIN_WEIGHTS;
    default:
      printf("Unknown AI Mode");
      return {};
  }
}


#endif
