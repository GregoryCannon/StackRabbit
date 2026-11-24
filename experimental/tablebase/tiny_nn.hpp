#ifndef TABLEBASE_TINY_NN_HPP
#define TABLEBASE_TINY_NN_HPP

#include "core.hpp"
#include "distill_data.hpp"
#include "nextbox_solver.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace tb {

// Compact cache-resident MLP predicting all 7 Next-Box values V_hat(b, 0..6) in expected lines.
//
// Input representation (NN_INPUT_DIM = 88 floats, featureVersion = 1):
//   [0..39]  : 40 binary board cells (row-major: r*10 + col)
//   [40..49] : 10 normalized column heights (h[col] / 4.0)
//   [50..59] : 10 normalized column hole counts (colHoles[col] / 3.0)
//   [60..63] : 4 normalized row fill fractions (popcount(row[r]) / 10.0)
//   [64..87] : 24 normalized global structural summaries (hole clustering/burial, cover-row
//              clearability, transitions, height/well profile, parity, flat pairs, ceiling density)
//
// Weight layout is chosen for sparsity-exploiting single-board inference:
//   W1: [inDim x h1]  (skips ~70% zero binary cells & zero heights/holes; contiguous SIMD over h1)
//   W2: [h1 x h2]     (skips ~50% ReLU-zero activations; contiguous SIMD over h2)
//   W3: [outDim x h2] (contiguous dot product over h2; evalPiece computes only 1 row instead of 7)

inline constexpr char NN_FILE_MAGIC[8] = {'S', 'R', 'N', 'N', 'V', '0', '0', '1'};
inline constexpr uint32_t NN_FEATURE_VERSION = 1;
inline constexpr int NN_FEAT_CELLS_OFF = 0;
inline constexpr int NN_FEAT_HEIGHTS_OFF = 40;
inline constexpr int NN_FEAT_COL_HOLES_OFF = 50;
inline constexpr int NN_FEAT_ROW_FILLS_OFF = 60;
inline constexpr int NN_FEAT_GLOBALS_OFF = 64;
inline constexpr int NN_INPUT_DIM = 88;
inline constexpr int NN_MAX_HIDDEN_DIM = 512;
inline constexpr int NN_FAST_STACK_SUM = 256; // Fast-path stack buffer (<4 KB total frame)

struct TinyNnHeader {
  char magic[8];
  uint32_t featureVersion;
  uint32_t inDim;
  uint32_t h1;
  uint32_t h2;
  uint32_t outDim;
  uint32_t rngMode; // 0 = Uniform, 1 = Nes
};
static_assert(sizeof(TinyNnHeader) == 32, "TinyNnHeader must be 32 bytes");

namespace detail {

struct ColMaskInfo {
  uint8_t height;
  uint8_t colHoles;
  uint8_t coveredMinos;
  uint8_t buriedDepth2Plus;
};

// Precomputed lookup table indexed by 4-bit column occupancy mask (bit r = row r filled).
inline constexpr std::array<ColMaskInfo, 16> makeColMaskLut() noexcept {
  std::array<ColMaskInfo, 16> lut{};
  for (int mask = 0; mask < 16; ++mask) {
    const int c0 = (mask >> 0) & 1;
    const int c1 = (mask >> 1) & 1;
    const int c2 = (mask >> 2) & 1;
    const int c3 = (mask >> 3) & 1;
    const int h = c3 ? 4 : c2 ? 3 : c1 ? 2 : c0 ? 1 : 0;
    const int cells[4] = {c0, c1, c2, c3};
    int colHoles = 0;
    int colCovered = 0;
    int buried2 = 0;
    int filledAbove = 0;
    for (int r = h - 1; r >= 0; --r) {
      if (cells[r] != 0) {
        ++filledAbove;
      } else {
        ++colHoles;
        colCovered = filledAbove;
        if (filledAbove >= 2) {
          ++buried2;
        }
      }
    }
    lut[static_cast<size_t>(mask)] = {
        static_cast<uint8_t>(h),
        static_cast<uint8_t>(colHoles),
        static_cast<uint8_t>(colCovered),
        static_cast<uint8_t>(buried2),
    };
  }
  return lut;
}

inline constexpr std::array<ColMaskInfo, 16> COL_MASK_LUT = makeColMaskLut();

// Deterministic 4-lane IEEE-754 dot product enabling NEON/SSE fmla without -ffast-math.
inline float dotProduct4(const float *__restrict__ a, const float *__restrict__ b,
                         int len) noexcept {
  float acc0 = 0.0f;
  float acc1 = 0.0f;
  float acc2 = 0.0f;
  float acc3 = 0.0f;
  int k = 0;
  for (; k + 3 < len; k += 4) {
    acc0 += a[k + 0] * b[k + 0];
    acc1 += a[k + 1] * b[k + 1];
    acc2 += a[k + 2] * b[k + 2];
    acc3 += a[k + 3] * b[k + 3];
  }
  float sum = (acc0 + acc1) + (acc2 + acc3);
  for (; k < len; ++k) {
    sum += a[k] * b[k];
  }
  return sum;
}

inline double huberLossAndGrad(double err, double delta, double &outDeriv) noexcept {
  const double absErr = std::abs(err);
  if (absErr <= delta) {
    outDeriv = err;
    return 0.5 * err * err;
  }
  outDeriv = (err > 0.0) ? delta : -delta;
  return delta * (absErr - 0.5 * delta);
}

} // namespace detail

