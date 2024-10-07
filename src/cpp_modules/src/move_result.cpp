#include "move_result.hpp"
#include <stdexcept>
#include <utility>

/**
 * Rates a hole from 0 to 1 based on how bad it is.
 * --Side effect-- marks the hole or tuck setup in the board data structure
 */
float analyzeHole(unsigned int board[20], int r, int c, int excludeHolesColumn, int surfaceArray[10]){
  // VARIABLE_RANGE_CHECKS_ENABLED
  if (true && (r < 0 || r >= 20)){
    printf("PANIK B, r=%d\n", r);
  }
  if (CAN_TUCK){
    if (c >= 4 
        && ((board[r] >> (9-c)) & 0b11111) == 0
        && (20 - surfaceArray[c-1] == r+1)
        && (surfaceArray[c-2] == surfaceArray[c-1])
        && (surfaceArray[c-3] == surfaceArray[c-2])
        && (surfaceArray[c-4] == surfaceArray[c-3])){
      board[r] |= TUCK_SETUP_BIT(c); // Mark this cell as an overhang cell
      return 0.2f; // Left side tuck, with ample space = 4+ pieces solve.
    }
    if (c >= 3 
        && ((board[r] >> (9-c)) & 0b1111) == 0
        && (20 - surfaceArray[c-1] == r+1)
        && (surfaceArray[c-2] == surfaceArray[c-1])
        && (surfaceArray[c-3] == surfaceArray[c-2])){
      board[r] |= TUCK_SETUP_BIT(c);
      return 0.35f; // Left side tuck, with some space = generally 3+ pieces solve.
    }
    if (c >= 3 
        && ((board[r] >> (9-c)) & 0b1111) == 0
        && (20 - surfaceArray[c-1] > r+1)
        && (surfaceArray[c-2] == surfaceArray[c-1])
        && (surfaceArray[c-3] == surfaceArray[c-2])){
      board[r] |= TUCK_SETUP_BIT(c);
      return 0.5f; // Left side tuck raised off the ground, with some space = generally 2 pieces solve.
    }
    if (c >= 2
        && ((board[r] >> (9-c)) & 0b111) == 0
        && (20 - surfaceArray[c-1] >= r+1)
        && (20 - surfaceArray[c-2] >= r+1)){
      board[r] |= TUCK_SETUP_BIT(c);
      return 0.75f; // Left side tuck, with minimal space = generally 1-piece solve.
    }
    if (c <= 5 
        && ((board[r] >> (5-c)) & 0b11111) == 0
        && (20 - surfaceArray[c+1] == r+1)
        && (surfaceArray[c+2] == surfaceArray[c+1])
        && (surfaceArray[c+3] == surfaceArray[c+2])
        && (surfaceArray[c+4] == surfaceArray[c+3])){
      board[r] |= TUCK_SETUP_BIT(c);
      return 0.1875; // Right side tuck, with ample space = 4+ piece solve
    }
    if (c <= 6 
        && ((board[r] >> (6-c)) & 0b1111) == 0
        && (20 - surfaceArray[c+1] == r+1)
        && (surfaceArray[c+2] == surfaceArray[c+1])
        && (surfaceArray[c+3] == surfaceArray[c+2])){
      board[r] |= TUCK_SETUP_BIT(c);
      return 0.325f; // Right side tuck, with some space = generally 3+ pieces solve.
    }
    if (c <= 6 
        && ((board[r] >> (6-c)) & 0b1111) == 0
        && (20 - surfaceArray[c+1] > r+1)
        && (surfaceArray[c+2] == surfaceArray[c+1])
        && (surfaceArray[c+3] == surfaceArray[c+2])){
      board[r] |= TUCK_SETUP_BIT(c);
      return 0.45f; // Right side tuck raised off the ground, with some space = generally 2 pieces solve.
    }
    if (c <= 7
        && ((board[r] >> (7-c)) & 0b111) == 0
        && (20 - surfaceArray[c+1] >= r+1)
        && (20 - surfaceArray[c+2] >= r+1)){
      board[r] |= TUCK_SETUP_BIT(c);
      return 0.65f; // Right side tuck, with minimal space = 1-piece solve + spin option.
    }
  }
  if (c == excludeHolesColumn) {
    if ((board[r] & ALL_HOLE_BITS) == 0){
      // Not strictly a problem, if the holes are cleared this is just a regular well
      return 0;
    } else {
      // The well needs to be filled to clear some hole. Treat it almost like a hole itself
      return SEMI_HOLE_PROPORTION;
    }
  }
  // Otherwise it's a hole
  // printf("MARKING HOLE %d %d\n", c, r);
  board[r] |= HOLE_BIT(c);
  return 1;
}


