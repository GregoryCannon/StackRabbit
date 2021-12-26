#ifndef PIECE_RANGES
#define PIECE_RANGES

#include "../data/tetrominoes.hpp"
#include <array>

using namespace std;

#define X_BOUNDS_COLLISION_TABLE_OFFSET 3

typedef array<array<array<int, 12>, 4>, 7> xtable;

extern const xtable X_BOUNDS_COLLISION_TABLE;

/**
 * Calculates a lookup table for the Y value you'd be at while doing shift number N.
 * This is used in the tuck search, since this would be the first Y value where you could perform a tuck after N inputs of a standard placement.
 */
void computeYValueOfEachShift(char const *inputFrameTimeline, int gravity, int initialY, OUT int result[7]);

const PieceRangeContext getPieceRangeContext(char const *inputFrameTimeline, int gravity);


#endif