inline void extractNnFeatures(uint64_t board, float out[NN_INPUT_DIM]) noexcept {
  static_assert(BOARD_WIDTH == 10 && BOARD_ROWS == 4,
                "extractNnFeatures assumes 4x10 Tetris board");
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

  int totalCells = 0;
  int rowFillSq = 0;
  int rowTransitions = 0;
  int holeRows = 0;
  int highestHoleRow = -1;
  int lowestHoleRowOverall = -1;
  for (int r = 0; r < BOARD_ROWS; ++r) {
    const uint64_t rowBits = rows[r];
    const int cnt = popcount64(rowBits);
    totalCells += cnt;
    rowFillSq += cnt * cnt;
    out[NN_FEAT_ROW_FILLS_OFF + r] = static_cast<float>(cnt) * (1.0f / 10.0f);
    if (getRow(holeMask, r) != 0) {
      ++holeRows;
      highestHoleRow = r;
      if (lowestHoleRowOverall < 0) {
        lowestHoleRowOverall = r;
      }
    }
    // Horizontal transitions treating both side walls (bits 0 and 11) as filled.
    const uint32_t padded = static_cast<uint32_t>((rowBits << 1) | 1ULL | (1ULL << 11));
    rowTransitions += __builtin_popcount((padded ^ (padded >> 1)) & 0x7FFU);

    float *rowOut = out + NN_FEAT_CELLS_OFF + r * BOARD_WIDTH;
    for (int col = 0; col < BOARD_WIDTH; ++col) {
      rowOut[col] = (rowBits & (1ULL << (BOARD_WIDTH - 1 - col))) ? 1.0f : 0.0f;
    }
  }

  // Vertical transitions treating floor (below row 0) as filled and above row 3 as empty.
  const uint64_t colTransMask = ((~rows[0]) & 0x3FFULL)
                                | ((rows[0] ^ rows[1]) << 10)
                                | ((rows[1] ^ rows[2]) << 20)
                                | ((rows[2] ^ rows[3]) << 30)
                                | (rows[3] << 40);
  const int colTransitions = popcount64(colTransMask);

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
  for (int col = 0; col < BOARD_WIDTH; ++col) {
    const int shift = BOARD_WIDTH - 1 - col;
    const int mask = static_cast<int>(((rows[0] >> shift) & 1ULL)
                                      | (((rows[1] >> shift) & 1ULL) << 1)
                                      | (((rows[2] >> shift) & 1ULL) << 2)
                                      | (((rows[3] >> shift) & 1ULL) << 3));
    const detail::ColMaskInfo info = detail::COL_MASK_LUT[static_cast<size_t>(mask)];
    const int h = static_cast<int>(info.height);
    heights[col] = h;
    sumHeightSq += h * h;
    if (h > maxHeight) {
      maxHeight = h;
    }
    if (h == 3) {
      ++height3Cols;
    } else if (h == BOARD_ROWS) {
      ++ceilingCols;
    }
    out[NN_FEAT_HEIGHTS_OFF + col] = static_cast<float>(h) * (1.0f / 4.0f);
    if ((col & 1) == 0) {
      evenSum += h;
    } else {
      oddSum += h;
    }
    if (info.colHoles > 0) {
      ++holeColumns;
    }
    out[NN_FEAT_COL_HOLES_OFF + col] = static_cast<float>(info.colHoles) * (1.0f / 3.0f);
    coveredMinoCount += static_cast<int>(info.coveredMinos);
    buriedDepth2Plus += static_cast<int>(info.buriedDepth2Plus);
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
      if (h == nextH && h <= 2) {
        ++safeFlatPairs;
      }
      if (h >= 3 && nextH >= 3) {
        ++adjHighColPairs;
      }
    }
    const int leftH = (col == 0) ? BOARD_ROWS : heights[col - 1];
    const int rightH = (col + 1 == BOARD_WIDTH) ? BOARD_ROWS : heights[col + 1];
    const int depth = std::min(leftH, rightH) - h;
    if (depth >= 2) {
      wellDepthSq += depth * depth;
      ++wellCount;
      if (depth > maxWellDepth) {
        maxWellDepth = depth;
      }
    }
  }

  int coverRowMissing = 0;
  if (highestHoleRow >= 0 && highestHoleRow + 1 < BOARD_ROWS) {
    coverRowMissing = BOARD_WIDTH - popcount64(rows[highestHoleRow + 1]);
  }
  const int minHoleRowPenalty =
      (lowestHoleRowOverall >= 0) ? (BOARD_ROWS - 1 - lowestHoleRowOverall) : 0;

  float *g = out + NN_FEAT_GLOBALS_OFF;
  g[0] = static_cast<float>(sealedHoles) * (1.0f / 6.0f);
  g[1] = static_cast<float>(overhangs) * (1.0f / 6.0f);
  g[2] = (holes > 0) ? 1.0f : 0.0f;
  g[3] = static_cast<float>(holeColumns) * (1.0f / 5.0f);
  g[4] = static_cast<float>(holeRows) * (1.0f / 3.0f);
  g[5] = static_cast<float>(coveredMinoCount) * (1.0f / 10.0f);
  g[6] = static_cast<float>(buriedDepth2Plus) * (1.0f / 6.0f);
  g[7] = static_cast<float>(coverRowMissing) * (1.0f / 10.0f);
  g[8] = static_cast<float>(minHoleRowPenalty) * (1.0f / 3.0f);
  g[9] = static_cast<float>(totalCells) * (1.0f / 40.0f);
  g[10] = static_cast<float>(sumHeightSq) * (1.0f / 100.0f);
  g[11] = static_cast<float>(maxHeight) * (1.0f / 4.0f);
  g[12] = static_cast<float>(height3Cols) * (1.0f / 10.0f);
  g[13] = static_cast<float>(ceilingCols) * (1.0f / 10.0f);
  g[14] = static_cast<float>(bumpiness) * (1.0f / 20.0f);
  g[15] = static_cast<float>(wellDepthSq) * (1.0f / 25.0f);
  g[16] = static_cast<float>(wellCount) * (1.0f / 4.0f);
  g[17] = static_cast<float>(maxWellDepth) * (1.0f / 4.0f);
  g[18] = static_cast<float>(safeFlatPairs) * (1.0f / 9.0f);
  g[19] = static_cast<float>(adjHighColPairs) * (1.0f / 9.0f);
  g[20] = static_cast<float>(rowFillSq) * (1.0f / 400.0f);
  g[21] = static_cast<float>(rowTransitions) * (1.0f / 30.0f);
  g[22] = static_cast<float>(colTransitions) * (1.0f / 30.0f);
  g[23] = static_cast<float>(std::abs(evenSum - oddSum)) * (1.0f / 10.0f);
}

class TinyNextBoxNet {
public:
  int inDim = NN_INPUT_DIM;
  int h1 = 0;
  int h2 = 0;
  int outDim = NUM_PIECES;
  RngMode rngMode = RngMode::Nes;

  // Flat contiguous parameter storage:
  //   W1[inDim * h1], b1[h1], W2[h1 * h2], b2[h2], W3[outDim * h2], b3[outDim]
  std::vector<float> params;

  bool isLoaded() const noexcept {
    return h1 > 0
           && h1 <= NN_MAX_HIDDEN_DIM
           && h2 > 0
           && h2 <= NN_MAX_HIDDEN_DIM
           && inDim == NN_INPUT_DIM
           && outDim == NUM_PIECES
           && params.size() == totalParams(inDim, h1, h2, outDim);
  }

  static constexpr size_t totalParams(int inD, int hid1, int hid2, int outD) noexcept {
    return static_cast<size_t>(inD) * static_cast<size_t>(hid1)
           + static_cast<size_t>(hid1)
           + static_cast<size_t>(hid1) * static_cast<size_t>(hid2)
           + static_cast<size_t>(hid2)
           + static_cast<size_t>(outD) * static_cast<size_t>(hid2)
           + static_cast<size_t>(outD);
  }

