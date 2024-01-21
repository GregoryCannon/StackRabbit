#include "high_level_search.hpp"
#include <string>
#include <math.h>
#include <unordered_map>
#include "params.hpp"
#include <limits>
#include "formatting.hpp"
using namespace std;

#define MAP_OFFSET 20000          // An offset to make any placement better than the default 0 in the map

/**
 * Performs a partial insertion sort such that the highest N elements of the list are guaranteed to be sorted and kept at the front of the list.
 * The other elements can be anywhere.
 */
void partiallySortPossibilityList(list<Possibility> &possibilityList, int keepTopN, OUT list<Possibility> &sortedList){
  auto cutoffPossibility = possibilityList.begin(); // The node on the "cutoff" between being in the top N placements and not
  int size = 0; // Tracking manually is cheaper than doing the O(n) operation each iteration

  for (auto it = begin(possibilityList); it != end(possibilityList); it++) {
    Possibility newPossibility = *it;
    if (size < keepTopN || newPossibility.evalScoreInclReward > cutoffPossibility->evalScoreInclReward) {
      // Insert into the list in its correct sorted place
      for (auto it2 = sortedList.begin(); true; it2++) {
        if (it2 == sortedList.end()) {
          sortedList.push_back(newPossibility);
          size++;
          break;
        }
        if (newPossibility.evalScoreInclReward > it2->evalScoreInclReward) {
          sortedList.insert(it2, newPossibility);
          size++;
          break;
        }
      }

      // Update the cutoff node
      if (size == keepTopN) {
        // Get the Nth elt
        auto ptr = sortedList.begin();
        advance(ptr, keepTopN - 1);
        cutoffPossibility = ptr;
      }
      if (size > keepTopN) {
        // Use the predecessor of the one that just got pushed out
        cutoffPossibility = prev(cutoffPossibility);
      }

    } else {
      // Add it at the end of the list (sorting not important)
      sortedList.push_back(newPossibility);
    }
  }

}

/** Searches 1-ply from a starting state, and performs an eval on each resulting state.
 * @returns an UNSORTED list of evaluated possibilities
 */
int searchDepth1(GameState gameState, const Piece *firstPiece, int keepTopN, const EvalContext *evalContext, OUT list<Possibility> &possibilityList){
  vector<LockPlacement> firstLockPlacements;
  moveSearch(gameState, firstPiece, evalContext->pieceRangeContext.inputFrameTimeline, firstLockPlacements);
  for (auto it = begin(firstLockPlacements); it != end(firstLockPlacements); ++it) {
    LockPlacement firstPlacement = *it;

    GameState resultingState = advanceGameState(gameState, firstPlacement, evalContext);
    float reward = getLineClearFactor(resultingState.lines - gameState.lines, evalContext->weights, evalContext->shouldRewardLineClears);
    float evalScoreInclReward = fastEval(gameState, resultingState, firstPlacement, evalContext);

    Possibility newPossibility = {
      { firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex },
      NULL_LOCK_LOCATION,
      resultingState,
      evalScoreInclReward,
      reward
    };
    possibilityList.push_back(newPossibility);
  }
  return (int) possibilityList.size();
}

/** Searches 2-ply from a starting state, and performs a fast eval on each of the resulting states. 
 * @returns an UNSORTED list of evaluated possibilities
 */
