//
//  main.cpp
//  StackRabbit
//
//  Created by Greg Cannon on 7/27/21.
//

#include "src/backend/main.cpp"
#include "src/backend/game_simulation.cpp"
#include <iostream>

/*
 I = 0
 O = 1
 L = 2
 J = 3
 T = 4
 S = 5
 Z = 6
 board | level | lines | curPiece | nextPiece | inputTimeline
 */

// clang-format off
char const *testInput =
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100010000111001000011100100001110010111111001111111110111111111011111111101111111110"
    "|19|22|3|2|X..|49|2|102|aggro|16|";
// char const * testInputWithSecondBoard =
// "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000001100000000110100000011111110001111111110|00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000010001100011000110101000011111110001111111110|19|1|6|2|X.....|343|3|";
// clang-format on

int runGames() {
  std::cout.imbue(std::locale("en_US.UTF-8")); // Allow for commas in printed numbers
  std::vector<std::pair<int, int>> games;
  int numGames = 1;
  int playoutCount = 49;
  int playoutLength = 2;
  char const *inputFrameTimeline = "X.....";
  int startingLevel = 18;
  int maxLines = 230;

  // Games with main weights
  printf("\n----- MAIN WEIGHTS ------\n");
  simulateGamesThreaded(numGames, inputFrameTimeline, startingLevel, maxLines, playoutCount,
                        playoutLength,
                        /* useTestWeights= */ false, SAFE, games);

  // // Games with test weights
  // printf("\n----- TEST WEIGHTS ------\n");
  // simulateGamesThreaded(numGames, inputFrameTimeline, startingLevel, maxLines, playoutCount,
  // playoutLength, /* useTestWeights= */ true, SAFE, games);

  // for (int i = 0; i < numGames; i++){
  //   printf("Game %d: %d points, %d lines\n", i, games[i].first, games[i].second);
  // }
  return 0;
}

int main(int argc, const char *argv[]) {
  printf("%s\n", mainProcess(testInput, GET_LOCK_VALUE_LOOKUP_DAS).c_str());
  // printf("%s\n", mainProcess(testInput, GET_MOVE_AB_TEST).c_str());
  // printf("%s\n", mainProcess(testInput, GET_MOVE).c_str());
  // printf("%s\n", mainProcess(testInputWithSecondBoard, GET_TOP_MOVES).c_str());
  // runGames();

  return 0;
}