  size_t sizeW1() const noexcept { return static_cast<size_t>(inDim) * static_cast<size_t>(h1); }
  size_t sizeW2() const noexcept { return static_cast<size_t>(h1) * static_cast<size_t>(h2); }
  size_t sizeW3() const noexcept { return static_cast<size_t>(outDim) * static_cast<size_t>(h2); }

  size_t offW1() const noexcept { return 0; }
  size_t offB1() const noexcept { return sizeW1(); }
  size_t offW2() const noexcept { return offB1() + static_cast<size_t>(h1); }
  size_t offB2() const noexcept { return offW2() + sizeW2(); }
  size_t offW3() const noexcept { return offB2() + static_cast<size_t>(h2); }
  size_t offB3() const noexcept { return offW3() + sizeW3(); }

  size_t paramCount() const noexcept { return params.size(); }
  size_t byteSize() const noexcept { return sizeof(TinyNnHeader) + params.size() * sizeof(float); }

  template <typename Fn> void forEachWeightMatrix(Fn &&fn) const {
    fn(offW1(), sizeW1());
    fn(offW2(), sizeW2());
    fn(offW3(), sizeW3());
  }

  bool initRandom(int hidden1, int hidden2, RngMode mode, uint64_t seed,
                  float initOutputBias = 0.0f) {
    assert(hidden1 > 0 && hidden1 <= NN_MAX_HIDDEN_DIM);
    assert(hidden2 > 0 && hidden2 <= NN_MAX_HIDDEN_DIM);
    assert(std::isfinite(initOutputBias));
    if (hidden1 <= 0
        || hidden1 > NN_MAX_HIDDEN_DIM
        || hidden2 <= 0
        || hidden2 > NN_MAX_HIDDEN_DIM
        || !std::isfinite(initOutputBias)) {
      return false;
    }
    inDim = NN_INPUT_DIM;
    h1 = hidden1;
    h2 = hidden2;
    outDim = NUM_PIECES;
    rngMode = mode;
    params.assign(totalParams(inDim, h1, h2, outDim), 0.0f);

    Rng rng(seed);
    auto nextUniform = [&rng](float scale) -> float {
      return static_cast<float>((rng.nextDouble() * 2.0 - 1.0) * static_cast<double>(scale));
    };

    // He uniform initialization for ReLU hidden layers; small scale on output layer.
    const float s1 = std::sqrt(6.0f / static_cast<float>(inDim));
    const float s2 = std::sqrt(6.0f / static_cast<float>(h1));
    const float s3 = std::sqrt(1.0f / static_cast<float>(h2));

    float *w1 = params.data() + offW1();
    for (size_t i = 0; i < sizeW1(); ++i) {
      w1[i] = nextUniform(s1);
    }
    float *w2 = params.data() + offW2();
    for (size_t i = 0; i < sizeW2(); ++i) {
      w2[i] = nextUniform(s2);
    }
    float *w3 = params.data() + offW3();
    for (size_t i = 0; i < sizeW3(); ++i) {
      w3[i] = nextUniform(s3);
    }
    float *b3 = params.data() + offB3();
    for (int p = 0; p < outDim; ++p) {
      b3[p] = initOutputBias;
    }
    return true;
  }

  float dotOutputRow(int pieceIdx, const float *__restrict__ a2) const noexcept {
    const float *__restrict__ wRow =
        params.data() + offW3() + static_cast<size_t>(pieceIdx) * static_cast<size_t>(h2);
    return params[offB3() + static_cast<size_t>(pieceIdx)] + detail::dotProduct4(wRow, a2, h2);
  }

  // Forward pass computing hidden activations a1[0..h1-1] and a2[0..h2-1] from pre-extracted x.
  void forwardToH2(const float *__restrict__ x, float *__restrict__ a1,
                   float *__restrict__ a2) const noexcept {
    assert(isLoaded());
    const float *__restrict__ w1 = params.data() + offW1();
    const float *__restrict__ b1 = params.data() + offB1();
    const float *__restrict__ w2 = params.data() + offW2();
    const float *__restrict__ b2 = params.data() + offB2();

    for (int j = 0; j < h1; ++j) {
      a1[j] = b1[j];
    }
    // Binary cell features [0..39]: pure additive accumulate on set cells.
    for (int i = 0; i < NN_FEAT_HEIGHTS_OFF; ++i) {
      if (x[i] == 0.0f) {
        continue;
      }
      const float *__restrict__ wRow = w1 + static_cast<size_t>(i) * static_cast<size_t>(h1);
      for (int j = 0; j < h1; ++j) {
        a1[j] += wRow[j];
      }
    }
    // Continuous structural features [40..87]: skip zeros.
    for (int i = NN_FEAT_HEIGHTS_OFF; i < NN_INPUT_DIM; ++i) {
      const float xi = x[i];
      if (xi == 0.0f) {
        continue;
      }
      const float *__restrict__ wRow = w1 + static_cast<size_t>(i) * static_cast<size_t>(h1);
      for (int j = 0; j < h1; ++j) {
        a1[j] += xi * wRow[j];
      }
    }
    for (int j = 0; j < h1; ++j) {
      a1[j] = (a1[j] > 0.0f) ? a1[j] : 0.0f;
    }

    for (int k = 0; k < h2; ++k) {
      a2[k] = b2[k];
    }
    for (int j = 0; j < h1; ++j) {
      const float aj = a1[j];
      if (aj == 0.0f) {
        continue;
      }
      const float *__restrict__ wRow = w2 + static_cast<size_t>(j) * static_cast<size_t>(h2);
      for (int k = 0; k < h2; ++k) {
        a2[k] += aj * wRow[k];
      }
    }
    for (int k = 0; k < h2; ++k) {
      a2[k] = (a2[k] > 0.0f) ? a2[k] : 0.0f;
    }
  }

  // Evaluates V_hat(board, 0..6) for all 7 pieces simultaneously (zero heap allocation for
  // h1 + h2 <= NN_FAST_STACK_SUM, keeping stack frames below 4 KB).
  void evalAll(uint64_t board, float outV[NUM_PIECES]) const noexcept {
    float x[NN_INPUT_DIM];
    extractNnFeatures(board, x);
    if (h1 + h2 <= NN_FAST_STACK_SUM) {
      float buf[NN_FAST_STACK_SUM];
      float *a1 = buf;
      float *a2 = buf + h1;
      forwardToH2(x, a1, a2);
      for (int p = 0; p < NUM_PIECES; ++p) {
        outV[p] = dotOutputRow(p, a2);
      }
      return;
    }
    float a1[NN_MAX_HIDDEN_DIM];
    float a2[NN_MAX_HIDDEN_DIM];
    forwardToH2(x, a1, a2);
    for (int p = 0; p < NUM_PIECES; ++p) {
      outV[p] = dotOutputRow(p, a2);
    }
  }

