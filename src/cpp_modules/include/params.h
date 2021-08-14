#ifndef PARAMS
#define PARAMS

const FastEvalWeights MAIN_WEIGHTS = {
  /* avgHeightCoef= */ -5,
  /* builtOutLeftCoef= */ 1.5,
  /* burnCoef= */ -8,
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
  /* coveredWellCoef= */ -1.25,
  /* col9Coef= */ -1,
  MAIN_WEIGHTS.deathCoef,
  MAIN_WEIGHTS.extremeGapCoef,
  MAIN_WEIGHTS.holeCoef,
  MAIN_WEIGHTS.tetrisCoef,
  MAIN_WEIGHTS.tetrisReadyCoef,
  MAIN_WEIGHTS.surfaceCoef
};

const FastEvalWeights LINEOUT_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  /* builtOutLeftCoef= */ 6,
  /* burnCoef= */ 0,
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
  if (mode == DIG) {
    return DIG_WEIGHTS;
  }
  return MAIN_WEIGHTS;
}

const EvalContext DEBUG_CONTEXT = {
  /* aiMode= */ STANDARD,
  /* countWellHoles= */ false,
  /* inputFrameTimeline= */ "X....",
  /* maxDirtyTetrisHeight= */ 1,
  /* maxSafeCol9Height= */ 6,
  /* scareHeight= */ 5,
  /* wellColumn= */ 9,
};


#endif