int searchDepth2(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, const EvalContext *evalContext, OUT list<Possibility> &possibilityList){

  // Get the placements of the first piece
  vector<LockPlacement> firstLockPlacements;
  moveSearch(gameState, firstPiece, evalContext->pieceRangeContext.inputFrameTimeline, firstLockPlacements);
  for (auto it = begin(firstLockPlacements); it != end(firstLockPlacements); ++it) {
    LockPlacement firstPlacement = *it;
    maybePrint("\n\n\n\nNEW FIRST MOVE: rot=%d x=%d\n", firstPlacement.rotationIndex, firstPlacement.x);

    GameState afterFirstMove = advanceGameState(gameState, firstPlacement, evalContext);
    for (int i = 0; i < 19; i++) {
      maybePrint("%d ", (afterFirstMove.board[i] & ALL_TUCK_SETUP_BITS) >> 20);
    }
    maybePrint("%d end of post first move\n", (afterFirstMove.board[19] & ALL_TUCK_SETUP_BITS) >> 20);
    if (LOGGING_ENABLED) {
      printBoard(afterFirstMove.board);
    }

    float firstMoveReward = getLineClearFactor(afterFirstMove.lines - gameState.lines, evalContext->weights, evalContext->shouldRewardLineClears);

    // Get the placements of the second piece
    vector<LockPlacement> secondLockPlacements;
    moveSearch(afterFirstMove, secondPiece, evalContext->pieceRangeContext.inputFrameTimeline, secondLockPlacements);

    for (auto secondPlacement : secondLockPlacements) {
      GameState resultingState = advanceGameState(afterFirstMove, secondPlacement, evalContext);
      float evalScore = firstMoveReward + fastEval(afterFirstMove, resultingState, secondPlacement, evalContext);
      float secondMoveReward = getLineClearFactor(resultingState.lines - afterFirstMove.lines, evalContext->weights, evalContext->shouldRewardLineClears);

      Possibility newPossibility = {
        { firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex },
        { secondPlacement.x, secondPlacement.y, secondPlacement.rotationIndex },
        resultingState,
        evalScore,
        firstMoveReward + secondMoveReward
      };

      possibilityList.push_back(newPossibility);
    }
  }
  return (int) possibilityList.size();
}

/** Plays one move from a given state, with or without knowledge of the next box.*/
LockLocation playOneMove(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int numCandidatesToPlayout, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> sortedList;
  
  // Search depth either 1 or 2 depending on whether a next piece was provided
  const Piece *lastSeenPiece;
  if (secondPiece == NULL){
    searchDepth1(gameState, firstPiece, numCandidatesToPlayout, evalContext, possibilityList);
    lastSeenPiece = firstPiece;
  } else {
    searchDepth2(gameState, firstPiece, secondPiece, numCandidatesToPlayout, evalContext, possibilityList);
    lastSeenPiece = secondPiece;
  }

  if (possibilityList.size() == 0){
    return NULL_LOCK_LOCATION; // Return an invalid lock location to indicate the agent has topped out
  }
  partiallySortPossibilityList(possibilityList, numCandidatesToPlayout, sortedList);

  if (playoutCount * playoutLength == 0){
    // Return the first element in the preliminary sorted list
    return (*sortedList.begin()).firstPlacement;
  }

  LockLocation const *bestLockLocation = NULL;
  float bestPossibilityScore = FLOAT_MIN;
  int numPlayedOut = 0;
  for (auto possibility : sortedList){
    if (numPlayedOut >= numCandidatesToPlayout) {
      break;
    }
    float overallScore = possibility.immediateReward + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, lastSeenPiece->index, /* playoutDataList */ NULL);

    // if (SHOULD_PLAY_PERFECT){
    //  overallScore = std::max(0.0f, overallScore); // 0 is the lowest possible eval score in the "play perfect" system
    //  overallScore = std::min(100.0f, overallScore); // 100 is the max possible eval score in the "play perfect" system
    // }

    maybePrint("Possibility %d %d has overallscore %f %f\n", possibility.firstPlacement.rotationIndex, possibility.firstPlacement.x - 3, overallScore, possibility.evalScoreInclReward);

    // Potentially update the best possibility
    if (bestLockLocation == NULL || overallScore > bestPossibilityScore) {
      bestLockLocation = &(possibility.firstPlacement);
      bestPossibilityScore = overallScore;
    }
    numPlayedOut++;
  }
  return *bestLockLocation;
}

