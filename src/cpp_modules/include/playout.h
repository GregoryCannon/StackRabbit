#ifndef PLAYOUT
#define PLAYOUT

#include "types.h"
#include "utils.h"
#include <vector>
#include <list>

SimState pickLockPlacement(GameState gameState,
                           EvalContext evalContext,
                           FastEvalWeights evalWeights,
                           OUT std::vector<SimState> &lockPlacements);

float getPlayoutScore(GameState gameState, int numPlayouts, EvalContext evalContext, FastEvalWeights weights);

float playSequence(GameState gameState, EvalContext evalContext, FastEvalWeights weights, const int pieceSequence[10]);

#endif