std::pair<int, float> getNewSurfaceAndNumNewHoles(int surfaceArray[10],
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
      int newHeight = 20 - topSurface[i] - lockPlacement.y;
      // VARIABLE RANGE CHECKS
      if (newHeight > 20){
        newHeight = 20;
      }
      newSurface[lockPlacement.x + i] = newHeight;
    }
  }
  
  // Adjust for the fact that tucks can place a piece UNDER the existing surface
  if (isTuck){
    for (int i = 0; i < 10; i++){
      newSurface[i] = std::max(surfaceArray[i], newSurface[i]);
    }
  }

  // Check for new holes by comparing the bottom surface of the piece to the surface of the stack
  int numNewTrueHoles = 0;
  float numNewPartialHoles = 0;
  unsigned int const *bottomSurface = lockPlacement.piece->bottomSurfaceByRotation[lockPlacement.rotationIndex];
  for (int i = 0; i < 4; i++) {
    if (bottomSurface[i] == NONE) {
      continue;
    }
    // Maybe skip well holes
    int c = lockPlacement.x + i;
    if (!evalContext->countWellHoles && c == evalContext->wellColumn) {
      continue;
    }
    int excludeHolesCol = evalContext->countWellHoles ? -1 : evalContext->wellColumn;

    // Loop through the cells between the bottom of the piece and the ground
    const int highestBoardCellInCol = 20 - surfaceArray[c];
    int holeWeightStartRow = -1; // Indicates that all rows above this row are weight on a hole
    for (int r = (lockPlacement.y + bottomSurface[i]); r < 20; r++) {
      // VARIABLE_RANGE_CHECKS
      if (r < 0 || r >= 20){
        printf("\n\nPANIK A, r=%d, y=%d, bottomSurface=%d\n", r, lockPlacement.y, bottomSurface[i]);
        printBoard(board);
        printBoardWithPiece(board, *lockPlacement.piece, lockPlacement.x, lockPlacement.y, lockPlacement.rotationIndex);
        continue;
      }
      if (r < highestBoardCellInCol){
        // Check for new holes
        float rating = analyzeHole(board, r, c, excludeHolesCol, surfaceArray);
        if (rating == 1) {
           // If it's a true hole
          holeWeightStartRow = r - 1;
          numNewTrueHoles += 1;
        } else {
          numNewPartialHoles += rating;
        }
      } else {
        // Check for existing holes that we're adding weight to
        if ((board[r] & HOLE_BIT(c))) {
          holeWeightStartRow = r - 1;
        }
      }
      
    }

    // Mark rows as needing to be cleared
    // maybePrint("marking needToClear (column %d): start row = %d, surface = %d\n", c, holeWeightStartRow, 20 - newSurface[c]);
    if (holeWeightStartRow != -1){
      for (int r = holeWeightStartRow; r >= 20 - newSurface[c]; r--) {
        if (VARIABLE_RANGE_CHECKS_ENABLED && (r < 0 || r >= 20)){
          printf("R value out of range %d %d\n", r, newSurface[c]);
          break;
          // throw std::invalid_argument( "r value out of range" );
        }
        if (!(board[r] & HOLE_BIT(c))) {
          board[r] |= HOLE_WEIGHT_BIT;
        }
      }
    }
  }

  return pair<int, float>(numNewTrueHoles, numNewPartialHoles);
}