  // Evaluates V_hat(board, pieceIdx) computing only the single output row for pieceIdx.
  float evalPiece(uint64_t board, int pieceIdx) const noexcept {
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    float x[NN_INPUT_DIM];
    extractNnFeatures(board, x);
    if (h1 + h2 <= NN_FAST_STACK_SUM) {
      float buf[NN_FAST_STACK_SUM];
      float *a1 = buf;
      float *a2 = buf + h1;
      forwardToH2(x, a1, a2);
      return dotOutputRow(pieceIdx, a2);
    }
    float a1[NN_MAX_HIDDEN_DIM];
    float a2[NN_MAX_HIDDEN_DIM];
    forwardToH2(x, a1, a2);
    return dotOutputRow(pieceIdx, a2);
  }

  // Computes E_{p3 | prevPiece}[max(0, V_hat(board, p3))] for 2-ply lookahead leaves.
  // Clamping each piece at >= 0 enforces the exact Tetris lower bound V*(b, p3) >= 0 so negative
  // NN extrapolations on an unplayable piece cannot drag down playable pieces in the expectation.
  double evalNextPieceExpected(uint64_t board, RngMode mode, int prevPiece) const noexcept {
    assert(prevPiece >= 0 && prevPiece < NUM_PIECES);
    float v[NUM_PIECES];
    evalAll(board, v);
    double ev = 0.0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      const double clamped = std::max(0.0, static_cast<double>(v[p]));
      ev += pieceTransitionProb(mode, prevPiece, p) * clamped;
    }
    return ev;
  }

  // Accumulates exact backprop gradients into gradParams given pre-masked dA2[0..h2-1].
  void accumulateBackwardFromA2(const float *__restrict__ x, const float *__restrict__ a1,
                                const float *__restrict__ dA2,
                                float *__restrict__ gradParams) const noexcept {
    const float *__restrict__ w2 = params.data() + offW2();
    float *__restrict__ gW1 = gradParams + offW1();
    float *__restrict__ gB1 = gradParams + offB1();
    float *__restrict__ gW2 = gradParams + offW2();

    float dA1[NN_MAX_HIDDEN_DIM];
    for (int j = 0; j < h1; ++j) {
      const float aj = a1[j];
      if (aj <= 0.0f) {
        dA1[j] = 0.0f;
        continue;
      }
      const float *__restrict__ wRow = w2 + static_cast<size_t>(j) * static_cast<size_t>(h2);
      float *__restrict__ gRow = gW2 + static_cast<size_t>(j) * static_cast<size_t>(h2);
      for (int k = 0; k < h2; ++k) {
        gRow[k] += aj * dA2[k];
      }
      const float acc = detail::dotProduct4(wRow, dA2, h2);
      dA1[j] = acc;
      gB1[j] += acc;
    }

    for (int i = 0; i < NN_FEAT_HEIGHTS_OFF; ++i) {
      if (x[i] == 0.0f) {
        continue;
      }
      float *__restrict__ gRow = gW1 + static_cast<size_t>(i) * static_cast<size_t>(h1);
      for (int j = 0; j < h1; ++j) {
        gRow[j] += dA1[j];
      }
    }
    for (int i = NN_FEAT_HEIGHTS_OFF; i < NN_INPUT_DIM; ++i) {
      const float xi = x[i];
      if (xi == 0.0f) {
        continue;
      }
      float *__restrict__ gRow = gW1 + static_cast<size_t>(i) * static_cast<size_t>(h1);
      for (int j = 0; j < h1; ++j) {
        gRow[j] += xi * dA1[j];
      }
    }
  }

  // Accumulates exact backprop gradients for a single active output row pieceIdx (fast path for
  // candidate-child updates, avoiding 7-way output zero scans).
  void accumulateBackwardSingleOutput(const float *__restrict__ x, const float *__restrict__ a1,
                                      const float *__restrict__ a2, int pieceIdx, float dVal,
                                      float *__restrict__ gradParams) const noexcept {
    assert(isLoaded());
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    if (dVal == 0.0f) {
      return;
    }
    const float *__restrict__ wRow =
        params.data() + offW3() + static_cast<size_t>(pieceIdx) * static_cast<size_t>(h2);
    float *__restrict__ gRow =
        gradParams + offW3() + static_cast<size_t>(pieceIdx) * static_cast<size_t>(h2);
    float *__restrict__ gB2 = gradParams + offB2();
    gradParams[offB3() + static_cast<size_t>(pieceIdx)] += dVal;

    float dA2[NN_MAX_HIDDEN_DIM];
    for (int k = 0; k < h2; ++k) {
      gRow[k] += dVal * a2[k];
      const float dk = (a2[k] > 0.0f) ? (dVal * wRow[k]) : 0.0f;
      dA2[k] = dk;
      gB2[k] += dk;
    }
    accumulateBackwardFromA2(x, a1, dA2, gradParams);
  }

  // Accumulates exact backprop gradients into gradParams given all 7 output gradients dOut[0..6].
  void accumulateBackward(const float *__restrict__ x, const float *__restrict__ a1,
                          const float *__restrict__ a2, const float dOut[NUM_PIECES],
                          float *__restrict__ gradParams) const noexcept {
    assert(isLoaded());
    const float *__restrict__ w3 = params.data() + offW3();
    float *__restrict__ gB2 = gradParams + offB2();
    float *__restrict__ gW3 = gradParams + offW3();
    float *__restrict__ gB3 = gradParams + offB3();

    float dA2[NN_MAX_HIDDEN_DIM];
    for (int k = 0; k < h2; ++k) {
      dA2[k] = 0.0f;
    }
    for (int p = 0; p < NUM_PIECES; ++p) {
      const float dp = dOut[p];
      if (dp == 0.0f) {
        continue;
      }
      gB3[p] += dp;
      const float *__restrict__ wRow = w3 + static_cast<size_t>(p) * static_cast<size_t>(h2);
      float *__restrict__ gRow = gW3 + static_cast<size_t>(p) * static_cast<size_t>(h2);
      for (int k = 0; k < h2; ++k) {
        gRow[k] += dp * a2[k];
        dA2[k] += dp * wRow[k];
      }
    }
    for (int k = 0; k < h2; ++k) {
      const float dk = (a2[k] > 0.0f) ? dA2[k] : 0.0f;
      dA2[k] = dk;
      gB2[k] += dk;
    }
    accumulateBackwardFromA2(x, a1, dA2, gradParams);
  }

  bool save(const std::string &path, std::string *errOut = nullptr) const {
    if (!isLoaded()) {
      if (errOut) {
        *errOut = "Cannot save uninitialized TinyNextBoxNet";
      }
      return false;
    }
    for (float v : params) {
      if (!std::isfinite(v)) {
        if (errOut) {
          *errOut = "Cannot save TinyNextBoxNet containing non-finite weights";
        }
        return false;
      }
    }
    TinyNnHeader hdr{};
    std::memcpy(hdr.magic, NN_FILE_MAGIC, sizeof(hdr.magic));
    hdr.featureVersion = NN_FEATURE_VERSION;
    hdr.inDim = static_cast<uint32_t>(inDim);
    hdr.h1 = static_cast<uint32_t>(h1);
    hdr.h2 = static_cast<uint32_t>(h2);
    hdr.outDim = static_cast<uint32_t>(outDim);
    hdr.rngMode = static_cast<uint32_t>(rngMode);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (errOut) {
        *errOut = "Failed to open '" + path + "' for writing";
      }
      return false;
    }
    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char *>(params.data()),
              static_cast<std::streamsize>(params.size() * sizeof(float)));
    out.flush();
    out.close();
    if (!out) {
      if (errOut) {
        *errOut = "Failed writing TinyNextBoxNet payload to '" + path + "'";
      }
      return false;
    }
    return true;
  }

  bool load(const std::string &path, std::string *errOut = nullptr) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
      if (errOut) {
        *errOut = "Failed to open '" + path + "'";
      }
      return false;
    }
    const std::streamoff fileSize = in.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(TinyNnHeader))) {
      if (errOut) {
        *errOut = "File too small to hold TinyNnHeader";
      }
      return false;
    }
    in.seekg(0, std::ios::beg);
    TinyNnHeader hdr{};
    in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    if (!in.good()) {
      if (errOut) {
        *errOut = "Failed to read TinyNnHeader";
      }
      return false;
    }
    if (std::memcmp(hdr.magic, NN_FILE_MAGIC, sizeof(hdr.magic)) != 0) {
      if (errOut) {
        *errOut = "Invalid TinyNextBoxNet magic (expected SRNNV001)";
      }
      return false;
    }
    if (hdr.featureVersion != NN_FEATURE_VERSION) {
      if (errOut) {
        *errOut = "Unsupported TinyNextBoxNet featureVersion";
      }
      return false;
    }
    if (hdr.inDim != static_cast<uint32_t>(NN_INPUT_DIM)
        || hdr.outDim != static_cast<uint32_t>(NUM_PIECES)
        || hdr.h1 == 0
        || hdr.h1 > static_cast<uint32_t>(NN_MAX_HIDDEN_DIM)
        || hdr.h2 == 0
        || hdr.h2 > static_cast<uint32_t>(NN_MAX_HIDDEN_DIM)
        || hdr.rngMode > 1U) {
      if (errOut) {
        *errOut = "Invalid TinyNextBoxNet dimensions or rngMode";
      }
      return false;
    }

    const size_t expectedParams =
        totalParams(static_cast<int>(hdr.inDim), static_cast<int>(hdr.h1), static_cast<int>(hdr.h2),
                    static_cast<int>(hdr.outDim));
    const uint64_t expectedBytes =
        sizeof(TinyNnHeader) + static_cast<uint64_t>(expectedParams) * sizeof(float);
    if (static_cast<uint64_t>(fileSize) != expectedBytes) {
      if (errOut) {
        *errOut = "TinyNextBoxNet file size mismatch";
      }
      return false;
    }

    std::vector<float> loaded(expectedParams);
    in.read(reinterpret_cast<char *>(loaded.data()),
            static_cast<std::streamsize>(expectedParams * sizeof(float)));
    if (!in.good()) {
      if (errOut) {
        *errOut = "Failed reading TinyNextBoxNet weights";
      }
      return false;
    }
    for (float v : loaded) {
      if (!std::isfinite(v)) {
        if (errOut) {
          *errOut = "TinyNextBoxNet contains non-finite weights";
        }
        return false;
      }
    }

    inDim = static_cast<int>(hdr.inDim);
    h1 = static_cast<int>(hdr.h1);
    h2 = static_cast<int>(hdr.h2);
    outDim = static_cast<int>(hdr.outDim);
    rngMode = (hdr.rngMode == 1U) ? RngMode::Nes : RngMode::Uniform;
    params = std::move(loaded);
    return true;
  }
};

