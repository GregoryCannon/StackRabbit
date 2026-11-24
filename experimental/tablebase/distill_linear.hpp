#ifndef TABLEBASE_DISTILL_LINEAR_HPP
#define TABLEBASE_DISTILL_LINEAR_HPP

#include "core.hpp"
#include "distill_data.hpp"
#include "heuristic.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <vector>

namespace tb {

// Exact 10 board-only features matching EvalWeights in heuristic.hpp (excludes lineClear and
// deathPenalty).
inline constexpr int V1_NUM_BOARD_FEATURES = 10;

inline constexpr const char *V1_FEATURE_NAMES[V1_NUM_BOARD_FEATURES] = {
    "sealedHoles", "overhangs",   "sumHeightSq",     "height3Cols",   "ceilingCols",
    "bumpiness",   "wellDepthSq", "parityImbalance", "safeFlatPairs", "rowFillSq",
};

inline void extractV1BoardFeatures(uint64_t board, double *out) {
  const HeuristicBreakdown hb = evaluateBoardBreakdown(board);
  out[0] = static_cast<double>(hb.sealedHoles);
  out[1] = static_cast<double>(hb.overhangs);
  out[2] = static_cast<double>(hb.sumHeightSq);
  out[3] = static_cast<double>(hb.height3Cols);
  out[4] = static_cast<double>(hb.ceilingCols);
  out[5] = static_cast<double>(hb.bumpiness);
  out[6] = static_cast<double>(hb.wellDepthSq);
  out[7] = static_cast<double>(hb.parityImbalance);
  out[8] = static_cast<double>(hb.safeFlatPairs);
  out[9] = static_cast<double>(hb.rowFillSq);
}

// Interpretable 26-feature linear board representation:
//   [0..9]   : All 10 V1 features (nested superset for exact A/B comparison)
//   [10..16] : Hole clustering, burial depth, and covering-row clearability (distinguishes
//              buried row-0 pinholes from contiguous cavities under near-full covering rows)
//   [17..25] : Surface geometry, transitions, edge/center heights, and ceiling-blockage density
enum V2FeatureIdx : int {
  V2_SEALED_HOLES = 0,
  V2_OVERHANGS = 1,
  V2_SUM_HEIGHT_SQ = 2,
  V2_HEIGHT3_COLS = 3,
  V2_CEILING_COLS = 4,
  V2_BUMPINESS = 5,
  V2_WELL_DEPTH_SQ = 6,
  V2_PARITY_IMBALANCE = 7,
  V2_SAFE_FLAT_PAIRS = 8,
  V2_ROW_FILL_SQ = 9,
  V2_HAS_ANY_HOLE = 10,
  V2_HOLE_COLUMNS = 11,
  V2_HOLE_ROWS = 12,
  V2_COVERED_MINO_COUNT = 13,
  V2_BURIED_DEPTH_2PLUS = 14,
  V2_COVER_ROW_MISSING = 15,
  V2_MIN_HOLE_ROW_PENALTY = 16,
  V2_TOTAL_CELLS = 17,
  V2_MAX_HEIGHT = 18,
  V2_ROW_TRANSITIONS = 19,
  V2_COL_TRANSITIONS = 20,
  V2_EDGE_HEIGHT_SUM = 21,
  V2_CENTER_HEIGHT_SUM = 22,
  V2_WELL_COUNT = 23,
  V2_MAX_WELL_DEPTH = 24,
  V2_ADJ_HIGH_COL_PAIRS = 25,
  V2_NUM_BOARD_FEATURES = 26,
};

inline constexpr const char *V2_FEATURE_NAMES[V2_NUM_BOARD_FEATURES] = {
    "sealedHoles",       "overhangs",        "sumHeightSq",      "height3Cols",
    "ceilingCols",       "bumpiness",        "wellDepthSq",      "parityImbalance",
    "safeFlatPairs",     "rowFillSq",        "hasAnyHole",       "holeColumns",
    "holeRows",          "coveredMinoCount", "buriedDepth2Plus", "coverRowMissing",
    "minHoleRowPenalty", "totalCells",       "maxHeight",        "rowTransitions",
    "colTransitions",    "edgeHeightSum",    "centerHeightSum",  "wellCount",
    "maxWellDepth",      "adjHighColPairs",
};

inline void extractV2BoardFeatures(uint64_t board, double *out) {
  static_assert(BOARD_WIDTH == 10, "centerHeightSum assumes 10-column Tetris board");
  const uint64_t holeMask = computeHoleMask(board);
  const uint64_t ohMask = computeOverhangMask(board);
  const int holes = popcount64(holeMask);
  const int overhangs = popcount64(ohMask);
  const int sealedHoles = holes - overhangs;

  const uint64_t rows[BOARD_ROWS] = {
      getRow(board, 0),
      getRow(board, 1),
      getRow(board, 2),
      getRow(board, 3),
  };

  int heights[BOARD_WIDTH];
  int sumHeightSq = 0;
  int height3Cols = 0;
  int ceilingCols = 0;
  int maxHeight = 0;
  int evenSum = 0;
  int oddSum = 0;
  int holeColumns = 0;
  int coveredMinoCount = 0;
  int buriedDepth2Plus = 0;
  int colTransitions = 0;

  for (int col = 0; col < BOARD_WIDTH; ++col) {
    const uint64_t bit = 1ULL << (BOARD_WIDTH - 1 - col);
    const int c0 = (rows[0] & bit) ? 1 : 0;
    const int c1 = (rows[1] & bit) ? 1 : 0;
    const int c2 = (rows[2] & bit) ? 1 : 0;
    const int c3 = (rows[3] & bit) ? 1 : 0;
    const int h = c3 ? 4 : c2 ? 3 : c1 ? 2 : c0 ? 1 : 0;
    heights[col] = h;
    sumHeightSq += h * h;
    if (h > maxHeight)
      maxHeight = h;
    if (h == 3)
      ++height3Cols;
    else if (h == BOARD_ROWS)
      ++ceilingCols;
    if ((col & 1) == 0)
      evenSum += h;
    else
      oddSum += h;

    // Vertical transitions treating floor (below row 0) as filled and above row 3 as empty.
    const int cells[BOARD_ROWS] = {c0, c1, c2, c3};
    int prev = 1;
    for (int r = 0; r < BOARD_ROWS; ++r) {
      if (cells[r] != prev)
        ++colTransitions;
      prev = cells[r];
    }
    if (prev != 0)
      ++colTransitions;

    // Hole burial depth & covered minos strictly above the lowest hole in this column.
    bool colHasHole = false;
    int colCoveredMinos = 0;
    int filledAbove = 0;
    for (int r = h - 1; r >= 0; --r) {
      if (cells[r] != 0) {
        ++filledAbove;
      } else {
        colHasHole = true;
        colCoveredMinos = filledAbove;
        if (filledAbove >= 2) {
          ++buriedDepth2Plus;
        }
      }
    }
    if (colHasHole) {
      ++holeColumns;
      coveredMinoCount += colCoveredMinos;
    }
  }

  int bumpiness = 0;
  int safeFlatPairs = 0;
  int wellDepthSq = 0;
  int wellCount = 0;
  int maxWellDepth = 0;
  int adjHighColPairs = 0;
  for (int col = 0; col < BOARD_WIDTH; ++col) {
    const int h = heights[col];
    if (col + 1 < BOARD_WIDTH) {
      const int nextH = heights[col + 1];
      bumpiness += std::abs(h - nextH);
      if (h == nextH && h <= 2)
        ++safeFlatPairs;
      if (h >= 3 && nextH >= 3)
        ++adjHighColPairs;
    }
    const int leftH = (col == 0) ? BOARD_ROWS : heights[col - 1];
    const int rightH = (col + 1 == BOARD_WIDTH) ? BOARD_ROWS : heights[col + 1];
    const int depth = std::min(leftH, rightH) - h;
    if (depth >= 2) {
      wellDepthSq += depth * depth;
      ++wellCount;
      if (depth > maxWellDepth)
        maxWellDepth = depth;
    }
  }

  int totalCells = 0;
  int rowFillSq = 0;
  int rowTransitions = 0;
  int holeRows = 0;
  int highestHoleRow = -1;
  int lowestHoleRowOverall = -1;
  for (int r = 0; r < BOARD_ROWS; ++r) {
    const int cnt = popcount64(rows[r]);
    totalCells += cnt;
    rowFillSq += cnt * cnt;
    const uint64_t rHole = getRow(holeMask, r);
    if (rHole != 0) {
      ++holeRows;
      highestHoleRow = r;
      if (lowestHoleRowOverall < 0)
        lowestHoleRowOverall = r;
    }
    // Horizontal transitions treating both walls as filled.
    int prev = 1;
    for (int col = 0; col < BOARD_WIDTH; ++col) {
      const int cur = (rows[r] & (1ULL << (BOARD_WIDTH - 1 - col))) ? 1 : 0;
      if (cur != prev)
        ++rowTransitions;
      prev = cur;
    }
    if (prev != 1)
      ++rowTransitions;
  }

  // Empty cells missing from the row immediately covering the highest hole (0 if no holes).
  int coverRowMissing = 0;
  if (highestHoleRow >= 0 && highestHoleRow + 1 < BOARD_ROWS) {
    coverRowMissing = BOARD_WIDTH - popcount64(rows[highestHoleRow + 1]);
  }
  // Row-0 holes are hardest to uncover (penalty = 3), row-1 = 2, row-2 = 1, no holes = 0.
  const int minHoleRowPenalty =
      (lowestHoleRowOverall >= 0) ? (BOARD_ROWS - 1 - lowestHoleRowOverall) : 0;

  out[V2_SEALED_HOLES] = static_cast<double>(sealedHoles);
  out[V2_OVERHANGS] = static_cast<double>(overhangs);
  out[V2_SUM_HEIGHT_SQ] = static_cast<double>(sumHeightSq);
  out[V2_HEIGHT3_COLS] = static_cast<double>(height3Cols);
  out[V2_CEILING_COLS] = static_cast<double>(ceilingCols);
  out[V2_BUMPINESS] = static_cast<double>(bumpiness);
  out[V2_WELL_DEPTH_SQ] = static_cast<double>(wellDepthSq);
  out[V2_PARITY_IMBALANCE] = static_cast<double>(std::abs(evenSum - oddSum));
  out[V2_SAFE_FLAT_PAIRS] = static_cast<double>(safeFlatPairs);
  out[V2_ROW_FILL_SQ] = static_cast<double>(rowFillSq);

  out[V2_HAS_ANY_HOLE] = (holes > 0) ? 1.0 : 0.0;
  out[V2_HOLE_COLUMNS] = static_cast<double>(holeColumns);
  out[V2_HOLE_ROWS] = static_cast<double>(holeRows);
  out[V2_COVERED_MINO_COUNT] = static_cast<double>(coveredMinoCount);
  out[V2_BURIED_DEPTH_2PLUS] = static_cast<double>(buriedDepth2Plus);
  out[V2_COVER_ROW_MISSING] = static_cast<double>(coverRowMissing);
  out[V2_MIN_HOLE_ROW_PENALTY] = static_cast<double>(minHoleRowPenalty);
  out[V2_TOTAL_CELLS] = static_cast<double>(totalCells);
  out[V2_MAX_HEIGHT] = static_cast<double>(maxHeight);
  out[V2_ROW_TRANSITIONS] = static_cast<double>(rowTransitions);
  out[V2_COL_TRANSITIONS] = static_cast<double>(colTransitions);
  out[V2_EDGE_HEIGHT_SUM] = static_cast<double>(heights[0] + heights[BOARD_WIDTH - 1]);
  out[V2_CENTER_HEIGHT_SUM] =
      static_cast<double>(heights[3] + heights[4] + heights[5] + heights[6]);
  out[V2_WELL_COUNT] = static_cast<double>(wellCount);
  out[V2_MAX_WELL_DEPTH] = static_cast<double>(maxWellDepth);
  out[V2_ADJ_HIGH_COL_PAIRS] = static_cast<double>(adjHighColPairs);
}

// Calibrated linear board evaluator in expected-line units (top-out = 0.0 lines).
template <int NumFeatures, void (*ExtractFn)(uint64_t, double *)> struct LinearBoardEval {
  double intercept = 0.0;
  double lineClearWeight = 1.0;
  std::array<double, static_cast<size_t>(NumFeatures)> weights{};

  static LinearBoardEval fromVector(double b0, double lcW, const std::vector<double> &w) {
    assert(w.size() == static_cast<size_t>(NumFeatures));
    LinearBoardEval ev{};
    ev.intercept = b0;
    ev.lineClearWeight = lcW;
    const size_t n = std::min(w.size(), ev.weights.size());
    for (size_t i = 0; i < n; ++i) {
      ev.weights[i] = w[i];
    }
    return ev;
  }

  double evalBoard(uint64_t board) const {
    double f[NumFeatures];
    ExtractFn(board, f);
    double v = intercept;
    for (int i = 0; i < NumFeatures; ++i) {
      v += weights[static_cast<size_t>(i)] * f[i];
    }
    return std::max(0.0, v);
  }

  double scoreTransition1Ply(uint64_t afterBoard, int linesCleared) const {
    return lineClearWeight * static_cast<double>(linesCleared) + evalBoard(afterBoard);
  }

  double scoreNextBox2Ply(uint64_t afterBoard, int linesCleared, int nextPiece) const {
    TransitionList tl2;
    generateTransitions(afterBoard, nextPiece, tl2);
    const double r1 = lineClearWeight * static_cast<double>(linesCleared);
    if (tl2.empty()) {
      return r1; // Top-out contributes 0.0 continuation lines
    }
    double best2 = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < tl2.size(); ++i) {
      const double sc = scoreTransition1Ply(tl2[i].afterBoard, tl2[i].linesCleared);
      if (sc > best2) {
        best2 = sc;
      }
    }
    return r1 + best2;
  }

