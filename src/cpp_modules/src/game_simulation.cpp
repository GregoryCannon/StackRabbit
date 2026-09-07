#include "types.hpp"
#include <iomanip>
#include <thread>
#include <utility>

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

vector<int> simulateGame(char const *inputFrameTimeline, int startingLevel, int maxLines, int playoutCount, int playoutLength){
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

void printStats(const std::vector<std::pair<int, int>>& games) {
    if (games.empty()) return;

    double n = games.size();
    double sum = 0.0;
    for (const auto& game : games) {
        sum += game.first;
    }
    double mean = sum / n;
    
    double sq_sum = 0.0;
    for (const auto& game : games) {
        sq_sum += (game.first - mean) * (game.first - mean);
    }
    double stdev = std::sqrt(sq_sum / (n - 1));
  
    
    // Common Z-Scores: 90% = 1.645 | 95% = 1.960 | 99% = 2.576
    const double z_90 = 1.645;
    const double z_95 = 1.960; 
    
    // Calculate standard error: SE = stdev / sqrt(n)
    double standard_error = stdev / std::sqrt(games.size());
    
    // Calculate margin of error
    double margin_of_error = z_90 * standard_error;
    double ci_lower = mean - margin_of_error;
    double ci_upper = mean + margin_of_error;

    // Set the locale to the user's environment default (en_US usually utilizes commas)
    // printf("Average: %.1f\n +/- %.1f (Stdev: %.1f)\n", mean, margin_of_error, stdev);
    std::cout << std::fixed << std::setprecision(0) << "Average: " << mean << "\t+/-: " << margin_of_error << " (" << ci_lower << " - " << ci_upper << ")\t(stdev: " << stdev << ")\n";
}

void simulateGames(int numGames, char const *inputFrameTimeline, int startingLevel, int maxLines, int playoutCount, int playoutLength, OUT std::vector<std::pair<int, int>> &games){
  printf("Starting game simulations...\n");

  auto time_start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  for (int i = 0; i < numGames; i++) {
    vector<int> result = simulateGame(inputFrameTimeline, startingLevel, maxLines, playoutCount, playoutLength);
    games.push_back({result[0], result[1]});
    std::cout << i << ": " << result[0] << "Lines: " << result[1] << std::endl;
  }

  auto time_end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  printf("Time elapsed: %lld seconds\n", (time_end - time_start)/1000);

  printStats(games);
}

// (Thanks Gemini)
void simulateGamesThreaded(int numGames, const char* inputFrameTimeline, int startingLevel, int maxLines, int playoutCount, int playoutLength, OUT std::vector<std::pair<int, int>>& games) {
    auto time_start = std::chrono::steady_clock::now();

    games.resize(numGames);
    
    // 2. Create an atomic counter that all threads can safely modify
    std::atomic<int> gamesCompleted{0}; 

    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;
    // numThreads = 4;
    std::cout << "Starting game simulations on " << numThreads << " threads...\n";
    
    std::vector<std::thread> threads;
    int chunkSize = std::ceil((double)numGames / numThreads);

    // 3. Update the worker to increment the counter
    auto worker = [&](int startIdx, int endIdx) {
        for (int i = startIdx; i < endIdx; ++i) {
            std::vector<int> result = simulateGame(inputFrameTimeline, startingLevel, maxLines, playoutCount, playoutLength);
            games[i] = {result[0], result[1]};
            gamesCompleted++; // Safely increments without locks
        }
    };

    for (unsigned int i = 0; i < numThreads; ++i) {
        int startIdx = i * chunkSize;
        int endIdx = std::min(startIdx + chunkSize, numGames);
        
        if (startIdx < endIdx) {
            threads.push_back(std::thread(worker, startIdx, endIdx));
        }
    }

    // 4. Main thread tracks progress while workers do the heavy lifting
    while (gamesCompleted < numGames) {
        // \r brings the cursor back to the start of the line, creating an updating effect
        std::cout << "\rSimulating: " << gamesCompleted << " / " << numGames 
                  << " (" << (gamesCompleted * 100 / numGames) << "%)" << std::flush;
                  
        // Sleep for 100ms so we don't spam the CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Print the final 100% state and move to a new line
    std::cout << "\rSimulating: " << numGames << " / " << numGames << " (100%)\n";

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto time_end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time_end - time_start).count();
    
    std::cout << "Time elapsed: " << duration / 1000.0 << " seconds\n";

    printStats(games);
}