#include "../include/eval.h"
#include "../include/utils.h"

#include <math.h>
#include <vector>
// #include "data/ranksOutput.cc"
using namespace std;

const int USE_RANKS = 0;
const int WELL_COLUMN = 9;

/**
 * A crude way to evaluate a surface for when I'm debugging and don't want to load the surfaces every time I
 * run.
 */
float calculateFlatness(int surfaceArray[10]) {
  float score = 30;
  for (int i = 0; i < 8; i++) {
    int diff = surfaceArray[i + 1] - surfaceArray[i];
    if (diff != 0) {
      score -= pow(abs(diff), 2.5);
    }
  }
  return score;
}

/** Gets the value of a surface. */
float rateSurface(int surfaceArray[10]) {
  // Backup option in case ranks aren't loaded
  if (!USE_RANKS) {
    return calculateFlatness(surfaceArray);
  }
  // Convert the surface array into the custom base-9 encoding
  int index = 0;
  for (int i = 0; i < 8; i++) {
    int diff = surfaceArray[i + 1] - surfaceArray[i];
    index *= 9;
    index += diff + 3;
  }
  return 1.0; // So it compiles without the ranks
  // return surfaceRanksRaw[index] * 0.1;
}

float getAverageHeightFactor(int surfaceArray[10], int wellColumn) {
  float totalHeight = 0;
  float weight = wellColumn >= 0 ? 0.1 : 0.111111;
  for (int i = 0; i < 10; i++) {
    if (i == wellColumn) {
      continue;
    }
    totalHeight += surfaceArray[i] * weight;
  }
  return totalHeight;
}

void getNewSurface(int surfaceArray[10], SimState lockPlacement, OUT int newSurface[10]) {
  for (int i = 0; i < 10; i++) {
    newSurface[i] = surfaceArray[i];
  }
  int *topSurface = lockPlacement.piece.topSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (topSurface[i] != -1) {
      newSurface[lockPlacement.x + i] = 20 - topSurface[i] - lockPlacement.y;
    }
  }
}

void updateSurfaceAfterLineClears(int surfaceArray[10], int board[20], int numLinesCleared) {
  for (int c = 0; c < 10; c++){
    int mask = 1 << (9 - c);
    int r = 20 - surfaceArray[c];
    while (r < 20 && !(board[r] & mask)){
      r++;
    }
    surfaceArray[c] = 20 - r;
  }
}

/**
 * Calculates the resulting board after placing a piece in a specified spot.
 * @returns the number of lines cleared
 */
int getNewBoardAndLinesCleared(int board[20], SimState lockPlacement, OUT int newBoard[20]) {
  int numLinesCleared = 0;
  // The rows below the piece are always the same
  for (int r = lockPlacement.y + 4; r < 20; r++) {
    newBoard[r] = board[r];
  }
  // Check the piece rows, bottom to top
  int *pieceRows = lockPlacement.piece.rowsByRotation[lockPlacement.rotationIndex];
  for (int i = 3; i >= 0; i--) {
    if (pieceRows[i] == 0) {
      newBoard[lockPlacement.y + i + numLinesCleared] = board[lockPlacement.y + i];
      continue;
    }
    int newRow = board[lockPlacement.y + i] | (SHIFTBY(pieceRows[i], lockPlacement.x));
    if (newRow == FULL_ROW) {
      numLinesCleared++;
      continue;
    }
    newBoard[lockPlacement.y + i + numLinesCleared] = newRow;
  }
  // Copy the rest of the rows
  for (int r = 0; r < lockPlacement.y; r++) {
    newBoard[r + numLinesCleared] = board[r];
  }
  // Add empty lines at the top of the board if needed
  for (int i = 0; i < numLinesCleared; i++) {
    newBoard[i] = 0;
  }
  return numLinesCleared;
}

float fastEval(
    int board[20], int surfaceArray[10], SimState lockPlacement, int wellColumn, FastEvalWeights weights) {
  maybePrint("\nEvaluating possibility %d %d %d\n",
         lockPlacement.rotationIndex,
         lockPlacement.x - SPAWN_X,
         lockPlacement.y);

  int newBoard[20];
  int numLinesCleared = getNewBoardAndLinesCleared(board, lockPlacement, newBoard);
  if (LOGGING_ENABLED){
    printBoard(newBoard);
  }

  int newSurface[10];
  getNewSurface(surfaceArray, lockPlacement, newSurface);
  if (numLinesCleared > 0){
    updateSurfaceAfterLineClears(newSurface, newBoard, numLinesCleared);
  }

  for (int i = 0; i < 9; i++) {
  maybePrint("%d ", newSurface[i]);
  }
  maybePrint("%d\n", newSurface[9]);

  float surfaceFactor = weights.surfaceCoef * rateSurface(newSurface);
  float avgHeightFactor = weights.avgHeightCoef * getAverageHeightFactor(newSurface, wellColumn);
  float lineClearFactor = numLinesCleared == 4 ? weights.tetrisCoef : weights.burnCoef * numLinesCleared;

  float total = surfaceFactor + avgHeightFactor + lineClearFactor;

  maybePrint("Surface %01f, AvgHeight %01f, LineClear %01f\t Total: %01f\n",
         surfaceFactor,
         avgHeightFactor,
         lineClearFactor,
         total);

  return total;
}
