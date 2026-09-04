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

char const * testInput = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011111100001111110101111011110111101111011110111101111101111111110111111111011111111101111111110111|19|200|0|6|X.....X.....X..X.....|";

int runGames(){
  std::vector<int> scores;
  int numGames = NUM_SIM_GAMES;
  int playoutCount = 50;
  int playoutLength = 2;
  simulateGames(numGames, "X..", 29, /* maxLines= */ -1, /* shouldAdjust= */ 0, /* reactionTime= */ 0, playoutCount, playoutLength, scores);
  int total = 0;
  for (int i : scores){
    printf("%d\n", i);
    total += i;
  }
  printf("\n\nAverage: %d\n", total / numGames);
  return 0;
}

int main(int argc, const char * argv[]) {
  printf("%s\n", mainProcess(testInput, GET_LOCK_VALUE_LOOKUP_DAS).c_str());
  // printf("%s\n", mainProcess(testInput, GET_MOVE).c_str());
//  runGames();
  
  // testAdjustments();
  return 0;
}
