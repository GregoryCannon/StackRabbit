#ifndef TABLEBASE_HEURISTIC_HPP
#define TABLEBASE_HEURISTIC_HPP

#include "core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tb {

struct EvalWeights {
  double lineClear = 2.4654;
  double sealedHole = -38.1551;
  double overhang = -29.5112;
  double heightSq = -0.1554;
  double height3Col = -2.2417;
  double ceilingCol = -3.7805;
  double bumpiness = -1.9988;
  double wellDepthSq = -3.4182;
  double parityImbalance = -0.7588;
  double safeFlatPairBonus = 1.4615;
  double rowFillSq = 0.1045;
  double deathPenalty = -394.9412;
};

struct HeuristicBreakdown {
  int holes = 0;       // Total covered empty cells beneath column surfaces
  int overhangs = 0;   // Hole cells horizontally adjacent to an open column (1-tuck candidates)
  int sealedHoles = 0; // Covered holes not side-accessible from an open column (holes - overhangs)
  int sumHeightSq = 0; // Sum of h[c]^2 across columns 0..9
  int height3Cols = 0; // Columns at h == 3 (only horizontal I can rest on top)
  int ceilingCols = 0; // Columns at h == 4 (no piece can rest on top)
  int bumpiness = 0;   // Sum of |h[c] - h[c+1]| across adjacent columns
  int wellDepthSq = 0; // Sum of d^2 for narrow wells with d = min(leftH, rightH) - h[c] >= 2
  int parityImbalance = 0; // |sum(even-col heights) - sum(odd-col heights)|
  int safeFlatPairs = 0;   // Adjacent equal-height column pairs with h <= 2 (safe for O-piece)
  int rowFillSq = 0;       // Sum of (filled cells in row r)^2 across rows 0..3
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

  const uint64_t r0 = getRow(board, 0);
  const uint64_t r1 = getRow(board, 1);
  const uint64_t r2 = getRow(board, 2);
  const uint64_t r3 = getRow(board, 3);

  int heights[BOARD_WIDTH];
  int evenSum = 0;
  int oddSum = 0;
  for (int col = 0; col < BOARD_WIDTH; ++col) {
    const uint64_t bit = 1ULL << (BOARD_WIDTH - 1 - col);
    const int h = (r3 & bit) ? 4 : (r2 & bit) ? 3 : (r1 & bit) ? 2 : (r0 & bit) ? 1 : 0;
    heights[col] = h;
    b.sumHeightSq += h * h;
    if (h == 3) {
      ++b.height3Cols;
    } else if (h == BOARD_ROWS) {
      ++b.ceilingCols;
    }
    if ((col & 1) == 0) {
      evenSum += h;
    } else {
      oddSum += h;
    }
  }
  b.parityImbalance = std::abs(evenSum - oddSum);

  for (int col = 0; col < BOARD_WIDTH; ++col) {
    const int h = heights[col];
    if (col + 1 < BOARD_WIDTH) {
      const int nextH = heights[col + 1];
      b.bumpiness += std::abs(h - nextH);
      if (h == nextH && h <= 2) {
        ++b.safeFlatPairs;
      }
    }
    const int leftH = (col == 0) ? BOARD_ROWS : heights[col - 1];
    const int rightH = (col + 1 == BOARD_WIDTH) ? BOARD_ROWS : heights[col + 1];
    const int depth = std::min(leftH, rightH) - h;
    if (depth >= 2) {
      b.wellDepthSq += depth * depth;
    }
  }

  const int c0 = popcount64(r0);
  const int c1 = popcount64(r1);
  const int c2 = popcount64(r2);
  const int c3 = popcount64(r3);
  b.rowFillSq = c0 * c0 + c1 * c1 + c2 * c2 + c3 * c3;

  b.boardScore = w.sealedHole * static_cast<double>(b.sealedHoles)
                 + w.overhang * static_cast<double>(b.overhangs)
                 + w.heightSq * static_cast<double>(b.sumHeightSq)
                 + w.height3Col * static_cast<double>(b.height3Cols)
                 + w.ceilingCol * static_cast<double>(b.ceilingCols)
                 + w.bumpiness * static_cast<double>(b.bumpiness)
                 + w.wellDepthSq * static_cast<double>(b.wellDepthSq)
                 + w.parityImbalance * static_cast<double>(b.parityImbalance)
                 + w.safeFlatPairBonus * static_cast<double>(b.safeFlatPairs)
                 + w.rowFillSq * static_cast<double>(b.rowFillSq);
  return b;
}

