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
  bool usedNoNextBoxPath = false;   // true when every initial placement locked before reactionTime
  PrecomputeValueMap lockValues[7]; // one map per next piece (shared 1-ply map in NNB mode)
  PrecomputeValueMap partialPenalties[7]; // DAS mode only
  PrecomputeValueMap minPenalties[7];     // DAS mode only
};

/**
 * Steps 1-4 of the pipeline: the current piece's full move search with the
 * player's real timeline, DAS charge and reaction time; the phantom placements;
 * one adjustment search per phantom that captured a mid-air state; and finally
 * the lock value lookups (seven concurrent 2-ply lookups when at least one
 * phantom can adjust, or one 1-ply next-box-independent lookup plus seven
 * concurrent shallow DAS penalty maps in DAS mode when every initial placement
 * locks before reaction time). Running the phantom and adjustment searches
 * before the timed lock lookups lets timed requests measure their exact
 * duration against the deadline before sizing playouts. A current piece with
 * no legal placement yields a state with no phantoms (the adjustment and
 * lookup steps are skipped entirely).
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

/** Per-next-piece breakdown for one phantom inside collapseAndFormatPrecompute. */
struct PrecomputePieceBreakdown {
  bool valid = false;
  bool isContinue = false;
  std::string key; // "rot|x|y"
  int rot = 0;
  int x = 0;
  int y = 0;
  int relRot = 0; // relative to spawn (INITIAL_X, piece->initialY)
  int relX = 0;
  int relY = 0;
  double rawLockValue = 0.0;
  double dasPenalty = 0.0;
  const char *dasBucket = "tap"; // "full" | "partial" | "min" | "tap"
  double inputCost = 0.0;
  double totalValue = 0.0;
  int dasChargeAfter = -1;
  int linesCleared = 0;
};

/** Full collapse diagnostic breakdown for one phantom. */
struct PrecomputePhantomBreakdown {
  int phantomIndex = 0;
  bool hasInitialPlacement = false;
  std::string defaultKey; // "rot|x|y", or "" when reactionTime == 0
  int rot = 0;
  int x = 0;
  int y = 0;
  int relRot = 0;
  int relX = 0;
  int relY = 0;
  int dasChargeAfter = -1;
  int linesCleared = 0;
  std::string inputPrefix;
  int numAdjustments = 0;
  std::vector<std::string> adjustmentKeys;
  double weightedAvg = 0.0;      // NES-weighted score WITH adjustments
  double noAdjWeightedAvg = 0.0; // NES-weighted score using initial placement only
  bool hasNoAdjAvg = false;
  bool isWinner = false;
  PrecomputePieceBreakdown byPiece[7]; // indexed by PIECE_LIST (I,O,L,J,T,S,Z)
};

/**
 * Evaluates every phantom in `state` using the exact same collapse math and
 * tie-breaking as collapseAndFormatPrecompute, returning per-phantom and
 * per-next-piece breakdowns sorted by descending weightedAvg (winner first).
 */
std::vector<PrecomputePhantomBreakdown> analyzePrecomputeCollapse(const PrecomputeState &state,
                                                                  const Piece *piece);

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