// 1-ply Next-Box move selection using TinyNextBoxNet: argmax_m [r_m + V_hat(b'_m, nextPiece)].
inline double scoreNextBoxNn1Ply(uint64_t afterBoard, int linesCleared, int nextPiece,
                                 const TinyNextBoxNet &net) noexcept {
  return static_cast<double>(linesCleared)
         + static_cast<double>(net.evalPiece(afterBoard, nextPiece));
}

inline ChosenTransition chooseNextBoxNnMove1Ply(uint64_t board, int curPiece, int nextPiece,
                                                const TinyNextBoxNet &net) noexcept {
  if (!net.isLoaded()
      || curPiece < 0
      || curPiece >= NUM_PIECES
      || nextPiece < 0
      || nextPiece >= NUM_PIECES) {
    return {};
  }
  TransitionList tl;
  generateTransitions(board, curPiece, tl);
  if (tl.empty()) {
    return {};
  }
  ChosenTransition best{};
  for (int i = 0; i < tl.size(); ++i) {
    const double sc = scoreNextBoxNn1Ply(tl[i].afterBoard, tl[i].linesCleared, nextPiece, net);
    if (!best.found || sc > best.score) {
      best = {tl[i], sc, true};
    }
  }
  return best;
}

// 2-ply Next-Box move selection using TinyNextBoxNet:
//   argmax_{m1} [ r_1 + max_{m2} (r_2 + E_{p3 | nextPiece}[max(0, V_hat(b''_{m2}, p3))]) ]
// When nextPiece tops out (no legal ply-2 moves), continuation is 0.0 lines.
inline double scoreNextBoxNn2Ply(uint64_t afterBoard, int linesCleared, int nextPiece,
                                 const TinyNextBoxNet &net, RngMode mode) noexcept {
  TransitionList tl2;
  generateTransitions(afterBoard, nextPiece, tl2);
  const double r1 = static_cast<double>(linesCleared);
  if (tl2.empty()) {
    return r1;
  }
  double best2 = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < tl2.size(); ++i) {
    const double cont = net.evalNextPieceExpected(tl2[i].afterBoard, mode, nextPiece);
    const double sc = static_cast<double>(tl2[i].linesCleared) + cont;
    if (sc > best2) {
      best2 = sc;
    }
  }
  return r1 + best2;
}

