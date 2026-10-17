#include "playout.hpp"
#include "eval.hpp"
#include "utils.hpp"
#include "params.hpp"
#include <algorithm>
#include "../data/canonical_sequences.hpp"

using namespace std;

/** Selects the highest value lock placement using the fast eval function. */
LockPlacement pickLockPlacement(GameState gameState,
                                const EvalContext *evalContext,
                                OUT vector<LockPlacement> &lockPlacements) {
  float bestSoFar = evalContext->weights.deathCoef - 1;
  LockPlacement bestPlacement = {};
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
 * Plays out a starting state N moves into the future.
 * @returns the total value of the playout (intermediate rewards + eval of the final board)
 */
float playSequence(GameState gameState, const PieceRangeContext pieceRangeContextLookup[3], const int pieceSequence[SEQUENCE_LENGTH], int playoutLength, OUT vector<PlayoutData> *playoutDataList) {
  // Note down the original AI mode to prevent the AI from putting itself in alternate modes to affect the valuations
  AiMode originalAiMode = getEvalContext(gameState, pieceRangeContextLookup, /* useTestWeights= */ false).aiMode;
  
  PlayoutData newPlayoutData;
  const bool trackPlayouts = playoutDataList != NULL;

  float totalReward = 0;
  for (int i = 0; i < playoutLength; i++) {
    // Figure out modes and eval context
    const EvalContext evalContextRaw = getEvalContext(gameState, pieceRangeContextLookup, /* useTestWeights= */ false);
    const EvalContext *evalContext = &evalContextRaw;
    FastEvalWeights weights = getWeights(evalContext->aiMode, /* useTestWeights= */ false);

    // Get the lock placements
    std::vector<LockPlacement> lockPlacements;
    Piece piece = PIECE_LIST[pieceSequence[i]];
    moveSearch(gameState, &piece, evalContext->pieceRangeContext.inputFrameTimeline, lockPlacements);

    if (lockPlacements.size() == 0) {
      return weights.deathCoef;
    }

    // Pick the best placement
    LockPlacement bestMove = pickLockPlacement(gameState, evalContext, lockPlacements);
    if (trackPlayouts){
      LockLocation bestMoveLocation = { bestMove.x, bestMove.y, bestMove.rotationIndex };
      newPlayoutData.pieceSequence += getPieceChar(piece.index);
      newPlayoutData.placements.push_back(bestMoveLocation);
    }

    // On the last move, do a final evaluation
    if (i == playoutLength - 1) {
      GameState nextState = advanceGameState(gameState, bestMove, evalContext);
      
      if (SHOULD_PLAY_PERFECT){
        float eval = evalForPerfectPlay(gameState, nextState, bestMove, evalContext);
        if (trackPlayouts){
          newPlayoutData.totalScore = totalReward + eval;
          copyBoard(nextState.board, newPlayoutData.resultingBoard);
          insertIntoList(newPlayoutData, playoutDataList);
        }
        return eval;
      }
      
      // In some contexts, override the current aiMode such that the end of a playout is always compared fairly against other playouts
      EvalContext contextRaw = *evalContext;
      if (originalAiMode == DIG || originalAiMode == STANDARD){
        contextRaw.aiMode = originalAiMode;
      }
      float evalScore = fastEval(gameState, nextState, bestMove, &contextRaw);
      if (PLAYOUT_LOGGING_ENABLED) {
        gameState = nextState;
        printBoard(gameState.board);
        printf("Best placement: %c %d, %d\n\n", bestMove.piece->id, bestMove.rotationIndex, bestMove.x - SPAWN_X);
        printf("Cumulative reward: %01f\n", totalReward);
        printf("Final eval score: %01f\n", evalScore);
        printf("*** TOTAL= %f ***\n", totalReward + evalScore);
      }
      if (trackPlayouts) {
        newPlayoutData.totalScore = totalReward + evalScore;
        copyBoard(nextState.board, newPlayoutData.resultingBoard);
        insertIntoList(newPlayoutData, playoutDataList);
      }
      return totalReward + evalScore;
    }

    // Otherwise, update the state to keep playing
    int oldLines = gameState.lines;
    gameState = advanceGameState(gameState, bestMove, evalContext);
    
    if (SHOULD_PLAY_PERFECT){
      if ((gameState.lines - oldLines) % 4 != 0){
        return 0; // 0% chance of continuing perfect
      }
    } else {
      FastEvalWeights rewardWeights = evalContext->aiMode == DIG ? getWeights(STANDARD, /* useTestWeights= */ false) : weights; // When the AI is digging, still deduct from the overall value of the sequence at standard amounts
      totalReward += getLineClearFactor(gameState.lines - oldLines, rewardWeights, evalContext->shouldRewardLineClears);
      if (PLAYOUT_LOGGING_ENABLED) {
        printBoard(gameState.board);
        printf("Best placement: %c %d, %d\n\n", bestMove.piece->id, bestMove.rotationIndex, bestMove.x - SPAWN_X);
      }
    }
  }
  return -1; // Doesn't reach here, always returns from i == 9 case
}


float getPlayoutScore(GameState gameState, int playoutCount, int playoutLength, const PieceRangeContext pieceRangeContextLookup[3], int firstPieceIndex, OUT vector<PlayoutData> *playoutDataList){
  // // Don't perform playouts if logging is enabled
  // if (LOGGING_ENABLED) {
  //   return 0;
  // }

  // Clamp playout counts to the batch size. Historically, counts over the old 1000-sequence
  // batches read past the end of the sequence data and corrupted memory (e.g. playouts>3000
  // with a T last-piece crashed the wasm build); the clamp makes large requests deterministic.
  playoutCount = std::min(playoutCount, CANONICAL_SEQUENCE_BATCH_SIZE);

  // Sequence batching based on the last known piece given by the in-game randomizer.
  // See canonical_sequences.hpp for the batch layout and the online-validated NES table.
  const int *lastPieceSequences = firstPieceIndex < 0
    ? correctedSequencesNoLastPieceKnown
    : canonicalPieceSequencesForLastPiece[firstPieceIndex];
	
  // Special case: if the playout count is equal to the full count of possible sequences at the requested length, use the exahustive sequence list,
  // as opposed to randomly generated ones.
  bool useExhaustiveSequences = (playoutCount == 7 && playoutLength == 1)
    || (playoutCount == 49 && playoutLength == 2)
    || (playoutCount == 343 && playoutLength == 3)
    || (playoutCount == 2401 && playoutLength == 4);

  float playoutScore = 0;
  vector<float> playoutScores;
  for (int i = 0; i < playoutCount; i++) {
    // Do one playout
    const int *pieceSequence = useExhaustiveSequences 
          ? exhaustivePieceSequences + i * EXHAUSTIVE_SEQUENCE_LENGTH // Index into the exhaustive list of possible sequences;
          : lastPieceSequences + i * SEQUENCE_LENGTH; // Index into the batch for this last-known piece;
    float resultScore = playSequence(gameState, pieceRangeContextLookup, pieceSequence, playoutLength, playoutDataList);
    playoutScores.push_back(resultScore);
    playoutScore += resultScore;
  }

  if (PLAYOUT_RESULT_LOGGING_ENABLED) {
    float mean = playoutScore / playoutCount;
    printf("PlayoutScore %.1f\n", mean);

    if (playoutCount > 1) {
      // Calculate standard deviation
      float variance = 0;
      for (int i = 0; i < playoutCount; i++) {
        float diff = playoutScores[i] - mean;
        variance += diff * diff;
      }
      variance /= (playoutCount - 1);
      float stdDev = sqrt(variance);

      // Calculate 95% confidence interval (using z=1.96 for large samples, or t-distribution approximation for small samples)
      float tValue = 1.96; // Default for large samples
      float marginOfError = tValue * (stdDev / sqrt(playoutCount));

      printf("Standard Deviation: %.2f\n", stdDev);
      printf("95%% Confidence Interval: [%.2f, %.2f]\n", mean - marginOfError, mean + marginOfError);
    }
  }
  return playoutCount == 0 ? 0 : (playoutScore / playoutCount);
}
