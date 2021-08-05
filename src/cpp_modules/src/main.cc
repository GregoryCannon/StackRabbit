
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
  maybePrint("Input string %s\n", inputStr);

  GameState startingGameState = {
    // /* board= */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1022, 1022, 1022},
    /* board= */ {},
    /* surfaceArray= */ {},
    /* adjustedNumHole= */ 0,
    /* lines= */ 0};
  Piece curPiece = PIECE_T;
  Piece nextPiece = PIECE_S;

  encodeBoard(inputStr, startingGameState.board);
  getSurfaceArray(startingGameState.board, startingGameState.surfaceArray);
  startingGameState.adjustedNumHoles += updateSurfaceAndHolesAfterLineClears(startingGameState.surfaceArray, startingGameState.board, DEBUG_CONTEXT);

  // Loop through the other args
  std::string s = std::string(inputStr + 201); // 201 = the length of the board string + 1 for the delimiter
  std::string delim = "|";
  auto start = 0U;
  auto end = s.find(delim);
  for (int i = 0; end != std::string::npos; i++) {
    int arg = atoi(s.substr(start, end - start).c_str());
    maybePrint("ARG %d: %d\n", i, arg);
    switch (i) {
    case 0:
      startingGameState.lines = arg;
      break;
    case 1:
      curPiece = PIECE_LIST[arg];
      break;
    case 2:
      nextPiece = PIECE_LIST[arg];
      break;
    default:
      break;
    }
  
    start = (int) end + (int) delim.length();
    end = s.find(delim, start);
  }

  std::string lookupMapEncoded = getLockValueLookupEncoded(startingGameState, curPiece, nextPiece, /* keepTopN= */ 20);
  return lookupMapEncoded;

  // Print ranks
  // for (int i = 0; i < 20; i++) {
  //    printf("ranks %d\n", surfaceRanksRaw[i]);
  // }
}
