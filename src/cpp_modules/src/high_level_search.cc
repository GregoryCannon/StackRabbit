#include "../include/high_level_search.h"
#include <string>
#include <unordered_map>
using namespace std;

#define UNEXPLORED_PENALTY -500   // A penalty for placements that weren't explored with playouts (could be worse than the eval indicates)
#define MAP_OFFSET 20000          // An offset to make any placement better than the default 0 in the map

std::string getLockValueLookupEncoded(GameState gameState, Piece firstPiece, Piece secondPiece, int keepTopN){
  unordered_map<string, float> lockValueMap;

  // Get the list of evaluated possibilities
  list<Depth2Possibility> possibilityList;
  searchDepth2(gameState, firstPiece, secondPiece, keepTopN, possibilityList);

  int i = 0;
  for (Depth2Possibility const& possibility : possibilityList) {
    char buffer[10];
    sprintf(buffer, "%d|%d|%d", possibility.firstPlacement.rotationIndex, possibility.firstPlacement.x, possibility.firstPlacement.y);
    string lockPosEncoded = string(buffer);
    float adjustedScore = possibility.evalScore + (i > keepTopN ? UNEXPLORED_PENALTY : 0) + MAP_OFFSET;

    if (adjustedScore > lockValueMap[lockPosEncoded]) {
      lockValueMap[lockPosEncoded] = adjustedScore;
    }
    i++;
  }
  // Encode lookup to JSON
  std::string mapEncoded = std::string("{");
  for( const auto& n : lockValueMap ) {
    char buf[20];
    sprintf(buf, "\"%s\":%f,", n.first.c_str(), n.second - MAP_OFFSET);
    mapEncoded.append(buf);
  }
  mapEncoded.pop_back(); // Remove the last comma
  mapEncoded.append("}");
  return mapEncoded;
}



/** Searches 2-ply from a starting state, and performs a fast eval on each of the resulting states. Maintains a sorted list of the top N possibilities, and adds all the rest onto the end in no specified order. */
int searchDepth2(GameState gameState, Piece firstPiece, Piece secondPiece, int keepTopN, OUT list<Depth2Possibility> &possibilityList){
  auto cutoffPossibility = possibilityList.begin(); // The node on the "cutoff" between being in the top N placements and not
  int size = 0; // Tracking manually is cheaper than doing the O(n) operation each iteration

  // Get the placements of the first piece
  vector<SimState> firstLockPlacements;
  moveSearch(gameState, firstPiece, firstLockPlacements);
  for (auto it = begin(firstLockPlacements); it != end(firstLockPlacements); ++it) {
    SimState firstPlacement = *it;
    GameState afterFirstMove = advanceGameState(gameState, firstPlacement, DEBUG_CONTEXT);
    float firstMoveReward = getLineClearFactor(afterFirstMove.lines - gameState.lines, DEBUG_WEIGHTS);

    // Get the placements of the second piece
    vector<SimState> secondLockPlacements;
    moveSearch(afterFirstMove, secondPiece, secondLockPlacements);

    for (auto secondPlacement : secondLockPlacements) {
      GameState resultingState = advanceGameState(afterFirstMove, secondPlacement, DEBUG_CONTEXT);
      float evalScore = firstMoveReward + fastEval(afterFirstMove, resultingState, secondPlacement, DEBUG_CONTEXT, DEBUG_WEIGHTS);

      Depth2Possibility newPossibility = {
        { firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex },
        { secondPlacement.x, secondPlacement.y, secondPlacement.rotationIndex },
        resultingState,
        evalScore
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
