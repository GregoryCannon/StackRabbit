#ifndef EVAL
#define EVAL

#include "types.hpp"
#include <string>

float getLineClearFactor(int numLinesCleared, const FastEvalWeights &weights,
                         int shouldRewardLineClears, int oldLines, bool lineoutAt29);

float fastEval(const GameState &gameState, const GameState &newState,
               const LockPlacement &lockPlacement, const EvalContext *evalContext,
               std::string *outExplanation = nullptr);

#endif
