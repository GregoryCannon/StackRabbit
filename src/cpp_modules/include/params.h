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
  /* tetrisCoef= */ 40,
  /* tetrisReadyCoef= */ 6,
  /* surfaceCoef= */ 1
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
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef
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
  /* tetrisCoef= */ 500,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef
};

const FastEvalWeights DIRTY_NEAR_KILLSCREEN_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.builtOutLeftCoef,
  /* burnCoef= */ 20,  // Incentivize getting the last few line clears in
  MAIN_WEIGHTS.coveredWellCoef,
  MAIN_WEIGHTS.col9Coef,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  /* tetrisCoef= */ 500,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef
};

const FastEvalWeights LINEOUT_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  /* builtOutLeftCoef= */ 6,
  /* burnCoef= */ 2,  // Reward line clears
  /* coveredWellCoef= */ 0,
  /* col9Coef= */ 0,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef
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

const EvalContext DEBUG_CONTEXT = {
  /* aiMode= */ STANDARD,
  /* countWellHoles= */ false,
  /* evalWeights= */ MAIN_WEIGHTS,
  /* inputFrameTimeline= */ "X....",
  /* maxDirtyTetrisHeight= */ 1,
  /* maxSafeCol9Height= */ 6,
  /* scareHeight= */ 5,
  /* wellColumn= */ 9,
};


#endif