  ChosenTransition pickBestNextBox2Ply(uint64_t board, int curPiece, int nextPiece) const {
    TransitionList tl1;
    generateTransitions(board, curPiece, tl1);
    if (tl1.empty()) {
      return {};
    }
    ChosenTransition best{};
    for (int i = 0; i < tl1.size(); ++i) {
      const double s = scoreNextBox2Ply(tl1[i].afterBoard, tl1[i].linesCleared, nextPiece);
      if (!best.found || s > best.score) {
        best = {tl1[i], s, true};
      }
    }
    return best;
  }
};

using LinearBoardEvalV1 = LinearBoardEval<V1_NUM_BOARD_FEATURES, extractV1BoardFeatures>;
using LinearBoardEvalV2 = LinearBoardEval<V2_NUM_BOARD_FEATURES, extractV2BoardFeatures>;

// Distilled 26-feature V2 linear evaluator trained via Boltzmann regret-weighted move ranking
// (T=1.0) on nb_final_nes.tb.bin (48.94 +- 0.46 lines across 10,000 held-out NES games, vs.
// 30.05 +- 0.28 for the original 10-feature EvalWeights baseline).
inline constexpr LinearBoardEvalV2 defaultV2LinearEval() {
  LinearBoardEvalV2 ev{};
  ev.intercept = 136.3784;
  ev.lineClearWeight = 1.0;
  ev.weights = {
      -0.6837, // sealedHoles
      -0.2083, // overhangs
      +0.0197, // sumHeightSq
      -0.3444, // height3Cols
      -0.5520, // ceilingCols
      -0.1242, // bumpiness
      -0.4305, // wellDepthSq
      -0.0815, // parityImbalance
      -0.3706, // safeFlatPairs
      -0.0156, // rowFillSq
      +1.3696, // hasAnyHole
      -0.3714, // holeColumns
      -2.2116, // holeRows
      -0.9559, // coveredMinoCount
      -0.8264, // buriedDepth2Plus
      -0.1795, // coverRowMissing
      -0.2760, // minHoleRowPenalty
      -0.0308, // totalCells
      +0.0297, // maxHeight
      -1.1355, // rowTransitions
      +0.0866, // colTransitions
      -0.2342, // edgeHeightSum
      +0.0781, // centerHeightSum
      +0.4574, // wellCount
      -0.0216, // maxWellDepth
      -0.2246, // adjHighColPairs
  };
  return ev;
}

