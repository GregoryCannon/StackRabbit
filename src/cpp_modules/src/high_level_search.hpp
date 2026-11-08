#ifndef HIGH_LEVEL_SEARCH
#define HIGH_LEVEL_SEARCH

#include "config.hpp"
#include "types.hpp"
#include "utils.hpp"
#include <algorithm>
#include <list>
#include <unordered_map>

LockLocation playOneMove(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                         int numCandidatesToPlayout, int playoutCount, int playoutLength,
                         const EvalContext *evalContext,
                         const PieceRangeContext pieceRangeContextLookup[3],
                         std::string *outExplanation = nullptr);

std::string getTopMoveList(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                           int pruningBreadth, int playoutCount, int playoutLength,
                           const EvalContext *evalContext,
                           const PieceRangeContext pieceRangeContextLookup[3],
                           bool generateExplanation = true,
                           unsigned int *playerBoardAfter = nullptr);

std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece,
                                      const Piece *secondPiece, int pruningBreadth,
                                      int playoutCount, int playoutLength,
                                      const EvalContext *evalContext,
                                      const PieceRangeContext pieceRangeContextLookup[3]);

/**
 * The three value maps behind a DAS lock value lookup: the partial-charge and
 * min-charge penalty maps (relative to full charge, clamped at zero) and the
 * deep lock value map. Returned directly so in-process consumers can use the
 * maps without re-parsing generated JSON.
 */
struct DasLockValueMaps {
  std::unordered_map<std::string, float> partialPenalty;
  std::unordered_map<std::string, float> minPenalty;
  std::unordered_map<std::string, float> lockValue;
};

DasLockValueMaps getLockValueLookupDasMaps(GameState gameState, const Piece *firstPiece,
                                           const Piece *secondPiece, int pruningBreadth,
                                           int playoutCount, int playoutLength,
                                           char const *dasInputFrameTimeline,
                                           const EvalContext *evalContext,
                                           const PieceRangeContext pieceRangeContextLookup[3]);

std::string getLockValueLookupDas(GameState gameState, const Piece *firstPiece,
                                  const Piece *secondPiece, int pruningBreadth, int playoutCount,
                                  int playoutLength, char const *dasInputFrameTimeline,
                                  const EvalContext *evalContext,
                                  const PieceRangeContext pieceRangeContextLookup[3]);

unordered_map<string, float> getLockValueLookupMapped(GameState gameState, const Piece *firstPiece,
                                                      const Piece *secondPiece, int pruningBreadth,
                                                      int playoutCount, int playoutLength,
                                                      const EvalContext *evalContext,
                                                      const PieceRangeContext pieceRangeContextLookup[3]);

int searchDepth1(GameState gameState, const Piece *firstPiece, const EvalContext *evalContext,
                 OUT std::list<Possibility> &possibilityList, bool generateExplanation = false);

int searchDepth2(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                 int dasCharge, int secondPieceDasCharge, char const *dasInputFrameTimeline,
                 const EvalContext *evalContext, OUT std::list<Possibility> &possibilityList,
                 bool generateExplanation = false);

#endif
