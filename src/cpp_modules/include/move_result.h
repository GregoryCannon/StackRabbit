#ifndef MOVE_RESULT
#define MOVE_RESULT

#include "types.h"
#include "utils.h"

int getNewSurfaceAndNumNewHoles(int surfaceArray[10],
                                SimState lockPlacement,
                                EvalContext evalContext,
                                OUT int newSurface[10]);

/**
 * Manually finds the surface heights and holes after lines have been cleared (since usual prediction tricks don't apply).
 * @returns the new hole count
 */
int updateSurfaceAndHolesAfterLineClears(int surfaceArray[10], int board[20], EvalContext evalContext);

/**
 * Calculates the resulting board after placing a piece in a specified spot.
 * @returns the number of lines cleared
 */
int getNewBoardAndLinesCleared(int board[20], SimState lockPlacement, OUT int newBoard[20]);

GameState advanceGameState(GameState gameState, SimState lockPlacement, EvalContext evalContext);

#endif