// Solves any nonsingular dense linear system A * x = b (dim x dim) using partial-pivoting
// Gaussian elimination in double precision. Returns false if singular/non-finite.
inline bool solveLinearSystem(std::vector<double> a, std::vector<double> b, int dim,
                              std::vector<double> &outX) {
  if (dim <= 0
      || a.size() != static_cast<size_t>(dim) * static_cast<size_t>(dim)
      || b.size() != static_cast<size_t>(dim)) {
    return false;
  }
  const auto idx = [dim](int r, int c) -> size_t {
    return static_cast<size_t>(r) * static_cast<size_t>(dim) + static_cast<size_t>(c);
  };

  for (int col = 0; col < dim; ++col) {
    int pivot = col;
    double bestAbs = std::abs(a[idx(col, col)]);
    for (int r = col + 1; r < dim; ++r) {
      const double v = std::abs(a[idx(r, col)]);
      if (v > bestAbs) {
        bestAbs = v;
        pivot = r;
      }
    }
    if (!std::isfinite(bestAbs) || bestAbs < 1e-14) {
      return false;
    }
    if (pivot != col) {
      for (int c = col; c < dim; ++c) {
        std::swap(a[idx(col, c)], a[idx(pivot, c)]);
      }
      std::swap(b[static_cast<size_t>(col)], b[static_cast<size_t>(pivot)]);
    }
    const double diag = a[idx(col, col)];
    for (int r = col + 1; r < dim; ++r) {
      const double factor = a[idx(r, col)] / diag;
      if (factor == 0.0) {
        continue;
      }
      a[idx(r, col)] = 0.0;
      for (int c = col + 1; c < dim; ++c) {
        a[idx(r, c)] -= factor * a[idx(col, c)];
      }
      b[static_cast<size_t>(r)] -= factor * b[static_cast<size_t>(col)];
    }
  }

  outX.assign(static_cast<size_t>(dim), 0.0);
  for (int r = dim - 1; r >= 0; --r) {
    double rhs = b[static_cast<size_t>(r)];
    for (int c = r + 1; c < dim; ++c) {
      rhs -= a[idx(r, c)] * outX[static_cast<size_t>(c)];
    }
    const double val = rhs / a[idx(r, r)];
    if (!std::isfinite(val)) {
      return false;
    }
    outX[static_cast<size_t>(r)] = val;
  }
  return true;
}

