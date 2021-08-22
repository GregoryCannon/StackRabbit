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

float getPlayoutScore(GameState gameState, int numPlayouts, int playoutLength);

float playSequence(GameState gameState, const int pieceSequence[SEQUENCE_LENGTH], int playoutLength);

#endif
