#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>     /* srand, rand */
#include <time.h>       /* time */
#include <string.h>


#include "params.hpp"
// I have to include the C++ files here due to a complication of node-gyp. Consider this the equivalent
// of listing all the C++ sources in the makefile (Node-gyp seems to only work with 1 source rn).
#include "../data/tetrominoes.cpp"
#include "eval.cpp"
#include "eval_context.cpp"
#include "move_result.cpp"
#include "move_search.cpp"
#include "piece_ranges.cpp"
#include "playout.cpp"
#include "high_level_search.cpp"
#include "piece_rng.cpp"
// #include "../data/ranks_output.cpp"

template<typename ... Args>
std::string string_format( const std::string& format, Args ... args )
{
    int size_s = std::snprintf( nullptr, 0, format.c_str(), args ... ) + 1; // Extra space for '\0'
    if( size_s <= 0 ){
//      throw std::runtime_error( "Error during formatting." );
      return NULL;
    }
    auto size = static_cast<size_t>( size_s );
    std::unique_ptr<char[]> buf( new char[ size ] );
    std::snprintf( buf.get(), size, format.c_str(), args ... );
    return std::string( buf.get(), buf.get() + size - 1 ); // We don't want the '\0' inside
}

std::string mainProcess(char const *inputStr, RequestType requestType) {
  maybePrint("Input string %s\n", inputStr);

  // Init empty data structures
  GameState startingGameState = {
    /* board= */ {},
    /* surfaceArray= */ {},
    /* adjustedNumHole= */ 0,
    /* lines= */ 0,
    /* level= */ 0
  };
  const Piece *curPiece = NULL;
  const Piece *nextPiece = NULL;
  int playoutCount = DEFAULT_PLAYOUT_COUNT;
  int playoutLength = DEFAULT_PLAYOUT_LENGTH;
  std::string inputFrameTimeline;

  // Loop through the other args
  std::string s = std::string(inputStr + 201); // 201 = the length of the board string + 1 for the delimiter
  std::string delim = "|";
  auto start = 0U;
  auto end = s.find(delim);
  for (int i = 0; end != std::string::npos; i++) {
    std::string arg = s.substr(start, end - start);
    int argAsInt = atoi(arg.c_str());
    maybePrint("ARG %d: %d\n", i, argAsInt);
    switch (i) {
    case 0:
      startingGameState.level = argAsInt;
      break;
    case 1:
      startingGameState.lines = argAsInt;
      break;
    case 2:
      curPiece = &(PIECE_LIST[argAsInt]);
      break;
    case 3:
      nextPiece = &(PIECE_LIST[argAsInt]);
      break;
    case 4:
      inputFrameTimeline = arg;
      break;
    case 5:
      playoutCount = argAsInt;
      break;
    case 6:
      playoutLength = argAsInt;
      break;
    default:
      break;
    }

    start = (int) end + (int) delim.length();
    end = s.find(delim, start);
  }
  int wellColumn = 9;
  // Fill in the data structures
  encodeBoard(inputStr, startingGameState.board);
  getSurfaceArray(startingGameState.board, startingGameState.surfaceArray);
  startingGameState.adjustedNumHoles = updateSurfaceAndHoles(startingGameState.surfaceArray, startingGameState.board, wellColumn);

  // Calculate global context for the 3 possible gravity values
  const PieceRangeContext pieceRangeContextLookup[3] = {
    getPieceRangeContext(inputFrameTimeline.c_str(), 1),
    getPieceRangeContext(inputFrameTimeline.c_str(), 2),
    getPieceRangeContext(inputFrameTimeline.c_str(), 3),
  };
  const EvalContext context = getEvalContext(startingGameState, pieceRangeContextLookup);

  // Recalculate holes once we have the eval context
  startingGameState.adjustedNumHoles = updateSurfaceAndHoles(startingGameState.surfaceArray, startingGameState.board, context.countWellHoles ? -1 : context.wellColumn);

  if (LOGGING_ENABLED) {
    printBoard(startingGameState.board);
    printBoardBits(startingGameState.board);
  }

  // Take the specified action on the input based on the request type
  switch (requestType) {
    case GET_LOCK_VALUE_LOOKUP: {
      return getLockValueLookupEncoded(startingGameState, curPiece, nextPiece, DEPTH_2_PRUNING_BREADTH, playoutCount, playoutLength, &context, pieceRangeContextLookup);
    }

    case GET_TOP_MOVES: {
      return getTopMoveList(startingGameState, curPiece, nextPiece, NUM_TOP_ENGINE_MOVES, playoutCount, playoutLength, &context, pieceRangeContextLookup);
    }

    case GET_MOVE: {
  //    int debugSequence[SEQUENCE_LENGTH] = {curPiece->index};
  //    playSequence(startingGameState, pieceRangeContextLookup, debugSequence, /* playoutLength= */ 1);
  //    return "Debug playout complete.";
      LockLocation bestMove = playOneMove(startingGameState, curPiece, nextPiece, /* numCandidatesToPlayout */ DEPTH_1_PRUNING_BREADTH, playoutCount, playoutLength, &context, pieceRangeContextLookup);
      int xOffset = bestMove.x - 3;
      int rot = bestMove.rotationIndex;

      return string_format("[%d, %d]", rot, xOffset);
    }

    default: {
      return "Unknown request";
    }
  }
}

// int main(){
//   printf("Starting...\n");
//   std::string result = mainProcess("0000000000000000000000000000000000000000000000000000000000000000001110000000111000000011110000"
//               "0111110000011110000011111100011101110011101110001111111000111111100111111110011111111001111111"
//               "101111111110|18|2|5|0|X...|");
//   printf("%s\n", result.c_str());
//   printf("Done!\n");
// }
