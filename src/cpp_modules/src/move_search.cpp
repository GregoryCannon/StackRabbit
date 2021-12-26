#include "move_search.hpp"
#include "piece_ranges.hpp"

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <unordered_set>
#include <vector>
#include "utils.hpp"
using namespace std;

#define INITIAL_X 3
#define NO_TUCK_NOTATION '.'

/**
 * Checks for collisions with the board and the edges of the screen
 */
int collision(unsigned int board[20], const Piece *piece, int x, int y, int rotIndex) {
  if (y > piece->maxYByRotation[rotIndex]) {
    return 1;
  }
  if (X_BOUNDS_COLLISION_TABLE[piece->index][rotIndex][x + X_BOUNDS_COLLISION_TABLE_OFFSET]) {
    return 1;
  }
  for (int r = 0; r < 4; r++) {
    // Don't collide above ceiling
    if (y + r < 0) {
      continue;
    }
    int pieceRow = piece->rowsByRotation[rotIndex][r];
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
  return MOD_4(curRotation + 1);
}

/**
 * Explores how far in a given direction a piece can be shifted, and registers all the legal placements along
 * the way
 */
int exploreHorizontally(unsigned int board[20],
                        SimState simState,
                        int shiftIncrement,
                        int maxOrMinX,
                        int goalRotationIndex,
                        char const *inputFrameTimeline,
                        int gravity,
                        vector<SimState> &legalPlacements,
                        int availableTuckCols[40]) {
  int rangeCurrent = 0;
  debugPrint("Exploring horizontally, inc=%d maxmin=%d goalRot=%d\n", shiftIncrement, maxOrMinX, goalRotationIndex);

  // Loop through hypothetical frames
  while (simState.x != maxOrMinX || simState.rotationIndex != goalRotationIndex) {
    int isInputFrame = shouldPerformInputsThisFrame(simState.arrIndex, inputFrameTimeline);
    int isGravityFrame =
      simState.frameIndex % gravity == gravity - 1;   // Returns true every Nth frame, where N = gravity
    // Event trackers to handle the ordering of a few edge cases (explained more below)
    int foundNewPlacementThisFrame = false;
    int didLockThisFrame = false;

    if (isInputFrame) {
      // Try shifting
      if (simState.x != maxOrMinX) {
        if (collision(
              board, simState.piece, simState.x + shiftIncrement, simState.y, simState.rotationIndex)) {
          debugPrint("Shift collision at xOff=%d\n", simState.x - INITIAL_X);
          return rangeCurrent;
        }
        simState.x += shiftIncrement;
      }

      // Try rotating
      if (simState.rotationIndex != goalRotationIndex) {
        int rotationAfter = rotateTowardsGoal(simState.rotationIndex, goalRotationIndex);
        if (collision(board, simState.piece, simState.x, simState.y, rotationAfter)) {
          if (MOVE_SEARCH_DEBUG_LOGGING){
            printf("Rotation collision at x=%d, rot=%d\n", simState.x - INITIAL_X, rotationAfter);
            // printBoardWithPiece(board, *(simState.piece), simState.x, simState.y, rotationAfter);
          }
          return rangeCurrent;
        }
        simState.rotationIndex = rotationAfter;
      }

      // If both succeeded, extend the range
      debugPrint("Extending range, current xOff= %d\n", simState.x - INITIAL_X);
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
    simState.arrIndex++;

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
      if (MOVE_SEARCH_DEBUG_LOGGING){
        printf("Found legal placement: rot=%d xOff=%d y=%d, frame=%d\n",
               simState.rotationIndex,
               simState.x - INITIAL_X,
               simState.y,
               simState.frameIndex);
      }
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
void explorePlacementsNearSpawn(unsigned int board[20],
                                SimState simState,
                                int goalRotationIndex,
                                char const *inputFrameTimeline,
                                int gravity,
                                vector<SimState> &legalPlacements,
                                int availableTuckCols[40]) {
  int rotationDifference = abs(goalRotationIndex - simState.rotationIndex);
  /* The goal placement is in the blind spot of the main algorithm if there are more rotations than shifts.
   Therefore, for double rotations, the X could be anywhere from -1 to 1. For single rotations, they would always be in place. */
  int rangeStart = rotationDifference == 2 ? -1 : 0;
  int rangeEnd = rotationDifference == 2 ? 1 : 0;

  for (int xOffset = rangeStart; xOffset <= rangeEnd; xOffset++) {
    // Check if the placement is legal.
    exploreHorizontally(board,
                        simState,
                        xOffset,
                        simState.x + xOffset,
                        goalRotationIndex,
                        inputFrameTimeline,
                        gravity,
                        legalPlacements,
                        availableTuckCols);
  }
}

/**
 * Optimized method to convert legal placements to lock placements.
 * (!!) Doesn't allow for tucks.
 */
void getLockPlacementsFast(vector<SimState> &legalPlacements,
                           unsigned int board[20],
                           int surfaceArray[10],
                           OUT int availableTuckCols[40],
                           OUT vector<LockPlacement> &lockPlacements) {
  for (auto simState : legalPlacements) {
    unsigned int const *bottomSurface = simState.piece->bottomSurfaceByRotation[simState.rotationIndex];
    int rowsToShift = 99999;
    for (int c = 0; c < 4; c++) {
      if (bottomSurface[c] == NONE) {
        continue; // Skip columns that the piece doesn't occupy
      }
      // Check how high the piece is above the stack
      int currentUnderSurface = 20 - bottomSurface[c] - simState.y;
      int colHeight = surfaceArray[simState.x + c];
      rowsToShift = min(rowsToShift, currentUnderSurface - colHeight);
    }
    // Shift down to its lock position
    simState.y += rowsToShift;
    // printf("Lock placement was %d %d %d\n", simState.rotationIndex, simState.x, simState.y);
    availableTuckCols[TUCK_COL_ENCODED(simState.rotationIndex, simState.x)] = simState.y;
    // printf("AvalTuckCols[%d] = %d\n", TUCK_COL_ENCODED(simState.rotationIndex, simState.x) + 40,
    // simState.y);
    lockPlacements.push_back({simState.x, simState.y, simState.rotationIndex, -1, NO_TUCK_NOTATION, simState.piece});
  }
}

char findTuckInput(unsigned int board[20],
                   SimState afterTuckState,
                   int availableTuckCols[40],
                   int minTuckYValsByNumPrevInputs[7]) {
  // Do rotations mod 4 or mod 2, depending on the piece (rotation logic skipped for O)
  int numOrientations = afterTuckState.piece->id == 'O'                    ? 1
                        : afterTuckState.piece->rowsByRotation[3][0] == NONE ? 2
                                                                          : 4;
  int rotationModulusMask = numOrientations == 4 ? 3 : 1;
  for (TuckInput tuckInput : TUCK_INPUTS) {
    maybePrint("Trying %c:\n", tuckInput.notation);
    // Apply the tuck in reverse to get the pre-tuck state
    int preTuckRotIndex = afterTuckState.rotationIndex;
    int preTuckX = afterTuckState.x;
    preTuckX -= tuckInput.xChange; // Do the input in reverse
    if (afterTuckState.piece->id != 'O') {
      preTuckRotIndex = (preTuckRotIndex - tuckInput.rotationChange + 4) & rotationModulusMask;
    }

    // Validate the pre-tuck state
    int index = TUCK_COL_ENCODED(preTuckRotIndex, preTuckX);
    int numRotsBeforeTuck = preTuckRotIndex == 3 ? 1 : preTuckRotIndex;
    int numInputs = std::max(numRotsBeforeTuck, std::abs(preTuckX - SPAWN_X));
    int minY = minTuckYValsByNumPrevInputs[numInputs + 1];
    int maxY = availableTuckCols[index];
    if (afterTuckState.y < minY || afterTuckState.y > maxY) {
      maybePrint("Tuck not in y range. Actual=%d, Range= %d to %d (orients=%d, rot=%d, x=%d, index=%d)\n",
                 afterTuckState.y,
                 minY,
                 maxY,
                 numOrientations,
                 preTuckRotIndex,
                 preTuckX,
                 index);
      continue;
    }
    // Check that it doesn't collide with the board after just the shift (the order goes Shift -> Rotate ->
    // Drop)
    if (collision(board, afterTuckState.piece, afterTuckState.x, afterTuckState.y, preTuckRotIndex)) {
      maybePrint("Tuck collided with board after shift\n");
      continue;
    }
    // Check that it doesn't collide with the board before both the shift and the rotation
    if (collision(board, afterTuckState.piece, preTuckX, afterTuckState.y, preTuckRotIndex)) {
      maybePrint("Tuck collided with board before tuck. x=%d, y=%d, rot=%d\n",
                 preTuckX,
                 afterTuckState.y,
                 preTuckRotIndex);
      continue;
    }
    return tuckInput.notation;
  }
  // No input found that made it work
  return NO_TUCK_NOTATION;
}

/**
   Searches for tucks by 1) Finding all of the overhang cells from the board array, then 2) looping over the
   overhang cells and trying all the ways that the piece could possibly fill that cell. Each piece has a
   precomputed list of the possible ways it can fill a tuck cell (defined in tetrominoes.h), which drastically
   reduces the number of placements to try each time.
 */
void findTucks(unsigned int board[20],
               const Piece *piece,
               int availableTuckCols[40],
               int minTuckYValsByNumPrevInputs[7],
               OUT std::vector<LockPlacement> &lockPlacements) {
  std::vector<LockPlacement> tuckLockPlacements;
  std::unordered_set<int> tuckLockSpots;
  for (int overhangY = 0; overhangY < 20; overhangY++) {
    if ((board[overhangY] & ALL_TUCK_SETUP_BITS) == 0) {
      continue;
    }
    for (int overhangX = 0; overhangX < 10; overhangX++) {
      if ((board[overhangY] & TUCK_SETUP_BIT(overhangX)) > 0) {
        // Found an overhang cell! Look for tucks here
        maybePrint("Looking for tucks at %d %d\n", overhangX, overhangY);
        for (TuckOriginSpot spot : TUCK_SPOTS_LIST[piece->index]) {
          int pieceX = overhangX - spot.x;
          int postTuckPieceY = overhangY - spot.y;
          int lockPieceY = postTuckPieceY; // Can differ from postTuckPieceY if the piece falls after the tuck
          maybePrint("Trying origin spot %d %d %d\n", spot.orientation, spot.x, spot.y);
          // The piece must fit into the board post-tuck
          if (!collision(board, piece, pieceX, postTuckPieceY, spot.orientation)) {
            maybePrint("Fits into board\n");
            // Found a new tuck! Gravity it down if needed
            while (!collision(board, piece, pieceX, lockPieceY + 1, spot.orientation)) {
              lockPieceY++;
            }

            int lockPositionHash = lockPieceY * 1000 + pieceX * 10 + spot.orientation;
            if (tuckLockSpots.find(lockPositionHash) == tuckLockSpots.end()) {
              char c = findTuckInput(board,
                                     {pieceX, postTuckPieceY, spot.orientation, -1, -1, piece},
                                     availableTuckCols,
                                     minTuckYValsByNumPrevInputs);
              if (c != NO_TUCK_NOTATION) {
                lockPlacements.push_back({pieceX, lockPieceY, spot.orientation, -1, c, piece});
                tuckLockSpots.insert(lockPositionHash);
              }
            }
          }
        }
      }
    }
  }
}

/**
 * Main move search implementation.
 * Wrapped in two parent functions depending on whether the move search is from standard spawn or from a midair adjustment spot.
 */
int moveSearchInternal(GameState gameState,
                       SimState spawnState,
                       const Piece *piece,
                       char const *inputFrameTimeline,
                       OUT std::vector<LockPlacement> &lockPlacements) {
  vector<SimState> legalMidairPlacements;
  int gravity = getGravity(gameState.level);

  // Encodes which rotation/column pairs are reachable, and stores the lowest Y value reached in that pair
  int availableTuckCols[40] = {};
  int minTuckYValsByNumPrevInputs[7] = {};
  computeYValueOfEachShift(inputFrameTimeline, gravity, piece->initialY, minTuckYValsByNumPrevInputs);

  for (int goalRotIndex = 0; goalRotIndex < 4; goalRotIndex++) {
    if (piece->rowsByRotation[goalRotIndex][0] == NONE) {
      // Rotation doesn't exist on this piece
      debugPrint("Rotation doesn't exist\n");
      continue;
    }

    // Check for immediate collision on spawn
    if (goalRotIndex == 0) {
      if (collision(gameState.board, piece, spawnState.x, spawnState.y, spawnState.rotationIndex)) {
        if (MOVE_SEARCH_DEBUG_LOGGING) {
          printf("Immediate collision\n");
          printBoardWithPiece(gameState.board, PIECE_T, spawnState.x, spawnState.y, spawnState.rotationIndex);
        }
        return 0;
      }
      // Otherwise the starting state is a legal placement
      legalMidairPlacements.push_back(spawnState);
    }

    // Search for placements as far as possible to both sides
    exploreHorizontally(gameState.board,
                        spawnState,
                        -1,
                        -99,
                        goalRotIndex,
                        inputFrameTimeline,
                        gravity,
                        legalMidairPlacements,
                        availableTuckCols);
    exploreHorizontally(gameState.board,
                        spawnState,
                        1,
                        99,
                        goalRotIndex,
                        inputFrameTimeline,
                        gravity,
                        legalMidairPlacements,
                        availableTuckCols);
    // Then double check for some we missed near spawn
    explorePlacementsNearSpawn(gameState.board,
                               spawnState,
                               goalRotIndex,
                               inputFrameTimeline,
                               gravity,
                               legalMidairPlacements,
                               availableTuckCols);
  }

  // Let the pieces fall until they lock
  getLockPlacementsFast(
    legalMidairPlacements, gameState.board, gameState.surfaceArray, availableTuckCols, lockPlacements);

  // Search for tucks
  if (CAN_TUCK) {
    findTucks(gameState.board, piece, availableTuckCols, minTuckYValsByNumPrevInputs, lockPlacements);
  }

  return (int)lockPlacements.size();
}

int moveSearch(GameState gameState,
               const Piece *piece,
               char const *inputFrameTimeline,
               OUT std::vector<LockPlacement> &lockPlacements) {
  SimState spawnState = {INITIAL_X, piece->initialY, /* rotationIndex= */ 0, /* frameIndex= */ 0, /* arrIndex= */ 0, piece};
  return moveSearchInternal(gameState, spawnState, piece, inputFrameTimeline, lockPlacements);
}

int adjustmentSearch(GameState gameState,
                     const Piece *piece,
                     char const *inputFrameTimeline,
                     int existingXOffset,
                     int existingYOffset,
                     int existingRotation,
                     int framesAlreadyElapsed,
                     int arrWasReset,
                     OUT std::vector<LockPlacement> &lockPlacements){
  SimState startState = {INITIAL_X + existingXOffset, piece->initialY + existingYOffset, existingRotation, framesAlreadyElapsed, /* arrIndex= */ arrWasReset ? 0 : framesAlreadyElapsed, piece};
  return moveSearchInternal(gameState, startState, piece, inputFrameTimeline, lockPlacements);
}

/* ----------- TESTS ----------- */

void testTuckSpots() {
  unsigned int testBoard[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1016, 1008, 1020, 1022};
  int overhangCellX = 6;
  int overhangCellY = 17;
  printBoard(testBoard);

  for (TuckOriginSpot spot : TUCK_SPOTS_J) {
    unsigned int newBoard[20];
    for (int i = 0; i < 20; i++) {
      newBoard[i] = testBoard[i];
    }
    int pieceX = overhangCellX - spot.x;
    int pieceY = overhangCellY - spot.y;
    if (!collision(testBoard, &PIECE_J, pieceX, pieceY, spot.orientation)) {
      for (int y = pieceY; y < pieceY + 4; y++) {
        int shiftedPieceRow = SHIFTBY(PIECE_J.rowsByRotation[spot.orientation][y - pieceY], pieceX);
        newBoard[y] = newBoard[y] | shiftedPieceRow;
      }
      if (MOVE_SEARCH_DEBUG_LOGGING) {
        printf("%d %d %d\n", spot.orientation, pieceX, pieceY);
        printBoard(newBoard);
      }
    }
  }
}

/** A generated set of test cases for use in fuzz testing against the original Node.js movesearch. */
int testCases[50][4] = {
  { -5, 10, 3, 41 }, { -2, 5, 0, 23 },  { -1, 1, 2, 6 },
  { 0, 10, 0, 43 },  { 0, 15, 3, 63 },  { 0, 9, 2, 39 },
  { 2, 1, 1, 7 },    { -3, 4, 2, 16 },  { 0, 9, 2, 39 },
  { -2, 12, 1, 51 }, { -3, 6, 1, 26 },  { -5, 6, 0, 24 },
  { 1, 12, 3, 48 },  { -3, 11, 2, 46 }, { -2, 17, 3, 68 },
  { 0, 2, 2, 10 },   { -3, 4, 3, 18 },  { 3, 14, 1, 58 },
  { -4, 12, 0, 50 }, { 0, 8, 1, 33 },   { -5, 11, 0, 45 },
  { -4, 16, 3, 67 }, { 3, 8, 2, 35 },   { -1, 6, 2, 25 },
  { 3, 15, 0, 63 },  { 0, 10, 0, 42 },  { 3, 12, 0, 50 },
  { -2, 10, 3, 42 }, { 0, 17, 2, 68 },  { -4, 14, 3, 57 },
  { -4, 3, 0, 12 },  { 2, 3, 0, 12 },   { -1, 13, 1, 53 },
  { 1, 17, 3, 70 },  { 0, 12, 1, 48 },  { -3, 17, 2, 70 },
  { -4, 1, 2, 4 },   { -1, 13, 0, 55 }, { -1, 4, 2, 18 },
  { 0, 3, 3, 12 },   { 0, 0, 0, 0 },    { 1, 4, 2, 16 },
  { -4, 1, 1, 6 },   { 2, 8, 3, 33 },   { 0, 7, 3, 29 },
  { -4, 1, 1, 5 },   { 3, 5, 3, 23 },   { -2, 8, 0, 33 },
  { 0, 2, 1, 11 },   { -2, 3, 3, 13 }
};


int testAdjustmentSearch(int testCase[4]){
  int xOffset = testCase[0];
  int yOffset = testCase[1];
  int rotation = testCase[2];
  int framesElapsed = testCase[3];
  int arrReset = false;

  GameState gameState = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1022, 1022, 1022},
    /* surfaceArray= */ {},
    /* adjustedNumHoles= */ 0,
    /* lines= */ 0,
    /* level= */ 18
  };
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  if (MOVE_SEARCH_DEBUG_LOGGING){
    printBoardWithPiece(gameState.board, PIECE_T, SPAWN_X + xOffset, PIECE_T.initialY + yOffset, rotation);
  }

  std::vector<LockPlacement> lockPlacements;
  int adjCount = adjustmentSearch(gameState, &PIECE_T, "X...", xOffset, yOffset, rotation, framesElapsed, arrReset, lockPlacements);
  if (MOVE_SEARCH_DEBUG_LOGGING) {
    for (auto state : lockPlacements) {
      if (MOVE_SEARCH_DEBUG_LOGGING) {
        printf("Found %d %d %d\n", state.x, state.y, state.rotationIndex);
      }
      printBoardWithPiece(gameState.board, PIECE_T, state.x, state.y, state.rotationIndex);
    }
  }

  return adjCount;
}

void testAdjustments(){
  for (int i = 0; i < 50; i++){
    printf("\n%d %d %d %d\n", testCases[i][0], testCases[i][1], testCases[i][2], testCases[i][3]);
    printf("%d\n", testAdjustmentSearch(testCases[i]));
  }
  // int singleTestCase[4] = {2, 8, 3, 33};
  // printf("\n\n\n%d\n", testAdjustmentSearch(singleTestCase));
}