inline ChosenTransition chooseNextBoxNnMove2Ply(uint64_t board, int curPiece, int nextPiece,
                                                const TinyNextBoxNet &net, RngMode mode) noexcept {
  if (!net.isLoaded()
      || curPiece < 0
      || curPiece >= NUM_PIECES
      || nextPiece < 0
      || nextPiece >= NUM_PIECES) {
    return {};
  }
  TransitionList tl1;
  generateTransitions(board, curPiece, tl1);
  if (tl1.empty()) {
    return {};
  }
  ChosenTransition best{};
  for (int i = 0; i < tl1.size(); ++i) {
    const double sc =
        scoreNextBoxNn2Ply(tl1[i].afterBoard, tl1[i].linesCleared, nextPiece, net, mode);
    if (!best.found || sc > best.score) {
      best = {tl1[i], sc, true};
    }
  }
  return best;
}

// Calibrated Next-Box tablebase move selector with optional TinyNextBoxNet fallback and partial-OUT
// scoring:
//   - When any legal move lands in TB:
//       * in-TB move m  : Q(m) = r_m + V_tb(b'_m, nextPiece)
//       * out-of-TB m   : if nnPtr is loaded and safetyMargin is finite (negative values clamp to
//                         0.0; non-finite values NaN / +-inf disable partial-OUT),
//                         Q(m) = r_m + max(0.0, estCont(b'_m, nextPiece) - safetyMargin), where
//                         estCont is 2-ply NN (or 1-ply NN with ply-2 survival check when
//                         outUse2Ply == false). On exact Q ties, in-TB moves are preferred.
//                         When partial-OUT is disabled, out-of-TB moves receive 0 continuation
//                         (bit-exact parity with chooseNextBoxTablebaseMove).
//   - When ALL legal moves land out-of-TB (all-OUT):
//       * sets usedFallback = true and scores all legal moves once with TinyNextBoxNet (if loaded)
//         or pickBestNextBoxHeuristic (if nnPtr == nullptr), preserving full relative move ordering
//         without margin clamping.
template <typename NextBoxTbType>
inline TablebaseMoveChoice
chooseNextBoxHybridFallbackMove(uint64_t board, int curPiece, int nextPiece,
                                const NextBoxTbType &tb, const TinyNextBoxNet *nnPtr,
                                double safetyMargin, RngMode evalRngMode, bool outUse2Ply = true,
                                const EvalWeights &w = {}) {
  if (curPiece < 0 || curPiece >= NUM_PIECES || nextPiece < 0 || nextPiece >= NUM_PIECES) {
    return {};
  }
  const bool hasNn = (nnPtr != nullptr && nnPtr->isLoaded());
  auto wrapFallback = [](const ChosenTransition &fb) -> TablebaseMoveChoice {
    if (!fb.found) {
      return {};
    }
    return {fb.transition, static_cast<double>(fb.transition.linesCleared), false, true, true};
  };

  if (!tb.isLoaded()) {
    const ChosenTransition fb =
        hasNn
            ? (outUse2Ply ? chooseNextBoxNnMove2Ply(board, curPiece, nextPiece, *nnPtr, evalRngMode)
                          : chooseNextBoxNnMove1Ply(board, curPiece, nextPiece, *nnPtr))
            : pickBestNextBoxHeuristic(board, curPiece, nextPiece, w);
    return wrapFallback(fb);
  }

  // When no NN is loaded, delegate directly to exact chooseNextBoxTablebaseMove.
  if (!hasNn) {
    return chooseNextBoxTablebaseMove(board, curPiece, nextPiece, tb, /*closedWorld=*/false, w);
  }

  // Non-finite safetyMargin (NaN, +inf, -inf) disables partial-OUT; finite negative clamps to 0.
  const bool allowPartialOut = std::isfinite(safetyMargin);
  const double clampedMargin = allowPartialOut ? std::max(0.0, safetyMargin) : 0.0;

  TransitionList tl;
  generateTransitions(board, curPiece, tl);
  if (tl.empty()) {
    return {};
  }

  // Pass 1: cheap TB lookup across all legal moves so all-OUT boards skip partial-OUT NN scoring.
  bool inTbArr[MAX_TRANSITIONS];
  float tbValArr[MAX_TRANSITIONS];
  bool anyInTb = false;
  for (int i = 0; i < tl.size(); ++i) {
    float v = 0.0f;
    const bool hit = tb.lookupPiece(tl[i].afterBoard, nextPiece, v);
    inTbArr[i] = hit;
    tbValArr[i] = hit ? v : 0.0f;
    anyInTb = anyInTb || hit;
  }

  if (!anyInTb) {
    // All legal moves exit the TB (all-OUT): rank directly with TinyNextBoxNet once without
    // margin clamping so relative ordering among low-value recovery moves is preserved.
    int bestFbIdx = 0;
    double bestFbScore = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < tl.size(); ++i) {
      const double sc =
          outUse2Ply ? scoreNextBoxNn2Ply(tl[i].afterBoard, tl[i].linesCleared, nextPiece, *nnPtr,
                                          evalRngMode)
                     : scoreNextBoxNn1Ply(tl[i].afterBoard, tl[i].linesCleared, nextPiece, *nnPtr);
      if (i == 0 || sc > bestFbScore) {
        bestFbScore = sc;
        bestFbIdx = i;
      }
    }
    return {tl[bestFbIdx], static_cast<double>(tl[bestFbIdx].linesCleared), false, true, true};
  }

  const auto evalOutContinuation = [&](uint64_t afterBoard) -> double {
    if (outUse2Ply) {
      return scoreNextBoxNn2Ply(afterBoard, 0, nextPiece, *nnPtr, evalRngMode);
    }
    if (!hasAnyLegalTransition(afterBoard, nextPiece)) {
      return 0.0;
    }
    return std::max(0.0, static_cast<double>(nnPtr->evalPiece(afterBoard, nextPiece)));
  };

  // Gate tieEps and in-TB tie preference on allowPartialOut to preserve exact tie-break parity
  // with chooseNextBoxTablebaseMove when partial-OUT is disabled.
  const double tieEps = allowPartialOut ? 1e-12 : 0.0;
  int bestIdx = 0;
  double bestQ = 0.0;
  double bestRefQ = 0.0;
  bool bestInTb = false;
  bool hasBestTie = false;
  double bestTieScore = 0.0;

  for (int i = 0; i < tl.size(); ++i) {
    const bool inTb = inTbArr[i];
    const double rew = static_cast<double>(tl[i].linesCleared);
    double q = rew;
    if (inTb) {
      q += static_cast<double>(tbValArr[i]);
    } else if (allowPartialOut) {
      const double estCont = evalOutContinuation(tl[i].afterBoard);
      q += std::max(0.0, estCont - clampedMargin);
    }

    if (i == 0 || q > bestRefQ + tieEps) {
      bestIdx = i;
      bestQ = q;
      bestRefQ = q;
      bestInTb = inTb;
      hasBestTie = false;
    } else if (std::abs(q - bestRefQ) <= tieEps) {
      bestRefQ = std::max(bestRefQ, q);
      if (allowPartialOut && inTb && !bestInTb) {
        bestIdx = i;
        bestQ = q;
        bestInTb = true;
        hasBestTie = false;
      } else if (!allowPartialOut || inTb == bestInTb) {
        if (!hasBestTie) {
          bestTieScore = scoreNextBoxHeuristicTransition(tl[bestIdx].afterBoard,
                                                         tl[bestIdx].linesCleared, nextPiece, w);
          hasBestTie = true;
        }
        const double candTie =
            scoreNextBoxHeuristicTransition(tl[i].afterBoard, tl[i].linesCleared, nextPiece, w);
        if (candTie > bestTieScore) {
          bestIdx = i;
          bestQ = q;
          bestInTb = inTb;
          bestTieScore = candTie;
        }
      }
    }
  }

  return {tl[bestIdx], bestQ, bestInTb, false, true};
}

