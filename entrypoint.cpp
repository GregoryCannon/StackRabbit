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

// char const * testInput = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000001011000001111110001111111111111111101111111110|18|0|5|5|X....|";
// char const * testInput = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011000000011000|18|0|3|5|X....|";
char const * testInput = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100100000010011100111000111111110111111111110111111101111111|19|85|4|-1|X.....|";

int runGames(){
  std::vector<int> scores;
  int numGames = 10;
  int playoutCount = 100;
  int playoutLength = 5;
  simulateGames(numGames, "X.....", 18, /* maxLines= */ 230, /* shouldAdjust= */ 0, /* reactionTime= */ 0, playoutCount, playoutLength, scores);
  return 0;
}

int main(int argc, const char * argv[]) {
  // printf("%s\n", mainProcess(testInput, GET_LOCK_VALUE_LOOKUP).c_str());
  // printf("%s\n", mainProcess(testInput, GET_MOVE).c_str());
  runGames();
  
  // testAdjustments();
  return 0;
}
