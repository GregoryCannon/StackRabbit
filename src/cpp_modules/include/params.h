#ifndef PARAMS
#define PARAMS

const FastEvalWeights MAIN_WEIGHTS = {
  /* avgHeightCoef= */ -5,
  /* builtOutLeftCoef= */ 1.5,
  /* burnCoef= */ -12,
  /* coveredWellCoef= */ -5,
  /* col9Coef= */ -3,
  /* deathCoef= */ -2000,
  /* extremeGapCoef= */ -2,
  /* holeCoef= */ -40,
  /* inaccessibleLeftCoef= */ -50,
  /* inaccessibleRightCoef= */ -200,
  /* tetrisCoef= */ 40,
  /* tetrisReadyCoef= */ 6,
  /* surfaceCoef= */ 1,
  /* surfaceLeft= */ 0,
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
  MAIN_WEIGHTS.inaccessibleLeftCoef,
  MAIN_WEIGHTS.inaccessibleRightCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef,
  MAIN_WEIGHTS.surfaceLeftCoef
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
  MAIN_WEIGHTS.surfaceLeftCoef
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
  MAIN_WEIGHTS.surfaceLeftCoef
};

const FastEvalWeights LINEOUT_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  /* builtOutLeftCoef= */ 10,
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
    case STANDARD:
      return MAIN_WEIGHTS;
  }
}


#endif