struct NnLossConfig {
  double huberDelta = 2.0;  // Huber threshold in lines for V_hat regression
  double valueWeight = 1.0; // Weight on parent 7-output Huber value loss
  double childValueWeight =
      1.0;                   // Weight on candidate-child Huber value loss (trains bad/trap moves)
  double rankWeight = 4.0;   // Weight on Boltzmann move-ranking cross-entropy loss
  double temperature = 1.0;  // Boltzmann target sharpness in lines
  double weightDecay = 1e-5; // L2 regularization on weight matrices (unregularized biases)
};

struct NnLossBreakdown {
  double totalLoss = 0.0;
  double valueLoss = 0.0;
  double childValueLoss = 0.0;
  double rankLoss = 0.0;
};

// Grow-only scratch workspace reused across samples inside a worker thread to avoid per-decision
// heap allocations and zero-fill resets.
struct NnBatchScratch {
  std::vector<float> childFeats;
  std::vector<float> childA1;
  std::vector<float> childA2;
  std::vector<double> scores;
  std::vector<double> targetProbs;
  std::vector<double> predProbs;

  void ensureCapacity(size_t mCount, int h1, int h2) {
    const size_t featNeed = mCount * static_cast<size_t>(NN_INPUT_DIM);
    const size_t a1Need = mCount * static_cast<size_t>(h1);
    const size_t a2Need = mCount * static_cast<size_t>(h2);
    if (childFeats.size() < featNeed)
      childFeats.resize(featNeed);
    if (childA1.size() < a1Need)
      childA1.resize(a1Need);
    if (childA2.size() < a2Need)
      childA2.resize(a2Need);
    if (scores.size() < mCount)
      scores.resize(mCount);
    if (targetProbs.size() < mCount)
      targetProbs.resize(mCount);
    if (predProbs.size() < mCount)
      predProbs.resize(mCount);
  }
};

