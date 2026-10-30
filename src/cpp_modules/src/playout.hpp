#ifndef PLAYOUT
#define PLAYOUT

#include "types.hpp"
#include "utils.hpp"
#include <vector>
#include <list>

LockPlacement pickLockPlacement(GameState gameState,
                           const EvalContext *evalContext,
                           OUT std::vector<LockPlacement> &lockPlacements);

/**
 * Plays out `playoutCount` piece sequences and returns their mean score.
 * @param playoutDataList - optional per-playout detail data (see PlayoutData)
 * @param perSequenceScores - optional OUT vector that receives one score per played
 *   sequence, in play order. Callers that score multiple states on the same sequence
 *   window (e.g. the tournament search) use this for paired comparisons.
 * @param batchOffset - offset into the canonical sequence batch, for callers that want
 *   different rounds to use different sequence windows while keeping the window shared
 *   across all states scored in the same round (common random numbers).
 */
float getPlayoutScore(GameState gameState, int playoutCount, int playoutLength, const PieceRangeContext pieceRangeContextLookup[3], int pieceOffsetIndex, AggressionLevel aggressionLevel, OUT vector<PlayoutData> *playoutDataList, OUT vector<float> *perSequenceScores = NULL, int batchOffset = 0);

#endif
