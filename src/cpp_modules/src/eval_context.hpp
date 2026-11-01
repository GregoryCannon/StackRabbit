#ifndef EVAL_CONTEXT
#define EVAL_CONTEXT

#include "types.hpp"

const EvalContext getEvalContext(GameState gameState, const PieceRangeContext pieceRangeContextLookup[], bool useTestWeights, AggressionLevel aggressionLevel, bool forceStandardMode = false);

#endif