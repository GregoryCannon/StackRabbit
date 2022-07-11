#include "move_result.hpp"
#include <stdexcept>

/**
 * Rates a hole from 0 to 1 based on how bad it is.
 * --Side effect-- marks the hole or tuck setup in the board data structure
 */
float analyzeHole(unsigned int board[20], int r, int c){
  // VARIABLE_RANGE_CHECKS_ENABLED
  if (true && (r < 0 || r >= 20)){
    printf("PANIK, r=%d\n", r);
  }
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
                                  unsigned int board[20],
                                  LockPlacement lockPlacement,
                                  const EvalContext *evalContext,
                                  int isTuck,
                                  OUT int newSurface[10]) {
  for (int i = 0; i < 10; i++) {
    newSurface[i] = surfaceArray[i];
  }

  // Calculate the new overall surface by superimposing the piece's top surface on the existing surface
  unsigned int const *topSurface = lockPlacement.piece->topSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (topSurface[i] != NONE) {
      newSurface[lockPlacement.x + i] = 20 - topSurface[i] - lockPlacement.y;
    }
  }
  
  // Adjust for the fact that tucks can place a piece UNDER the existing surface
  if (isTuck){
    for (int i = 0; i < 10; i++){
      newSurface[i] = std::max(surfaceArray[i], newSurface[i]);
      if (newSurface[i] > 20){
        newSurface[i] = 20;
      }
    }
  }

  // Check for new holes by comparing the bottom surface of the piece to the surface of the stack
  float numNewHoles = 0;
  unsigned int const *bottomSurface = lockPlacement.piece->bottomSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (bottomSurface[i] == NONE) {
      continue;
    }
    // Maybe skip well holes
    if (!evalContext->countWellHoles && lockPlacement.x + i == evalContext->wellColumn) {
      continue;
    }

    // Loop through the cells between the bottom of the piece and the ground
    int c = lockPlacement.x + i;
    const int highestRowInCol = 20 - surfaceArray[c];
    int holeWeightStartRow = -1; // Indicates that all rows above this row are weight on a hole
    for (int r = (lockPlacement.y + bottomSurface[i]); r < highestRowInCol; r++) {
      // VARIABLE_RANGE_CHECKS
      if (r < 0 || r >= 20){
        continue;
      }
      float rating = analyzeHole(board, r, c);
      if (std::abs(rating - TUCK_SETUP_HOLE_PROPORTION) > FLOAT_EPSILON) { // If it's NOT a tuck setup
        holeWeightStartRow = r - 1;
      }
      numNewHoles += rating;
    }
    // If placing a piece on top of a row that's already weighing on a hole, then the new piece is adding weight to that
    if (highestRowInCol < 20 && board[highestRowInCol] & HOLE_WEIGHT_BIT) {
      holeWeightStartRow = highestRowInCol - 1;
    }
    // Mark rows as needing to be cleared
//    maybePrint("marking needToClear (column %d): start row = %d, surface = %d\n", c, holeWeightStartRow, 20 - newSurface[c]);
    if (holeWeightStartRow != -1){
      for (int r = holeWeightStartRow; r >= 20 - newSurface[c]; r--) {
        if (VARIABLE_RANGE_CHECKS_ENABLED && (r < 0 || r >= 20)){
          printf("R value out of range %d %d\n", r, newSurface[c]);
          throw std::invalid_argument( "r value out of range" );
        }
        if (!(board[r] & HOLE_BIT(c))) {
          board[r] |= HOLE_WEIGHT_BIT;
        }
      }
    }
  }

  return numNewHoles;
}

/**
 * Manually finds the surface heights and holes after lines have been cleared (since usual prediction tricks
 * don't apply).
 * @param excludeHolesColumn - a prespecified column to ignore holes in (usually the well). A value of -1 disables this behavior.
 * @returns the new hole count
 */
float updateSurfaceAndHoles(int surfaceArray[10], unsigned int board[20], int excludeHolesColumn) {
  // Reset hole and tuck setup bits
  for (int i = 0; i < 20; i++) {
    board[i] &= ~ALL_AUXILIARY_BITS;
  }
  float numHoles = 0;
  for (int c = 0; c < 10; c++) {
    int mask = 1 << (9 - c);
    int r = 20 - surfaceArray[c];
    while (r >= 0 && r < 20 && !(board[r] & mask)) {
      r++;
    }
    // Update the new surface array
    surfaceArray[c] = 20 - r;

    // VARIABLE_RANGE_CHECKS
    r = max(0, r);
    r = min(19, r);
    int lowestHoleInCol = -1;
    while (r < 20) {
      // Add new holes to the overall count, unless they're in the well
      if (!(board[r] & mask) && c != excludeHolesColumn) {
        float rating = analyzeHole(board, r, c);
        // Check that it's a hole (1.0) and not a tuck setup (eg. 0.9)
        if (rating > TUCK_SETUP_HOLE_PROPORTION + FLOAT_EPSILON) {
          lowestHoleInCol = r;
        }
        numHoles += rating;
      }
      r++;
    }
    // Mark rows as needing to be cleared
    for (int r = lowestHoleInCol - 1; r >= 20 - surfaceArray[c]; r--) {
      board[r] |= HOLE_WEIGHT_BIT;
    }
  }
  return numHoles;
}

/**
 * Calculates the resulting board after placing a piece in a specified spot.
 * @returns the number of lines cleared
 */
int getNewBoardAndLinesCleared(unsigned int board[20], LockPlacement lockPlacement, OUT unsigned int newBoard[20]) {
  int numLinesCleared = 0;
  // The rows below the piece are always the same
  for (int r = lockPlacement.y + 4; r < 20; r++) {
    newBoard[r] = board[r];
  }
  // Check the piece rows, bottom to top
  unsigned int const *pieceRows = lockPlacement.piece->rowsByRotation[lockPlacement.rotationIndex];
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


float adjustHoleCountAndBoardAfterTuck(unsigned int board[20], LockPlacement lockPlacement){
  int tuckCellsFilled = 0;
  unsigned int const *pieceRows = lockPlacement.piece->rowsByRotation[lockPlacement.rotationIndex];
  for (int i = 3; i >= 0; i--) {
    // Don't add any minos that are off the board
    if (lockPlacement.y + i < 0) {
      continue;
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
GameState advanceGameState(GameState gameState, LockPlacement lockPlacement, const EvalContext *evalContext) {
  GameState newState = {{}, {}, gameState.adjustedNumHoles, gameState.lines, gameState.level};
  float numNewHoles = 0;
  int isTuck = lockPlacement.tuckFrame == -1;
  // Post-process after tucks
  // This has to happen before getNewBoardAndLinesCleared, which updates the tuck cell bits
  if (isTuck) {
    numNewHoles += adjustHoleCountAndBoardAfterTuck(gameState.board, lockPlacement);
  }
  int numLinesCleared = getNewBoardAndLinesCleared(gameState.board, lockPlacement, newState.board);
  numNewHoles +=
    getNewSurfaceAndNumNewHoles(gameState.surfaceArray, newState.board, lockPlacement, evalContext, isTuck, OUT newState.surfaceArray);
  // Post-process after line clears
  if (numLinesCleared > 0) {
    newState.adjustedNumHoles =
      updateSurfaceAndHoles(newState.surfaceArray, newState.board, evalContext->countWellHoles ? -1 : evalContext->wellColumn);
  } else {
    newState.adjustedNumHoles += numNewHoles;
  }

  newState.lines += numLinesCleared;
  newState.level = getLevelAfterLineClears(gameState.level, gameState.lines, numLinesCleared);

  return newState;
}
