#ifndef MOVE_RESULT
#define MOVE_RESULT

#include "types.hpp"
#include "utils.hpp"

float getNewSurfaceAndNumNewHoles(int surfaceArray[10],
                                  unsigned int board[20],
                                  LockPlacement lockPlacement,
                                  const EvalContext *evalContext,
                                  int isTuck,
                                  OUT int newSurface[10]);

/**
 * Manually finds the surface heights and holes after lines have been cleared (since usual prediction tricks don't apply).
 * @returns the new hole count
 */
float updateSurfaceAndHoles(int surfaceArray[10], unsigned int board[20], int excludeHolesColumn);

/**
 * Calculates the resulting board after placing a piece in a specified spot.
 * @returns the number of lines cleared
 */
int getNewBoardAndLinesCleared(unsigned int board[20], LockPlacement lockPlacement, OUT unsigned int newBoard[20]);

GameState advanceGameState(GameState gameState, LockPlacement lockPlacement, const EvalContext *evalContext);

#endif
