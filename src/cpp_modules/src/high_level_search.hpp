#ifndef HIGH_LEVEL_SEARCH
#define HIGH_LEVEL_SEARCH

#include "types.hpp"
#include <unordered_map>
#include <vector>

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
                                      const PieceRangeContext pieceRangeContextLookup[3],
                                      const SearchBudget *budget = nullptr);

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

/** Precomputed first-piece placement and post-placement state shared across next-piece lookups. */
struct FirstMoveBranch {
  LockPlacement firstPlacement;
  int dasChargeAfter; // DAS mode only: leftover charge after first placement (-1 in tap mode)
  GameState afterFirstMove;
  float firstMoveReward;
};

struct FirstMoveSearchResult {
  bool useDas = false;
  std::vector<FirstMoveBranch> branches;
};

FirstMoveSearchResult buildFirstMoveBranches(const GameState &gameState, const Piece *firstPiece,
                                             int firstPieceDasCharge,
                                             char const *dasInputFrameTimeline,
                                             const EvalContext *evalContext);

DasLockValueMaps getLockValueLookupDasMapsWithFirstMoves(
    const FirstMoveSearchResult &firstMoves, const Piece *secondPiece, int pruningBreadth,
    int playoutCount, int playoutLength, char const *dasInputFrameTimeline,
    const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3],
    const SearchBudget *budget = nullptr);

DasLockValueMaps getLockValueLookupDasMaps(GameState gameState, const Piece *firstPiece,
                                           const Piece *secondPiece, int pruningBreadth,
                                           int playoutCount, int playoutLength,
                                           char const *dasInputFrameTimeline,
                                           const EvalContext *evalContext,
                                           const PieceRangeContext pieceRangeContextLookup[3],
                                           const SearchBudget *budget = nullptr);

std::string getLockValueLookupDas(GameState gameState, const Piece *firstPiece,
                                  const Piece *secondPiece, int pruningBreadth, int playoutCount,
                                  int playoutLength, char const *dasInputFrameTimeline,
                                  const EvalContext *evalContext,
                                  const PieceRangeContext pieceRangeContextLookup[3],
                                  const SearchBudget *budget = nullptr);

unordered_map<string, float> getLockValueLookupMappedWithFirstMoves(
    const FirstMoveSearchResult &firstMoves, const Piece *secondPiece, int pruningBreadth,
    int playoutCount, int playoutLength, const EvalContext *evalContext,
    const PieceRangeContext pieceRangeContextLookup[3], const SearchBudget *budget = nullptr);

unordered_map<string, float> getLockValueLookupMapped(
    GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int pruningBreadth,
    int playoutCount, int playoutLength, const EvalContext *evalContext,
    const PieceRangeContext pieceRangeContextLookup[3], const SearchBudget *budget = nullptr);

std::vector<const Possibility *>
sortPossibilitiesDesc(const std::vector<Possibility> &possibilities);

int searchDepth1(GameState gameState, const Piece *firstPiece, const EvalContext *evalContext,
                 OUT std::vector<Possibility> &possibilities, bool generateExplanation = false);

int searchDepth2(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                 int dasCharge, int secondPieceDasCharge, char const *dasInputFrameTimeline,
                 const EvalContext *evalContext, OUT std::vector<Possibility> &possibilities,
                 bool generateExplanation = false);

#endif
