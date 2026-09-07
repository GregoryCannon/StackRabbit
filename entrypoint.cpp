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
