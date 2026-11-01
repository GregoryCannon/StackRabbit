//
//  generate_positions.cpp
//  Generates benchmark positions: simulates games and logs every position
//  encountered to src/benchmark/position_log.jsonl (see position_logging.hpp).
//
//  Build and run (from repo root):
//    g++ src/benchmark/generate_positions.cpp -o build/generate_positions && ./build/generate_positions
//

#include <iostream>
#include "../cpp_modules/src/main.cpp"
#include "../cpp_modules/src/game_simulation.cpp"

int main(int argc, const char * argv[]) {
  std::cout.imbue(std::locale("en_US.UTF-8")); // Allow for commas in printed numbers
  std::vector<std::pair<int, int>> games;
  int numGames = 3;
  int playoutCount = 49;
  int playoutLength = 2;
  // Note: the agent is intentionally left weak (low playout budget) so that games get
  // into bad board situations, which diversifies the logged positions.
  char const *inputFrameTimeline = "X.....";
  int startingLevel = 18;
  int maxLines = 230;

  openPositionLogFile(POSITION_LOG_PATH, inputFrameTimeline, startingLevel, maxLines);

  printf("\n----- BENCHMARK GAME GENERATION ------\n");
  simulateGamesThreaded(numGames, inputFrameTimeline, startingLevel, maxLines, playoutCount, playoutLength, /* useTestWeights= */ false, SAFE, games);

  closePositionLogFile();
  return 0;
}