inline double evalLineoutBoard(uint64_t board, const EvalWeights &w = EvalWeights{}) {
  return evaluateBoardBreakdown(board, w).boardScore;
}

inline double scoreTransition1Ply(uint64_t afterBoard, int linesCleared,
                                  const EvalWeights &w = EvalWeights{}) {
  return w.lineClear * static_cast<double>(linesCleared) + evalLineoutBoard(afterBoard, w);
}

namespace detail {

// Admissible sign-agnostic upper bound on scoreTransition1Ply(b'', r2, w) across any post-clear
// 4x10 board b'' and linesCleared in [0..4]. Couples column-height counts (n0..n4, 1001 multisets)
// with rowFillSq and safeFlatPairs so positive rowFillSq/flatSpot bonuses cannot loosen the bound
// by ignoring mandatory height penalties.
constexpr double maxPossible1PlyScore(const EvalWeights &w) {
  constexpr int MAX_POST_CLEAR_HOLES = (BOARD_ROWS - 1) * BOARD_WIDTH;             // 30
  constexpr int MAX_BUMPINESS = (BOARD_WIDTH - 1) * BOARD_ROWS;                    // 36
  constexpr int MAX_WELL_DEPTH_SQ = (BOARD_WIDTH / 2) * (BOARD_ROWS * BOARD_ROWS); // 80
  constexpr int MAX_PARITY_IMBALANCE = (BOARD_WIDTH / 2) * BOARD_ROWS;             // 20
  constexpr int MAX_ROW_FILL_POST_CLEAR = BOARD_WIDTH - 1;                         // 9

  const auto pos = [](int maxCount, double weight) constexpr {
    return (weight > 0.0) ? static_cast<double>(maxCount) * weight : 0.0;
  };

  // Since sealedHoles >= 0, overhangs >= 0, and sealedHoles + overhangs <= MAX_POST_CLEAR_HOLES,
  // their combined contribution is bounded above by MAX_POST_CLEAR_HOLES * max(0, sealedHole,
  // overhang).
  const double maxHoleTerm = pos(MAX_POST_CLEAR_HOLES, std::max(w.sealedHole, w.overhang));
  const double uncoupledMax = pos(BOARD_ROWS, w.lineClear)
                              + maxHoleTerm
                              + pos(MAX_BUMPINESS, w.bumpiness)
                              + pos(MAX_WELL_DEPTH_SQ, w.wellDepthSq)
                              + pos(MAX_PARITY_IMBALANCE, w.parityImbalance);

  // Maximize the coupled height + flat-pair + rowFillSq terms over all 1001 column-height multisets
  // (n0 + n1 + n2 + n3 + n4 == 10).
  double bestCoupled = -1e300;
  for (int n4 = 0; n4 <= BOARD_WIDTH; ++n4) {
    for (int n3 = 0; n3 <= BOARD_WIDTH - n4; ++n3) {
      for (int n2 = 0; n2 <= BOARD_WIDTH - n4 - n3; ++n2) {
        for (int n1 = 0; n1 <= BOARD_WIDTH - n4 - n3 - n2; ++n1) {
          const int n0 = BOARD_WIDTH - n4 - n3 - n2 - n1;
          const int sumHSq = n1 + 4 * n2 + 9 * n3 + 16 * n4;

          // At most max(0, n0 + n1 + n2 - 1) adjacent column pairs can share height <= 2.
          const int maxFlatPairs = std::max(0, n0 + n1 + n2 - 1);
          const double flatTerm = (w.safeFlatPairBonus > 0.0)
                                      ? static_cast<double>(maxFlatPairs) * w.safeFlatPairBonus
                                      : 0.0;

          // In row r, a column can have a filled cell only if its height >= r + 1, and every column
          // of exact height r + 1 has its top cell filled in row r (so n_{r+1} <= c_r <= min(9,
          // sum_{k>r} n_k)).
          double rowFillTerm = 0.0;
          if (w.rowFillSq >= 0.0) {
            const int maxC3 = std::min(MAX_ROW_FILL_POST_CLEAR, n4);
            const int maxC2 = std::min(MAX_ROW_FILL_POST_CLEAR, n3 + n4);
            const int maxC1 = std::min(MAX_ROW_FILL_POST_CLEAR, n2 + n3 + n4);
            const int maxC0 = std::min(MAX_ROW_FILL_POST_CLEAR, n1 + n2 + n3 + n4);
            const int maxRowFillSq = maxC0 * maxC0 + maxC1 * maxC1 + maxC2 * maxC2 + maxC3 * maxC3;
            rowFillTerm = static_cast<double>(maxRowFillSq) * w.rowFillSq;
          } else {
            const int minC3 = n4;
            const int minC2 = n3;
            const int minC1 = n2;
            const int minC0 = n1;
            // On a post-clear board n4 <= 9, and if n_k == 10 (all cols equal height k in 1..3)
            // that row would have cleared; clamping to 9 keeps the bound valid on all post-clear
            // boards.
            const int c3 = std::min(MAX_ROW_FILL_POST_CLEAR, minC3);
            const int c2 = std::min(MAX_ROW_FILL_POST_CLEAR, minC2);
            const int c1 = std::min(MAX_ROW_FILL_POST_CLEAR, minC1);
            const int c0 = std::min(MAX_ROW_FILL_POST_CLEAR, minC0);
            const int minRowFillSq = c0 * c0 + c1 * c1 + c2 * c2 + c3 * c3;
            rowFillTerm = static_cast<double>(minRowFillSq) * w.rowFillSq;
          }

          const double coupled = w.heightSq * static_cast<double>(sumHSq)
                                 + w.height3Col * static_cast<double>(n3)
                                 + w.ceilingCol * static_cast<double>(n4)
                                 + flatTerm
                                 + rowFillTerm;
          if (coupled > bestCoupled) {
            bestCoupled = coupled;
          }
        }
      }
    }
  }

  return std::max(w.deathPenalty, uncoupledMax + bestCoupled);
}

// Piece evaluation order for 2-ply expectimax: test O, S, Z first because they top out most
// frequently on dangerous surfaces, triggering early branch-and-bound cutoffs sooner.
inline constexpr int EXPECTIMAX_PIECE_ORDER[NUM_PIECES] = {
    parsePieceChar('O'), parsePieceChar('S'), parsePieceChar('Z'), parsePieceChar('L'),
    parsePieceChar('J'), parsePieceChar('T'), parsePieceChar('I'),
};

inline double bestReplyScore1Ply(uint64_t board, int pieceIdx, const EvalWeights &w,
                                 TransitionList &scratch) {
  generateTransitions(board, pieceIdx, scratch);
  if (scratch.empty()) {
    return w.deathPenalty;
  }
  double best = scoreTransition1Ply(scratch[0].afterBoard, scratch[0].linesCleared, w);
  for (int j = 1; j < scratch.size(); ++j) {
    const double s = scoreTransition1Ply(scratch[j].afterBoard, scratch[j].linesCleared, w);
    if (s > best) {
      best = s;
    }
  }
  return best;
}

} // namespace detail

