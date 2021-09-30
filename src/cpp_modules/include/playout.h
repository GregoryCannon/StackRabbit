#ifndef PLAYOUT
#define PLAYOUT

#include "types.h"
#include "utils.h"
#include <vector>
#include <list>

SimState pickLockPlacement(GameState gameState,
                           const EvalContext *evalContext,
                           OUT std::vector<SimState> &lockPlacements);

float getPlayoutScore(GameState gameState, const PieceRangeContext pieceRangeContextLookup[3], int offsetIndex);

#endif