/**
 * Finds the move out of a list of possibilities that has the resulting board equal to the player's resulting board.
 * NB: REMOVES THE ELEMENT FROM THE LIST IN-PLACE (to avoid having to do that later in rateMove())
 */
Possibility findPlayerMove(list<Possibility> possibilityList, unsigned int playerBoardAfter[20]){
  // Find the player move
  for (list<Possibility>::iterator iter=possibilityList.begin(); iter!=possibilityList.end(); iter++) {
    bool boardEqual = true;
    for (int i = 19; i >= 0; i--){
      if (playerBoardAfter[i] != (*iter).resultingState.board[i]){
        boardEqual = false;
        break;
      }
    }
    if (boardEqual){
      possibilityList.erase(iter);
      return *iter;
    }
  }
  // Error out
  return {NULL_LOCK_LOCATION,NULL_LOCK_LOCATION,{}, -1, -1 /* rest default initializer */};
}

std::string rateMove(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, unsigned int playerBoardAfter[20], int numCandidatesToPlayout, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  list<Possibility> possibilityListD1;
  list<Possibility> possibilityListD2;
  list<Possibility> sortedListD1; // Does not include player move
  list<Possibility> sortedListD2; // Includes player move

  // Search depth 1
  searchDepth1(gameState, firstPiece, numCandidatesToPlayout, evalContext, possibilityListD1);
  searchDepth2(gameState, firstPiece, secondPiece, numCandidatesToPlayout, evalContext, possibilityListD2);
  if (possibilityListD1.size() == 0 || possibilityListD2.size() == 0){
    return std::string("Error: no legal moves found");
  }
  
  // Find the player move (and remove it from the D1 possibility list)
  Possibility playerMove = findPlayerMove(possibilityListD1, playerBoardAfter);
  if (playerMove.firstPlacement.x == NONE){     // Check for the particular error value supplied by the function
    return std::string("Error: player move not found");
  }

  // Sort the rest of the possibilities
  partiallySortPossibilityList(possibilityListD1, numCandidatesToPlayout, sortedListD1);
  partiallySortPossibilityList(possibilityListD2, 999999, sortedListD2); // Large numCandidatesToPlayout = full sort

  float playerValNoAdj = FLOAT_MIN;
  float bestValNoAdj = FLOAT_MIN;
  float playerValAfterAdj = FLOAT_MIN;
  float bestValAfterAdj = FLOAT_MIN;
  
  // NO PLAYOUTS NEEDED
  if (playoutCount * playoutLength == 0){
    // NNB
    // If no playouts are requested, add the NNB values based on the already sorted list
    playerValNoAdj = playerMove.evalScoreInclReward;
    bestValNoAdj = playerValNoAdj;
    if (sortedListD1.size() > 0){
      float bestOtherVal = (*sortedListD1.begin()).evalScoreInclReward;
      bestValNoAdj = std::max(playerValNoAdj, bestOtherVal);
    }

    // WITH NB
    // Find the best NB values, as well as the best NB value that uses the player move for the first move
    bestValAfterAdj = (*sortedListD2.begin()).evalScoreInclReward;
    for (auto possibility : sortedListD2){
      if (lockLocationEquals(possibility.firstPlacement, playerMove.firstPlacement)){
        playerValAfterAdj = possibility.evalScoreInclReward;
        break;
      }
    }
  } 
  // PLAYOUTS NEEDED
  else {    
    // NNB Playouts (first on the player move, then on the rest)
    playerValNoAdj = playerMove.immediateReward + getPlayoutScore(playerMove.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, firstPiece->index, /* playoutDataList */ NULL);

    bestValNoAdj = playerValNoAdj;
    int numPlayedOut = 0;
    for (auto possibility : sortedListD1){
      if (numPlayedOut >= numCandidatesToPlayout) {
        break;
      }
      float overallScore = possibility.immediateReward + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, firstPiece->index, /* playoutDataList */ NULL);
      if (overallScore > bestValNoAdj) {
        bestValNoAdj = overallScore;
      }
      numPlayedOut++;
    }

    // NB Playouts
    bool bestValUnset = true;
    bestValAfterAdj = FLOAT_MIN;
    bool playerValUnset = true;
    playerValAfterAdj = FLOAT_MIN;
    numPlayedOut = 0;
    for (auto possibility : sortedListD2){
      if (numPlayedOut >= numCandidatesToPlayout) {
        break;
      }
      float overallScore = possibility.immediateReward + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, secondPiece->index, /* playoutDataList */ NULL);
      if (bestValUnset || overallScore > bestValAfterAdj) {
        bestValUnset = false;
        bestValAfterAdj = overallScore;
      }
      if (lockLocationEquals(playerMove.firstPlacement, possibility.firstPlacement) 
            && (playerValUnset || overallScore > playerValAfterAdj)) {
        playerValUnset = false;
        playerValAfterAdj = overallScore;
      }
      numPlayedOut++;
    }
  }

  return formatRateMove(playerValNoAdj, bestValNoAdj, playerValAfterAdj, bestValAfterAdj);
}

