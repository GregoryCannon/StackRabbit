#ifndef EVAL
#define EVAL

#include "types.hpp"
#include <string>
#include <vector>

inline float getLineClearFactor(int numLinesCleared, const FastEvalWeights &weights, int shouldRewardLineClears) {
  return numLinesCleared == 4
             ? weights.tetrisCoef
             : weights.burnCoef * numLinesCleared * (shouldRewardLineClears ? -0.25 : 1);
}

float fastEval(const GameState &gameState, const GameState &newState, const LockPlacement &lockPlacement,
               const EvalContext *evalContext, std::string *outExplanation = nullptr);

#endif