struct RidgeRegressionResult {
  bool ok = false;
  double intercept = 0.0;
  std::vector<double> weights; // Size numFeatures
  double r2 = 0.0;
  double rmse = 0.0;
  double mae = 0.0;
  std::array<double, NUM_HOLE_BUCKETS> maeByHoles{};
  std::array<uint64_t, NUM_HOLE_BUCKETS> countByHoles{};
};

struct ResidualOutlier {
  uint64_t board = 0;
  double vStar = 0.0;
  double vHat = 0.0;
  double error = 0.0; // vHat - vStar (+ means heuristic overvalues a trap; - means undervalues)
};

// Evaluates a linear value model on a labeled sample set, computing R^2, RMSE, MAE, and
// hole-stratified MAE.
template <typename FeatureFn>
inline void evaluateRidgeFit(const std::vector<LabeledDecisionSample> &samples, int numFeatures,
                             double intercept, const std::vector<double> &weights,
                             FeatureFn &&extractFeatures, RidgeRegressionResult &res) {
  res.r2 = 0.0;
  res.rmse = 0.0;
  res.mae = 0.0;
  res.maeByHoles.fill(0.0);
  res.countByHoles.fill(0);
  if (numFeatures <= 0 || weights.size() != static_cast<size_t>(numFeatures)) {
    return;
  }

  std::vector<double> feat(static_cast<size_t>(numFeatures), 0.0);
  size_t used = 0;
  double meanY = 0.0;
  for (const auto &s : samples) {
    if (s.boardInTb) {
      meanY += s.expectedVStar;
      ++used;
    }
  }
  if (used == 0) {
    return;
  }
  meanY /= static_cast<double>(used);

  double ssRes = 0.0;
  double ssTot = 0.0;
  double sumAbs = 0.0;
  std::array<double, NUM_HOLE_BUCKETS> absByHoles{};
  for (const auto &s : samples) {
    if (!s.boardInTb) {
      continue;
    }
    extractFeatures(s.board, feat.data());
    double pred = intercept;
    for (int j = 0; j < numFeatures; ++j) {
      pred += weights[static_cast<size_t>(j)] * feat[static_cast<size_t>(j)];
    }
    const double err = pred - s.expectedVStar;
    ssRes += err * err;
    const double dy = s.expectedVStar - meanY;
    ssTot += dy * dy;
    sumAbs += std::abs(err);
    const int hb = HoleStratifiedCounts::bucket(countHoles(s.board));
    absByHoles[static_cast<size_t>(hb)] += std::abs(err);
    ++res.countByHoles[static_cast<size_t>(hb)];
  }
  res.rmse = std::sqrt(ssRes / static_cast<double>(used));
  res.mae = sumAbs / static_cast<double>(used);
  res.r2 = (ssTot > 0.0) ? (1.0 - ssRes / ssTot) : ((ssRes == 0.0) ? 1.0 : 0.0);
  for (size_t b = 0; b < NUM_HOLE_BUCKETS; ++b) {
    res.maeByHoles[b] = (res.countByHoles[b] > 0)
                            ? (absByHoles[b] / static_cast<double>(res.countByHoles[b]))
                            : 0.0;
  }
}

