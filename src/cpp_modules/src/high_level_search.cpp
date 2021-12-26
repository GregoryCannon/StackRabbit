#include "high_level_search.hpp"
#include <string>
#include <unordered_map>
#include "params.hpp"
using namespace std;

#define UNEXPLORED_PENALTY -500   // A penalty for placements that weren't explored with playouts (could be worse than the eval indicates)
#define MAP_OFFSET 20000          // An offset to make any placement better than the default 0 in the map

/** Concatenates the position of a piece into a single string. */
std::string encodeLockPosition(LockLocation lockLocation){
  if (VARIABLE_RANGE_CHECKS_ENABLED){
    // Check variable ranges to avoid buffer overflows
    if (lockLocation.rotationIndex > 4 || lockLocation.rotationIndex < 0){
      printf("rotation index out of range %d\n", lockLocation.rotationIndex);
      throw std::invalid_argument( "rotation index out of range" );
    }
    if (lockLocation.x > 7 || lockLocation.x < -2){
      printf("x index out of range %d\n", lockLocation.x);
      throw std::invalid_argument( "x index out of range" );
    }
    if (lockLocation.y < -2 || lockLocation.y > 19){
      printf("y index out of range %d\n", lockLocation.y);
      throw std::invalid_argument( "y index out of range" );
    }
  }
  char buffer[10];
  sprintf(buffer, "%d|%d|%d", lockLocation.rotationIndex, lockLocation.x, lockLocation.y);
  return string(buffer);
}

/**
 * Processes a Possibility object and formats the result how the various agents would expect it.
 * @returns a string of resulting data, in the format:
 * {number of shifts | number of right rotations | string of inputs | resulting board | resulting level | resulting lines}
 */
std::string formatPossibility(Possibility possibility){
  // TODO: implement
  // char buffer[300];
  // sprintf(buffer, "%d")
  return "";
}

/** Plays one move from a given state, with or without knowledge of the next box.*/
LockLocation playOneMove(GameState gameState, Piece *firstPiece, Piece *secondPiece, int playoutLength, int keepTopN, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  // TODO: implement
  return {};
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
    float evalScore = reward + fastEval(gameState, resultingState, firstPlacement, evalContext);

    Possibility newPossibility = {
      { firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex },
      {},
      resultingState,
      evalScore,
      reward
    };
    possibilityList.push_back(newPossibility);
  }
  return (int) possibilityList.size();
}


/**
 * Performs a partial insertion sort such that the highest N elements of the list are guaranteed to be sorted and kept at the front of the list.
 * The other elements can be anywhere.
 */
void partiallySortPossibilityList(list<Possibility> &possibilityList, int keepTopN, OUT list<Possibility> &sortedList){
  auto cutoffPossibility = possibilityList.begin(); // The node on the "cutoff" between being in the top N placements and not
  int size = 0; // Tracking manually is cheaper than doing the O(n) operation each iteration

  for (auto it = begin(possibilityList); it != end(possibilityList); ++it) {
    Possibility newPossibility = *it;
    if (size < keepTopN || newPossibility.evalScore + newPossibility.immediateReward > cutoffPossibility->evalScore + cutoffPossibility->immediateReward) {
      // Insert into the list in its correct sorted place
      for (auto it2 = sortedList.begin(); true; it2++) {
        if (it2 == sortedList.end()) {
          sortedList.push_back(newPossibility);
          size++;
          break;
        }
        if (newPossibility.evalScore > it2->evalScore) {
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


/** Searches 2-ply from a starting state, and performs a fast eval on each of the resulting states. Maintains a sorted list of the top N possibilities, and adds all the rest onto the end in no specified order. */
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
    if (LOGGING_ENABLED){
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


/** Calculates the valuation of every possible terminal position for a given piece on a given board, and stores it in a map.
 * @param keepTopN - How many possibilities to evaluate via a full set of playouts, as opposed to just the eval function.
 */
std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int keepTopN, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  unordered_map<string, float> lockValueMap;
  unordered_map<string, int> lockValueRepeatMap;

  // Keep a running list of the top X possibilities as the move search is happening.
  // Keep twice as many as we'll eventually need, since some duplicates may be removed before playouts start
  int numSorted = keepTopN * 2;

  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> sortedList;
  searchDepth2(gameState, firstPiece, secondPiece, numSorted, evalContext, possibilityList);
  partiallySortPossibilityList(possibilityList, keepTopN, sortedList);

  // Perform playouts on the promising possibilities
  int i = 0;
  int numPlayedOut = 0;
  int unexploredPenalty = evalContext->weights.deathCoef * 1.5;
  for (Possibility const& possibility : sortedList) {
    string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
    // Cap the number of times a lock position can be repeated (despite differing second placements)
    int shouldPlayout = i < numSorted && numPlayedOut < keepTopN && lockValueRepeatMap[lockPosEncoded] < 3;
    // if (shouldPlayout){
    //   printf("Doing playout for: %s %s\n", encodeLockPosition(possibility.firstPlacement).c_str(), encodeLockPosition(possibility.secondPlacement).c_str());
    // }

    float overallScore = MAP_OFFSET + (shouldPlayout
      ? possibility.immediateReward + getPlayoutScore(possibility.resultingState, pieceRangeContextLookup, secondPiece->index)
      : possibility.immediateReward + possibility.evalScore + unexploredPenalty);
    if (overallScore > lockValueMap[lockPosEncoded]) {
      if (PLAYOUT_LOGGING_ENABLED) {
        printf("Adding to map: %s %f (%f + %f)\n", lockPosEncoded.c_str(), overallScore - MAP_OFFSET, possibility.immediateReward, overallScore - possibility.immediateReward - MAP_OFFSET);
      }
      lockValueMap[lockPosEncoded] = overallScore;
      lockValueRepeatMap[lockPosEncoded] += 1;
    }
    i++;
    if (shouldPlayout) {
      numPlayedOut++;
    }
  }

  // Encode lookup to JSON
  std::string mapEncoded = std::string("{");
  for( const auto& n : lockValueMap ) {
    char buf[20];
    sprintf(buf, "\"%s\":%f,", n.first.c_str(), n.second - MAP_OFFSET);
    mapEncoded.append(buf);
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
