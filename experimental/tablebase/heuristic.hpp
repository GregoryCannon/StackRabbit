#ifndef TABLEBASE_HEURISTIC_HPP
#define TABLEBASE_HEURISTIC_HPP

#include "core.hpp"

#include <cstdlib>

namespace tb {

struct EvalWeights {
  double lineClear = 8.0;
  double sealedHole = -14.0;
  double overhang = -7.0;
  double heightSq = -0.85;
  double ceilingCol = -5.5;
  double bumpiness = -1.6;
  double flatSpotBonus = 2.0;
};

struct HeuristicBreakdown {
  int holes = 0;       // Total covered empty cells beneath column surfaces
  int overhangs = 0;   // Hole cells horizontally adjacent to an open column (1-tuck candidates)
  int sealedHoles = 0; // Covered holes not side-accessible from an open column (holes - overhangs)
  int sumHeightSq = 0;
  int ceilingCols = 0;
  int bumpiness = 0;
  bool hasSafeFlatPair = false;
  double boardScore = 0.0;
};

// Evaluates a post-clear 4x10 board's structural health for Very Low Lineout.
inline HeuristicBreakdown evaluateBoardBreakdown(uint64_t board,
                                                 const EvalWeights &w = EvalWeights{}) {
  HeuristicBreakdown b{};
  const uint64_t holeMask = computeHoleMask(board);
  const uint64_t ohMask = computeOverhangMask(board);
  b.holes = popcount64(holeMask);
  b.overhangs = popcount64(ohMask);
  b.sealedHoles = b.holes - b.overhangs;

  int heights[BOARD_WIDTH];
  getColumnHeights(board, heights);

  for (int col = 0; col < BOARD_WIDTH; ++col) {
    const int h = heights[col];
    b.sumHeightSq += h * h;
    if (h == BOARD_ROWS) {
      ++b.ceilingCols;
    }
    if (col + 1 < BOARD_WIDTH) {
      const int nextH = heights[col + 1];
      b.bumpiness += std::abs(h - nextH);
      // An adjacent equal-height pair at h <= 2 guarantees O-piece placement without hitting h > 4.
      if (h == nextH && h <= 2) {
        b.hasSafeFlatPair = true;
      }
    }
  }

  b.boardScore = w.sealedHole * static_cast<double>(b.sealedHoles)
                 + w.overhang * static_cast<double>(b.overhangs)
                 + w.heightSq * static_cast<double>(b.sumHeightSq)
                 + w.ceilingCol * static_cast<double>(b.ceilingCols)
                 + w.bumpiness * static_cast<double>(b.bumpiness)
                 + (b.hasSafeFlatPair ? w.flatSpotBonus : 0.0);
  return b;
}

inline double evalBoard(uint64_t board, const EvalWeights &w = EvalWeights{}) {
  return evaluateBoardBreakdown(board, w).boardScore;
}

inline double scoreTransition1Ply(uint64_t afterBoard, int linesCleared,
                                  const EvalWeights &w = EvalWeights{}) {
  return w.lineClear * static_cast<double>(linesCleared) + evalBoard(afterBoard, w);
}

} // namespace tb

#endif // TABLEBASE_HEURISTIC_HPP