/**
 * Gets a list of the top moves, formatted as a JSON string. (See formatting.hpp for exact format details).
 */
std::string getTopMoveList(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  // Keep a running list of the top X possibilities as the move search is happening.
  // Keep twice as many as we'll eventually need, since some duplicates may be removed before playouts start
  int numSorted = keepTopN * 2;
  printf("SecondPiece %p %d\n", secondPiece, secondPiece == NULL);

  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> initiallySortedList;
  list<EngineMoveData> sortedList;
  
  // Search depth either 1 or 2 depending on whether a next piece was provided
  const Piece *lastSeenPiece;
  if (secondPiece == NULL){
    searchDepth1(gameState, firstPiece, numSorted, evalContext, possibilityList);
    lastSeenPiece = firstPiece;
  } else {
    searchDepth2(gameState, firstPiece, secondPiece, numSorted, evalContext, possibilityList);
    lastSeenPiece = secondPiece;
  }

  if (possibilityList.size() == 0){
    return "No legal moves";
  }
  partiallySortPossibilityList(possibilityList, numSorted, initiallySortedList);

  // Perform playouts on the promising possibilities
  int numAdded = 0;
  for (Possibility const& possibility : initiallySortedList) {
    if (numAdded >= keepTopN){
      break;
    }
    // printf("Doing playout for: %s %s\n", encodeLockPosition(possibility.firstPlacement).c_str(), encodeLockPosition(possibility.secondPlacement).c_str());
    string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
    vector<PlayoutData> playoutDataList = {};
    float overallScore = possibility.immediateReward 
          + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, lastSeenPiece->index, &playoutDataList);

    // Pick 7 playouts from the sorted playout list
    int len = playoutDataList.size();
    EngineMoveData newMoveData = {
      possibility.firstPlacement,
      possibility.secondPlacement,
      /* playoutScore */ overallScore,
      /* shallowEvalScore */ possibility.evalScoreInclReward,
      /* resultingBoard */ formatBoard(possibility.resultingState.board),
      /* playout1 (best case) */ playoutDataList.at(0),
      /* playout2 (83 %ile case) */ playoutDataList.at(len / 6), // Fractions are "backwards" because moves are ordered best (100%ile) to worst (0%ile).
      /* playout3 (66 %ile case) */ playoutDataList.at(len / 3),
      /* playout4 (median case) */ playoutDataList.at(len / 2),
      /* playout5 (33 %ile case) */ playoutDataList.at(len * 2 / 3),
      /* playout6 (16 %ile case) */ playoutDataList.at(len * 5 / 6),
      /* playout7 (worst case) */ playoutDataList.at(len - 1),
    };
    insertIntoList(newMoveData, sortedList);
    numAdded++;
  }

  return formatEngineMoveList(sortedList);
}



