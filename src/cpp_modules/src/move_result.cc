#include "../include/move_result.h"

int getNewSurfaceAndNumNewHoles(int surfaceArray[10],
                                SimState lockPlacement,
                                EvalContext evalContext,
                                OUT int newSurface[10]) {
  for (int i = 0; i < 10; i++) {
    newSurface[i] = surfaceArray[i];
  }
  // Check for new holes by comparing the bottom surface of the piece to the surface of the stack
  int numNewHoles = 0;
  int *bottomSurface = lockPlacement.piece.bottomSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (bottomSurface[i] == -1) {
      continue;
    }
    // Maybe skip well holes
    if (!evalContext.countWellHoles && lockPlacement.x + i == evalContext.wellColumn) {
      continue;
    }
    // Add a hole for each cell of difference between the bottom of the piece and the stack
    int diff = (20 - bottomSurface[i] - lockPlacement.y) - surfaceArray[lockPlacement.x + i];
    numNewHoles += diff;
  }
  // Calculate the new overall surface by superimposing the piece's top surface on the existing surface
  int *topSurface = lockPlacement.piece.topSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (topSurface[i] != -1) {
      newSurface[lockPlacement.x + i] = 20 - topSurface[i] - lockPlacement.y;
    }
  }
  return numNewHoles;
}

/**
 * Manually finds the surface heights and holes after lines have been cleared (since usual prediction tricks
 * don't apply).
 * @returns the new hole count
 */
int updateSurfaceAndHolesAfterLineClears(int surfaceArray[10], int board[20], EvalContext evalContext) {
  int numHoles = 0;
  for (int c = 0; c < 10; c++) {
    int mask = 1 << (9 - c);
    int r = 20 - surfaceArray[c];
    while (r < 20 && !(board[r] & mask)) {
      r++;
    }
    // Update the new surface array
    surfaceArray[c] = 20 - r;
    while (r < 20) {
      // Add new holes to the overall count, unless they're in the well
      if (!(board[r] & mask) && (evalContext.countWellHoles || c != evalContext.wellColumn)) {
        numHoles++;
      }
      r++;
    }
  }
  return numHoles;
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

/** Gets the game state after completing a given move */
GameState advanceGameState(GameState gameState, SimState lockPlacement, EvalContext evalContext) {
  GameState newState = {{}, {}, gameState.adjustedNumHoles, gameState.lines};

  int numLinesCleared = getNewBoardAndLinesCleared(gameState.board, lockPlacement, newState.board);
  newState.lines += numLinesCleared;

  int numNewHoles =
      getNewSurfaceAndNumNewHoles(gameState.surfaceArray, lockPlacement, evalContext, newState.surfaceArray);
  if (numLinesCleared > 0) {
    newState.adjustedNumHoles =
        updateSurfaceAndHolesAfterLineClears(newState.surfaceArray, newState.board, evalContext);
  } else {
    newState.adjustedNumHoles += numNewHoles;
  }

  return newState;
}