// Evaluates a single 1st-ply transition (afterBoard, linesCleared) using full 2-ply NNB expectimax:
//   w.lineClear * r1 + (1/7) * sum_{c2} max_{b'' in Moves(afterBoard, c2)} score1Ply(b'', r2).
// Accumulates in EXPECTIMAX_PIECE_ORDER starting from 7 * w.lineClear * r1 so scores match
// pickBestTransition2Ply bit-for-bit.
inline double scoreTransition2Ply(uint64_t afterBoard, int linesCleared,
                                  const EvalWeights &w = EvalWeights{}) {
  TransitionList tl2;
  double scaledSum =
      static_cast<double>(NUM_PIECES) * w.lineClear * static_cast<double>(linesCleared);
  for (int k = 0; k < NUM_PIECES; ++k) {
    scaledSum += detail::bestReplyScore1Ply(afterBoard, detail::EXPECTIMAX_PIECE_ORDER[k], w, tl2);
  }
  return scaledSum * (1.0 / static_cast<double>(NUM_PIECES));
}

struct ChosenTransition {
  Transition transition{0ULL, 0};
  double score = 0.0;
  bool found = false;
};

inline ChosenTransition pickBestTransition1Ply(uint64_t board, int pieceIdx,
                                               const EvalWeights &w = EvalWeights{}) {
  TransitionList tl;
  generateTransitions(board, pieceIdx, tl);
  if (tl.empty()) {
    return {};
  }
  ChosenTransition best{tl[0], scoreTransition1Ply(tl[0].afterBoard, tl[0].linesCleared, w), true};
  for (int i = 1; i < tl.size(); ++i) {
    const double s = scoreTransition1Ply(tl[i].afterBoard, tl[i].linesCleared, w);
    if (s > best.score) {
      best = {tl[i], s, true};
    }
  }
  return best;
}

