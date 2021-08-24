#include "../include/playout.h"
#include "../include/eval.h"
#include "../include/utils.h"
#include "../include/params.h"
#include "../data/canonical_sequences.h"

using namespace std;

/** Selects the highest value lock placement using the fast eval function. */
SimState pickLockPlacement(GameState gameState,
                           EvalContext const *evalContext,
                           OUT vector<SimState> &lockPlacements) {
  float bestSoFar = evalContext->weights.deathCoef;
  SimState bestPlacement = {};
  for (auto lockPlacement : lockPlacements) {
    GameState newState = advanceGameState(gameState, lockPlacement, evalContext);
    float evalScore = fastEval(gameState, newState, lockPlacement, evalContext);
    if (evalScore > bestSoFar) {
      bestSoFar = evalScore;
      bestPlacement = lockPlacement;
    }
  }
  maybePrint("\nBest placement: %d %d\n", bestPlacement.rotationIndex, bestPlacement.x - SPAWN_X);
  return bestPlacement;
}


/**
 * Plays out a starting state 10 moves into the future.
 * @returns the total value of the playout (intermediate rewards + eval of the final board)
 */
float playSequence(GameState gameState, char const *inputFrameTimeline, const int pieceSequence[SEQUENCE_LENGTH], int playoutLength) {
  float totalReward = 0;
  for (int i = 0; i < playoutLength; i++) {
    // Figure out modes and eval context
    const EvalContext evalContextRaw = getEvalContext(gameState, inputFrameTimeline);
    const EvalContext *evalContext = &evalContextRaw;
    FastEvalWeights weights = getWeights(evalContext->aiMode);

    // Get the lock placements
    std::vector<SimState> lockPlacements;
    Piece piece = PIECE_LIST[pieceSequence[i]];
    moveSearch(gameState, piece, evalContext->inputFrameTimeline, lockPlacements);

    if (lockPlacements.size() == 0) {
      return weights.deathCoef;
    }

    // Pick the best placement
    SimState bestMove = pickLockPlacement(gameState, evalContext, lockPlacements);

    // On the last move, do a final evaluation
    if (i == playoutLength - 1) {
      GameState nextState = advanceGameState(gameState, bestMove, evalContext);
      float evalScore = fastEval(gameState, nextState, bestMove, evalContext);
      if (PLAYOUT_LOGGING_ENABLED) {
        gameState = nextState;
        printBoard(gameState.board);
        printf("Best placement: %c %d, %d\n\n", bestMove.piece.id, bestMove.rotationIndex, bestMove.x - SPAWN_X);
        printf("Cumulative reward: %01f\n", totalReward);
        printf("Final eval score: %01f\n", evalScore);
      }
      return totalReward + evalScore;
    }

    // Otherwise, update the state to keep playing
    int oldLines = gameState.lines;
    gameState = advanceGameState(gameState, bestMove, evalContext);
    FastEvalWeights rewardWeights = evalContext->aiMode == DIG ? getWeights(STANDARD) : weights; // When the AI is digging, still deduct from the overall value of the sequence at standard levels
    totalReward += getLineClearFactor(gameState.lines - oldLines, rewardWeights);
    if (PLAYOUT_LOGGING_ENABLED) {
      printBoard(gameState.board);
      printf("Best placement: %c %d, %d\n\n", bestMove.piece.id, bestMove.rotationIndex, bestMove.x - SPAWN_X);
    }
  }
  return -1; // Doesn't reach here, always returns from i == 9 case
}


float getPlayoutScore(GameState gameState, char const *inputFrameTimeline, int numPlayouts, int playoutLength){
  float totalScore = 0;
  for (int i = 0; i < numPlayouts; i++) {
    // Do one playout
    const int *pieceSequence = canonicalPieceSequences + i * SEQUENCE_LENGTH; // Index into the mega array of piece sequences;
    float playoutScore = playSequence(gameState, inputFrameTimeline, pieceSequence, playoutLength);
    totalScore += playoutScore;
  }
  return totalScore / numPlayouts;
}