// Fits y ≈ intercept + sum_j weights[j] * x[j] with L2 ridge penalty (unpenalized intercept).
template <typename FeatureFn>
inline RidgeRegressionResult fitRidgeValueModel(const std::vector<LabeledDecisionSample> &samples,
                                                int numFeatures, double ridgeLambda,
                                                FeatureFn &&extractFeatures) {
  RidgeRegressionResult res{};
  if (numFeatures <= 0 || samples.empty() || !std::isfinite(ridgeLambda) || ridgeLambda < 0.0) {
    return res;
  }
  const int dim = numFeatures + 1;
  std::vector<double> xtx(static_cast<size_t>(dim) * static_cast<size_t>(dim), 0.0);
  std::vector<double> xty(static_cast<size_t>(dim), 0.0);
  std::vector<double> row(static_cast<size_t>(dim), 0.0);

  size_t used = 0;
  for (const auto &s : samples) {
    if (!s.boardInTb) {
      continue;
    }
    row[0] = 1.0;
    extractFeatures(s.board, row.data() + 1);
    const double y = s.expectedVStar;
    for (int i = 0; i < dim; ++i) {
      const double ri = row[static_cast<size_t>(i)];
      xty[static_cast<size_t>(i)] += ri * y;
      double *xtxRow = &xtx[static_cast<size_t>(i) * static_cast<size_t>(dim)];
      for (int j = i; j < dim; ++j) {
        xtxRow[j] += ri * row[static_cast<size_t>(j)];
      }
    }
    ++used;
  }
  if (used == 0) {
    return res;
  }

  for (int i = 0; i < dim; ++i) {
    for (int j = 0; j < i; ++j) {
      xtx[static_cast<size_t>(i) * static_cast<size_t>(dim) + static_cast<size_t>(j)] =
          xtx[static_cast<size_t>(j) * static_cast<size_t>(dim) + static_cast<size_t>(i)];
    }
    if (i > 0) {
      xtx[static_cast<size_t>(i) * static_cast<size_t>(dim) + static_cast<size_t>(i)] +=
          ridgeLambda;
    }
  }

  std::vector<double> sol;
  if (!solveLinearSystem(std::move(xtx), std::move(xty), dim, sol)) {
    return res;
  }
  res.ok = true;
  res.intercept = sol[0];
  res.weights.assign(sol.begin() + 1, sol.end());
  evaluateRidgeFit(samples, numFeatures, res.intercept, res.weights, extractFeatures, res);
  return res;
}

// Collects unique boards from samples with the largest positive (overestimated traps) and negative
// (underestimated strong setups) residuals.
template <typename FeatureFn>
inline void
collectResidualOutliers(const std::vector<LabeledDecisionSample> &samples, int numFeatures,
                        double intercept, const std::vector<double> &weights, int topK,
                        FeatureFn &&extractFeatures, std::vector<ResidualOutlier> &outOverestimated,
                        std::vector<ResidualOutlier> &outUnderestimated) {
  outOverestimated.clear();
  outUnderestimated.clear();
  if (numFeatures <= 0 || weights.size() != static_cast<size_t>(numFeatures) || topK <= 0) {
    return;
  }
  std::vector<ResidualOutlier> all;
  all.reserve(samples.size());
  std::vector<double> feat(static_cast<size_t>(numFeatures), 0.0);

  for (const auto &s : samples) {
    if (!s.boardInTb) {
      continue;
    }
    extractFeatures(s.board, feat.data());
    double pred = intercept;
    for (int j = 0; j < numFeatures; ++j) {
      pred += weights[static_cast<size_t>(j)] * feat[static_cast<size_t>(j)];
    }
    all.push_back({s.board, s.expectedVStar, pred, pred - s.expectedVStar});
  }
  std::sort(all.begin(), all.end(),
            [](const ResidualOutlier &a, const ResidualOutlier &b) { return a.board < b.board; });
  all.erase(std::unique(all.begin(), all.end(),
                        [](const ResidualOutlier &a, const ResidualOutlier &b) {
                          return a.board == b.board;
                        }),
            all.end());

  const size_t k = std::min<size_t>(static_cast<size_t>(topK), all.size());
  std::sort(all.begin(), all.end(),
            [](const ResidualOutlier &a, const ResidualOutlier &b) { return a.error < b.error; });
  outUnderestimated.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(k));
  outOverestimated.assign(all.rbegin(), all.rbegin() + static_cast<std::ptrdiff_t>(k));
}

