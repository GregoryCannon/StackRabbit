#include "../include/move_search.h"
#include "../include/piece_ranges.h"

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <vector>
using namespace std;

#define INITIAL_X 3

/**
 * Given a string such as X.... that represents a loop of which frames are allowed for inputs,
 * determines if a given frame index is an input frame
 */
int shouldPerformInputsThisFrame(int frameIndex, char const *inputFrameTimeline) {
  int len = strlen(inputFrameTimeline);
  int index = frameIndex % len;
  return inputFrameTimeline[index] == 'X';
}

/**
 * Checks for collisions with the board and the edges of the screen
 */
int collision(int board[20], Piece piece, int x, int y, int rotIndex) {
  if (y > piece.maxYByRotation[rotIndex]) {
    return 1;
  }
  if (X_BOUNDS_COLLISION_TABLE[piece.index][rotIndex][x + X_BOUNDS_COLLISION_TABLE_OFFSET]) {
    return 1;
  }
  for (int r = 0; r < 4; r++) {
    // Don't collide above ceiling
    if (y + r < 0) {
      continue;
    }
    int pieceRow = piece.rowsByRotation[rotIndex][r];
    if (pieceRow == 0) {
      continue;
    }
    // Board collisions
    if (SHIFTBY(pieceRow, x) & board[y + r]) {
      return 1;
    }
  }
  return 0;
}

/**
 * Determines which direction the piece should rotate to get to the goal rotation.
 * Favors right rotations when ambiguous.
 */
int rotateTowardsGoal(int curRotation, int goalRotation) {
  if (curRotation == goalRotation) {
    return curRotation;
  }
  if (goalRotation == curRotation - 1 || goalRotation == (curRotation + 3)) {
    // Gets to the goal after one left rotation
    return goalRotation;
  }
  // Otherwise, it does a right rotation whether or not that gets to the goal
  return curRotation + 1;
}

/**
 * Explores how far in a given direction a piece can be shifted, and registers all the legal placements along
 * the way
 */
int exploreHorizontally(int board[20],
                        SimState simState,
                        int shiftIncrement,
                        int maxShifts,
                        int goalRotationIndex,
                        char const *inputFrameTimeline,
                        int gravity,
                        vector<SimState> &legalPlacements) {
  int rangeCurrent = 0;

  // Loop through hypothetical frames
  while (simState.x != INITIAL_X + maxShifts || simState.rotationIndex != goalRotationIndex) {
    int isInputFrame = shouldPerformInputsThisFrame(simState.frameIndex, inputFrameTimeline);
    // int isInputFrame = !(simState.frameIndex & 3);
    int isGravityFrame =
        simState.frameIndex % gravity == gravity - 1; // Returns true every Nth frame, where N = gravity
    // Event trackers to handle the ordering of a few edge cases (explained more below)
    int foundNewPlacementThisFrame = false;
    int didLockThisFrame = false;

    if (isInputFrame) {
      // Try shifting
      if (simState.x != INITIAL_X + maxShifts) {
        if (collision(
                board, simState.piece, simState.x + shiftIncrement, simState.y, simState.rotationIndex)) {
          // printf("Shift collision at x=%d\n", simState.x - INITIAL_X);
          return rangeCurrent;
        }
        simState.x += shiftIncrement;
      }

      // Try rotating
      if (simState.rotationIndex != goalRotationIndex) {
        int rotationAfter = rotateTowardsGoal(simState.rotationIndex, goalRotationIndex);
        if (collision(board, simState.piece, simState.x, simState.y, rotationAfter)) {
          // printf("Rotation collision at x=%d\n", simState.x - INITIAL_X);
          return rangeCurrent;
        }
        simState.rotationIndex = rotationAfter;
      }

      // If both succeeded, extend the range
      rangeCurrent = simState.x;
      // ...and register a new legal placement if we were in the goal rotation
      if (simState.rotationIndex == goalRotationIndex) {
        foundNewPlacementThisFrame = true;
      }
    }

    if (isGravityFrame) {
      if (collision(board, simState.piece, simState.x, simState.y + 1, simState.rotationIndex)) {
        didLockThisFrame = true;
      } else {
        simState.y++;
      }
    }

    simState.frameIndex++;

    /*
     This setup is done this way such that the simStates represent the state going into the next input,
     (i.e. looking for tucks). This means that the y position and frame index need to be updated before
      we register the state as a legal placement.
      Edge cases:
      - If the piece locked on this frame, it's still a legal placement. The y value doesn't increment,
        so that its position represents its real resting spot on the board. No tucks will be possible anyway,
     so it's a moot point that the y value is different than usual.
    */
    if (foundNewPlacementThisFrame) {
      // printf("Found legal placement: %d %d %d, frame=%d\n",
      //        simState.rotationIndex,
      //        simState.x - INITIAL_X,
      //        simState.y,
      //        simState.frameIndex);
      legalPlacements.push_back(simState);
    }
    if (didLockThisFrame) {
      // printf("LOCKED due to gravity: %d %d %d, frame=%d", simState.rotationIndex, simState.x - INITIAL_X,
      // simState.y, simState.frameIndex);
      return rangeCurrent;
    }
  }
  return 0; // Should never reach here
}