/**
 * Manually finds the surface heights and holes after lines have been cleared (since usual prediction tricks
 * don't apply).
 * @param excludeHolesColumn - a prespecified column to ignore holes in (usually the well). A value of -1 disables this behavior.
 * @returns the new hole count
 */
std::pair<int, float> updateSurfaceAndHoles(int surfaceArray[10], unsigned int board[20], int excludeHolesColumn) {
  // Reset hole and tuck setup bits
  for (int i = 0; i < 20; i++) {
    board[i] &= ~ALL_AUXILIARY_BITS;
  }
  int numTrueHoles = 0;
  float numPartialHoles = 0;
  
  // Calculate the new surface array first, since its value is used in subsequent calculations
  for (int c = 0; c < 10; c++) {
    int mask = 1 << (9 - c);
    int r = 20 - surfaceArray[c];
    while (r >= 0 && r < 20 && !(board[r] & mask)) {
      r++;
    }
    // Update the new surface array
    surfaceArray[c] = 20 - r;
  }
  
  // Update hole and tuck setup info
  for (int c = 0; c < 10; c++) {
    int mask = 1 << (9 - c);
    int r = 20 - surfaceArray[c];
    // VARIABLE_RANGE_CHECKS
    r = max(0, r);
    r = min(20, r);
    int lowestHoleInCol = -1;
    while (r < 20) {
      // Add new holes to the overall count, unless they're in the well
      if (!(board[r] & mask)) {
        float rating = analyzeHole(board, r, c, excludeHolesColumn, surfaceArray);
        // Check that it's a hole (1.0) and not a tuck setup (eg. 0.9)
        if (rating == 1){
          lowestHoleInCol = r;
          numTrueHoles += 1;
        } else {
          numPartialHoles += rating;
        }
      }
      r++;
    }
    // Mark rows as needing to be cleared
    for (int r = lowestHoleInCol - 1; r >= 20 - surfaceArray[c]; r--) {
      if (VARIABLE_RANGE_CHECKS_ENABLED && (r < 0 || r >= 20)){
        printf("R value out of range %d\n", r);
        break;
        // throw std::invalid_argument( "r value out of range" );
      }
      board[r] |= HOLE_WEIGHT_BIT;
    }
  }
  return pair<int, float>(numTrueHoles, numPartialHoles);
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
  return -1 * SEMI_HOLE_PROPORTION * tuckCellsFilled;
}


/** Gets the game state after completing a given move */
GameState advanceGameState(GameState gameState, LockPlacement lockPlacement, const EvalContext *evalContext) {
  GameState newState = {{}, {}, gameState.numTrueHoles, gameState.numPartialHoles, gameState.lines, gameState.level};
  bool isTuck = lockPlacement.tuckInput != NO_TUCK_NOTATION;
  int numLinesCleared = getNewBoardAndLinesCleared(gameState.board, lockPlacement, newState.board);
  std::pair<int, float> initialResult =
    getNewSurfaceAndNumNewHoles(gameState.surfaceArray, newState.board, lockPlacement, evalContext, isTuck, OUT newState.surfaceArray);
  if (numLinesCleared == 0 && gameState.numPartialHoles == 0){
    // Use the initial result, since its predictions are reliable for boards with no holes or overhangs
    newState.numTrueHoles += initialResult.first;
    newState.numPartialHoles += initialResult.second;
  } else {
    // Recalculate the holes and overhangs from scratch
    std::pair<int, float> recalcResult = updateSurfaceAndHoles(newState.surfaceArray, newState.board, evalContext->countWellHoles ? -1 : evalContext->wellColumn);
    newState.numTrueHoles = recalcResult.first;
    newState.numPartialHoles = recalcResult.second;
  }

  newState.lines += numLinesCleared;
  newState.level = getLevelAfterLineClears(gameState.level, gameState.lines, numLinesCleared);

  return newState;
}
