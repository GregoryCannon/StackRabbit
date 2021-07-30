
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>     /* srand, rand */
#include <time.h>       /* time */
#include <string.h>

#include "../include/piece_ranges.h"
#include "../include/tetrominoes.h"
// I have to include the C++ files here due to a complication of node-gyp. Consider this the equivalent
// of listing all the C++ sources in the makefile (Node-gyp seems to only work with 1 source rn).
#include "board_methods.cc"
#include "eval.cc"
#include "move_result.cc"
#include "move_search.cc"
#include "playout.cc"
#include "high_level_search.cc"
// #include "data/ranksOutput.cc"

#define USE_RANDOM_SEQUENCE false


int testPlayout(GameState startingGameState, int numRepetitions){
  if (USE_RANDOM_SEQUENCE) {
    srand(59902);
    // srand(time(NULL));
  }

  int result = 0;
  for (int i = 0; i < 0; i++) {
    int sequence[10] = {0, 1, 2, 3, 4, 5, 6, 0, 2, 5}; // A balanced sample of the pieces, performance-wise
    if (USE_RANDOM_SEQUENCE) {
      // Overwrite with random sequence
      for (int j = 0; j < 10; j++) {
        sequence[j] = rand() % 7;
      }
    }

    result = static_cast<int>(playSequence(startingGameState, sequence));
  }
  return result;
}

int testDepth2Search(GameState startingGameState, int numRepetitions){
  int result = 0;
  for (int i = 0; i < numRepetitions; i++) {
    list<Depth2Possibility> possibilityList;
    result = searchDepth2(startingGameState, PIECE_S, PIECE_L, 10, possibilityList);

    // Print results
    if (LOGGING_ENABLED) {
      maybePrint("List has %d members\n", (int) possibilityList.size());
      int numPrinted = 0;
      for (auto const& p : possibilityList) {
        if (numPrinted > 10) {
          break;
        }
        maybePrint("%d) %d,%d  %d,%d  has value %f\n",
                   numPrinted + 1,
                   p.firstPlacement.rotationIndex,
                   p.firstPlacement.x - SPAWN_X,
                   p.secondPlacement.rotationIndex,
                   p.secondPlacement.x - SPAWN_X,
                   p.evalScore);
        numPrinted++;
      }
    }
  }
  return result;
}


std::string mainProcess(char const *inputStr) {

  printf("Input string %s\n", inputStr);

  GameState startingGameState = {
    /* board= */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1022, 1022, 1022},
    /* surfaceArray= */ {},
    /* adjustedNumHole= */ 0,
    0};
  getSurfaceArray(startingGameState.board, startingGameState.surfaceArray);

  // int result = testDepth2Search(startingGameState, 1);
  // printf("%d\n", result);

  getLockValueLookupEncoded(startingGameState, PIECE_S, PIECE_T, 20);

  // Print ranks
  // for (int i = 0; i < 20; i++) {
  //    printf("ranks %d\n", surfaceRanksRaw[i]);
  // }

  // printf("Done\n");
  return std::string("Daffodil");
  // return result.rotationIndex * 100 + (result.x - SPAWN_X);
}