/**
 * Explores for moves with more rotations than shifts (the only blind spot of the default exploration
 * behavior).
 */
void explorePlacementsNearSpawn(int board[20],
                                SimState simState,
                                int rotationIndex,
                                char const *inputFrameTimeline,
                                int gravity,
                                vector<SimState> &legalPlacements) {
  int rangeStart = rotationIndex == 2 ? -1 : 0;
  int rangeEnd = rotationIndex == 2 ? 1 : 0;

  for (int xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
    // Check if the placement is legal.
    exploreHorizontally(
        board, simState, xOffset, xOffset, rotationIndex, inputFrameTimeline, gravity, legalPlacements);
  }
}

/**
 * Optimized method to convert legal placements to lock placements.
 * (!!) Doesn't allow for tucks.
 */
void getLockPlacementsFast(vector<SimState> &legalPlacements,
                           int board[20],
                           int surfaceArray[10],
                           vector<SimState> &lockPlacements) {
  for (auto simState : legalPlacements) {
    int *bottomSurface = simState.piece.bottomSurfaceByRotation[simState.rotationIndex];
    int rowsToShift = 99999;
    for (int c = 0; c < 4; c++) {
      if (bottomSurface[c] == -1) {
        continue; // Skip columns that the piece doesn't occupy
      }
      // Check how high the piece is above the stack
      int currentUnderSurface = 20 - bottomSurface[c] - simState.y;
      int colHeight = surfaceArray[simState.x + c];
      // printf("Possibility %d %d will end at y %d\n",
      //        simState.rotationIndex,
      //        simState.x - SPAWN_X,
      //        simState.y + currentUnderSurface - colHeight);
      rowsToShift = min(rowsToShift, currentUnderSurface - colHeight);
    }
    // Shift down to its lock position
    simState.y += rowsToShift;
    lockPlacements.push_back(simState);
  }
}

/**
 * Looks at each legal placement and lets the piece fall until it locks into the stack.
 * (!!) This method is SLOW! (>75% of the total movesearch runtime if it's included)
 */
void exploreLegalPlacementsUntilLock(vector<SimState> &legalPlacements,
                                     int board[20],
                                     int gravity,
                                     char const *inputFrameTimeline,
                                     vector<SimState> lockStates) {
  int bestValue = -99999999;
  vector<SimState> potentialTuckSpinStates;
  int lockHeightLookup[36]; // Indexed by (rotIndex * 9) + (x + 1)

  for (auto simState : legalPlacements) {
    // Let piece drop
    int *pieceRows = simState.piece.rowsByRotation[simState.rotationIndex];
    int startedLookingForTucks = false;
    int lastSeenY = -1;

    while (true) {
      int isGravityFrame =
          simState.frameIndex % gravity == gravity - 1; // Returns true every Nth frame, where N = gravity

      if (!startedLookingForTucks && shouldPerformInputsThisFrame(simState.frameIndex, inputFrameTimeline)) {
        startedLookingForTucks = true;
      }

      if (startedLookingForTucks && simState.y != lastSeenY) {
        potentialTuckSpinStates.push_back(simState);
        lastSeenY = simState.y;
      }

      if (true) {
        if (collision(board, simState.piece, simState.x, simState.y + 1, simState.rotationIndex)) {
          lockStates.push_back(simState);
          int encodedLockCol = simState.rotationIndex * 9 + (simState.x + 1);
          lockHeightLookup[encodedLockCol] = simState.y;
          break;
        }
        simState.y++;
      }

      simState.frameIndex++;
    }
  }
}

int moveSearch(GameState gameState, Piece piece, OUT vector<SimState> &lockPlacements) {
  vector<SimState> legalMidairPlacements;

  for (int rotIndex = 0; rotIndex < 4; rotIndex++) {
    if (piece.rowsByRotation[rotIndex][0] == -1) {
      // Rotation doesn't exist on this piece
      continue;
    }

    // Initialize the starting state
    SimState simState = {INITIAL_X, piece.initialY, /* rotationIndex= */ 0, /* frameIndex= */ 0, piece};
    int gravity = 3;

    // Check for immediate collision on spawn
    if (rotIndex == 0) {
      if (collision(gameState.board, piece, simState.x, simState.y, simState.rotationIndex)) {
        return 0;
      }
      // Otherwise the starting state is a legal placement
      legalMidairPlacements.push_back(simState);
    }

    // Search for placements as far as possible to both sides
    exploreHorizontally(gameState.board, simState, -1, -99, rotIndex, "X...", gravity, legalMidairPlacements);
    exploreHorizontally(gameState.board, simState, 1, 99, rotIndex, "X...", gravity, legalMidairPlacements);
    // Then double check for some we missed near spawn
    explorePlacementsNearSpawn(gameState.board, simState, rotIndex, "X...", gravity, legalMidairPlacements);
  }

  // Let the pieces fall until they lock
  // exploreLegalPlacementsUntilLock(legalPlacements, board, gravity, "X...", lockPlacements);
  getLockPlacementsFast(legalMidairPlacements, gameState.board, gameState.surfaceArray, lockPlacements);

  return lockPlacements.size();
}