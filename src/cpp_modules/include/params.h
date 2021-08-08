#ifndef PARAMS
#define PARAMS

const FastEvalWeights MAIN_WEIGHTS = {
  /* avgHeightCoef= */ -5,
  /* avgHeightExponent= */ 2,
  /* burnCoef= */ -8,
  /* coveredWellCoef= */ -5,
  /* col9Coef= */ -3,
  /* deathCoef= */ -10000,
  /* extremeGapCoef= */ -2,
  /* holeCoef= */ -40,
  /* tetrisCoef= */ 40,
  /* tetrisReadyCoef= */ 6,
  /* surfaceCoef= */ 1
};

const FastEvalWeights DIG_WEIGHTS = {
  MAIN_WEIGHTS.avgHeightCoef,
  MAIN_WEIGHTS.avgHeightExponent,
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

FastEvalWeights getWeights(AiMode mode){
  if (mode == DIG) {
    return DIG_WEIGHTS;
  }
  return MAIN_WEIGHTS;
}

const EvalContext DEBUG_CONTEXT = {
  /* inputFrameTimeline= */ "X....",
  /* maxSafeCol9Height= */ 7,
  /* scareHeight= */ 4,
  /* wellColumn= */ 9,
  /* countWellHoles= */ false
};

#endif
