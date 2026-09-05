#include "types.hpp"
#include <iomanip>

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

vector<int> simulateGame(char const *inputFrameTimeline, int startingLevel, int maxLines, int shouldAdjust, int reactionTime, int playoutCount, int playoutLength){
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
  return {score, gameState.lines};
}

void printStats(std::vector<int>& data) {
    if (data.empty() || data.size() == 0) return;

    double n = data.size();
    double mean = std::accumulate(data.begin(), data.end(), 0.0) / n;
    
    double sq_sum = 0.0;
    for (int x : data) sq_sum += (x - mean) * (x - mean);
    
    // Uses sample standard deviation (divides by N - 1). For population, use data.size().
    double stdev = std::sqrt(sq_sum / (n - 1));
    
    double ci_lower = mean;
    double ci_upper = mean;
    
    // Common Z-Scores: 90% = 1.645 | 95% = 1.960 | 99% = 2.576
    const double z_90 = 1.645;
    const double z_95 = 1.960; 
    
    // Calculate standard error: SE = stdev / sqrt(n)
    double standard_error = stdev / std::sqrt(data.size());
    
    // Calculate margin of error
    double margin_of_error = z_90 * standard_error;

    // Set the locale to the user's environment default (en_US usually utilizes commas)
    // printf("Average: %.1f\n +/- %.1f (Stdev: %.1f)\n", mean, margin_of_error, stdev);
    std::cout << std::fixed << std::setprecision(0) << "Average: " << mean << "\t+/-: " << margin_of_error << "\t(stdev: " << stdev << ")\n";
}

void simulateGames(int numGames, char const *inputFrameTimeline, int startingLevel, int maxLines, int shouldAdjust, int reactionTime, int playoutCount, int playoutLength, OUT std::vector<int> &scores){
  printf("Starting game simulations...\n");

  auto time_start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::cout.imbue(std::locale("en_US.UTF-8"));


  for (int i = 0; i < numGames; i++) {
    vector<int> result = simulateGame(inputFrameTimeline, startingLevel, maxLines, /* shouldAdjust= */ false, /* reactionTime */ 21, playoutCount, playoutLength);
    scores.push_back(result[0]);
    std::cout << i << ": " << result[0] << "Lines: " << result[1] << std::endl;
  }

  auto time_end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  printf("Time elapsed: %lld seconds\n", (time_end - time_start)/1000);

  printStats(scores);
}
