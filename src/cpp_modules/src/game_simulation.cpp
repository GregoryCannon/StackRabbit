#include "types.hpp"

const int SCORE_REWARDS[] = {
  0,
  40,
  100,
  300,
  1200
};

int countInputsBeforeReactionTime(int reactionTime, char const *inputFrameTimeline) {
  int numInputs = 0;
  for (int i = 0; i < reactionTime; i++){
    if (shouldPerformInputsThisFrame(i, inputFrameTimeline)){
      numInputs++;
    }
  }
  return numInputs;
}

int simulateGame(char const *inputFrameTimeline, int startingLevel, int maxLines, int shouldAdjust, int reactionTime, int playoutCount, int playoutLength){
  // Init empty data structures
  GameState gameState = {
    /* board= */ {},
    /* surfaceArray= */ {},
    /* numTrueHole= */ 0,
    /* numPartialHoles= */ 0,
    /* lines= */ 0,
    /* level= */ startingLevel
  };
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  Piece curPiece;
  Piece nextPiece = PIECE_LIST[qualityRandom(0,7)];

  // Calculate global context for the 4 possible gravity values
  const PieceRangeContext pieceRangeContextLookup[4] = {
    getPieceRangeContext(inputFrameTimeline, 1, /* gravityDoubled= */ true),
    getPieceRangeContext(inputFrameTimeline, 1, /* gravityDoubled= */ false),
    getPieceRangeContext(inputFrameTimeline, 2, /* gravityDoubled= */ false),
    getPieceRangeContext(inputFrameTimeline, 3, /* gravityDoubled= */ false),
  };
  int score = 0;
  int numMoves = 0;

  while (true) {
    numMoves++;
    
    // Get pieces
    curPiece = nextPiece;
    nextPiece = getRandomPiece(curPiece);

    // Figure out modes and eval context
    const EvalContext evalContextRaw = getEvalContext(gameState, pieceRangeContextLookup);
    const EvalContext *evalContext = &evalContextRaw;

    LockLocation bestMove = playOneMove(gameState, &curPiece, NULL, DEFAULT_PRUNING_BREADTH, playoutCount, playoutLength, evalContext, pieceRangeContextLookup);
    if (bestMove.x == NONE){
      // Agent died, simulated game is complete
      break;
    }
    LockPlacement bestPlacement = {
      bestMove.x, bestMove.y, bestMove.rotationIndex, -1, NO_TUCK_NOTATION, &curPiece
    };
    // Update the state to keep playing
    int oldLines = gameState.lines;
    gameState = advanceGameState(gameState, bestPlacement, evalContext);
    score += SCORE_REWARDS[gameState.lines - oldLines] * gameState.level;

    if (SIMULATION_LOGGING_ENABLED) {
      printBoard(gameState.board);
      printf("Best placement: %c %d, %d\n\n", bestPlacement.piece->id, bestPlacement.rotationIndex, bestPlacement.x - SPAWN_X);
      printf("Score: %d, Lines: %d, Level: %d\n", score, gameState.lines, gameState.level);
    }

    if (maxLines > 0 && gameState.lines > maxLines) {
      break;
    }
  }
  return score;
}

void simulateGames(int numGames, char const *inputFrameTimeline, int startingLevel, int maxLines, int shouldAdjust, int reactionTime, int playoutCount, int playoutLength, OUT std::vector<int> &scores){
  printf("Starting game simulations...");
  for (int i = 0; i < numGames; i++) {
    int score = simulateGame(inputFrameTimeline, startingLevel, maxLines, /* shouldAdjust= */ false, /* reactionTime */ 21, playoutCount, playoutLength);
    scores.push_back(score);
    printf("%d: %d\n", i, score);
  }
}
