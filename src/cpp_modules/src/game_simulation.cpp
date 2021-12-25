#include "types.hpp"

const int SCORE_REWARDS[] = {
  0,
  40,
  100,
  300,
  1200
};

int simulateGame(char const *inputFrameTimeline, int startingLevel, int maxLines, int shouldAdjust, int reactionTime){
  // Init empty data structures
  GameState gameState = {
    /* board= */ {},
    /* surfaceArray= */ {},
    /* adjustedNumHole= */ 0,
    /* lines= */ 0,
    /* level= */ startingLevel
  };
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  Piece curPiece;
  Piece nextPiece = PIECE_LIST[qualityRandom(0,7)];

  // Calculate global context for the 3 possible gravity values
  const PieceRangeContext pieceRangeContextLookup[3] = {
    getPieceRangeContext(inputFrameTimeline, 1),
    getPieceRangeContext(inputFrameTimeline, 2),
    getPieceRangeContext(inputFrameTimeline, 3),
  };
  int score = 0;

  while (true) {
    // Get pieces
    curPiece = nextPiece;
    nextPiece = getRandomPiece(curPiece);

    // Figure out modes and eval context
    const EvalContext evalContextRaw = getEvalContext(gameState, pieceRangeContextLookup);
    const EvalContext *evalContext = &evalContextRaw;

    // Get the lock placements
    std::vector<LockPlacement> lockPlacements;
    moveSearch(gameState, &curPiece, evalContext->pieceRangeContext.inputFrameTimeline, lockPlacements);

    if (lockPlacements.size() == 0) {
      break;
    }

    // Pick the best placement
    LockPlacement bestMove = pickLockPlacement(gameState, evalContext, lockPlacements);

//    if (shouldAdjust){
//      predictSearchStateAtAdjustmentTime()
//    }

    // Otherwise, update the state to keep playing
    int oldLines = gameState.lines;
    gameState = advanceGameState(gameState, bestMove, evalContext);
    score += SCORE_REWARDS[gameState.lines - oldLines] * gameState.level;

    if (PLAYOUT_LOGGING_ENABLED) {
      printBoard(gameState.board);
      printf("Best placement: %c %d, %d\n\n", bestMove.piece->id, bestMove.rotationIndex, bestMove.x - SPAWN_X);
      printf("Score: %d, Lines: %d, Level: %d\n", score, gameState.lines, gameState.level);
    }

    if (maxLines > 0 && gameState.lines > maxLines) {
      break;
    }
  }
  return score;
}

void simulateGames(int numGames, char const *inputFrameTimeline, int startingLevel, int maxLines, int reactionTime, OUT std::vector<int> scores){
  for (int i = 0; i < numGames; i++) {
    scores.push_back(simulateGame(inputFrameTimeline, startingLevel, maxLines, /* shouldAdjust= */ false, /* reactionTime */ 21));
  }
}
