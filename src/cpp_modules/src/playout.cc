#include "../include/playout.h"
#include "../include/eval.h"

const FastEvalWeights DEBUG_WEIGHTS = {
    /* avgHeight: */ -1, /* burnCoef: */ -5, 0, 0, 0, /* tetrisCoef: */ 40, 0, /* surface: */ 1};
const EvalContext DEBUG_CONTEXT = {/* scareHeight: */ 5};

SimState pickLockPlacement(int board[20], int surfaceArray[10], OUT std::vector<SimState> &lockPlacements) {
  float bestSoFar = -99999999.0F;
  SimState bestPlacement = {};
  for (auto lockPlacement : lockPlacements) {
    float evalRating = fastEval(board, surfaceArray, lockPlacement, 9, DEBUG_WEIGHTS);
    if (evalRating > bestSoFar) {
      bestSoFar = evalRating;
      bestPlacement = lockPlacement;
    }
  }
  maybePrint("\nBest placement: %d %d\n", bestPlacement.rotationIndex, bestPlacement.x - SPAWN_X);
  return bestPlacement;
}