#ifndef HIGH_LEVEL_SEARCH
#define HIGH_LEVEL_SEARCH

#include "types.hpp"
#include "utils.hpp"
#include <list>
#include <algorithm>

LockLocation playOneMove(GameState gameState, const Piece *curPiece, const Piece *nextPiece, int numCandidatesToPlayout, int playoutCount, int playoutLength, const EvalContext *context, const PieceRangeContext pieceRangeContextLookup[3]);

std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]);

#endif
