#include "high_level_search.hpp"
#include <string>
#include <math.h>
#include <unordered_map>
#include "params.hpp"
#include <limits>
#include "formatting.hpp"
using namespace std;

#define MAP_OFFSET 5000          // An offset to make any placement better than the default 0 in the map

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
    if (SHOULD_PLAY_PERFECT && ((resultingState.lines - gameState.lines) % 4) != 0) {
      continue; // While playing perfect, ignore any placements that burn lines
    }
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
int searchDepth2(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, int secondPieceDelay, const EvalContext *evalContext, OUT list<Possibility> &possibilityList){

  // Get the placements of the first piece
  vector<LockPlacement> firstLockPlacements;
  moveSearch(gameState, firstPiece, evalContext->pieceRangeContext.inputFrameTimeline, firstLockPlacements);
  for (auto it = begin(firstLockPlacements); it != end(firstLockPlacements); ++it) {
    LockPlacement firstPlacement = *it;
    maybePrint("\n\n\n\nNEW FIRST MOVE: rot=%d x=%d\n", firstPlacement.rotationIndex, firstPlacement.x);

    GameState afterFirstMove = advanceGameState(gameState, firstPlacement, evalContext);
    if (SHOULD_PLAY_PERFECT && ((afterFirstMove.lines - gameState.lines) % 4) != 0) {
      continue; // While playing perfect, ignore any placements that burn lines
    }
    for (int i = 0; i < 19; i++) {
      maybePrint("%d ", (afterFirstMove.board[i] & ALL_TUCK_SETUP_BITS) >> 20);
    }
    maybePrint("%d end of post first move\n", (afterFirstMove.board[19] & ALL_TUCK_SETUP_BITS) >> 20);
    if (LOGGING_ENABLED) {
      printBoard(afterFirstMove.board);
    }

    float firstMoveReward = getLineClearFactor(afterFirstMove.lines - gameState.lines, evalContext->weights, evalContext->shouldRewardLineClears);

    // Apply the second piece delay, if there is any
    std::string effInputTimeline = std::string("");
    if (secondPieceDelay > 0){
      for (int i = 0; i < secondPieceDelay; i++){
        effInputTimeline.append(".");
      }
      // 57 frames is an upper bound on how long a piece can be on screen. We have to fully extend the timeline so it doesn't repeat and make the agent do this delay again.
      // Calculation: 19 cells dropped (long bar flat) * 3 frames/gravity (level 18)
      while (effInputTimeline.length() < 57){
        effInputTimeline.append(evalContext->pieceRangeContext.inputFrameTimeline);
      }
    } else {
      effInputTimeline = evalContext->pieceRangeContext.inputFrameTimeline;
    }

    // Get the placements of the second piece
    vector<LockPlacement> secondLockPlacements;
    moveSearch(afterFirstMove, secondPiece, effInputTimeline.c_str(), secondLockPlacements);

    for (auto secondPlacement : secondLockPlacements) {
      GameState resultingState = advanceGameState(afterFirstMove, secondPlacement, evalContext);
      if (SHOULD_PLAY_PERFECT && ((resultingState.lines - afterFirstMove.lines) % 4) != 0) {
        continue; // While playing perfect, ignore any placements that burn lines
      }
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
    searchDepth2(gameState, firstPiece, secondPiece, numCandidatesToPlayout, /* secondPieceDelay= */ 0, evalContext, possibilityList);
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

  LockLocation bestLockLocation = {NONE, NONE, NONE};
  float bestPossibilityScore = FLOAT_MIN;
  int numPlayedOut = 0;
  for (auto possibility : sortedList){
    if (numPlayedOut >= numCandidatesToPlayout) {
      break;
    }
    float overallScore = possibility.immediateReward + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, lastSeenPiece->index, /* playoutDataList */ NULL);

    maybePrint("Possibility %d %d has overallscore %f %f\n", possibility.firstPlacement.rotationIndex, possibility.firstPlacement.x - 3, overallScore, possibility.evalScoreInclReward);

    // Potentially update the best possibility
    if (bestLockLocation.x == NONE || overallScore > bestPossibilityScore) {
      bestLockLocation = possibility.firstPlacement;
      bestPossibilityScore = overallScore;
    }
    numPlayedOut++;
  }

  if (SHOULD_PLAY_PERFECT && bestPossibilityScore < 0.0001){
    // Game is over
    return {NONE, NONE, NONE};
  }
  return bestLockLocation;
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
      unsigned int srMoveRow = ((*iter).resultingState.board[i] & FULL_ROW); // Filter for only the cell bits, since the player-provided board hasn't calculated any of the extra stuff
      if (playerBoardAfter[i] != srMoveRow){
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
  bool hasNb = secondPiece != NULL;

  // Search depth 1
  searchDepth1(gameState, firstPiece, numCandidatesToPlayout, evalContext, possibilityListD1);
  if (hasNb){
    searchDepth2(gameState, firstPiece, secondPiece, numCandidatesToPlayout, /* secondPieceDelay= */ 0, evalContext, possibilityListD2);
  }
  if (possibilityListD1.size() == 0 || (hasNb && possibilityListD2.size() == 0)){
    return std::string("Error: no legal moves found");
  }
  
  // Find the player move (and remove it from the D1 possibility list)
  Possibility playerMove = findPlayerMove(possibilityListD1, playerBoardAfter);
  if (playerMove.firstPlacement.x == NONE){     // Check for the particular error value supplied by the function
    return std::string("Error: player move not found");
  }

  // Sort the rest of the possibilities
  partiallySortPossibilityList(possibilityListD1, numCandidatesToPlayout, sortedListD1);
  if (hasNb){
    partiallySortPossibilityList(possibilityListD2, 999999, sortedListD2); // Large numCandidatesToPlayout = full sort
  }

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

    if (hasNb){
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
    
    if (hasNb){
      // NB Playouts
      bool bestValUnset = true;
      bestValAfterAdj = FLOAT_MIN;
      bool playerValUnset = true;
      playerValAfterAdj = FLOAT_MIN;
      numPlayedOut = 0;
      for (auto possibility : sortedListD2){
        if (numPlayedOut >= numCandidatesToPlayout && !playerValUnset) {
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
  }

  return formatRateMove(playerValNoAdj, bestValNoAdj, playerValAfterAdj, bestValAfterAdj, hasNb);
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
    searchDepth2(gameState, firstPiece, secondPiece, numSorted, /* secondPieceDelay= */ 0, evalContext, possibilityList);
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

    // If this position has no legal playouts, ignore it
    if (playoutDataList.size() == 0){
      continue;
    }
    // Pick 7 playouts from the sorted playout list
    int len = (int) playoutDataList.size();
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

  return formatEngineMoveList(sortedList, firstPiece, secondPiece);
}

/** Calculates the valuation of every possible terminal position for a given piece on a given board, and stores it in a map.
 * @param keepTopN - How many possibilities to evaluate via a full set of playouts, as opposed to just the eval function.
 * @param secondPieceDelay - A very particular parameter that's usually 0 and only used to help simulate the loss of DAS while placing the first piece.
 */
unordered_map<string, float> getLockValueLookup(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, int playoutCount, int playoutLength, int secondPieceDelay, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  unordered_map<string, float> lockValueMap;
  unordered_map<string, int> lockValueRepeatMap;

  // Keep a running list of the top X possibilities as the move search is happening.
  // Keep twice as many as we'll eventually need, since some duplicates may be removed before playouts start
  int numSorted = keepTopN * 2;
  
  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> sortedList;
  searchDepth2(gameState, firstPiece, secondPiece, numSorted, secondPieceDelay, evalContext, possibilityList);
  partiallySortPossibilityList(possibilityList, numSorted, sortedList);

  // If no playouts, just use the eval
  if (playoutCount * playoutLength == 0){
    for (Possibility const& possibility : sortedList) {
      string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
      float overallScore = possibility.evalScoreInclReward;
      if (lockValueMap.count(lockPosEncoded) == 0 || overallScore > lockValueMap[lockPosEncoded]) {
        lockValueMap[lockPosEncoded] = overallScore;
      }
    }
  } else {
    // Perform playouts on the promising possibilities
    int i = 0;
    int numPlayedOut = 0;
    int firstPlacementRepeatCap = floor(LOCK_POSITION_REPEAT_CAP_PROPORTION * keepTopN);
    for (Possibility const& possibility : sortedList) {
      string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
      // Cap the number of times a lock position can be repeated (despite differing second placements)
      int shouldPlayout = i < numSorted && numPlayedOut < keepTopN && lockValueRepeatMap[lockPosEncoded] < firstPlacementRepeatCap;
      if (PLAYOUT_LOGGING_ENABLED) {
        printf("\n----%s, repeats %d, willPlay %d\n", lockPosEncoded.c_str(), lockValueRepeatMap[lockPosEncoded], shouldPlayout);
      }
      lockValueRepeatMap[lockPosEncoded] += 1;

      float overallScore = (shouldPlayout
         ? possibility.immediateReward + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, secondPiece->index, /* playoutDataList */ NULL)
         : (SHOULD_PLAY_PERFECT ? 0 : evalContext->weights.deathCoef));
      
      if (lockValueMap.count(lockPosEncoded) == 0 || overallScore > lockValueMap[lockPosEncoded]) {
        if (PLAYOUT_LOGGING_ENABLED || PLAYOUT_RESULT_LOGGING_ENABLED) {
          if (shouldPlayout) {
            printf("Adding to map: %s %f (%f + %f)\n", lockPosEncoded.c_str(), overallScore, possibility.immediateReward, overallScore - possibility.immediateReward);
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
  }
  
  return lockValueMap;
}

std::string encodeMapToJSON(unordered_map<string, float> map){
  // Encode lookup to JSON
  std::string mapEncoded = std::string("{");
  // float globalMax = 0; // Only used for perfect play
  for( const auto& n : map ) {
    char mapEntryBuf[30];
    snprintf(mapEntryBuf, 30, "\"%s\":%.2f,", n.first.c_str(), n.second);
    mapEncoded.append(mapEntryBuf);
    // if (SHOULD_PLAY_PERFECT){
    //   globalMax = std::max(globalMax, n.second);
    // }
  }
  // if (SHOULD_PLAY_PERFECT && globalMax < FLOAT_EPSILON){
  //   return "{\"abort\": true}";
  // }
  if (map.size() > 0) {
    mapEncoded.pop_back(); // Remove the last comma
  }
  mapEncoded.append("}");
  return mapEncoded;
}

/** Calculates the valuation of every possible terminal position for a given piece on a given board, and stores it in a map.
 * @param keepTopN - How many possibilities to evaluate via a full set of playouts, as opposed to just the eval function.
 */
std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  unordered_map<string, float> lockValueMap = getLockValueLookup(gameState, firstPiece, secondPiece, keepTopN, playoutCount, playoutLength, /* secondPieceDelay= */ 0, evalContext, pieceRangeContextLookup);
  return encodeMapToJSON(lockValueMap);
}

std::string getLockValueLookupDas(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, int playoutCount, int playoutLength, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  // First, do multiple shallow searches at low depth to check if having partial or no DAS charge clearly misses the best placement
  // E.g. if the first placement is a tuck setup and the second placement resolves it (very common pattern), the second piece needs enough DAS charge to be able to actually do the tuck.
  // Alternatively, if the first placement is resolving a tuck setup, and the best second placement is something not DAS-intensive, then there should be no penalty.
  unordered_map<string, float> shallowMapFullCharge = getLockValueLookup(
    gameState, firstPiece, secondPiece, keepTopN, /* playoutCount= */ 0, /* playoutLength= */ 0, 
    /* secondPieceDelay= */ 0, evalContext, pieceRangeContextLookup
  );

  // A DAS charge of 10 (partial charge) is equivalent to waiting 5 frames before the first shift
  unordered_map<string, float> shallowMapPartialCharge = getLockValueLookup(
    gameState, firstPiece, secondPiece, keepTopN, /* playoutCount= */ 0, /* playoutLength= */ 0, 
    /* secondPieceDelay= */ 5, evalContext, pieceRangeContextLookup
  );
  
  // A DAS charge of 5 is equivalent to 10 frame wait before the first shift. It's also the worst possible DAS charge, since even with 0 charge you can 
  // just first-frame tap without holding during ARE, and it ends up being exactly equivalent:
  // Charge < 5 : shift->0  1  2  3   4  5  6  7  8  9    10
  // Charge = 5:   6       7  8  9  10 11 12 13 14 15 shift->10
  unordered_map<string, float> shallowMapMinCharge = getLockValueLookup(
    gameState, firstPiece, secondPiece, keepTopN, /* playoutCount= */ 0, /* playoutLength= */ 0, 
    /* secondPieceDelay= */ 10, evalContext, pieceRangeContextLookup
  );

  // Replace the raw value maps with maps of the penalty relative to the best placement (as negative numbers)
  for (const auto& pair : shallowMapFullCharge) {
    string key = pair.first;
    float value = pair.second;
    float penaltyPartial = shallowMapPartialCharge.at(key) - value;
    float penaltyMin = shallowMapMinCharge.at(key) - value;
    shallowMapPartialCharge[key] = penaltyPartial;
    shallowMapMinCharge[key] = penaltyMin;
  }


  // Then, do one deep search to see what the actual best move is, irrespective of DAS
  unordered_map<string, float> deepValue = getLockValueLookup(
    gameState, firstPiece, secondPiece, keepTopN, playoutCount, playoutLength, /* secondPieceDelay= */ 0, evalContext, pieceRangeContextLookup
  );
  
  // Compile result as array of JSONs
  std::string resultStr = std::string("[\n");
  resultStr.append(encodeMapToJSON(shallowMapPartialCharge));
  resultStr.append(",\n");
  resultStr.append(encodeMapToJSON(shallowMapMinCharge));
  resultStr.append(",\n");
  resultStr.append(encodeMapToJSON(deepValue));
  resultStr.append("\n]");
  return resultStr;
}



// void evaluatePossibilitiesWithPlayouts(int timeoutMs){
//   auto millisec_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
//
// }
