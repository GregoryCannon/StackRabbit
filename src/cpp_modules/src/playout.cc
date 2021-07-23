#include "../include/playout.h"
#include "../include/eval.h"

const FastEvalWeights DEBUG_WEIGHTS = {
    /* avgHeight= */ -1,
    /* burnCoef= */ -5,
    0,
    0,
    /* holeCoef= */ -30,
    /* tetrisCoef= */ 40,
    0,
    /* surface= */ 1};
const EvalContext DEBUG_CONTEXT = {/* scareHeight= */ 5, /* wellColumn= */ 9, /* countWellHoles= */ false};

SimState pickLockPlacement(GameState gameState,
                           EvalContext evalContext,
                           FastEvalWeights evalWeights,
                           OUT std::vector<SimState> &lockPlacements) {
  float bestSoFar = -99999999.0F;
  SimState bestPlacement = {};
  for (auto lockPlacement : lockPlacements) {
    float evalScore = fastEval(gameState, lockPlacement, evalContext, evalWeights);
    if (evalScore > bestSoFar) {
      bestSoFar = evalScore;
      bestPlacement = lockPlacement;
    }
  }
  maybePrint("\nBest placement: %d %d\n", bestPlacement.rotationIndex, bestPlacement.x - SPAWN_X);
  return bestPlacement;
}

void playSequence(GameState gameState, int pieceSequence[10]) {
  for (int i = 0; i < 10; i++) {
    std::vector<SimState> lockPlacements;

    Piece piece = PIECE_LIST[pieceSequence[i]];
    moveSearch(gameState, piece, lockPlacements);
    SimState bestMove = pickLockPlacement(gameState, DEBUG_CONTEXT, DEBUG_WEIGHTS, lockPlacements);

    // Update the state
    gameState = advanceGameState(gameState, bestMove, DEBUG_CONTEXT);

    maybePrint("\nBest placement: %d %d\n\n\n\n\n\n\n\n", bestMove.rotationIndex, bestMove.x - SPAWN_X);
  }
}