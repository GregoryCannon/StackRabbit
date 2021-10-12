#ifndef MOVE_SEARCH
#define MOVE_SEARCH

#include "types.hpp"
#include "utils.hpp"
#include <vector>

int moveSearch(GameState gameState, const Piece *piece, char const *inputFrameTimeline, OUT std::vector<LockPlacement> &lockPlacements, OUT int availableTuckCols[40]);

#endif
