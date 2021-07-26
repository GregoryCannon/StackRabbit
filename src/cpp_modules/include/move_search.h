#ifndef MOVE_SEARCH
#define MOVE_SEARCH

#include "types.h"
#include "utils.h"
#include <vector>

int moveSearch(GameState gameState, Piece piece, OUT std::vector<SimState> &lockPlacements);

#endif