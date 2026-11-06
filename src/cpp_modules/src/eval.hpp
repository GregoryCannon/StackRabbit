#ifndef EVAL
#define EVAL

#include "types.hpp"
#include <string>
#include <vector>

float getLineClearFactor(int numLinesCleared, FastEvalWeights weights, int shouldRewardLineClears);

float fastEval(GameState gameState, GameState newState, LockPlacement lockPlacement,
               const EvalContext *evalContext, std::string *outExplanation = nullptr);

#endif
