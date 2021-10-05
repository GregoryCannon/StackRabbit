#ifndef PIECE_RANGES
#define PIECE_RANGES

#include "../data/tetrominoes.hpp"
#include "utils.hpp"
#include <array>

using namespace std;

#define X_BOUNDS_COLLISION_TABLE_OFFSET 3

typedef array<array<array<int, 12>, 4>, 7> xtable;

xtable getRangeXTable() {
  xtable table = {};
  for (int p = 0; p < 7; p++) {
    for (int rot = 0; rot < 4; rot++) {
      for (int x = -3; x <= 8;
           x++) { // Lowest possible x is -2, highest possible is 7. Then one extra on either side.
        int tableX = x + X_BOUNDS_COLLISION_TABLE_OFFSET;
        table[p][rot][tableX] = 0; // No collision by default
        for (int row = 0; row < 4; row++) {
          if (SHIFTBY(PIECE_LIST[p].rowsByRotation[rot][row], x) >= 1024) {
            table[p][rot][tableX] = 1; // Mark the collision
            break;
          }
          if (SHIFTBY(PIECE_LIST[p].rowsByRotation[rot][row], x - 1) & 1) {
            table[p][rot][tableX] = 1; // Mark the collision
            break;
          }
        }
      }
    }
  }
  return table;
}

const xtable X_BOUNDS_COLLISION_TABLE = getRangeXTable();

/**
 * Calculates a lookup table for the Y value you'd be at while doing shift number N.
 * This is used in the tuck search, since this would be the first Y value where you could perform a tuck after N inputs of a standard placement.
 */
void computeYValueOfEachShift(char const *inputFrameTimeline, int gravity, int initialY, OUT int result[7]);

const PieceRangeContext getPieceRangeContext(char const *inputFrameTimeline, int gravity);


#endif
