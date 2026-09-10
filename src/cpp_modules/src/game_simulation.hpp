#ifndef GAME_SIMULATION
#define GAME_SIMULATION

#include "types.hpp"
#include <utility>

std::vector<int> simulateGame(char const *inputFrameTimeline, int startingLevel, int maxLines, int playoutCount, int playoutLength, bool useTestWeights);

void simulateGames(int numGames, char const *inputFrameTimeline, int startingLevel, int maxLines, int playoutCount, int playoutLength, bool useTestWeights, OUT std::vector<std::pair<int, int>> &games);

void simulateGamesThreaded(int numGames, char const *inputFrameTimeline, int startingLevel, int maxLines, int playoutCount, int playoutLength, bool useTestWeights, OUT std::vector<std::pair<int, int>> &games);

#endif