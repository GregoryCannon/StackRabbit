#include "types.hpp"

AiMode getAiMode(const GameState &gameState, const PieceRangeContext pieceRangeContextLookup[]);

const EvalContext getEvalContext(GameState gameState,
                                 const PieceRangeContext pieceRangeContextLookup[],
                                 bool useTestWeights, AggressionLevel aggressionLevel);