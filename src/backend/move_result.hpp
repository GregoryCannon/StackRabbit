#ifndef MOVE_RESULT
#define MOVE_RESULT

#include "types.hpp"

std::pair<int, float> getNewSurfaceAndNumNewHoles(const int surfaceArray[10],
                                                  unsigned int board[20],
                                                  const LockPlacement &lockPlacement,
                                                  const EvalContext *evalContext, int isTuck,
                                                  OUT int newSurface[10]);

/**
 * Manually finds the surface heights and holes after lines have been cleared (since usual
 * prediction tricks don't apply).
 * @returns the new hole count
 */
std::pair<int, float> updateSurfaceAndHoles(int surfaceArray[10], unsigned int board[20],
                                            int excludeHolesColumn, bool isDigMode);

/**
 * Calculates the resulting board after placing a piece in a specified spot.
 * @returns the number of lines cleared
 */
int getNewBoardAndLinesCleared(const unsigned int board[20], const LockPlacement &lockPlacement,
                               OUT unsigned int newBoard[20]);

GameState advanceGameState(const GameState &gameState, const LockPlacement &lockPlacement,
                           const EvalContext *evalContext);

#endif