/** Calculates the valuation of every possible terminal position for a given piece on a given board, and stores it in a map.
 * @param keepTopN - How many possibilities to evaluate via a full set of playouts, as opposed to just the eval function.
 */
std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  unordered_map<string, float> lockValueMap;
  unordered_map<string, int> lockValueRepeatMap;

  // Keep a running list of the top X possibilities as the move search is happening.
  // Keep twice as many as we'll eventually need, since some duplicates may be removed before playouts start
  int numSorted = keepTopN * 2;
  
  if (SHOULD_PLAY_PERFECT){
    float baseEval = fastEval(gameState, gameState, /* lockPlacement */ { NO_PLACEMENT }, evalContext);
    if (baseEval == 0){
      printf("Already dead, aborting...");
      return "{}";
    }
  }

  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> sortedList;
  searchDepth2(gameState, firstPiece, secondPiece, numSorted, evalContext, possibilityList);
  partiallySortPossibilityList(possibilityList, numSorted, sortedList);

  // Perform playouts on the promising possibilities
  int i = 0;
  int numPlayedOut = 0;
  for (Possibility const& possibility : sortedList) {
    string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
    // Cap the number of times a lock position can be repeated (despite differing second placements)
    int shouldPlayout = i < numSorted && numPlayedOut < keepTopN && lockValueRepeatMap[lockPosEncoded] < 3;
    if (PLAYOUT_LOGGING_ENABLED) {
      printf("\n----%s, repeats %d, willPlay %d\n", lockPosEncoded.c_str(), lockValueRepeatMap[lockPosEncoded], shouldPlayout);
    }
    lockValueRepeatMap[lockPosEncoded] += 1;

    // if (shouldPlayout){
    //   printf("Doing playout for: %s %s\n", encodeLockPosition(possibility.firstPlacement).c_str(), encodeLockPosition(possibility.secondPlacement).c_str());
    // }

    float overallScore = MAP_OFFSET + (shouldPlayout
       ? possibility.immediateReward + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, secondPiece->index, /* playoutDataList */ NULL)
       : evalContext->weights.deathCoef);

//    if (SHOULD_PLAY_PERFECT){
//      overallScore = std::max(MAP_OFFSET + 0.0f, overallScore); // 0 is the lowest possible eval score in the "play perfect" system
//      overallScore = std::min(MAP_OFFSET + 100.0f, overallScore); // 100 is the max possible eval score in the "play perfect" system
//    }
    
    if (overallScore > lockValueMap[lockPosEncoded]) {
      if (PLAYOUT_LOGGING_ENABLED || PLAYOUT_RESULT_LOGGING_ENABLED) {
        if (shouldPlayout) {
          printf("Adding to map: %s %f (%f + %f)\n", lockPosEncoded.c_str(), overallScore - MAP_OFFSET, possibility.immediateReward, overallScore - possibility.immediateReward - MAP_OFFSET);
        }
      }
      lockValueMap[lockPosEncoded] = overallScore;
    } else if (PLAYOUT_LOGGING_ENABLED || PLAYOUT_RESULT_LOGGING_ENABLED) {
      if (shouldPlayout) {
        printf("Score of %.1f is worse than existing move %.1f\n", overallScore, lockValueMap[lockPosEncoded]);
      }
    }
    i++;
    if (shouldPlayout) {
      numPlayedOut++;
    }
  }

  // Encode lookup to JSON
  std::string mapEncoded = std::string("{");
  for( const auto& n : lockValueMap ) {
    char mapEntryBuf[30];
    sprintf(mapEntryBuf, "\"%s\":%.2f,", n.first.c_str(), n.second - MAP_OFFSET);
    mapEncoded.append(mapEntryBuf);
  }
  if (lockValueMap.size() > 0) {
    mapEncoded.pop_back(); // Remove the last comma
  }
  mapEncoded.append("}");
  return mapEncoded;
}


// void evaluatePossibilitiesWithPlayouts(int timeoutMs){
//   auto millisec_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
//
// }
