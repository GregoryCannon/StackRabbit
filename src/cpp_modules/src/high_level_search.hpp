#ifndef HIGH_LEVEL_SEARCH
#define HIGH_LEVEL_SEARCH

#include "types.hpp"
#include "utils.hpp"
#include "config.hpp"
#include <list>
#include <algorithm>
#include <unordered_map>

LockLocation playOneMove(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int numCandidatesToPlayout, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3], std::string *outExplanation = nullptr);

std::string getTopMoveList(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int pruningBreadth, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3], bool generateExplanation = true, unsigned int *playerBoardAfter = nullptr);

std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int pruningBreadth, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]);

std::string getLockValueLookupDas(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int pruningBreadth, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]);

int searchDepth1(GameState gameState, const Piece *firstPiece, const EvalContext *evalContext, OUT list<Possibility> &possibilityList, bool generateExplanation = false);

int searchDepth2(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int secondPieceDelay, const EvalContext *evalContext, OUT list<Possibility> &possibilityList, bool generateExplanation = false);

#endif
