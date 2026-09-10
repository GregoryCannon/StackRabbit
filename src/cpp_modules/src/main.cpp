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
#include "../data/ranks_base_7.cpp"

template<typename ... Args>
std::string string_format( const std::string& format, Args ... args )
{
    int size_s = std::snprintf( nullptr, 0, format.c_str(), args ... ) + 1; // Extra space for '\0'
    if( size_s <= 0 ){
      // throw std::runtime_error( "Error during formatting." );
      // (^^ if I remember right, that code fails for node-gyp integrations due to compatibility issues)
      return "";
    }
    auto size = static_cast<size_t>( size_s );
    std::unique_ptr<char[]> buf( new char[ size ] );
    std::snprintf( buf.get(), size, format.c_str(), args ... );
    return std::string( buf.get(), buf.get() + size - 1 ); // We don't want the '\0' inside
}

std::string mainProcess(char const *inputStr, RequestType requestType) {
  maybePrint("Input string %s, requestType %d\n", inputStr, requestType);

  // Init empty data structures
  GameState startingGameState = {
    /* board= */ {},
    /* surfaceArray= */ {},
    /* numTrueHoles */ 0,
    /* numPartialHoles= */ 0,
    /* lines= */ 0,
    /* level= */ 0
  };
  unsigned int secondBoard[20];
  const Piece *curPiece = NULL;
  const Piece *nextPiece = NULL;
  int playoutCount = DEFAULT_PLAYOUT_COUNT;
  int playoutLength = DEFAULT_PLAYOUT_LENGTH;
  int pruningBreadth = DEFAULT_PRUNING_BREADTH;
  std::string inputFrameTimeline;

  // Loop through the other args
  std::string nonBoardInputString;
  std::string secondBoardStr;
  if (requestType == RATE_MOVE){
    // Rate move requests have two boards;
    secondBoardStr = std::string(inputStr + 201);
    nonBoardInputString = std::string(inputStr + 402); // board1 + delim + board2 + delim
  } else {
    nonBoardInputString = std::string(inputStr + 201); // 201 = the length of the board string + 1 for the delimiter
  }
  
  std::string delim = "|";
  auto start = 0U;
  auto end = nonBoardInputString.find(delim);
  for (int i = 0; end != std::string::npos; i++) {
    std::string arg = nonBoardInputString.substr(start, end - start);
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
      if (argAsInt == -1){
        return "Error: please provide a value for currentPiece.";
      }
      curPiece = &(PIECE_LIST[argAsInt]);
      break;
    case 3:
      if (argAsInt == -1){
        nextPiece = NULL;
      } else {
        nextPiece = &(PIECE_LIST[argAsInt]);
      }
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
    case 7:
      pruningBreadth = argAsInt;
    default:
      break;
    }

    start = (int) end + (int) delim.length();
    end = nonBoardInputString.find(delim, start);
  }
  int wellColumn = 9;
  // Fill in the data structures
  encodeBoard(inputStr, startingGameState.board);
  if (secondBoardStr.length() > 0){
    encodeBoard(secondBoardStr.c_str(), secondBoard);
  }
  getSurfaceArray(startingGameState.board, startingGameState.surfaceArray);
  std::pair<int, float> result = updateSurfaceAndHoles(startingGameState.surfaceArray, startingGameState.board, wellColumn, /* isDigMode= */ false);
  startingGameState.numTrueHoles = result.first;
  startingGameState.numPartialHoles = result.second;

  // Calculate global context for the 3 possible gravity values
  const PieceRangeContext pieceRangeContextLookup[4] = {
    getPieceRangeContext(inputFrameTimeline.c_str(), 1, /* gravityDoubled= */ true),
    getPieceRangeContext(inputFrameTimeline.c_str(), 1, /* gravityDoubled= */ false),
    getPieceRangeContext(inputFrameTimeline.c_str(), 2, /* gravityDoubled= */ false),
    getPieceRangeContext(inputFrameTimeline.c_str(), 3, /* gravityDoubled= */ false),
  };
  const EvalContext context = getEvalContext(startingGameState, pieceRangeContextLookup, /* useTestWeights= */ false);

  // Recalculate holes once we have the eval context
  pair<int, float> result2 = updateSurfaceAndHoles(startingGameState.surfaceArray, startingGameState.board, context.countWellHoles ? -1 : context.wellColumn, context.aiMode == DIG);
  startingGameState.numTrueHoles = result2.first;
  startingGameState.numPartialHoles = result2.second;

  if (LOGGING_ENABLED) {
    printBoard(startingGameState.board);
    printBoardBits(startingGameState.board);
  }

  // Take the specified action on the input based on the request type
  switch (requestType) {
    case GET_LOCK_VALUE_LOOKUP: {
      return getLockValueLookupEncoded(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount, playoutLength, &context, pieceRangeContextLookup);
    }

    case GET_LOCK_VALUE_LOOKUP_DAS: {
      return getLockValueLookupDas(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount, playoutLength, &context, pieceRangeContextLookup);
    }

    case GET_TOP_MOVES: {
      // needExplanation(GET_TOP_MOVES) is true, so per-move evalExplanation strings are populated.
      return getTopMoveList(startingGameState, curPiece, nextPiece, NUM_TOP_ENGINE_MOVES, playoutCount, playoutLength, &context, pieceRangeContextLookup, needExplanation(GET_TOP_MOVES));
    }

    case GET_TOP_MOVES_HYBRID: {
      // needExplanation(GET_TOP_MOVES_HYBRID) is true, so both lists include evalExplanation.
      std::string nnbResult = getTopMoveList(startingGameState, curPiece, /* nextPiece= */ NULL, NUM_TOP_ENGINE_MOVES, playoutCount, playoutLength, &context, pieceRangeContextLookup, needExplanation(GET_TOP_MOVES_HYBRID));
      std::string nbResult = getTopMoveList(startingGameState, curPiece, nextPiece, NUM_TOP_ENGINE_MOVES, playoutCount, playoutLength, &context, pieceRangeContextLookup, needExplanation(GET_TOP_MOVES_HYBRID));
      return "{\"noNextBox\":" + nnbResult + ", \"nextBox\":" + nbResult + "}";
    }

    case RATE_MOVE: {
      return rateMove(startingGameState, curPiece, nextPiece, secondBoard, pruningBreadth, playoutCount, playoutLength, &context, pieceRangeContextLookup);
    }

    case GET_MOVE: {
      LockLocation bestMove = playOneMove(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount, playoutLength, &context, pieceRangeContextLookup);
      int xOffset = bestMove.x - 3;
      int rot = bestMove.rotationIndex;
      int yOffset = bestMove.y - curPiece->initialY;
      return string_format("[%d, %d, %d]", rot, xOffset, yOffset);
      // int debugSequence[SEQUENCE_LENGTH] = {curPiece->index};
      // playSequence(startingGameState, pieceRangeContextLookup, debugSequence, /* playoutLength= */ 1);
      // return "Debug playout complete.";
    }

    case GET_MOVE_AB_TEST: {
      // needExplanation(GET_MOVE_AB_TEST) is true, so capture explanations for both weight sets.
      // Passing nullptr (as GET_MOVE does) would skip the extra std::string work.
      std::string explanationMain;
      std::string explanationTest;
      std::string *mainPtr = needExplanation(GET_MOVE_AB_TEST) ? &explanationMain : nullptr;
      std::string *testPtr = needExplanation(GET_MOVE_AB_TEST) ? &explanationTest : nullptr;
      LockLocation bestMove = playOneMove(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount, playoutLength, &context, pieceRangeContextLookup, mainPtr);
      int xOffset = bestMove.x - 3;
      int rot = bestMove.rotationIndex;
      int yOffset = bestMove.y - curPiece->initialY;

      const EvalContext contextTest = getEvalContext(startingGameState, pieceRangeContextLookup, /* useTestWeights= */ true);
      LockLocation bestMoveTest = playOneMove(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount, playoutLength, &contextTest, pieceRangeContextLookup, testPtr);
      int xOffsetTest = bestMoveTest.x - 3;
      int rotTest = bestMoveTest.rotationIndex;
      int yOffsetTest = bestMoveTest.y - curPiece->initialY;

      // Escape for safe JSON embedding (explanations are ;-separated "name=value" pairs).
      std::string escMain = escapeJsonString(explanationMain);
      std::string escTest = escapeJsonString(explanationTest);
      // Extra array element keeps [0]=agree/disagree, [1]=main, [2]=test backward-compatible
      // with the existing JS consumer while exposing both explanations.
      bool agree = xOffset == xOffsetTest && rot == rotTest && yOffset == yOffsetTest;
      if (agree){
        return string_format("[\"agree\", [%d, %d, %d], {\n\"explanationMain\":\"%s\"}]", rot, xOffset, yOffset, escMain.c_str());
      } else {
        return string_format("[\"disagree\", [%d, %d, %d], [%d, %d, %d], {\n\"explanationMain\":\"%s\", \n\"explanationTest\":\"%s\"}]", rot, xOffset, yOffset, rotTest, xOffsetTest, yOffsetTest, escMain.c_str(), escTest.c_str());
      }
    }

    default: {
      return "Unknown request";
    }
  }
}

