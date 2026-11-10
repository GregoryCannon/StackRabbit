#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h> /* srand, rand */
#include <time.h>   /* time */
#include <string.h>

#include "params.hpp"
// I have to include the C++ files here due to a complication of node-gyp. Consider this the
// equivalent of listing all the C++ sources in the makefile (Node-gyp seems to only work with 1
// source rn).
#include "../data/tetrominoes.hpp"
#include "../data/canonical_sequences.cpp"
#include "eval.cpp"
#include "eval_context.cpp"
#include "move_result.cpp"
#include "move_search.cpp"
#include "full_move_search.cpp"
#include "piece_ranges.cpp"
#include "playout.cpp"
#include "high_level_search.cpp"
#ifndef __EMSCRIPTEN__
// The precompute path is not part of the WASM build.
#include "precompute.cpp"
#endif
#include "tournament_search.cpp"
#include "piece_rng.cpp"
// #include "../data/ranks_output.cpp"
#include "../data/ranks_base_7.cpp"

template <typename... Args> std::string string_format(const std::string &format, Args... args) {
  int size_s = std::snprintf(nullptr, 0, format.c_str(), args...) + 1; // Extra space for '\0'
  if (size_s <= 0) {
    // throw std::runtime_error( "Error during formatting." );
    // (^^ if I remember right, that code fails for node-gyp integrations due to compatibility
    // issues)
    return "";
  }
  auto size = static_cast<size_t>(size_s);
  std::unique_ptr<char[]> buf(new char[size]);
  std::snprintf(buf.get(), size, format.c_str(), args...);
  return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

// Returns true if the string parses as a plain integer (optional leading '-', digits only).
// Used to validate the dasCharge field: unlike the other numeric fields (where
// atoi("junk") -> 0 is harmless), a silent 0 would flip the engine into DAS mode.
static bool isIntegerString(std::string const &arg) {
  size_t start = (!arg.empty() && arg[0] == '-') ? 1 : 0;
  if (arg.length() <= start) {
    return false;
  }
  for (size_t i = start; i < arg.length(); i++) {
    if (arg[i] < '0' || arg[i] > '9') {
      return false;
    }
  }
  return true;
}

std::string mainProcess(char const *inputStr, RequestType requestType) {
  maybePrint("Input string %s, requestType %d\n", inputStr, requestType);

  // Init empty data structures
  GameState startingGameState = {/* board= */ {},
                                 /* surfaceArray= */ {},
                                 /* numTrueHoles */ 0,
                                 /* numPartialHoles= */ 0,
                                 /* lines= */ 0,
                                 /* level= */ 0};
  unsigned int secondBoard[20] = {};
  const Piece *curPiece = NULL;
  const Piece *nextPiece = NULL;
  int playoutCount = DEFAULT_PLAYOUT_COUNT;
  int playoutLength = DEFAULT_PLAYOUT_LENGTH;
  int pruningBreadth = DEFAULT_PRUNING_BREADTH;
  std::string inputFrameTimeline;
  AggressionLevel aggressionLevel = SAFE;
  // Field 10 of the pipe string (appended after aggressionLevel). -1 = tap mode;
  // 0..16 = the player is holding DAS with this much charge. The field is
  // mandatory -- see TESTING.md's input format.
  int dasCharge = -1;
  // Field 11, present only on PRECOMPUTE requests: the frames the player has to
  // react. 0..60, where 0 means the piece is still unplaced at the reaction time.
  int reactionTime = 0;

  // Loop through the other args
  std::string nonBoardInputString;
  std::string secondBoardStr;
  // The board must be exactly 200 characters of 0/1 followed by the '|' delimiter;
  // anything else (too short, wrong length, bad characters, missing delimiter) is a
  // malformed request: the slicing below would misparse the args or read past the buffer.
  bool board1Valid = strlen(inputStr) > 200 && inputStr[200] == '|';
  for (int i = 0; board1Valid && i < 200; i++) {
    if (inputStr[i] != '0' && inputStr[i] != '1') {
      board1Valid = false;
    }
  }
  if (!board1Valid) {
    return "Error: invalid board string (expected 200 characters of 0/1).";
  }

  // Detect an optional second board: board1 (200 chars of 0/1) + '|' + board2 (200 chars of 0/1) +
  // '|' + rest. RATE_MOVE always has two boards; GET_TOP_MOVES / GET_TOP_MOVES_HYBRID may
  // optionally include one.
  bool hasSecondBoard = false;
  {
    std::string inputStrStd(inputStr);
    if (inputStrStd.length() > 402 && inputStrStd[200] == '|' && inputStrStd[401] == '|') {
      hasSecondBoard = true;
      for (int i = 0; i < 200; i++) {
        if (inputStr[i] != '0' && inputStr[i] != '1') {
          hasSecondBoard = false;
          break;
        }
      }
      if (hasSecondBoard) {
        for (int i = 0; i < 200; i++) {
          if (inputStr[201 + i] != '0' && inputStr[201 + i] != '1') {
            hasSecondBoard = false;
            break;
          }
        }
      }
    }
  }
  if (hasSecondBoard) {
    // Requests with two boards (RATE_MOVE always; GET_TOP_MOVES / GET_TOP_MOVES_HYBRID optionally).
    secondBoardStr = std::string(inputStr + 201, 200);
    nonBoardInputString = std::string(inputStr + 402); // board1 + delim + board2 + delim
  } else if (requestType == RATE_MOVE) {
    return "Error: RATE_MOVE requests require a value for secondBoard.";
  } else {
    nonBoardInputString =
        std::string(inputStr + 201); // 201 = the length of the board string + 1 for the delimiter
  }

  std::string delim = "|";
  auto start = 0U;
  auto end = nonBoardInputString.find(delim);
  // Every non-board field is mandatory: callers must state each value explicitly
  // (use -1 / the documented defaults rather than omitting a field). A missing or
  // malformed field is rejected with an error instead of silently degrading -- e.g.
  // atoi("")->0 would otherwise play the I piece for a missing currentPiece, an
  // empty timeline makes the engine never tap (x % 0), and stale defaults can
  // silently kick off an expensive search.
  bool fieldProvided[11] = {};
  for (int i = 0; end != std::string::npos; i++) {
    std::string arg = nonBoardInputString.substr(start, end - start);
    int argAsInt = atoi(arg.c_str());
    maybePrint("ARG %d: %d\n", i, argAsInt);
    switch (i) {
    case 0:
      if (!isIntegerString(arg)) {
        return "Error: invalid level value (expected an integer): " + arg;
      }
      if (argAsInt < 18) {
        return "Error: currently only 18, 19, and 29 starts are supported by StackRabbit. Requested: "
               + arg;
      }
      startingGameState.level = argAsInt;
      fieldProvided[0] = true;
      break;
    case 1:
      if (!isIntegerString(arg)) {
        return "Error: invalid lines value (expected an integer): " + arg;
      }
      if (argAsInt < 0) {
        return "Error: invalid line count: " + arg;
      }
      startingGameState.lines = argAsInt;
      fieldProvided[1] = true;
      break;
    case 2:
      if (!isIntegerString(arg)) {
        return "Error: invalid currentPiece value (expected a piece index or -1): " + arg;
      }
      if (argAsInt == -1) {
        return "Error: please provide a value for currentPiece.";
      }
      if (argAsInt < 0 || argAsInt > 6) {
        return "Error: unknown current piece: " + arg;
      }
      curPiece = &(PIECE_LIST[argAsInt]);
      fieldProvided[2] = true;
      break;
    case 3:
      if (!isIntegerString(arg)) {
        return "Error: invalid nextPiece value (expected a piece index or -1): " + arg;
      }
      if (argAsInt == -1) {
        nextPiece = NULL;
      } else {
        if (argAsInt < 0 || argAsInt > 6) {
          return "Error: unknown next piece: " + arg;
        }
        nextPiece = &(PIECE_LIST[argAsInt]);
      }
      fieldProvided[3] = true;
      break;
    case 4:
      // The timeline drives all tap timing: an empty string (or one with no input
      // frames) makes shouldPerformInputsThisFrame do x % 0 and the engine never
      // taps (or hangs on some platforms), so it is validated strictly.
      if (arg.empty()
          || arg.find_first_not_of("X.") != std::string::npos
          || arg.find('X') == std::string::npos) {
        return "Error: invalid inputFrameTimeline (expected X/. frames with at least one input "
               "frame): "
               + arg;
      }
      inputFrameTimeline = arg;
      fieldProvided[4] = true;
      break;
    case 5:
      if (!isIntegerString(arg)) {
        return "Error: invalid playoutCount value (expected an integer): " + arg;
      }
      if (argAsInt < 0) {
        return "Error: invalid playout count: " + arg;
      }
      playoutCount = argAsInt;
      fieldProvided[5] = true;
      break;
    case 6:
      if (!isIntegerString(arg)) {
        return "Error: invalid playoutLength value (expected an integer): " + arg;
      }
      if (argAsInt < 0 || argAsInt > 20) {
        return "Error: invalid playout length (max is 20): " + arg;
      }
      playoutLength = argAsInt;
      fieldProvided[6] = true;
      break;
    case 7:
      // -1 = tournament mode (playoutCount is the total playout budget instead of the
      // per-candidate count; see getLockValueLookupTournament in tournament_search.cpp)
      if (!isIntegerString(arg)) {
        return "Error: invalid pruningBreadth value (expected an integer): " + arg;
      }
      if (argAsInt < -1 || argAsInt > 1156) {
        return "Error: invalid pruning breadth (max is 1156, or 34x34): " + arg;
      }
      pruningBreadth = argAsInt;
      fieldProvided[7] = true;
      break;
    case 8:
      // Play style: "safe", "aggro", or "hyper"; anything else is rejected.
      if (arg == "safe") {
        aggressionLevel = SAFE;
      } else if (arg == "aggro") {
        aggressionLevel = AGGRO;
      } else if (arg == "hyper") {
        aggressionLevel = HYPER;
      } else {
        return "Error: invalid aggressionLevel value (expected safe, aggro, or hyper): " + arg;
      }
      fieldProvided[8] = true;
      break;
    case 9:
      // Incoming DAS charge: -1 = tap mode, 0..16 = DAS mode.
      if (!isIntegerString(arg)) {
        return "Error: invalid dasCharge value (expected -1 or 0-16): " + arg;
      }
      if (argAsInt < -1 || argAsInt > 16) {
        return "Error: illegal DAS charge (expected -1 for tap mode, or 0-16): " + arg;
      }
      dasCharge = argAsInt;
      fieldProvided[9] = true;
      break;
    case 10:
      if (!isPrecomputePipeRequest(requestType)) {
        break; // other request types ignore trailing fields
      }
      if (!isIntegerString(arg)) {
        return "Error: invalid reactionTime value (expected an integer between 0 and 60): " + arg;
      }
      if (argAsInt < 0 || argAsInt > 60) {
        return "Error: reaction time exceeds the maximum possible of 60 frames. Did you "
               "accidentally send it in milliseconds? " + arg;
      }
      reactionTime = argAsInt;
      fieldProvided[10] = true;
      break;
    default:
      break;
    }

    start = (int)end + (int)delim.length();
    end = nonBoardInputString.find(delim, start);
  }
  // All ten non-board fields are mandatory (dasCharge uses -1 for tap mode);
  // the precompute pipe format additionally requires the reaction time as
  // field 11.
  char const *fieldNames[11] = {
      "level",        "lines",         "currentPiece",   "nextPiece",       "inputFrameTimeline",
      "playoutCount", "playoutLength", "pruningBreadth", "aggressionLevel", "dasCharge",
      "reactionTime"};
  const int requiredFields = isPrecomputePipeRequest(requestType) ? 11 : 10;
  for (int i = 0; i < requiredFields; i++) {
    if (!fieldProvided[i]) {
      return std::string("Error: please provide a value for ") + fieldNames[i] + ".";
    }
  }
  if (isPrecomputePipeRequest(requestType)) {
    // In tournament mode (pruningBreadth == -1) the count is the total playout
    // budget; otherwise the volume is count * length.
    long long totalPlayoutVolume = pruningBreadth == -1
                                       ? playoutCount
                                       : static_cast<long long>(playoutCount) * playoutLength;
    if (totalPlayoutVolume > MAX_CPP_PLAYOUT_MOVES) {
      return "Error: playout volume exceeds the current limit of "
             + std::to_string(MAX_CPP_PLAYOUT_MOVES)
             + " moves. Current volume: " + std::to_string(totalPlayoutVolume);
    }
  }
  int wellColumn = 9;
  // Fill in the data structures
  encodeBoard(inputStr, startingGameState.board);
  if (secondBoardStr.length() > 0) {
    encodeBoard(secondBoardStr.c_str(), secondBoard);
  }
  getSurfaceArray(startingGameState.board, startingGameState.surfaceArray);
  std::pair<int, float> result = updateSurfaceAndHoles(
      startingGameState.surfaceArray, startingGameState.board, wellColumn, /* isDigMode= */ false);
  startingGameState.numTrueHoles = result.first;
  startingGameState.numPartialHoles = result.second;

  // Calculate global context for the 3 possible gravity values.
  // DAS requests (dasCharge >= 0) carry the player's real timeline on the wire for
  // the DAS move search; the piece-range / eval / playout contexts keep the
  // "DAS-equivalent" tapper approximation
  std::string dasEquivalentTimelineStorage;
  char const *contextInputFrameTimeline = inputFrameTimeline.c_str();
  if (dasCharge >= 0) {
    dasEquivalentTimelineStorage = getDasEquivalentInputTimeline(inputFrameTimeline.c_str());
    contextInputFrameTimeline = dasEquivalentTimelineStorage.c_str();
  }
  const PieceRangeContext pieceRangeContextLookup[4] = {
      getPieceRangeContext(contextInputFrameTimeline, 1, /* gravityDoubled= */ true),
      getPieceRangeContext(contextInputFrameTimeline, 1, /* gravityDoubled= */ false),
      getPieceRangeContext(contextInputFrameTimeline, 2, /* gravityDoubled= */ false),
      getPieceRangeContext(contextInputFrameTimeline, 3, /* gravityDoubled= */ false),
  };
  const EvalContext context = getEvalContext(startingGameState, pieceRangeContextLookup,
                                             /* useTestWeights= */ false, aggressionLevel);

  // Recalculate holes once we have the eval context
  pair<int, float> result2 = updateSurfaceAndHoles(
      startingGameState.surfaceArray, startingGameState.board,
      context.countWellHoles ? -1 : context.wellColumn, context.aiMode == DIG);
  startingGameState.numTrueHoles = result2.first;
  startingGameState.numPartialHoles = result2.second;

  if (LOGGING_ENABLED) {
    printBoard(startingGameState.board);
    printBoardBits(startingGameState.board);
  }

  // Take the specified action on the input based on the request type
  // The lock value lookup is a 2-ply search, so it requires the next piece to be known.
  if ((requestType == GET_LOCK_VALUE_LOOKUP || requestType == GET_LOCK_VALUE_LOOKUP_DAS)
      && nextPiece == NULL) {
    return "Error: lock value lookup requests require a value for nextPiece.";
  }
  switch (requestType) {
  case GET_LOCK_VALUE_LOOKUP: {
    return getLockValueLookupEncoded(startingGameState, curPiece, nextPiece, pruningBreadth,
                                     playoutCount, playoutLength, &context,
                                     pieceRangeContextLookup);
  }

  case GET_LOCK_VALUE_LOOKUP_DAS: {
    // The real timeline goes to the DAS move search; the piece-range/playout
    // contexts above already hold the derived tapper approximation.
    return getLockValueLookupDas(startingGameState, curPiece, nextPiece, pruningBreadth,
                                 playoutCount, playoutLength, inputFrameTimeline.c_str(), &context,
                                 pieceRangeContextLookup);
  }

  case GET_TOP_MOVES: {
    // needExplanation(GET_TOP_MOVES) is true, so per-move evalExplanation strings are populated.
    // Optionally includes the player's move (from secondBoard) even if it wouldn't otherwise make
    // the top N.
    unsigned int *playerBoard = secondBoardStr.length() > 0 ? secondBoard : nullptr;
    return getTopMoveList(startingGameState, curPiece, nextPiece, NUM_ENGINE_MOVES, playoutCount,
                          playoutLength, &context, pieceRangeContextLookup,
                          needExplanation(GET_TOP_MOVES), playerBoard);
  }

  case GET_TOP_MOVES_HYBRID: {
    // needExplanation(GET_TOP_MOVES_HYBRID) is true, so both lists include evalExplanation.
    unsigned int *playerBoardHybrid = secondBoardStr.length() > 0 ? secondBoard : nullptr;
    std::string nnbResult =
        getTopMoveList(startingGameState, curPiece, /* nextPiece= */ NULL, NUM_ENGINE_MOVES,
                       playoutCount, playoutLength, &context, pieceRangeContextLookup,
                       needExplanation(GET_TOP_MOVES_HYBRID), playerBoardHybrid);
    std::string nbResult =
        getTopMoveList(startingGameState, curPiece, nextPiece, NUM_ENGINE_MOVES_ADJUSTMENTS,
                       playoutCount, playoutLength, &context, pieceRangeContextLookup,
                       needExplanation(GET_TOP_MOVES_HYBRID), playerBoardHybrid);
    return "{\"noNextBox\":" + nnbResult + ", \"nextBox\":" + nbResult + "}";
  }

  case RATE_MOVE: {
    return rateMove(startingGameState, curPiece, nextPiece, secondBoard, pruningBreadth,
                    playoutCount, playoutLength, &context, pieceRangeContextLookup);
  }

  case GET_MOVE: {
    LockLocation bestMove =
        playOneMove(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount,
                    playoutLength, &context, pieceRangeContextLookup);
    int xOffset = bestMove.x - 3;
    int rot = bestMove.rotationIndex;
    int yOffset = bestMove.y - curPiece->initialY;
    return string_format("[%d, %d, %d]", rot, xOffset, yOffset);
    // int debugSequence[SEQUENCE_LENGTH] = {curPiece->index};
    // playSequence(startingGameState, pieceRangeContextLookup, debugSequence, /* playoutLength= */
    // 1); return "Debug playout complete.";
  }

  case GET_MOVE_AB_TEST: {
    // needExplanation(GET_MOVE_AB_TEST) is true, so capture explanations for both weight sets.
    // Passing nullptr (as GET_MOVE does) would skip the extra std::string work.
    std::string explanationMain;
    std::string explanationTest;
    std::string *mainPtr = needExplanation(GET_MOVE_AB_TEST) ? &explanationMain : nullptr;
    std::string *testPtr = needExplanation(GET_MOVE_AB_TEST) ? &explanationTest : nullptr;
    LockLocation bestMove =
        playOneMove(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount,
                    playoutLength, &context, pieceRangeContextLookup, mainPtr);
    int xOffset = bestMove.x - 3;
    int rot = bestMove.rotationIndex;
    int yOffset = bestMove.y - curPiece->initialY;

    const EvalContext contextTest = getEvalContext(startingGameState, pieceRangeContextLookup,
                                                   /* useTestWeights= */ true, aggressionLevel);
    LockLocation bestMoveTest =
        playOneMove(startingGameState, curPiece, nextPiece, pruningBreadth, playoutCount,
                    playoutLength, &contextTest, pieceRangeContextLookup, testPtr);
    int xOffsetTest = bestMoveTest.x - 3;
    int rotTest = bestMoveTest.rotationIndex;
    int yOffsetTest = bestMoveTest.y - curPiece->initialY;

    // Escape for safe JSON embedding (explanations are ;-separated "name=value" pairs).
    std::string escMain = escapeJsonString(explanationMain);
    std::string escTest = escapeJsonString(explanationTest);
    // Extra array element keeps [0]=agree/disagree, [1]=main, [2]=test backward-compatible
    // with the existing JS consumer while exposing both explanations.
    bool agree = xOffset == xOffsetTest && rot == rotTest && yOffset == yOffsetTest;
    if (agree) {
      return string_format("[\"agree\", [%d, %d, %d], {\n\"explanationMain\":\"%s\"}]", rot,
                           xOffset, yOffset, escMain.c_str());
    } else {
      return string_format("[\"disagree\", [%d, %d, %d], [%d, %d, %d], "
                           "{\n\"explanationMain\":\"%s\", \n\"explanationTest\":\"%s\"}]",
                           rot, xOffset, yOffset, rotTest, xOffsetTest, yOffsetTest,
                           escMain.c_str(), escTest.c_str());
    }
  }

  case PRECOMPUTE:
  case PRECOMPUTE_BACKUP: {
#ifndef __EMSCRIPTEN__
    // The real timeline goes to the move searches; the piece-range / eval /
    // playout contexts above keep the derived tapper approximation.
    if (requestType == PRECOMPUTE) {
      return runPrecompute(startingGameState, curPiece, inputFrameTimeline.c_str(), dasCharge,
                           reactionTime, pruningBreadth, playoutCount, playoutLength, &context,
                           pieceRangeContextLookup);
    }
    return runPrecomputeBackup(startingGameState, curPiece, inputFrameTimeline.c_str(), dasCharge,
                               reactionTime, pruningBreadth, playoutCount, playoutLength, &context,
                               pieceRangeContextLookup);
#else
    // The precompute path is not part of the WASM build.
    return "Precompute unavailable";
#endif
  }

  default: {
    return "Unknown request";
  }
  }
}
