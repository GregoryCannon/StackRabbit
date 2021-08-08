#ifndef MOVE_SEARCH
#define MOVE_SEARCH

#include "types.h"
#include "utils.h"
#include <vector>

int moveSearch(GameState gameState, Piece piece, char const *inputFrameTimeline, OUT std::vector<SimState> &lockPlacements);

#endif
