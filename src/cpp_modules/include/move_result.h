#ifndef MOVE_RESULT
#define MOVE_RESULT

#include "types.h"
#include "utils.h"

int getNewSurfaceAndNumNewHoles(int surfaceArray[10],
                                SimState lockPlacement,
                                EvalContext evalContext,
                                OUT int newSurface[10]);

void updateSurfaceAfterLineClears(int surfaceArray[10], int board[20], int numLinesCleared);

/**
 * Calculates the resulting board after placing a piece in a specified spot.
 * @returns the number of lines cleared
 */
int getNewBoardAndLinesCleared(int board[20], SimState lockPlacement, OUT int newBoard[20]);

GameState advanceGameState(GameState gameState, SimState lockPlacement, EvalContext evalContext);

#endif