#ifndef EVAL
#define EVAL

#include <vector>
#include "types.h"

float getLineClearFactor(int numLinesCleared, FastEvalWeights weights);

float fastEval(GameState gameState, SimState lockPlacement, EvalContext evalContext, FastEvalWeights weights);

#endif