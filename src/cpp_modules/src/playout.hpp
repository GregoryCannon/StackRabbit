#ifndef PLAYOUT
#define PLAYOUT

#include "types.hpp"
#include <vector>

LockPlacement pickLockPlacement(GameState gameState, const EvalContext *evalContext,
                                OUT std::vector<LockPlacement> &lockPlacements);

float getPlayoutScore(GameState gameState, int playoutCount, int playoutLength,
                      const PieceRangeContext pieceRangeContextLookup[3], int pieceOffsetIndex,
                      AggressionLevel aggressionLevel, AiMode originalAiMode,
                      OUT vector<PlayoutData> *playoutDataList,
                      OUT vector<float> *outPlayoutScores = nullptr, int sequenceOffset = 0);

#endif
