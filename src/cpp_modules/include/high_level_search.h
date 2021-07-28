#ifndef HIGH_LEVEL_SEARCH
#define HIGH_LEVEL_SEARCH

#include "types.h"
#include "utils.h"
#include <list>

int searchDepth2(GameState, Piece firstPiece, Piece secondPiece, int keepTopN, OUT list<Depth2Possibility> &possibilityList);

#endif
