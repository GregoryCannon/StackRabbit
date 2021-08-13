#include "../include/high_level_search.h"
#include <string>
#include <unordered_map>
#include "../include/params.h"
using namespace std;

#define UNEXPLORED_PENALTY -500   // A penalty for placements that weren't explored with playouts (could be worse than the eval indicates)
#define MAP_OFFSET 20000          // An offset to make any placement better than the default 0 in the map

int bestMoveRankingCount[DEPTH_2_PRUNING_BREADTH] = {}; // 12 long since we look at the top 12 moves

/** Concatenates the position of a piece into a single string. */
std::string encodeLockPosition(LockLocation lockLocation){
  char buffer[10];
  sprintf(buffer, "%d|%d|%d", lockLocation.rotationIndex, lockLocation.x, lockLocation.y);
  return string(buffer);
}

/** Calculates the valuation of every possible terminal position for a given piece on a given board, and stores it in a map. */
std::string getLockValueLookupEncoded(GameState gameState, Piece firstPiece, Piece secondPiece, int keepTopN, EvalContext evalContext, FastEvalWeights weights){
  unordered_map<string, float> lockValueMap;

  // Get the list of evaluated possibilities
  list<Depth2Possibility> possibilityList;
  searchDepth2(gameState, firstPiece, secondPiece, keepTopN, evalContext, weights, possibilityList);

  // Perform playouts on the promising possibilities
  int i = 0;
  float bestSeen = weights.deathCoef - 1;
  int bestIndex = -1;
  for (Depth2Possibility const& possibility : possibilityList) {
    string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);

    float overallScore = MAP_OFFSET + (i < keepTopN
      ? possibility.immediateReward + getPlayoutScore(possibility.resultingState, 56, evalContext, weights)
      : possibility.evalScore + UNEXPLORED_PENALTY);
    // Track the best move seen and where it ranked in the initial sorted list
    // if (overallScore > bestSeen){
    //   bestSeen = overallScore;
    //   bestIndex = i;
    // }

    if (overallScore > lockValueMap[lockPosEncoded]) {
      lockValueMap[lockPosEncoded] = overallScore;
    }
    i++;
  }
  
  // Track where the overall best move ranked in the list by initial eval
  // bestMoveRankingCount[bestIndex] += 1;
  // for (int i = 0; i < DEPTH_2_PRUNING_BREADTH; i++){
  //   printf("%d:%d, ", i+1, bestMoveRankingCount[i]);
  // }
  // printf("\n");

  // Encode lookup to JSON
  std::string mapEncoded = std::string("{");
  for( const auto& n : lockValueMap ) {
    char buf[20];
    sprintf(buf, "\"%s\":%f,", n.first.c_str(), n.second - MAP_OFFSET);
    mapEncoded.append(buf);
  }
  if (lockValueMap.size() > 0){
    mapEncoded.pop_back(); // Remove the last comma
  }
  mapEncoded.append("}");
  return mapEncoded;
}



/** Searches 2-ply from a starting state, and performs a fast eval on each of the resulting states. Maintains a sorted list of the top N possibilities, and adds all the rest onto the end in no specified order. */
int searchDepth2(GameState gameState, Piece firstPiece, Piece secondPiece, int keepTopN, EvalContext evalContext, FastEvalWeights weights, OUT list<Depth2Possibility> &possibilityList){
  auto cutoffPossibility = possibilityList.begin(); // The node on the "cutoff" between being in the top N placements and not
  int size = 0; // Tracking manually is cheaper than doing the O(n) operation each iteration

  // Get the placements of the first piece
  vector<SimState> firstLockPlacements;
  moveSearch(gameState, firstPiece, evalContext.inputFrameTimeline, firstLockPlacements);
  for (auto it = begin(firstLockPlacements); it != end(firstLockPlacements); ++it) {
    SimState firstPlacement = *it;
    GameState afterFirstMove = advanceGameState(gameState, firstPlacement, evalContext);
    float firstMoveReward = getLineClearFactor(afterFirstMove.lines - gameState.lines, weights);

    // Get the placements of the second piece
    vector<SimState> secondLockPlacements;
    moveSearch(afterFirstMove, secondPiece, evalContext.inputFrameTimeline, secondLockPlacements);

    for (auto secondPlacement : secondLockPlacements) {
      GameState resultingState = advanceGameState(afterFirstMove, secondPlacement, evalContext);
      float evalScore = firstMoveReward + fastEval(afterFirstMove, resultingState, secondPlacement, evalContext, weights);
      float secondMoveReward = getLineClearFactor(resultingState.lines - afterFirstMove.lines, weights);

      Depth2Possibility newPossibility = {
        { firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex },
        { secondPlacement.x, secondPlacement.y, secondPlacement.rotationIndex },
        resultingState,
        evalScore,
        firstMoveReward + secondMoveReward
      };

      if (size < keepTopN || evalScore > cutoffPossibility->evalScore) {
        // Insert into the list in its correct sorted place
        for (auto it = possibilityList.begin(); true; it++) {
          if (it == possibilityList.end()) {
            // printf("Adding to end\n");
            possibilityList.push_back(newPossibility);
            size++;
            break;
          }
          if (evalScore > it->evalScore) {
            // printf("Inserting\n");
            possibilityList.insert(it, newPossibility);
            size++;
            break;
          }
        }

        // Update the cutoff node
        if (size == keepTopN) {
          // Get the Nth elt
          auto ptr = possibilityList.begin();
          advance(ptr, keepTopN - 1);
          cutoffPossibility = ptr;
          // printf("INITIAL CUTOFF: %f\n", cutoffPossibility->evalScore);
        }
        if (size > keepTopN) {
          // Use the predecessor of the one that just got pushed out
          cutoffPossibility = prev(cutoffPossibility);
          // printf("NEW CUTOFF: %f\n", cutoffPossibility->evalScore);
        }

      } else {
        // Add it at the end of the list (sorting not important)
        possibilityList.push_back(newPossibility);
        // maybe("Not top 10, but adding\n");
      }
    }
  }
  return (int) possibilityList.size();
}


// void evaluatePossibilitiesWithPlayouts(int timeoutMs){
//   auto millisec_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
//
// }
