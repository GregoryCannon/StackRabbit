#ifndef MOVE_SEARCH
#define MOVE_SEARCH

#include "types.h"
#include "utils.h"
#include <vector>

int moveSearch(int board[20], int surfaceArray[10], Piece piece, OUT std::vector<SimState> &lockPlacements);

#endif