inline constexpr int LINEAR_PARAM_INTERCEPT = 0;
inline constexpr int LINEAR_PARAM_LINE_CLEAR = 1;
inline constexpr int LINEAR_PARAM_BOARD_OFFSET = 2;

struct LinearRankingConfig {
  double temperature = 1.0;      // Boltzmann target sharpness in lines
  double valueLossWeight = 0.02; // Anchors intercept & scale to calibrated expected lines
  double l2Reg = 1e-4;
  double learningRate = 0.05;
  int epochs = 25;
  int batchSize = 512;
  bool useExpectedTarget = true; // Use piece-averaged r + E[V*(b')] (clean for board-only V(b))
  bool fixLineClearToOne = true; // Keep calibrated in exact line-count units
  uint64_t seed = 12345ULL;
  bool verbose = false;
};

struct LinearRankingResult {
  double intercept = 0.0;
  double lineClearWeight = 1.0;
  std::vector<double> boardWeights; // Size numBoardFeatures
  double finalTrainLoss = 0.0;
  double bestValLoss = 0.0;
};

// Evaluates exact joint loss & gradient on a slice of decisions:
//   L = L_Boltzmann_rank(s, Q*) + valueLossWeight * 0.5 * (V_hat(b) - E[V*(b)])^2 + 0.5*l2*||w||^2
// Parameter layout in fullWeights: [0] = intercept, [1] = lineClearWeight, [2..dim-1] = boardW.
template <typename BoardFeatureFn>
inline double computeLinearJointLossAndGrad(
    const std::vector<LabeledDecisionSample> &decisions, const std::vector<size_t> &indices,
    size_t start, size_t end, const std::vector<double> &fullWeights, double temperature,
    double valueLossWeight, double l2Reg, bool useExpectedTarget, bool fixLineClearToOne,
    BoardFeatureFn &&extractBoardFeatures, std::vector<double> &outGrad) {
  const int dim = static_cast<int>(fullWeights.size());
  assert(dim >= LINEAR_PARAM_BOARD_OFFSET + 1);
  assert(std::isfinite(temperature) && temperature > 0.0);
  outGrad.assign(fullWeights.size(), 0.0);
  if (dim < LINEAR_PARAM_BOARD_OFFSET + 1
      || !std::isfinite(temperature)
      || temperature <= 0.0
      || start >= end
      || end > indices.size()) {
    return 0.0;
  }
  const int numBoardFeat = dim - LINEAR_PARAM_BOARD_OFFSET;
  const size_t count = end - start;

  std::vector<double> featBuf;
  std::vector<double> boardFeat(static_cast<size_t>(numBoardFeat), 0.0);
  std::vector<double> scores;
  std::vector<double> targetProbs;
  std::vector<double> predProbs;

  double totalLoss = 0.0;
  const double invTemp = 1.0 / temperature;
  const double intercept = fullWeights[LINEAR_PARAM_INTERCEPT];
  const double lineClearW = fullWeights[LINEAR_PARAM_LINE_CLEAR];

  for (size_t idxPos = start; idxPos < end; ++idxPos) {
    const LabeledDecisionSample &d = decisions[indices[idxPos]];
    const size_t mCount = d.moves.size();
    if (mCount >= 2) {
      featBuf.resize(mCount * static_cast<size_t>(numBoardFeat));
      scores.resize(mCount);
      targetProbs.resize(mCount);
      predProbs.resize(mCount);

      // Compute maxQ directly across moves so shift-invariant softmax never overflows on custom
      // samples; intercept is omitted from scores[m] for the same shift-invariance reason.
      double maxQ = -std::numeric_limits<double>::infinity();
      double maxScore = -std::numeric_limits<double>::infinity();
      for (size_t m = 0; m < mCount; ++m) {
        double *f = &featBuf[m * static_cast<size_t>(numBoardFeat)];
        extractBoardFeatures(d.moves[m].afterBoard, f);
        double sc = lineClearW * static_cast<double>(d.moves[m].linesCleared);
        for (int j = 0; j < numBoardFeat; ++j) {
          sc += fullWeights[static_cast<size_t>(j + LINEAR_PARAM_BOARD_OFFSET)] * f[j];
        }
        scores[m] = sc;
        if (sc > maxScore) {
          maxScore = sc;
        }
        const double qVal = useExpectedTarget ? d.moves[m].expectedQStar : d.moves[m].qStar;
        if (qVal > maxQ) {
          maxQ = qVal;
        }
      }

      double sumExpQ = 0.0;
      double sumExpS = 0.0;
      for (size_t m = 0; m < mCount; ++m) {
        const double qVal = useExpectedTarget ? d.moves[m].expectedQStar : d.moves[m].qStar;
        const double eq = std::exp((qVal - maxQ) * invTemp);
        targetProbs[m] = eq;
        sumExpQ += eq;
        const double es = std::exp((scores[m] - maxScore) * invTemp);
        predProbs[m] = es;
        sumExpS += es;
      }

      const double logSumExpS = std::log(sumExpS);
      for (size_t m = 0; m < mCount; ++m) {
        targetProbs[m] /= sumExpQ;
        predProbs[m] /= sumExpS;
        const double logP = (scores[m] - maxScore) * invTemp - logSumExpS;
        totalLoss -= targetProbs[m] * logP;
        const double dScore = (predProbs[m] - targetProbs[m]) * invTemp;
        outGrad[LINEAR_PARAM_LINE_CLEAR] += dScore * static_cast<double>(d.moves[m].linesCleared);
        const double *f = &featBuf[m * static_cast<size_t>(numBoardFeat)];
        for (int j = 0; j < numBoardFeat; ++j) {
          outGrad[static_cast<size_t>(j + LINEAR_PARAM_BOARD_OFFSET)] += dScore * f[j];
        }
      }
    }

    if (valueLossWeight > 0.0 && d.boardInTb) {
      extractBoardFeatures(d.board, boardFeat.data());
      double vHat = intercept;
      for (int j = 0; j < numBoardFeat; ++j) {
        vHat += fullWeights[static_cast<size_t>(j + LINEAR_PARAM_BOARD_OFFSET)]
                * boardFeat[static_cast<size_t>(j)];
      }
      const double diff = vHat - d.expectedVStar;
      totalLoss += 0.5 * valueLossWeight * diff * diff;
      const double dV = valueLossWeight * diff;
      outGrad[LINEAR_PARAM_INTERCEPT] += dV;
      for (int j = 0; j < numBoardFeat; ++j) {
        outGrad[static_cast<size_t>(j + LINEAR_PARAM_BOARD_OFFSET)] +=
            dV * boardFeat[static_cast<size_t>(j)];
      }
    }
  }

  const double invN = 1.0 / static_cast<double>(count);
  totalLoss *= invN;
  for (int j = 0; j < dim; ++j) {
    outGrad[static_cast<size_t>(j)] *= invN;
  }

  const int regStart = fixLineClearToOne ? LINEAR_PARAM_BOARD_OFFSET : LINEAR_PARAM_LINE_CLEAR;
  for (int j = regStart; j < dim; ++j) {
    const double wj = fullWeights[static_cast<size_t>(j)];
    totalLoss += 0.5 * l2Reg * wj * wj;
    outGrad[static_cast<size_t>(j)] += l2Reg * wj;
  }
  if (fixLineClearToOne) {
    outGrad[LINEAR_PARAM_LINE_CLEAR] = 0.0;
  }
  return totalLoss;
}

