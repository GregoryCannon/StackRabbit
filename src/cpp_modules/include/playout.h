#ifndef PLAYOUT
#define PLAYOUT

#include "types.h"
#include "utils.h"
#include <vector>

SimState
pickLockPosition(int board[20], int surfaceArray[10], Piece piece, OUT std::vector<SimState> &lockPlacements);

#endif