
#include <stdio.h>
#include <stdlib.h>

#include "../include/piece_ranges.h"
#include "../include/tetrominoes.h"
// I have to include the C++ files here due to a complication of node-gyp. Consider this the equivalent
// of listing all the C++ sources in the makefile (Node-gyp seems to only work with 1 source rn).
#include "board_methods.cc"
#include "eval.cc"
#include "move_result.cc"
#include "move_search.cc"
#include "playout.cc"
// #include "data/ranksOutput.cc"

int mainProcess(char const *inputStr) {

  printf("%s\n", inputStr);

  GameState startingGameState = {
      /* board= */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1022, 1022, 1022},
      /* surfaceArray= */ {},
      /* adjustedNumHole= */ 0,
      0};
  getSurfaceArray(startingGameState.board, startingGameState.surfaceArray);

  int result = 0;
  for (int i = 0; i < 1; i++) {
    int sequence[10] = {0, 1, 2, 3, 4, 5, 6, 0, 2, 5}; // A balanced sample of the pieces, performance-wise
    result = static_cast<int>(playSequence(startingGameState, sequence));
  }

  // Print ranks
  // for (int i = 0; i < 20; i++) {
  //    printf("ranks %d\n", surfaceRanksRaw[i]);
  // }

  // printf("Done\n");
  return result;
  // return result.rotationIndex * 100 + (result.x - SPAWN_X);
}

int main() { mainProcess("Bananas"); }