// Trains calibrated linear move-ranking weights with mini-batch Adam + cosine LR decay.
template <typename BoardFeatureFn>
inline LinearRankingResult
trainLinearRegretRanking(const std::vector<LabeledDecisionSample> &trainDecisions,
                         const std::vector<LabeledDecisionSample> &valDecisions,
                         int numBoardFeatures, double initIntercept,
                         const std::vector<double> &initBoardWeights, double initLineClearWeight,
                         const LinearRankingConfig &cfg, BoardFeatureFn &&extractBoardFeatures) {
  assert(numBoardFeatures > 0);
  assert(initBoardWeights.size() == static_cast<size_t>(numBoardFeatures));
  assert(std::isfinite(cfg.temperature) && cfg.temperature > 0.0);

  LinearRankingResult res{};
  if (numBoardFeatures <= 0
      || initBoardWeights.size() != static_cast<size_t>(numBoardFeatures)
      || !std::isfinite(cfg.temperature)
      || cfg.temperature <= 0.0) {
    return res;
  }

  const int dim = numBoardFeatures + LINEAR_PARAM_BOARD_OFFSET;
  std::vector<double> w(static_cast<size_t>(dim), 0.0);
  w[LINEAR_PARAM_INTERCEPT] = initIntercept;
  w[LINEAR_PARAM_LINE_CLEAR] = cfg.fixLineClearToOne ? 1.0 : initLineClearWeight;
  for (int j = 0; j < numBoardFeatures; ++j) {
    w[static_cast<size_t>(j + LINEAR_PARAM_BOARD_OFFSET)] =
        initBoardWeights[static_cast<size_t>(j)];
  }

  if (cfg.epochs <= 0 || trainDecisions.empty()) {
    res.intercept = w[LINEAR_PARAM_INTERCEPT];
    res.lineClearWeight = w[LINEAR_PARAM_LINE_CLEAR];
    res.boardWeights.assign(w.begin() + LINEAR_PARAM_BOARD_OFFSET, w.end());
    return res;
  }

  std::vector<size_t> trainOrder(trainDecisions.size());
  std::iota(trainOrder.begin(), trainOrder.end(), 0);
  std::vector<size_t> valOrder(valDecisions.size());
  std::iota(valOrder.begin(), valOrder.end(), 0);

  std::vector<double> m1(static_cast<size_t>(dim), 0.0);
  std::vector<double> m2(static_cast<size_t>(dim), 0.0);
  std::vector<double> grad(static_cast<size_t>(dim), 0.0);

  constexpr double BETA1 = 0.9;
  constexpr double BETA2 = 0.999;
  constexpr double EPS = 1e-8;
  constexpr double MIN_LR_FRAC = 0.05;
  constexpr double PI = 3.14159265358979323846;

  Rng rng(cfg.seed);
  const int batchSize = std::max(1, cfg.batchSize);
  const size_t batchesPerEpoch =
      (trainOrder.size() + static_cast<size_t>(batchSize) - 1) / static_cast<size_t>(batchSize);
  const size_t totalSteps = std::max<size_t>(1, static_cast<size_t>(cfg.epochs) * batchesPerEpoch);

  uint64_t stepNum = 0;
  double beta1Pow = 1.0;
  double beta2Pow = 1.0;
  double lastTrainLoss = 0.0;
  double bestValLoss = std::numeric_limits<double>::infinity();
  std::vector<double> bestW = w;

  for (int ep = 1; ep <= cfg.epochs; ++ep) {
    for (size_t i = trainOrder.size(); i > 1; --i) {
      const size_t j = static_cast<size_t>(rng.nextU64() % static_cast<uint64_t>(i));
      std::swap(trainOrder[i - 1], trainOrder[j]);
    }

    double epochLossSum = 0.0;
    size_t epochBatches = 0;
    for (size_t bStart = 0; bStart < trainOrder.size(); bStart += static_cast<size_t>(batchSize)) {
      const size_t bEnd = std::min(trainOrder.size(), bStart + static_cast<size_t>(batchSize));
      const double bLoss = computeLinearJointLossAndGrad(
          trainDecisions, trainOrder, bStart, bEnd, w, cfg.temperature, cfg.valueLossWeight,
          cfg.l2Reg, cfg.useExpectedTarget, cfg.fixLineClearToOne, extractBoardFeatures, grad);
      epochLossSum += bLoss;
      ++epochBatches;
      ++stepNum;
      beta1Pow *= BETA1;
      beta2Pow *= BETA2;

      const double progress = static_cast<double>(stepNum) / static_cast<double>(totalSteps);
      const double lr =
          cfg.learningRate
          * (MIN_LR_FRAC + (1.0 - MIN_LR_FRAC) * 0.5 * (1.0 + std::cos(PI * progress)));
      const double bc1 = 1.0 - beta1Pow;
      const double bc2 = 1.0 - beta2Pow;

      for (int j = 0; j < dim; ++j) {
        if (cfg.fixLineClearToOne && j == LINEAR_PARAM_LINE_CLEAR) {
          continue;
        }
        const double g = grad[static_cast<size_t>(j)];
        m1[static_cast<size_t>(j)] = BETA1 * m1[static_cast<size_t>(j)] + (1.0 - BETA1) * g;
        m2[static_cast<size_t>(j)] = BETA2 * m2[static_cast<size_t>(j)] + (1.0 - BETA2) * g * g;
        const double mHat = m1[static_cast<size_t>(j)] / bc1;
        const double vHat = m2[static_cast<size_t>(j)] / bc2;
        w[static_cast<size_t>(j)] -= lr * mHat / (std::sqrt(vHat) + EPS);
      }
    }

    lastTrainLoss = (epochBatches > 0) ? (epochLossSum / static_cast<double>(epochBatches)) : 0.0;
    double valLoss = lastTrainLoss;
    if (!valOrder.empty()) {
      valLoss = computeLinearJointLossAndGrad(
          valDecisions, valOrder, 0, valOrder.size(), w, cfg.temperature, cfg.valueLossWeight,
          cfg.l2Reg, cfg.useExpectedTarget, cfg.fixLineClearToOne, extractBoardFeatures, grad);
    }
    if (valLoss < bestValLoss) {
      bestValLoss = valLoss;
      bestW = w;
    }
    if (cfg.verbose && (ep == 1 || ep == cfg.epochs || ep % 5 == 0)) {
      std::printf("  [rank epoch %2d/%d] train_loss=%.5f | val_loss=%.5f | b0=%.2f lc=%.3f\n", ep,
                  cfg.epochs, lastTrainLoss, valLoss, w[LINEAR_PARAM_INTERCEPT],
                  w[LINEAR_PARAM_LINE_CLEAR]);
      std::fflush(stdout);
    }
  }

  res.intercept = bestW[LINEAR_PARAM_INTERCEPT];
  res.lineClearWeight = bestW[LINEAR_PARAM_LINE_CLEAR];
  res.boardWeights.assign(bestW.begin() + LINEAR_PARAM_BOARD_OFFSET, bestW.end());
  res.finalTrainLoss = lastTrainLoss;
  res.bestValLoss = bestValLoss;
  return res;
}

} // namespace tb

#endif // TABLEBASE_DISTILL_LINEAR_HPP
