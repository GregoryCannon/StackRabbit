#ifndef MOVE_SEARCH
#define MOVE_SEARCH

#include "types.hpp"
#include <vector>

int fastMoveSearch(const GameState &gameState, const Piece *piece, char const *inputFrameTimeline,
                   OUT std::vector<LockPlacement> &lockPlacements);

int adjustmentSearch(const GameState &gameState, const Piece *piece, char const *inputFrameTimeline,
                     int existingXOffset, int existingYOffset, int existingRotation,
                     int framesAlreadyElapsed, int arrWasReset,
                     OUT std::vector<LockPlacement> &lockPlacements);

#endif
