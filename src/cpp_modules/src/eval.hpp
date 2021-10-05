#ifndef EVAL
#define EVAL

#include <vector>
#include "types.hpp"

float getLineClearFactor(int numLinesCleared, FastEvalWeights weights, int shouldRewardLineClears);

float fastEval(GameState gameState, GameState newState, SimState lockPlacement, const EvalContext *evalContext);

#endif
