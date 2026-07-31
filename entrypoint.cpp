//
//  main.cpp
//  StackRabbit
//
//  Created by Greg Cannon on 7/27/21.
//

#include <iostream>
#include "src/cpp_modules/src/main.cpp"
#include "src/cpp_modules/src/game_simulation.cpp"

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

char const * testInput = "00000000000000000000000000000000000000000000000000000000000000000000000001000000000111110000011111000001111100001111111000111111100111111110111111111011111111101111111110111111111011111111101111111110|19|200|1|0|X.....|";

int runGames(){
  std::cout.imbue(std::locale("en_US.UTF-8")); // Allow for commas in printed numbers
  std::vector<int> scores;
  std::vector<int> lines;
  int numGames = 100;
  int playoutCount = 7;
  int playoutLength = 1;
  simulateGamesThreaded(numGames, "X.....", 19, /* maxLines= */ 230, playoutCount, playoutLength, scores, lines);

  for (int i = 0; i < numGames; i++){
    printf("Game %d: %d points, %d lines\n", i, scores[i], lines[i]);
  }
  return 0;
}

int main(int argc, const char * argv[]) {
  // printf("%s\n", mainProcess(testInput, GET_LOCK_VALUE_LOOKUP).c_str());
  // printf("%s\n", mainProcess(testInput, GET_MOVE).c_str());
  runGames();
  
  // testAdjustments();
  return 0;
}
