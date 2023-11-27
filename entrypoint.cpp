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
char const * testInput = "00000000000000000000000000010000000001110000000111000000111100000111100000011111000001111100010111110001111110100111111010011111101001111110100111111011011111111101111111111111111011111111101101111111|18|85|5|6|X....|";

int runGames(){
  std::vector<int> scores;
  int numGames = NUM_SIM_GAMES;
  simulateGames(numGames, "X..", 29, /* maxLines= */ -1, /* shouldAdjust= */ 0, /* reactionTime= */ 0, scores);
  int total = 0;
  for (int i : scores){
    printf("%d\n", i);
    total += i;
  }
  printf("\n\nAverage: %d\n", total / numGames);
  return 0;
}

int main(int argc, const char * argv[]) {
   printf("%s\n", mainProcess(testInput, GET_LOCK_VALUE_LOOKUP).c_str());
//  printf("%s\n", mainProcess(testInput, PLAY_MOVE_NO_NEXT_BOX).c_str());
//  runGames();
  
  // testAdjustments();
  return 0;
}
