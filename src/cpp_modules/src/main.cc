
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

int mainProcess(char *inputStr) {

  // printf("%s\n", inputStr);

  // GameState startingGameState = {
  //     /* board= */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1022, 1022, 1022},
  //     /* surfaceArray= */ {},
  //     /* adjustedNumHole= */ 0,
  //     /* holeMap= */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  GameState startingGameState = {
      /* board= */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 48, 766, 1022},
      /* surfaceArray= */ {},
      /* adjustedNumHole= */ 0,
      /* holeMap= */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  getSurfaceArray(startingGameState.board, startingGameState.surfaceArray);

  int numPlacements = 0;

  for (int i = 0; i < 1; i++) {
    // int sequence[10] = {0, 4, 2, 4, 6, 6, 1, 4, 5, 2};
    // int sequence[10] = {0, 1, 2, 3, 4, 5, 6, 0, 2, 5};
    int sequence[10] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    playSequence(startingGameState, sequence);
  }

  // Print ranks
  // for (int i = 0; i < 20; i++) {
  //    printf("ranks %d\n", surfaceRanksRaw[i]);
  // }

  printf("Done\n");
  return 1;
  // return result.rotationIndex * 100 + (result.x - SPAWN_X);
}

int main() { mainProcess("Bananas"); }
