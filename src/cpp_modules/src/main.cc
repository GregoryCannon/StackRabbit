
#include <stdio.h>
#include <stdlib.h>

#include "../include/piece_ranges.h"
#include "../include/tetrominoes.h"
// I have to include the C++ files here due to a complication of node-gyp. Consider this the equivalent
// of listing all the C++ sources in the makefile (Node-gyp seems to only work with 1 source rn).
#include "board_methods.cc"
#include "eval.cc"
#include "move_search.cc"
#include "playout.cc"
// #include "data/ranksOutput.cc"

int mainProcess(char const * inputStr) {

  printf("%s\n", inputStr);

  // int board[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 991, 990, 991};
  int board[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1022, 1022, 1022};
  int surface[10];
  getSurfaceArray(board, surface);

  int numPlacements = 0;
  SimState result;

  for (int i = 0; i < 1; i++) {
    std::vector<SimState> lockPlacements;
    numPlacements += moveSearch(board, surface, PIECE_T, lockPlacements);
    result = pickLockPlacement(board, surface, lockPlacements);
  }

  // for (int i = 0; i < 12; i++) {
  //   printf("%d %d\n", i - 6, X_BOUNDS_COLLISION_TABLE[6][1][i]);
  // }

  // Print ranks
  // for (int i = 0; i < 20; i++) {
  //    printf("ranks %d\n", surfaceRanksRaw[i]);
  // }

  printf("Done\n");
  return result.rotationIndex * 100 + (result.x - SPAWN_X);
  // char *responseStr;
  // return sprintf(responseStr, "%d, %d | out of %d", result.rotationIndex, result.x - SPAWN_X,
  // numPlacements);
}

int main(){
  return mainProcess("bop");
}