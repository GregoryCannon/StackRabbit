#ifndef PLAYOUT
#define PLAYOUT

#include "types.hpp"
#include "utils.hpp"
#include <vector>
#include <list>

SimState pickLockPlacement(GameState gameState,
                           const EvalContext *evalContext,
                           OUT std::vector<SimState> &lockPlacements);

float getPlayoutScore(GameState gameState, const PieceRangeContext pieceRangeContextLookup[3], int offsetIndex);

#endif
