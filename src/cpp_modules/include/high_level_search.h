#ifndef HIGH_LEVEL_SEARCH
#define HIGH_LEVEL_SEARCH

#include "types.h"
#include "utils.h"
#include <list>
#include <algorithm>

int searchDepth2(GameState gameState, Piece firstPiece, Piece secondPiece, int keepTopN, EvalContext const *evalContext, OUT list<Depth2Possibility> &possibilityList);

std::string getLockValueLookupEncoded(GameState gameState, Piece firstPiece, Piece secondPiece, int keepTopN, EvalContext const *evalContext);

#endif
