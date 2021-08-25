#include "../include/move_result.h"
#include <stdexcept>


float getAdjustedHoleRating(int board[20], int r, int c){
  // Check if it's a tuck setup
  if (
    (c >= 2 && ((board[r] >> (9-c)) & 7) == 0) ||   // left side tuck (2 cells of open space)
    (c <= 7 && ((board[r] >> (7-c)) & 7) == 0)      // right side tuck (2 cells of open space)
    ) {
    // printf("MARKING TUCK SETUP %d %d, %d\n", c, r, board[r] >> 20);
    board[r] |= TUCK_SETUP_BIT(c); // Mark this cell as an overhang cell
    // printf("After mark: %d\n", board[r] >> 20);
    return TUCK_SETUP_HOLE_PROPORTION;
  }
  // Otherwise it's a hole
  // printf("MARKING HOLE %d %d\n", c, r);
  board[r] |= HOLE_BIT(c);
  return 1;
}


float getNewSurfaceAndNumNewHoles(int surfaceArray[10],
                                  int board[20],
                                  SimState lockPlacement,
                                  const EvalContext *evalContext,
                                  OUT int newSurface[10]) {
  for (int i = 0; i < 10; i++) {
    newSurface[i] = surfaceArray[i];
  }

  // Calculate the new overall surface by superimposing the piece's top surface on the existing surface
  int *topSurface = lockPlacement.piece.topSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (topSurface[i] != -1) {
      newSurface[lockPlacement.x + i] = 20 - topSurface[i] - lockPlacement.y;
    }
  }

  // Check for new holes by comparing the bottom surface of the piece to the surface of the stack
  float numNewHoles = 0;
  int *bottomSurface = lockPlacement.piece.bottomSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (bottomSurface[i] == -1) {
      continue;
    }
    // Maybe skip well holes
    if (!evalContext->countWellHoles && lockPlacement.x + i == evalContext->wellColumn) {
      continue;
    }

    // Loop through the cells between the bottom of the piece and the ground
    int c = lockPlacement.x + i;
    int highestCellInCol = 20 - surfaceArray[c];
    int lowestHoleInCol = -1;
    for (int r = (lockPlacement.y + bottomSurface[i]); r < highestCellInCol; r++) {
      float rating = getAdjustedHoleRating(board, r, c);
      if (rating != TUCK_SETUP_HOLE_PROPORTION) {
        lowestHoleInCol = r;
      }
      numNewHoles += rating;
    }
    if (board[highestCellInCol] & NEED_TO_CLEAR_BIT) {
      lowestHoleInCol = highestCellInCol - 1;
    }
    // Mark rows as needing to be cleared
    maybePrint("lowestHoleInCol %d %d, surf %d\n", c, lowestHoleInCol - 1, 20 - newSurface[c]);
    for (int r = lowestHoleInCol - 1; r >= 20 - newSurface[c]; r--) {
      if (!(board[r] & HOLE_BIT(c))) {
        board[r] |= NEED_TO_CLEAR_BIT;
      }
    }
  }

  return numNewHoles;
}

/**
 * Manually finds the surface heights and holes after lines have been cleared (since usual prediction tricks
 * don't apply).
 * @returns the new hole count
 */
float updateSurfaceAndHolesAfterLineClears(int surfaceArray[10], int board[20], int excludeHolesColumn) {
  // Reset hole and tuck setup bits
  for (int i = 0; i < 20; i++) {
    board[i] &= ~ALL_AUXILIARY_BITS;
  }
  float numHoles = 0;
  for (int c = 0; c < 10; c++) {
    int mask = 1 << (9 - c);
    int r = 20 - surfaceArray[c];
    while (r < 20 && !(board[r] & mask)) {
      r++;
    }
    // Update the new surface array
    surfaceArray[c] = 20 - r;

    int lowestHoleInCol = -1;
    while (r < 20) {
      // Add new holes to the overall count, unless they're in the well
      if (!(board[r] & mask) && c != excludeHolesColumn) {
        float rating = getAdjustedHoleRating(board, r, c);
        if (rating > TUCK_SETUP_HOLE_PROPORTION + __FLT_EPSILON__) {
          lowestHoleInCol = r;
        }
        numHoles += rating;
      }
      r++;
    }
    // Mark rows as needing to be cleared
    for (int r = lowestHoleInCol - 1; r >= 20 - surfaceArray[c]; r--) {
      board[r] |= NEED_TO_CLEAR_BIT;
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
    // Don't add any minos off the board
    if (lockPlacement.y + i < 0) {
      continue;
    }

    if (pieceRows[i] == 0) {
      newBoard[lockPlacement.y + i + numLinesCleared] = board[lockPlacement.y + i];
      continue;
    }
    int newRow = (board[lockPlacement.y + i]
                  | SHIFTBY(pieceRows[i], lockPlacement.x))      // Add the piece to the board
                 & ~(SHIFTBY(pieceRows[i], lockPlacement.x - 20)); // Clear out those cells from tuck setups
    if ((newRow & FULL_ROW) == FULL_ROW) {
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


float adjustHoleCountAndBoardAfterTuck(int board[20], SimState lockPlacement){
  int tuckCellsFilled = 0;
  int *pieceRows = lockPlacement.piece.rowsByRotation[lockPlacement.rotationIndex];
  for (int i = 3; i >= 0; i--) {
    // Don't add any minos off the board
    if (lockPlacement.y + i < 0) {
      continue;;
    }

    if (pieceRows[i] == 0) {
      continue;
    }
    // Count the number of tuck cells filled in this row of the piece
    int intersection = board[lockPlacement.y + i] & SHIFTBY(pieceRows[i], lockPlacement.x - 20);
    while ((intersection & ALL_TUCK_SETUP_BITS) > 0) {
      if (intersection & (1 << 20)) {
        tuckCellsFilled++;
      }
      intersection = intersection >> 1;
    }
  }
  return -1 * TUCK_SETUP_HOLE_PROPORTION * tuckCellsFilled;
}


/** Gets the game state after completing a given move */
GameState advanceGameState(GameState gameState, SimState lockPlacement, const EvalContext *evalContext) {
  GameState newState = {{}, {}, gameState.adjustedNumHoles, gameState.lines, gameState.level};
  float numNewHoles = 0;
  // Post-process after tucks
  // This has to happen before getNewBoardAndLinesCleared, which updates the tuck cell bits
  if (lockPlacement.frameIndex == -1) {        // -1 frame index is an artifact of tucks
    numNewHoles += adjustHoleCountAndBoardAfterTuck(gameState.board, lockPlacement);
  }
  int numLinesCleared = getNewBoardAndLinesCleared(gameState.board, lockPlacement, newState.board);
  numNewHoles +=
    getNewSurfaceAndNumNewHoles(gameState.surfaceArray, newState.board, lockPlacement, evalContext, newState.surfaceArray);
  // Post-process after line clears
  if (numLinesCleared > 0) {
    newState.adjustedNumHoles =
      updateSurfaceAndHolesAfterLineClears(newState.surfaceArray, newState.board, evalContext->countWellHoles ? -1 : evalContext->wellColumn);
  } else {
    newState.adjustedNumHoles += numNewHoles;
  }

  newState.lines += numLinesCleared;
  newState.level = getLevelAfterLineClears(gameState.level, gameState.lines, numLinesCleared);

  return newState;
}
