#ifndef PRECOMPUTE_HPP
#define PRECOMPUTE_HPP

#include "full_move_search.hpp"
#include "types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

/**
 * The synchronous precompute pipeline (see precompute.cpp for the full
 * description). It splits into two halves so each can run on its own:
 *
 *   - preparePrecomputeState runs the searches (steps 1-4) and returns the
 *     mid-pipeline state: the phantom placements with their adjustment
 *     searches, the per-next-piece lock value maps, and the two request
 *     fields (DAS mode, reaction time) the collapse needs.
 *   - collapseAndFormatPrecompute reduces that state to the response string
 *     (steps 5-6). It is a pure function of the state plus the board, piece,
 *     level, lines and push-down flag, so it can also be driven with a
 *     hand-built state: synthetic phantom sets and lock value maps.
 */
struct PrecomputeState;

/** One placement outcome shaped for the collapse and the formatters. */
struct PrecomputePossibility {
  int relRot; // placement relative to the search's entry state
  int relX;
  int relY;
  int rot; // absolute lock coordinates: the lock value map keys
  int x;
  int y;
  std::string inputSequence;
  char tuckInput; // '.' for basic placements
  int dasChargeAfter;
  int numLinesCleared;
};

/** One candidate initial placement and its adjustment search. */
struct PrecomputePhantom {
  std::string inputSequence; // pre-reaction input prefix (up to the reaction-time slice)
  bool hasInitialPlacement = false;
  PrecomputePossibility initial{};
  bool hasAdjustmentState = false;
  FullMoveSearchContext adjustmentCtx;
  int adjustmentDasCharge = 0;
  std::vector<PrecomputePossibility> possibleAdjustments;
};

typedef std::unordered_map<std::string, double> PrecomputeValueMap;

/** The searches' output: what the collapse and formatters consume. */
struct PrecomputeState {
  std::vector<PrecomputePhantom> phantoms;
  bool useDAS = false; // false = tap mode (incoming dasCharge == -1)
  int reactionTime = 0;
  PrecomputeValueMap lockValues[7];       // one map per next piece
  PrecomputeValueMap partialPenalties[7]; // DAS mode only
  PrecomputeValueMap minPenalties[7];     // DAS mode only
};

/**
 * Steps 1-4 of the pipeline: the current piece's full move search with the
 * player's real timeline, DAS charge and reaction time; the phantom placements;
 * one adjustment search per phantom that captured a mid-air state; and finally
 * one lock value lookup per next piece (seven concurrent threads, one per piece).
 * Running the phantom and adjustment searches before the timed lock lookups lets
 * timed requests measure their exact duration against the deadline before
 * sizing playouts. A current piece with no legal placement yields a state with
 * no phantoms (the adjustment and lookup steps are skipped entirely).
 */
PrecomputeState preparePrecomputeState(GameState gameState, const Piece *curPiece,
                                       char const *inputFrameTimeline, int dasCharge,
                                       int reactionTime, int pruningBreadth, int playoutCount,
                                       int playoutLength, const EvalContext *evalContext,
                                       const PieceRangeContext pieceRangeContextLookup[3],
                                       const SearchBudget *budget = nullptr);

/**
 * Steps 5-6: the collapse and the response formatting. Per phantom, the best
 * value per next piece weighted by the NES piece-transition probabilities and
 * averaged over the seven pieces; the phantom with the highest average wins
 * (ties keep the first). pushDown enables the push-down input rewrite on the
 * per-piece lines when the level after the placement is 18.
 */
std::string collapseAndFormatPrecompute(const PrecomputeState &state, const unsigned int board[20],
                                        const Piece *piece, int level, int lines, bool pushDown);

/**
 * Formats one spawn-entry placement from fullMoveSearch into the wire format:
 * "relRot,relX,relY|inputSequence|boardAfter|levelAfter|linesAfter|dasChargeAfter".
 */
std::string formatSinglePlacement(const FullMovePossibility &possibility,
                                  const unsigned int board[20], const Piece *piece, int level,
                                  int lines, bool pushDown = false);

/**
 * The full pipeline for one request: prepare the state from the game state,
 * then collapse and format it. pushDown comes from SHOULD_PUSHDOWN.
 */
std::string runPrecompute(GameState gameState, const Piece *curPiece,
                          char const *inputFrameTimeline, int dasCharge, int reactionTime,
                          int pruningBreadth, int playoutCount, int playoutLength,
                          const EvalContext *evalContext,
                          const PieceRangeContext pieceRangeContextLookup[3],
                          const SearchBudget *budget = nullptr);

/**
 * The backup response for the live game server: the best-eval placement of the
 * current piece plus keep-playing-the-default markers for every next piece.
 * Served when the full precompute has not finished by poll time. Takes the
 * same arguments as runPrecompute (the search parameters are unused -- only
 * the first pipeline step runs) so callers can switch between the two freely.
 * The response is "none" when the piece has no legal placement, otherwise
 * "Default:<placement>" plus one ":continue" line per next piece.
 */
std::string runPrecomputeBackup(GameState gameState, const Piece *curPiece,
                                char const *inputFrameTimeline, int dasCharge, int reactionTime,
                                int pruningBreadth, int playoutCount, int playoutLength,
                                const EvalContext *evalContext,
                                const PieceRangeContext pieceRangeContextLookup[3]);

#endif // PRECOMPUTE_HPP
