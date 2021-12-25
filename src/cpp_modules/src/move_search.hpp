#ifndef MOVE_SEARCH
#define MOVE_SEARCH

#include "types.hpp"
#include "utils.hpp"
#include <vector>

int moveSearch(GameState gameState, const Piece *piece, char const *inputFrameTimeline, OUT std::vector<LockPlacement> &lockPlacements, OUT int availableTuckCols[40]);

int adjustmentSearch(GameState gameState,
                     const Piece *piece,
                     char const *inputFrameTimeline,
                     int existingXOffset,
                     int existingYOffset,
                     int existingRotation,
                     int framesAlreadyElapsed,
                     int arrWasReset,
                     OUT std::vector<LockPlacement> &lockPlacements);

#endif
