#ifndef POSITION_LOGGING
#define POSITION_LOGGING

#include <fstream>
#include <mutex>
#include "../cpp_modules/src/types.hpp"
#include "../cpp_modules/src/utils.hpp"
#include "../cpp_modules/src/formatting.hpp"
#include "../cpp_modules/src/eval.hpp"
#include "../cpp_modules/src/eval_context.hpp"
#include "../cpp_modules/src/move_result.hpp"
#include "../cpp_modules/src/config.hpp"

/**
 * Benchmark position logging.
 *
 * Writes one JSON line per position encountered in a simulated game, for use as a
 * benchmark of engine positions. The first line records the run configuration (the
 * input timeline is required to reconstruct the eval context of a position).
 *
 * NB: PIECE_LIST indices are I=0 O=1 L=2 J=3 T=4 S=5 Z=6, and the AiMode enum order
 * is STANDARD DIG LINEOUT NEAR_KILLSCREEN DIRTY_NEAR_KILLSCREEN.
 */

const char *POSITION_LOG_PATH = "src/benchmark/position_log.jsonl"; // Relative to the repo root, where the binary is built and run from

std::ofstream positionLogFile;
std::mutex positionLogMutex;

void openPositionLogFile(const char *path, const char *inputFrameTimeline, int startingLevel, int maxLines) {
  positionLogFile.open(path, std::ios::out | std::ios::trunc);
  if (!positionLogFile.is_open()) {
    printf("Error: could not open position log file: %s\n", path);
    return;
  }
  printf("Logging positions to %s\n", path);
  positionLogFile << "{\"type\":\"header\",\"timeline\":\"" << inputFrameTimeline << "\",\"startingLevel\":" << startingLevel << ",\"maxLines\":" << maxLines << "}\n";
  positionLogFile.flush();
}

void closePositionLogFile() {
  if (positionLogFile.is_open()) {
    positionLogFile.close();
  }
}

/** Builds a standardized copy of a position for benchmark purposes: recomputes the surface
 * and holes from scratch the same way mainProcess does for a fresh request (well column 9,
 * non-dig mode), so the results are reproducible from just the stored board and level.
 * The original gameState is untouched, since this recomputation mutates the board's
 * auxiliary bits. */
GameState getStandardizedPositionState(const GameState &gameState) {
  GameState evalState = gameState;
  getSurfaceArray(evalState.board, evalState.surfaceArray);
  std::pair<int, float> holeResult = updateSurfaceAndHoles(evalState.surfaceArray, evalState.board, /* excludeHolesColumn= */ WELL_COLUMN, /* isDigMode= */ false);
  evalState.numTrueHoles = holeResult.first;
  evalState.numPartialHoles = holeResult.second;
  return evalState;
}

/** Gets the static eval of a standardized position, forced into STANDARD mode
 * (ignoring DIG, LINEOUT, etc). */
float getShallowStandardEval(const GameState &standardizedState, const PieceRangeContext pieceRangeContextLookup[]) {
  const EvalContext evalContext = getEvalContext(standardizedState, pieceRangeContextLookup, /* useTestWeights= */ false, SAFE, /* forceStandardMode= */ true);
  LockPlacement nullPlacement = {0, 0, 0, -1, NO_TUCK_NOTATION, &PIECE_LIST[0]};
  return fastEval(standardizedState, standardizedState, nullPlacement, &evalContext);
}

/** Logs one position (the state before a piece placement) to the position log file. */
void logPosition(int gameId, int moveIndex, const GameState &gameState, const Piece *curPiece, const Piece *nextPiece, AiMode actualAiMode, const PieceRangeContext pieceRangeContextLookup[]) {
  if (!POSITION_LOGGING_ENABLED || !positionLogFile.is_open()) {
    return;
  }

  GameState standardizedState = getStandardizedPositionState(gameState);
  float shallowEval = getShallowStandardEval(standardizedState, pieceRangeContextLookup);

  char lineBuffer[400];
  snprintf(lineBuffer, 400,
    "{\"gameId\":%d,\"moveIndex\":%d,\"board\":\"%s\",\"level\":%d,\"lines\":%d,\"curPiece\":%d,\"nextPiece\":%d,\"aiMode\":%d,\"holes\":%d,\"partialHoles\":%.2f,\"shallowEval\":%.4f}\n",
    gameId,
    moveIndex,
    formatBoard(gameState.board).c_str(),
    gameState.level,
    gameState.lines,
    curPiece->index,
    nextPiece->index,
    (int) actualAiMode,
    standardizedState.numTrueHoles,
    standardizedState.numPartialHoles,
    shallowEval);

  std::lock_guard<std::mutex> lock(positionLogMutex);
  positionLogFile << lineBuffer;
  positionLogFile.flush();
}

#endif
