#include "../include/high_level_search.h"


/** Searches 2-ply from a starting state, and performs a fast eval on each of the resulting states. Maintains a sorted list of the top N possibilities, and adds all the rest onto the end in no specified order. */
int searchDepth2(GameState gameState, Piece firstPiece, Piece secondPiece, int keepTopN, OUT list<Depth2Possibility> &possibilityList){
  auto cutoffPossibility = possibilityList.begin(); // The node on the "cutoff" between being in the top N placements and not
  int size = 0; // Tracking manually is cheaper than doing the O(n) operation each iteration

  // Get the placements of the first piece
  vector<SimState> firstLockPlacements;
  moveSearch(gameState, firstPiece, firstLockPlacements);

  for (auto firstPlacement : firstLockPlacements) {
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