// Computes exact joint multi-task loss & parameter gradients over indices[start..end):
//   L = valueWeight      * mean_p Huber(V_hat(b, p) - V*(b, p))
//     + childValueWeight * mean_m Huber(V_hat(b'_m, n) - V*(b'_m, n))
//     + rankWeight       * CE(softmax(Q*/T), softmax((r_m + V_hat(b'_m, n))/T))
//     + 0.5 * weightDecay * ||W||^2
inline NnLossBreakdown computeNnSliceLossAndGrad(
    const TinyNextBoxNet &net, const std::vector<LabeledDecisionSample> &decisions,
    const std::vector<size_t> &indices, size_t start, size_t end, const NnLossConfig &cfg,
    std::vector<float> &outGrad, NnBatchScratch &scratch, bool includeWeightDecay = true) {
  assert(net.isLoaded());
  outGrad.assign(net.params.size(), 0.0f);
  NnLossBreakdown out{};
  if (!net.isLoaded()
      || start >= end
      || end > indices.size()
      || !std::isfinite(cfg.huberDelta)
      || cfg.huberDelta <= 0.0
      || !std::isfinite(cfg.temperature)
      || cfg.temperature <= 0.0
      || !std::isfinite(cfg.valueWeight)
      || cfg.valueWeight < 0.0
      || !std::isfinite(cfg.childValueWeight)
      || cfg.childValueWeight < 0.0
      || !std::isfinite(cfg.rankWeight)
      || cfg.rankWeight < 0.0
      || !std::isfinite(cfg.weightDecay)
      || cfg.weightDecay < 0.0) {
    return out;
  }

  const size_t count = end - start;
  const double invCount = 1.0 / static_cast<double>(count);
  const double invTemp = 1.0 / cfg.temperature;
  const double delta = cfg.huberDelta;
  const int h1 = net.h1;
  const int h2 = net.h2;

  float xParent[NN_INPUT_DIM];
  float a1Parent[NN_MAX_HIDDEN_DIM];
  float a2Parent[NN_MAX_HIDDEN_DIM];
  float dOut[NUM_PIECES];

  double valLossSum = 0.0;
  double childValLossSum = 0.0;
  double rankLossSum = 0.0;

  for (size_t idxPos = start; idxPos < end; ++idxPos) {
    const size_t decIdx = indices[idxPos];
    if (decIdx >= decisions.size()) {
      continue;
    }
    const LabeledDecisionSample &d = decisions[decIdx];

    // 1. Multi-output Huber value supervision on all 7 pieces V*(b, 0..6).
    if (cfg.valueWeight > 0.0 && d.boardInTb) {
      extractNnFeatures(d.board, xParent);
      net.forwardToH2(xParent, a1Parent, a2Parent);
      const double scale = (cfg.valueWeight * invCount) / static_cast<double>(NUM_PIECES);
      double sampleValLoss = 0.0;
      for (int p = 0; p < NUM_PIECES; ++p) {
        const float pred = net.dotOutputRow(p, a2Parent);
        const double err =
            static_cast<double>(pred) - static_cast<double>(d.vStar[static_cast<size_t>(p)]);
        double deriv = 0.0;
        sampleValLoss += detail::huberLossAndGrad(err, delta, deriv);
        dOut[p] = static_cast<float>(scale * deriv);
      }
      valLossSum += sampleValLoss / static_cast<double>(NUM_PIECES);
      net.accumulateBackward(xParent, a1Parent, a2Parent, dOut, outGrad.data());
    }

    // 2. Candidate-child Huber value supervision + Boltzmann move-ranking CE on Q*(b, c, n, m).
    const size_t mCount = d.moves.size();
    const bool doRank = (cfg.rankWeight > 0.0 && mCount >= 2);
    const bool doChildVal = (cfg.childValueWeight > 0.0 && mCount >= 1);
    if ((doRank || doChildVal) && d.nextPiece >= 0 && d.nextPiece < NUM_PIECES) {
      scratch.ensureCapacity(mCount, h1, h2);
      const int nxt = d.nextPiece;

      double maxQ = -std::numeric_limits<double>::infinity();
      double maxScore = -std::numeric_limits<double>::infinity();
      for (size_t m = 0; m < mCount; ++m) {
        float *xf = &scratch.childFeats[m * static_cast<size_t>(NN_INPUT_DIM)];
        float *a1 = &scratch.childA1[m * static_cast<size_t>(h1)];
        float *a2 = &scratch.childA2[m * static_cast<size_t>(h2)];
        extractNnFeatures(d.moves[m].afterBoard, xf);
        net.forwardToH2(xf, a1, a2);
        const float vPred = net.dotOutputRow(nxt, a2);
        const double sc = static_cast<double>(d.moves[m].linesCleared) + static_cast<double>(vPred);
        scratch.scores[m] = sc;
        if (sc > maxScore) {
          maxScore = sc;
        }
        if (d.moves[m].qStar > maxQ) {
          maxQ = d.moves[m].qStar;
        }
      }

      double sumExpQ = 0.0;
      double sumExpS = 0.0;
      double logSumExpS = 0.0;
      if (doRank) {
        for (size_t m = 0; m < mCount; ++m) {
          const double eq = std::exp((d.moves[m].qStar - maxQ) * invTemp);
          scratch.targetProbs[m] = eq;
          sumExpQ += eq;
          const double es = std::exp((scratch.scores[m] - maxScore) * invTemp);
          scratch.predProbs[m] = es;
          sumExpS += es;
        }
        logSumExpS = std::log(sumExpS);
      }

      const double rankScale = cfg.rankWeight * invCount * invTemp;
      const double childValScale = (cfg.childValueWeight * invCount) / static_cast<double>(mCount);
      double sampleChildValLoss = 0.0;
      for (size_t m = 0; m < mCount; ++m) {
        double gradNxt = 0.0;
        if (doRank) {
          const double tProb = scratch.targetProbs[m] / sumExpQ;
          const double pProb = scratch.predProbs[m] / sumExpS;
          const double logP = (scratch.scores[m] - maxScore) * invTemp - logSumExpS;
          rankLossSum -= tProb * logP;
          gradNxt += rankScale * (pProb - tProb);
        }
        if (doChildVal) {
          // scores[m] - qStar == V_hat(b'_m, nxt) - V*(b'_m, nxt) (linesCleared cancels exactly).
          const double err = scratch.scores[m] - d.moves[m].qStar;
          double deriv = 0.0;
          sampleChildValLoss += detail::huberLossAndGrad(err, delta, deriv);
          gradNxt += childValScale * deriv;
        }
        net.accumulateBackwardSingleOutput(
            &scratch.childFeats[m * static_cast<size_t>(NN_INPUT_DIM)],
            &scratch.childA1[m * static_cast<size_t>(h1)],
            &scratch.childA2[m * static_cast<size_t>(h2)], nxt, static_cast<float>(gradNxt),
            outGrad.data());
      }
      if (doChildVal) {
        childValLossSum += sampleChildValLoss / static_cast<double>(mCount);
      }
    }
  }

  out.valueLoss = valLossSum * invCount;
  out.childValueLoss = childValLossSum * invCount;
  out.rankLoss = rankLossSum * invCount;
  out.totalLoss = cfg.valueWeight * out.valueLoss
                  + cfg.childValueWeight * out.childValueLoss
                  + cfg.rankWeight * out.rankLoss;

  if (includeWeightDecay && cfg.weightDecay > 0.0) {
    double regSum = 0.0;
    net.forEachWeightMatrix([&](size_t offset, size_t len) {
      for (size_t i = 0; i < len; ++i) {
        const double w = static_cast<double>(net.params[offset + i]);
        regSum += w * w;
        outGrad[offset + i] += static_cast<float>(cfg.weightDecay * w);
      }
    });
    out.totalLoss += 0.5 * cfg.weightDecay * regSum;
  }
  return out;
}

struct NnValueEvalMetrics {
  size_t numSamples = 0; // Visitation-weighted count of in-TB samples evaluated
  double mae = 0.0;
  double rmse = 0.0;
  std::array<double, NUM_HOLE_BUCKETS> maeByHoles{};
  std::array<uint64_t, NUM_HOLE_BUCKETS> countByHoles{};
};

inline NnValueEvalMetrics
evaluateNnValueAccuracy(const TinyNextBoxNet &net,
                        const std::vector<LabeledDecisionSample> &samples) {
  NnValueEvalMetrics m{};
  if (!net.isLoaded()) {
    return m;
  }
  double sumAbs = 0.0;
  double sumSq = 0.0;
  std::array<double, NUM_HOLE_BUCKETS> absByHoles{};
  float pred[NUM_PIECES];

  for (const auto &s : samples) {
    if (!s.boardInTb) {
      continue;
    }
    net.evalAll(s.board, pred);
    double boardAbs = 0.0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      const double err =
          static_cast<double>(pred[p]) - static_cast<double>(s.vStar[static_cast<size_t>(p)]);
      boardAbs += std::abs(err);
      sumSq += err * err;
    }
    boardAbs /= static_cast<double>(NUM_PIECES);
    sumAbs += boardAbs;
    ++m.numSamples;
    const int hb = HoleStratifiedCounts::bucket(countHoles(s.board));
    absByHoles[static_cast<size_t>(hb)] += boardAbs;
    ++m.countByHoles[static_cast<size_t>(hb)];
  }
  if (m.numSamples == 0) {
    return m;
  }
  m.mae = sumAbs / static_cast<double>(m.numSamples);
  m.rmse = std::sqrt(sumSq / static_cast<double>(m.numSamples * NUM_PIECES));
  for (size_t b = 0; b < NUM_HOLE_BUCKETS; ++b) {
    m.maeByHoles[b] =
        (m.countByHoles[b] > 0) ? (absByHoles[b] / static_cast<double>(m.countByHoles[b])) : 0.0;
  }
  return m;
}

} // namespace tb

#endif // TABLEBASE_TINY_NN_HPP