// Exact 2-ply NNB expectimax move selector with lossless branch-and-bound pruning across second-ply
// pieces. Returns the exact same winning Transition and score (bit-for-bit) as scoring every
// candidate with scoreTransition2Ply.
inline ChosenTransition pickBestTransition2Ply(uint64_t board, int pieceIdx,
                                               const EvalWeights &w = EvalWeights{},
                                               bool enablePruning = true) {
  TransitionList tl1;
  generateTransitions(board, pieceIdx, tl1);
  if (tl1.empty()) {
    return {};
  }

  struct RootCand {
    Transition t;
    double score1;
    int origIdx;
  };
  RootCand cands[MAX_TRANSITIONS];
  const int n1 = tl1.size();
  for (int i = 0; i < n1; ++i) {
    cands[i] = {tl1[i], scoreTransition1Ply(tl1[i].afterBoard, tl1[i].linesCleared, w), i};
  }
  if (enablePruning && n1 > 1) {
    std::sort(cands, cands + n1, [](const RootCand &a, const RootCand &b) {
      if (a.score1 != b.score1)
        return a.score1 > b.score1;
      return a.origIdx < b.origIdx;
    });
  }

  const double maxLeaf = detail::maxPossible1PlyScore(w);

  TransitionList tl2;
  ChosenTransition best{};
  int bestOrigIdx = n1;
  double bestScaledTotal = 0.0;

  for (int i = 0; i < n1; ++i) {
    const Transition &t1 = cands[i].t;
    double scaledSum =
        static_cast<double>(NUM_PIECES) * w.lineClear * static_cast<double>(t1.linesCleared);
    bool pruned = false;

    for (int k = 0; k < NUM_PIECES; ++k) {
      if (enablePruning && best.found) {
        const int rem = NUM_PIECES - k;
        const double pruneEps =
            1e-9 * (1.0 + std::abs(bestScaledTotal) + static_cast<double>(rem) * std::abs(maxLeaf));
        if (scaledSum + static_cast<double>(rem) * maxLeaf < bestScaledTotal - pruneEps) {
          pruned = true;
          break;
        }
      }
      scaledSum +=
          detail::bestReplyScore1Ply(t1.afterBoard, detail::EXPECTIMAX_PIECE_ORDER[k], w, tl2);
    }

    if (!pruned) {
      const double score = scaledSum * (1.0 / static_cast<double>(NUM_PIECES));
      if (!best.found
          || score > best.score
          || (score == best.score && cands[i].origIdx < bestOrigIdx)) {
        bestScaledTotal = scaledSum;
        bestOrigIdx = cands[i].origIdx;
        best = {t1, score, true};
      }
    }
  }
  return best;
}

// Evaluates placing the current piece into (afterBoard, linesCleared) when nextPiece is known:
//   w.lineClear * r1 + max_{b'' in Moves(afterBoard, nextPiece)} [w.lineClear * r2 + eval(b'')],
// returning w.lineClear * r1 + w.deathPenalty if nextPiece has no legal placement on afterBoard.
inline double scoreNextBoxHeuristicTransition(uint64_t afterBoard, int linesCleared, int nextPiece,
                                              const EvalWeights &w = EvalWeights{}) {
  TransitionList tl2;
  return w.lineClear * static_cast<double>(linesCleared)
         + detail::bestReplyScore1Ply(afterBoard, nextPiece, w, tl2);
}

// 2-ply Next-Box heuristic selector: chooses the placement of curPiece maximizing the combined
// two-piece score with the known nextPiece (breaking exact ties in move-generation order).
inline ChosenTransition pickBestNextBoxHeuristic(uint64_t board, int curPiece, int nextPiece,
                                                 const EvalWeights &w = EvalWeights{}) {
  TransitionList tl1;
  generateTransitions(board, curPiece, tl1);
  if (tl1.empty()) {
    return {};
  }
  TransitionList tl2;
  ChosenTransition best{};
  for (int i = 0; i < tl1.size(); ++i) {
    const double s = w.lineClear * static_cast<double>(tl1[i].linesCleared)
                     + detail::bestReplyScore1Ply(tl1[i].afterBoard, nextPiece, w, tl2);
    if (!best.found || s > best.score) {
      best = {tl1[i], s, true};
    }
  }
  return best;
}

} // namespace tb

#endif // TABLEBASE_HEURISTIC_HPP
