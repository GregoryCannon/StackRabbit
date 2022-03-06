#ifndef PLAYOUT
#define PLAYOUT

#include "types.hpp"
#include "utils.hpp"
#include <vector>
#include <list>

LockPlacement pickLockPlacement(GameState gameState,
                           const EvalContext *evalContext,
                           OUT std::vector<LockPlacement> &lockPlacements);

float getPlayoutScore(GameState gameState, const PieceRangeContext pieceRangeContextLookup[3], int pieceOffsetIndex);

#endif
