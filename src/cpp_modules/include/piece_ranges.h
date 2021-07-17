#ifndef PIECE_RANGES
#define PIECE_RANGES

#include <array>
#include "tetrominoes.h"
#include "utils.h"

using namespace std;

#define X_OFFSET 2
#define Y_OFFSET 2

typedef array<array<array<int, 9>, 4>, 7> xtable;

constexpr xtable getRangeXTable() {
  xtable table = {};
  for (int p = 0; p < 7; p++) {
    for (int rot = 0; rot < 4; rot++){
      for (int x = 0; x < 9; x++){
        table[p][rot][x] = 1; // In range by default
        for (int row = 0; row < 4; row++){
          if (SHIFTBY(PIECE_LIST[p].rowsByRotation[rot][row], x - X_OFFSET) >= 1024){
            table[p][rot][x] = 0; // Remove from range if collision
          }
        }
      }
    }
  }
  return table;
}

const array<array<array<int, 9>, 4>, 7> rangeXTable = getRangeXTable();

#endif