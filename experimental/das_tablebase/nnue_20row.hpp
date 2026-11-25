#ifndef DAS_TABLEBASE_NNUE_20ROW_HPP
#define DAS_TABLEBASE_NNUE_20ROW_HPP

#include "core_20row.hpp"
#include "solver_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace das {

// Compact cache-resident 20-row NNUE predicting all 7 Next-Box values V_hat(b, 0..6) in
// normalized Tetris-equivalents (featureVersion = 2, NN_INPUT_DIM = 272 floats).
//
// Input layout (272 floats):
//   [  0..199] : 200 raw binary board cells (row 0..19 x col 0..9; ~75-85% zeros skipped)
//   [200..209] : 10 normalized column heights (h[c] / 20.0)
//   [210..219] : 10 normalized per-column hole counts (min(colHoles[c], 10) / 10.0)
//   [220..239] : 20 normalized row fill fractions (popcount(row[r]) / 10.0)
//   [240..268] : 29 normalized structural & DAS features (hole clustering/burial, cover-row
//                clearability, transitions, wells, right-well Tetris readiness, L19 tap reach)
//   [269..271] : 3 line-count horizon features (0.0 in stationary mode)
//
// Weight layout (SRNNV001):
//   W1: [inDim x h1]  (skips zero cells & zero features; contiguous SIMD over h1)
//   W2: [h1 x h2]     (branchless SIMD over h2; specialized for 128x64 and 256x64)
//   W3: [outDim x h2] (contiguous 4-lane dot product over h2; evalPiece reads 1 row)

inline constexpr char NN_FILE_MAGIC[8] = {'S', 'R', 'N', 'N', 'V', '0', '0', '1'};
inline constexpr uint32_t NN_FEATURE_VERSION_20 = 2U;
inline constexpr int NN_FEAT_CELLS_OFF = 0;
inline constexpr int NN_FEAT_HEIGHTS_OFF = 200;
inline constexpr int NN_FEAT_COL_HOLES_OFF = 210;
inline constexpr int NN_FEAT_ROW_FILLS_OFF = 220;
inline constexpr int NN_FEAT_GLOBALS_OFF = 240;
inline constexpr int NN_FEAT_HORIZON_OFF = 269;
inline constexpr int NN_INPUT_DIM = 272;
inline constexpr int NN_MAX_HIDDEN_DIM = 512;
inline constexpr int NN_FAST_STACK_SUM = 384; // Keeps hot stack frame < 2 KB on ARM64
inline constexpr int NN_DEFAULT_3PLY_TOPK = 6;
inline constexpr double NN_3PLY_PRESCREEN_SLACK = 1.8; // Scaled for normalized Tetris-equivalents

#if defined(_MSC_VER)
#define DAS_NN_NOINLINE __declspec(noinline)
#else
#define DAS_NN_NOINLINE __attribute__((noinline))
#endif

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

// Deterministic 4-lane IEEE-754 dot product enabling ARM NEON fmla without -ffast-math.
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

// Extracts 272 normalized floats from any 20x10 row bitmask array (works both inside and outside
// the K16 key domain). Optional horizon[3] supplies line-count features when non-null.
inline void extractNnFeatures20(const uint16_t rows[BOARD_ROWS], float out[NN_INPUT_DIM],
                                const float horizon[3] = nullptr) noexcept {
  // Zero output once so empty top rows skip cell writes completely.
  std::memset(out, 0, sizeof(float) * NN_INPUT_DIM);

  int heights[BOARD_WIDTH] = {};
  int colHoles[BOARD_WIDTH] = {};
  int coveredPerCol[BOARD_WIDTH] = {};
  int filledAboveCol[BOARD_WIDTH] = {};
  int buriedDepth2Plus = 0;
  int totalCells = 0;
  int rowTransitions = 0;
  int colTransitions = 0;
  int holeRows = 0;
  int highestHoleRow = -1; // smallest row index (closest to top) containing a hole
  int lowestHoleRow = -1;  // largest row index (closest to bottom) containing a hole
  int totalHoles = 0;
  int tuckableHoles = 0;
  int tetrisReadyRows = 0;
  uint32_t covered = 0U;

  uint32_t prevRow = 0U;
  for (int r = 0; r < BOARD_ROWS; ++r) {
    const uint32_t row = static_cast<uint32_t>(rows[r]) & FULL_ROW_MASK;
    if (r > 0) {
      colTransitions += __builtin_popcount(prevRow ^ row);
    } else {
      colTransitions += __builtin_popcount(row); // transition from empty ceiling
    }
    prevRow = row;

    if (row == 0U && covered == 0U) {
      // Completely empty top row above the stack: 2 wall transitions (left+right) only.
      rowTransitions += 2;
      continue;
    }

    const int cnt = __builtin_popcount(row);
    totalCells += cnt;
    out[NN_FEAT_ROW_FILLS_OFF + r] = static_cast<float>(cnt) * 0.1f;

    // Horizontal transitions treating side walls (bits 0 and 11) as filled.
    const uint32_t padded = (row << 1) | 1U | (1U << 11);
    rowTransitions += __builtin_popcount((padded ^ (padded >> 1)) & 0x7FFU);

    const uint32_t newTop = row & ~covered;
    if (newTop) {
      uint32_t nt = newTop << 22;
      while (nt) {
        const int col = __builtin_clz(nt);
        nt ^= (0x80000000U >> col);
        heights[col] = BOARD_ROWS - r;
      }
    }

    const uint32_t holes = (~row) & covered;
    if (holes) {
      holeRows++;
      if (highestHoleRow < 0)
        highestHoleRow = r;
      lowestHoleRow = r;

      uint32_t hMask = holes << 22;
      while (hMask) {
        const int c = __builtin_clz(hMask);
        hMask ^= (0x80000000U >> c);
        totalHoles++;
        colHoles[c]++;
        coveredPerCol[c] = filledAboveCol[c];
        if (filledAboveCol[c] >= 2) {
          buriedDepth2Plus++;
        }
        // Quick overhang/tuckable check (adjacent horizontal corridor of width >= 2 open).
        if ((c >= 2 && ((row >> (9 - c)) & 0x7U) == 0)
            || (c <= 7 && ((row >> (7 - c)) & 0x7U) == 0)) {
          tuckableHoles++;
        }
      }
    }

    // Fused single bit-scan: write raw binary cells and track filled cells above future holes.
    if (row != 0U) {
      float *cellOut = out + NN_FEAT_CELLS_OFF + r * BOARD_WIDTH;
      uint32_t rb = row << 22;
      while (rb) {
        const int c = __builtin_clz(rb);
        rb ^= (0x80000000U >> c);
        cellOut[c] = 1.0f;
        filledAboveCol[c]++;
      }
    }

    // Clean right-well Tetris-ready row (cols 0..8 filled, col 9 empty, col 9 uncovered).
    if (row == 0x3FEU && (covered & 1U) == 0U) {
      tetrisReadyRows++;
    }
    covered |= row;
  }
  // Floor transitions below row 19 (floor is solid 0x3FF).
  colTransitions += __builtin_popcount((~prevRow) & FULL_ROW_MASK);

  int sumH = 0;
  int sumHSq = 0;
  int maxH = 0;
  int holeCols = 0;
  int coveredMinoCount = 0;
  int evenSum = 0;
  int oddSum = 0;
  int bumpiness = 0;
  int safeFlatPairs = 0;
  int wellCount = 0;
  int maxWellDepth = 0;
  int wellDepthSq = 0;

  // Level 19 10Hz ("X.....") approximate tap-reach height caps for left/right columns.
  static constexpr int L19_LEFT_CAP[5] = {5, 5, 8, 11, 14};
  static constexpr int L19_RIGHT_CAP[4] = {14, 11, 8, 8}; // cols 6, 7, 8, 9
  int inaccLeft = 0;
  int inaccRight = 0;

  for (int c = 0; c < BOARD_WIDTH; ++c) {
    const int h = heights[c];
    out[NN_FEAT_HEIGHTS_OFF + c] = static_cast<float>(h) * (1.0f / 20.0f);
    out[NN_FEAT_COL_HOLES_OFF + c] = static_cast<float>(std::min(colHoles[c], 10)) * (1.0f / 10.0f);

    sumH += h;
    sumHSq += h * h;
    if (h > maxH)
      maxH = h;
    if (colHoles[c] > 0)
      holeCols++;
    coveredMinoCount += coveredPerCol[c];
    if ((c & 1) == 0) {
      evenSum += h;
    } else {
      oddSum += h;
    }

    if (c < 5) {
      inaccLeft = std::max(inaccLeft, h - L19_LEFT_CAP[c]);
    }
    if (c >= 6) {
      inaccRight = std::max(inaccRight, h - L19_RIGHT_CAP[c - 6]);
    }

    if (c + 1 < BOARD_WIDTH) {
      const int nh = heights[c + 1];
      bumpiness += std::abs(h - nh);
      if (h == nh && h <= 10) {
        safeFlatPairs++;
      }
    }
    const int leftH = (c == 0) ? BOARD_ROWS : heights[c - 1];
    const int rightH = (c + 1 == BOARD_WIDTH) ? BOARD_ROWS : heights[c + 1];
    const int depth = std::min(leftH, rightH) - h;
    if (depth >= 2) {
      wellCount++;
      wellDepthSq += depth * depth;
      if (depth > maxWellDepth)
        maxWellDepth = depth;
    }
  }

  const int col9WellDepth = std::max(0, heights[8] - heights[9]);
  int coverRowMissing = 0;
  if (highestHoleRow > 0) {
    coverRowMissing =
        BOARD_WIDTH
        - __builtin_popcount(static_cast<uint32_t>(rows[highestHoleRow - 1]) & FULL_ROW_MASK);
  }
  const int sealedHoles = totalHoles - tuckableHoles;
  const int highestHoleHeight = (highestHoleRow >= 0) ? (BOARD_ROWS - highestHoleRow) : 0;
  const int lowestHoleHeight = (lowestHoleRow >= 0) ? (BOARD_ROWS - lowestHoleRow) : 0;

  float *g = out + NN_FEAT_GLOBALS_OFF;
  g[0] = static_cast<float>(std::min(totalHoles, 20)) * (1.0f / 10.0f);
  g[1] = static_cast<float>(std::min(sealedHoles, 20)) * (1.0f / 10.0f);
  g[2] = static_cast<float>(std::min(tuckableHoles, 10)) * (1.0f / 10.0f);
  g[3] = (totalHoles > 0) ? 1.0f : 0.0f;
  g[4] = static_cast<float>(holeCols) * (1.0f / 10.0f);
  g[5] = static_cast<float>(holeRows) * (1.0f / 10.0f);
  g[6] = static_cast<float>(std::min(coveredMinoCount, 40)) * (1.0f / 20.0f);
  g[7] = static_cast<float>(std::min(buriedDepth2Plus, 20)) * (1.0f / 10.0f);
  g[8] = static_cast<float>(coverRowMissing) * (1.0f / 10.0f);
  g[9] = static_cast<float>(highestHoleHeight) * (1.0f / 20.0f);
  g[10] = static_cast<float>(lowestHoleHeight) * (1.0f / 20.0f);
  g[11] = static_cast<float>(totalCells) * (1.0f / 200.0f);
  g[12] = static_cast<float>(sumH) * (1.0f / 200.0f); // avgHeight / 20
  g[13] = static_cast<float>(maxH) * (1.0f / 20.0f);
  g[14] = static_cast<float>(sumHSq) * (1.0f / 2000.0f);
  g[15] = static_cast<float>(bumpiness) * (1.0f / 40.0f);
  g[16] = static_cast<float>(rowTransitions) * (1.0f / 100.0f);
  g[17] = static_cast<float>(colTransitions) * (1.0f / 100.0f);
  g[18] = static_cast<float>(wellCount) * (1.0f / 5.0f);
  g[19] = static_cast<float>(maxWellDepth) * (1.0f / 10.0f);
  g[20] = static_cast<float>(std::min(wellDepthSq, 200)) * (1.0f / 100.0f);
  g[21] = static_cast<float>(col9WellDepth) * (1.0f / 10.0f);
  g[22] = static_cast<float>(tetrisReadyRows) * (1.0f / 10.0f);
  g[23] = static_cast<float>(heights[9]) * (1.0f / 20.0f);
  g[24] = static_cast<float>(heights[0] + heights[1]) * (1.0f / 40.0f);
  g[25] = static_cast<float>(inaccLeft) * (1.0f / 10.0f);
  g[26] = static_cast<float>(inaccRight) * (1.0f / 10.0f);
  g[27] = static_cast<float>(std::abs(evenSum - oddSum)) * (1.0f / 20.0f);
  g[28] = static_cast<float>(safeFlatPairs) * (1.0f / 9.0f);

  if (horizon != nullptr) {
    out[NN_FEAT_HORIZON_OFF + 0] = horizon[0];
    out[NN_FEAT_HORIZON_OFF + 1] = horizon[1];
    out[NN_FEAT_HORIZON_OFF + 2] = horizon[2];
  }
}

class TinyDasNet {
public:
  int inDim = NN_INPUT_DIM;
  int h1 = 0;
  int h2 = 0;
  int outDim = NUM_PIECES;
  RngMode rngMode = RngMode::Nes;
  std::vector<float> params;

  static size_t totalParams(int inD, int hid1, int hid2, int outD = NUM_PIECES) noexcept {
    return static_cast<size_t>(inD) * static_cast<size_t>(hid1)
           + static_cast<size_t>(hid1)
           + static_cast<size_t>(hid1) * static_cast<size_t>(hid2)
           + static_cast<size_t>(hid2)
           + static_cast<size_t>(outD) * static_cast<size_t>(hid2)
           + static_cast<size_t>(outD);
  }

  bool isLoaded() const noexcept {
    return h1 > 0
           && h2 > 0
           && inDim == NN_INPUT_DIM
           && outDim == NUM_PIECES
           && params.size() == totalParams(inDim, h1, h2, outDim);
  }

  size_t offW1() const noexcept { return 0; }
  size_t offB1() const noexcept { return static_cast<size_t>(inDim) * static_cast<size_t>(h1); }
  size_t offW2() const noexcept { return offB1() + static_cast<size_t>(h1); }
  size_t offB2() const noexcept {
    return offW2() + static_cast<size_t>(h1) * static_cast<size_t>(h2);
  }
  size_t offW3() const noexcept { return offB2() + static_cast<size_t>(h2); }
  size_t offB3() const noexcept {
    return offW3() + static_cast<size_t>(outDim) * static_cast<size_t>(h2);
  }

  void initRandom(int hid1, int hid2, RngMode mode, uint64_t seed) {
    assert(hid1 > 0 && hid1 <= NN_MAX_HIDDEN_DIM);
    assert(hid2 > 0 && hid2 <= NN_MAX_HIDDEN_DIM);
    inDim = NN_INPUT_DIM;
    h1 = hid1;
    h2 = hid2;
    outDim = NUM_PIECES;
    rngMode = mode;
    params.assign(totalParams(inDim, h1, h2, outDim), 0.0f);

    Rng rng(seed);
    auto initBlock = [&](size_t off, size_t count, double scale) {
      for (size_t i = 0; i < count; ++i) {
        // Uniform [-scale, +scale] He-scaled initialization.
        const double u = rng.nextUniform01() * 2.0 - 1.0;
        params[off + i] = static_cast<float>(u * scale);
      }
    };
    initBlock(offW1(), static_cast<size_t>(inDim) * static_cast<size_t>(h1),
              std::sqrt(6.0 / static_cast<double>(inDim)));
    initBlock(offW2(), static_cast<size_t>(h1) * static_cast<size_t>(h2),
              std::sqrt(6.0 / static_cast<double>(h1)));
    initBlock(offW3(), static_cast<size_t>(outDim) * static_cast<size_t>(h2),
              std::sqrt(3.0 / static_cast<double>(h2)));
  }

  // Forward pass from pre-extracted feature vector.
  void forwardFeatures(const float x[NN_INPUT_DIM], float out[NUM_PIECES]) const noexcept {
    assert(isLoaded());
    if (h1 + h2 <= NN_FAST_STACK_SUM) {
      alignas(16) float buf[NN_FAST_STACK_SUM];
      forwardWithBuf(x, buf, buf + h1, out);
    } else {
      forwardLarge(x, out);
    }
  }

  float forwardFeaturesPiece(const float x[NN_INPUT_DIM], int pieceIdx) const noexcept {
    assert(isLoaded() && pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    if (h1 + h2 <= NN_FAST_STACK_SUM) {
      alignas(16) float buf[NN_FAST_STACK_SUM];
      return forwardPieceWithBuf(x, buf, buf + h1, pieceIdx);
    }
    return forwardPieceLarge(x, pieceIdx);
  }

  void evalAll(const uint16_t rows[BOARD_ROWS], float out[NUM_PIECES],
               const float horizon[3] = nullptr) const noexcept {
    float x[NN_INPUT_DIM];
    extractNnFeatures20(rows, x, horizon);
    forwardFeatures(x, out);
  }

  float evalPiece(const uint16_t rows[BOARD_ROWS], int pieceIdx,
                  const float horizon[3] = nullptr) const noexcept {
    float x[NN_INPUT_DIM];
    extractNnFeatures20(rows, x, horizon);
    return forwardFeaturesPiece(x, pieceIdx);
  }

  double evalExpected(const uint16_t rows[BOARD_ROWS], RngMode mode,
                      const float horizon[3] = nullptr) const noexcept {
    float v[NUM_PIECES];
    evalAll(rows, v, horizon);
    double ev = 0.0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      const double vp = std::isfinite(v[p]) ? std::max(0.0, static_cast<double>(v[p])) : 0.0;
      ev += openingPieceProb(mode, p) * vp;
    }
    return ev;
  }

  double evalNextPieceExpected(const uint16_t rows[BOARD_ROWS], RngMode mode, int prevPiece,
                               const float horizon[3] = nullptr) const noexcept {
    float v[NUM_PIECES];
    evalAll(rows, v, horizon);
    double ev = 0.0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      const double vp = std::isfinite(v[p]) ? std::max(0.0, static_cast<double>(v[p])) : 0.0;
      ev += pieceTransitionProb(mode, prevPiece, p) * vp;
    }
    return ev;
  }

  bool save(const std::string &path, std::string *errOut = nullptr) const {
    if (!isLoaded()) {
      if (errOut)
        *errOut = "Cannot save unloaded TinyDasNet";
      return false;
    }
    for (float v : params) {
      if (!std::isfinite(v)) {
        if (errOut)
          *errOut = "Refusing to save non-finite NNUE weights";
        return false;
      }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (errOut)
        *errOut = "Failed to open NNUE file for writing: " + path;
      return false;
    }
    TinyNnHeader hdr{};
    std::memcpy(hdr.magic, NN_FILE_MAGIC, sizeof(NN_FILE_MAGIC));
    hdr.featureVersion = NN_FEATURE_VERSION_20;
    hdr.inDim = static_cast<uint32_t>(inDim);
    hdr.h1 = static_cast<uint32_t>(h1);
    hdr.h2 = static_cast<uint32_t>(h2);
    hdr.outDim = static_cast<uint32_t>(outDim);
    hdr.rngMode = static_cast<uint32_t>(rngMode);
    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char *>(params.data()),
              static_cast<std::streamsize>(params.size() * sizeof(float)));
    if (!out.good()) {
      if (errOut)
        *errOut = "I/O error writing NNUE file: " + path;
      return false;
    }
    return true;
  }

  // Transactional load: existing network stays intact on any validation/I/O error.
  bool load(const std::string &path, std::string *errOut = nullptr) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
      if (errOut)
        *errOut = "Failed to open NNUE file: " + path;
      return false;
    }
    const std::streamoff fileSize = in.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(TinyNnHeader))) {
      if (errOut)
        *errOut = "NNUE file smaller than header: " + path;
      return false;
    }
    in.seekg(0, std::ios::beg);
    TinyNnHeader hdr{};
    in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    if (!in.good() || std::memcmp(hdr.magic, NN_FILE_MAGIC, sizeof(NN_FILE_MAGIC)) != 0) {
      if (errOut)
        *errOut = "Invalid SRNNV001 magic in: " + path;
      return false;
    }
    if (hdr.featureVersion != NN_FEATURE_VERSION_20
        || hdr.inDim != NN_INPUT_DIM
        || hdr.outDim != NUM_PIECES
        || hdr.h1 == 0
        || hdr.h1 > NN_MAX_HIDDEN_DIM
        || hdr.h2 == 0
        || hdr.h2 > NN_MAX_HIDDEN_DIM
        || hdr.rngMode > 1U) {
      if (errOut)
        *errOut = "Unsupported NNUE header dimensions or featureVersion in: " + path;
      return false;
    }
    const size_t expectedParams =
        totalParams(static_cast<int>(hdr.inDim), static_cast<int>(hdr.h1), static_cast<int>(hdr.h2),
                    static_cast<int>(hdr.outDim));
    const uint64_t expectedBytes =
        sizeof(TinyNnHeader) + static_cast<uint64_t>(expectedParams) * sizeof(float);
    if (static_cast<uint64_t>(fileSize) != expectedBytes) {
      if (errOut)
        *errOut = "NNUE file size mismatch: " + path;
      return false;
    }
    std::vector<float> loaded(expectedParams);
    in.read(reinterpret_cast<char *>(loaded.data()),
            static_cast<std::streamsize>(expectedParams * sizeof(float)));
    if (!in.good()) {
      if (errOut)
        *errOut = "Failed reading NNUE weights: " + path;
      return false;
    }
    for (float v : loaded) {
      if (!std::isfinite(v)) {
        if (errOut)
          *errOut = "NNUE file contains non-finite weights: " + path;
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

private:
  template <int H1, int H2>
  void computeHiddenFixed(const float *__restrict__ x, float *__restrict__ a1,
                          float *__restrict__ a2) const noexcept {
    const float *__restrict__ w1 = params.data() + offW1();
    const float *__restrict__ b1 = params.data() + offB1();
    const float *__restrict__ w2 = params.data() + offW2();
    const float *__restrict__ b2 = params.data() + offB2();

    for (int j = 0; j < H1; ++j) {
      a1[j] = b1[j];
    }
    for (int i = 0; i < NN_INPUT_DIM; ++i) {
      const float xi = x[i];
      if (xi == 0.0f)
        continue;
      const float *__restrict__ row = w1 + static_cast<size_t>(i) * static_cast<size_t>(H1);
      for (int j = 0; j < H1; ++j) {
        a1[j] += xi * row[j];
      }
    }
    for (int j = 0; j < H1; ++j) {
      const float v = a1[j];
      a1[j] = (v > 0.0f) ? v : 0.0f;
    }

    // Branchless Layer 2 keeps all H2=64 accumulators in NEON registers (no 50% ReLU mispredicts).
    for (int k = 0; k < H2; ++k) {
      a2[k] = b2[k];
    }
    for (int j = 0; j < H1; ++j) {
      const float aj = a1[j];
      const float *__restrict__ row = w2 + static_cast<size_t>(j) * static_cast<size_t>(H2);
      for (int k = 0; k < H2; ++k) {
        a2[k] += aj * row[k];
      }
    }
    for (int k = 0; k < H2; ++k) {
      const float v = a2[k];
      a2[k] = (v > 0.0f) ? v : 0.0f;
    }
  }

  void computeHidden(const float *__restrict__ x, float *__restrict__ a1,
                     float *__restrict__ a2) const noexcept {
    if (h1 == 128 && h2 == 64) {
      computeHiddenFixed<128, 64>(x, a1, a2);
      return;
    }
    if (h1 == 256 && h2 == 64) {
      computeHiddenFixed<256, 64>(x, a1, a2);
      return;
    }
    const float *__restrict__ w1 = params.data() + offW1();
    const float *__restrict__ b1 = params.data() + offB1();
    const float *__restrict__ w2 = params.data() + offW2();
    const float *__restrict__ b2 = params.data() + offB2();

    for (int j = 0; j < h1; ++j) {
      a1[j] = b1[j];
    }
    for (int i = 0; i < inDim; ++i) {
      const float xi = x[i];
      if (xi == 0.0f)
        continue;
      const float *__restrict__ row = w1 + static_cast<size_t>(i) * static_cast<size_t>(h1);
      for (int j = 0; j < h1; ++j) {
        a1[j] += xi * row[j];
      }
    }
    for (int j = 0; j < h1; ++j) {
      const float v = a1[j];
      a1[j] = (v > 0.0f) ? v : 0.0f;
    }

    for (int k = 0; k < h2; ++k) {
      a2[k] = b2[k];
    }
    for (int j = 0; j < h1; ++j) {
      const float aj = a1[j];
      const float *__restrict__ row = w2 + static_cast<size_t>(j) * static_cast<size_t>(h2);
      for (int k = 0; k < h2; ++k) {
        a2[k] += aj * row[k];
      }
    }
    for (int k = 0; k < h2; ++k) {
      const float v = a2[k];
      a2[k] = (v > 0.0f) ? v : 0.0f;
    }
  }

  void forwardWithBuf(const float *x, float *a1, float *a2, float out[NUM_PIECES]) const noexcept {
    computeHidden(x, a1, a2);
    const float *__restrict__ w3 = params.data() + offW3();
    const float *__restrict__ b3 = params.data() + offB3();
    for (int p = 0; p < NUM_PIECES; ++p) {
      out[p] = b3[p]
               + detail::dotProduct4(w3 + static_cast<size_t>(p) * static_cast<size_t>(h2), a2, h2);
    }
  }

  float forwardPieceWithBuf(const float *x, float *a1, float *a2, int pieceIdx) const noexcept {
    computeHidden(x, a1, a2);
    const float *__restrict__ w3 =
        params.data() + offW3() + static_cast<size_t>(pieceIdx) * static_cast<size_t>(h2);
    return params[offB3() + static_cast<size_t>(pieceIdx)] + detail::dotProduct4(w3, a2, h2);
  }

  DAS_NN_NOINLINE void forwardLarge(const float x[NN_INPUT_DIM],
                                    float out[NUM_PIECES]) const noexcept {
    float buf[NN_MAX_HIDDEN_DIM * 2];
    forwardWithBuf(x, buf, buf + h1, out);
  }

  DAS_NN_NOINLINE float forwardPieceLarge(const float x[NN_INPUT_DIM],
                                          int pieceIdx) const noexcept {
    float buf[NN_MAX_HIDDEN_DIM * 2];
    return forwardPieceWithBuf(x, buf, buf + h1, pieceIdx);
  }
};

// ---------------- Neural Search & Calibrated Partial-OUT Selector ----------------

struct NnSearchConfig20 {
  int ply = 2;
  int topK = NN_DEFAULT_3PLY_TOPK;

  static constexpr NnSearchConfig20 ply1() noexcept { return {1, NN_DEFAULT_3PLY_TOPK}; }
  static constexpr NnSearchConfig20 ply2() noexcept { return {2, NN_DEFAULT_3PLY_TOPK}; }
  static constexpr NnSearchConfig20 ply3(int k = NN_DEFAULT_3PLY_TOPK) noexcept { return {3, k}; }
};

namespace detail {

// Best ply-2 reply value: max_m (r_m + gamma * E_{pNext | piece}[max(0, V_hat(b'_m, pNext))]).
// Returns 0.0 on immediate top-out.
inline double bestReplyNn2Ply20(const uint16_t rows[BOARD_ROWS], int piece, int gravity,
                                double gamma, const TinyDasNet &net, RngMode mode,
                                PlacementList &pl) noexcept {
  const Board20 b20 = Board20::fromRows(rows);
  findPlacements10Hz(b20, piece, gravity, pl);
  if (pl.count == 0) {
    return 0.0;
  }
  uint16_t after2[BOARD_ROWS];
  double best = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < pl.count; ++i) {
    const CompactPlacement &p = pl.data[i];
    const uint8_t lc = applyPlacement20(b20, piece, p.rot, p.x, p.y, after2);
    const double sc =
        REWARD_NORM_TETRIS[lc] + gamma * net.evalNextPieceExpected(after2, mode, piece);
    if (sc > best) {
      best = sc;
    }
  }
  return best;
}

struct Ply2HybridEval20 {
  bool hasMove = false;
  bool anyInTb = false;
  bool bestGroundedFromTb = false;
  uint8_t bestGroundedLc = 0;
  uint8_t bestAllOutLc = 0;
  CompactPlacement bestGroundedMove = {};
  CompactPlacement bestAllOutMove = {};
  double bestIfTbGrounded = 0.0;
  double bestIfAllOut = 0.0;
};

// Best ply-2 reply over all legal nextPiece placements, checking the TB on every ply-2 leaf
// before falling back to NNUE. Ply-2 leaves that re-enter the TB use exact expected V_tb with
// zero NN safety-margin penalty; leaves that remain out-of-TB apply safetyMargin only when at
// least one TB-grounded move (ply-1 or ply-2) is available.
template <typename TbType>
inline Ply2HybridEval20 bestReplyHybrid2Ply20(const uint16_t rows[BOARD_ROWS], int nextPiece,
                                              int gravity, double gamma, const TbType *tb,
                                              const TinyDasNet *nn, double safetyMargin,
                                              RngMode mode, PlacementList &pl) noexcept {
  Ply2HybridEval20 res{};
  if (nextPiece < 0 || nextPiece >= NUM_PIECES) {
    return res;
  }
  const Board20 b20 = Board20::fromRows(rows);
  findPlacements10Hz(b20, nextPiece, gravity, pl);
  if (pl.count == 0) {
    return res;
  }
  res.hasMove = true;

  const bool hasTb = (tb != nullptr && tb->isLoaded());
  const bool hasNn = (nn != nullptr && nn->isLoaded());
  const bool finiteMargin = std::isfinite(safetyMargin);
  const double clampedMargin = finiteMargin ? std::max(0.0, safetyMargin) : 0.0;
  const double tieEps = 1e-12;

  uint16_t after2[BOARD_ROWS];
  float tbRow[NUM_PIECES];
  double bestGrounded = -std::numeric_limits<double>::infinity();
  double bestAllOut = -std::numeric_limits<double>::infinity();
  bool bestGroundedTb = false;
  CompactPlacement bestGroundedMove{};
  CompactPlacement bestAllOutMove{};
  uint8_t bestGroundedLc = 0;
  uint8_t bestAllOutLc = 0;

  for (int i = 0; i < pl.count; ++i) {
    const CompactPlacement &p = pl.data[i];
    const uint8_t lc2 = applyPlacement20(b20, nextPiece, p.rot, p.x, p.y, after2);
    const double rew2 = REWARD_NORM_TETRIS[lc2];
    const int sr =
        (lc2 == 0)
            ? std::max(0, std::min(static_cast<int>(b20.topNonEmptyRow), static_cast<int>(p.y)))
            : 0;
    BoardKey key2{};
    bool hitTb = false;
    double qGrounded = rew2;
    double qAllOut = rew2;

    if (hasTb && encodeBoardKey(after2, key2, sr) && tb->lookup(key2, tbRow)) {
      double evTb = 0.0;
      for (int p3 = 0; p3 < NUM_PIECES; ++p3) {
        evTb += pieceTransitionProb(mode, nextPiece, p3) * static_cast<double>(tbRow[p3]);
      }
      hitTb = true;
      res.anyInTb = true;
      qGrounded = rew2 + gamma * evTb;
      qAllOut = qGrounded;
    } else if (hasNn) {
      const double evNn = nn->evalNextPieceExpected(after2, mode, nextPiece);
      qAllOut = rew2 + gamma * evNn;
      qGrounded = finiteMargin ? (rew2 + gamma * std::max(0.0, evNn - clampedMargin)) : rew2;
    }

    if (i == 0 || qGrounded > bestGrounded + tieEps) {
      bestGrounded = qGrounded;
      bestGroundedTb = hitTb;
      bestGroundedMove = p;
      bestGroundedLc = lc2;
    } else if (std::abs(qGrounded - bestGrounded) <= tieEps && hitTb && !bestGroundedTb) {
      bestGrounded = qGrounded;
      bestGroundedTb = true;
      bestGroundedMove = p;
      bestGroundedLc = lc2;
    }
    if (i == 0 || qAllOut > bestAllOut) {
      bestAllOut = qAllOut;
      bestAllOutMove = p;
      bestAllOutLc = lc2;
    }
  }

  res.bestIfTbGrounded = bestGrounded;
  res.bestIfAllOut = bestAllOut;
  res.bestGroundedFromTb = bestGroundedTb;
  res.bestGroundedMove = bestGroundedMove;
  res.bestAllOutMove = bestAllOutMove;
  res.bestGroundedLc = bestGroundedLc;
  res.bestAllOutLc = bestAllOutLc;
  return res;
}

inline double bestReplyNn3PlyTopK20(const uint16_t afterRows[BOARD_ROWS], int nextPiece,
                                    int gravity, double gamma, const TinyDasNet &net, RngMode mode,
                                    int topK = NN_DEFAULT_3PLY_TOPK) noexcept {
  if (!net.isLoaded() || nextPiece < 0 || nextPiece >= NUM_PIECES) {
    return 0.0;
  }
  const Board20 b2 = Board20::fromRows(afterRows);
  PlacementList pl2;
  findPlacements10Hz(b2, nextPiece, gravity, pl2);
  if (pl2.count == 0) {
    return 0.0;
  }
  if (topK <= 0) {
    return bestReplyNn2Ply20(afterRows, nextPiece, gravity, gamma, net, mode, pl2);
  }

  struct Ply2Cand {
    double sc;
    int idx;
    uint8_t lc;
    uint16_t rows[BOARD_ROWS];
  };
  std::vector<Ply2Cand> cands(static_cast<size_t>(pl2.count));
  for (int i = 0; i < pl2.count; ++i) {
    const CompactPlacement &p = pl2.data[i];
    cands[static_cast<size_t>(i)].idx = i;
    cands[static_cast<size_t>(i)].lc =
        applyPlacement20(b2, nextPiece, p.rot, p.x, p.y, cands[static_cast<size_t>(i)].rows);
    cands[static_cast<size_t>(i)].sc =
        REWARD_NORM_TETRIS[cands[static_cast<size_t>(i)].lc]
        + gamma * net.evalNextPieceExpected(cands[static_cast<size_t>(i)].rows, mode, nextPiece);
  }
  const int keep = std::min(pl2.count, topK);
  if (keep < pl2.count) {
    std::partial_sort(cands.begin(), cands.begin() + keep, cands.end(),
                      [](const Ply2Cand &a, const Ply2Cand &b) {
                        return (a.sc != b.sc) ? (a.sc > b.sc) : (a.idx < b.idx);
                      });
  }

  PlacementList pl3;
  double best2 = -std::numeric_limits<double>::infinity();
  for (int k = 0; k < keep; ++k) {
    double expPly3 = 0.0;
    for (int p3 = 0; p3 < NUM_PIECES; ++p3) {
      const double b3 =
          bestReplyNn2Ply20(cands[static_cast<size_t>(k)].rows, p3, gravity, gamma, net, mode, pl3);
      expPly3 += pieceTransitionProb(mode, nextPiece, p3) * b3;
    }
    const double sc2 = REWARD_NORM_TETRIS[cands[static_cast<size_t>(k)].lc] + gamma * expPly3;
    if (sc2 > best2) {
      best2 = sc2;
    }
  }
  return best2;
}

} // namespace detail

inline double scoreMoveNn1Ply20(const uint16_t afterRows[BOARD_ROWS], uint8_t linesCleared,
                                int nextPiece, int gravity, double gamma,
                                const TinyDasNet &net) noexcept {
  const double r1 = REWARD_NORM_TETRIS[linesCleared];
  if (!net.isLoaded() || nextPiece < 0 || nextPiece >= NUM_PIECES) {
    return r1;
  }
  const Board20 b20 = Board20::fromRows(afterRows);
  if (!hasAnyLegalMove10Hz(b20, nextPiece, gravity)) {
    return r1;
  }
  return r1 + gamma * std::max(0.0, static_cast<double>(net.evalPiece(afterRows, nextPiece)));
}

inline double scoreMoveNn2Ply20(const uint16_t afterRows[BOARD_ROWS], uint8_t linesCleared,
                                int nextPiece, int gravity, double gamma, const TinyDasNet &net,
                                RngMode mode) noexcept {
  const double r1 = REWARD_NORM_TETRIS[linesCleared];
  if (!net.isLoaded() || nextPiece < 0 || nextPiece >= NUM_PIECES) {
    return r1;
  }
  PlacementList pl2;
  return r1
         + gamma * detail::bestReplyNn2Ply20(afterRows, nextPiece, gravity, gamma, net, mode, pl2);
}

inline double scoreMoveNn3PlyTopK20(const uint16_t afterRows[BOARD_ROWS], uint8_t linesCleared,
                                    int nextPiece, int gravity, double gamma, const TinyDasNet &net,
                                    RngMode mode, int topK = NN_DEFAULT_3PLY_TOPK) noexcept {
  const double r1 = REWARD_NORM_TETRIS[linesCleared];
  if (!net.isLoaded() || nextPiece < 0 || nextPiece >= NUM_PIECES) {
    return r1;
  }
  return r1
         + gamma
               * detail::bestReplyNn3PlyTopK20(afterRows, nextPiece, gravity, gamma, net, mode,
                                               topK);
}

inline double scoreMoveNn20(const uint16_t afterRows[BOARD_ROWS], uint8_t linesCleared,
                            int nextPiece, int gravity, double gamma, const TinyDasNet &net,
                            RngMode mode, NnSearchConfig20 cfg = {}) noexcept {
  if (cfg.ply >= 3) {
    return scoreMoveNn3PlyTopK20(afterRows, linesCleared, nextPiece, gravity, gamma, net, mode,
                                 cfg.topK);
  }
  if (cfg.ply == 2) {
    return scoreMoveNn2Ply20(afterRows, linesCleared, nextPiece, gravity, gamma, net, mode);
  }
  return scoreMoveNn1Ply20(afterRows, linesCleared, nextPiece, gravity, gamma, net);
}

struct HybridMoveChoice20 {
  bool found = false;
  CompactPlacement placement = {};
  uint8_t linesCleared = 0;
  uint16_t afterRows[BOARD_ROWS] = {};
  BoardKey afterKey = EMPTY_KEY;
  bool afterFitsKey = false;
  bool chosenInTb = false;
  bool usedAllOutFallback = false;
  double score = 0.0;
};

// Calibrated Partial-OUT Hybrid TB + NNUE move selector:
//   - Pass 1: TB lookup across all legal placements.
//   - If ALL moves exit TB (all-OUT): rank directly via NN without margin clamping.
//   - If >= 1 move stays in TB: in-TB scores r + gamma * V_tb; out-of-TB scores
//     r + gamma * max(0, estCont - margin) when margin is finite (in-TB wins ties).
template <typename TbType>
inline HybridMoveChoice20
chooseHybridMove20(const Board20 &board, int curPiece, int nextPiece, int gravity, double gamma,
                   const TbType *tb, const TinyDasNet *nn, double safetyMargin, RngMode rngMode,
                   NnSearchConfig20 outCfg = {}) noexcept {
  HybridMoveChoice20 out{};
  if (curPiece < 0 || curPiece >= NUM_PIECES || nextPiece < 0 || nextPiece >= NUM_PIECES) {
    return out;
  }
  PlacementList pl;
  findPlacements10Hz(board, curPiece, gravity, pl);
  if (pl.count == 0) {
    return out;
  }

  const bool hasTb = (tb != nullptr && tb->isLoaded());
  const bool hasNn = (nn != nullptr && nn->isLoaded());
  const bool allowPartialOut = hasNn && std::isfinite(safetyMargin);
  const double clampedMargin = allowPartialOut ? std::max(0.0, safetyMargin) : 0.0;

  struct CandMeta {
    uint8_t lc;
    uint8_t fitsKey;
    uint8_t inTb;
    float tbVal;
    BoardKey key;
    uint16_t rows[BOARD_ROWS];
    detail::Ply2HybridEval20 ply2;
  };
  std::vector<CandMeta> metas(static_cast<size_t>(pl.count));
  bool anyInTb = false;
  double bestInTbQ = -std::numeric_limits<double>::infinity();

  for (int i = 0; i < pl.count; ++i) {
    const CompactPlacement &p = pl.data[i];
    auto &cm = metas[static_cast<size_t>(i)];
    cm.lc = applyPlacement20(board, curPiece, p.rot, p.x, p.y, cm.rows);
    const int startRow =
        (cm.lc == 0)
            ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow), static_cast<int>(p.y)))
            : 0;
    cm.fitsKey = static_cast<uint8_t>(encodeBoardKey(cm.rows, cm.key, startRow) ? 1 : 0);
    cm.inTb = 0;
    cm.tbVal = 0.0f;
    if (hasTb && cm.fitsKey) {
      float v = 0.0f;
      if (tb->lookupPiece(cm.key, nextPiece, v)) {
        cm.inTb = 1;
        cm.tbVal = v;
        anyInTb = true;
        const double q = REWARD_NORM_TETRIS[cm.lc] + gamma * static_cast<double>(v);
        if (q > bestInTbQ)
          bestInTbQ = q;
      }
    }
  }

  // For ply == 2, expand out-of-TB ply-1 moves and check ply-2 leaves against the TB first so
  // temporary overhangs/tucks resolved by nextPiece re-enter the TB with exact value (no margin).
  if (outCfg.ply == 2) {
    bool anyTbGrounded = anyInTb;
    PlacementList pl2;
    for (int i = 0; i < pl.count; ++i) {
      auto &cm = metas[static_cast<size_t>(i)];
      if (!cm.inTb) {
        cm.ply2 = detail::bestReplyHybrid2Ply20(cm.rows, nextPiece, gravity, gamma, tb, nn,
                                                safetyMargin, rngMode, pl2);
        if (cm.ply2.anyInTb) {
          anyTbGrounded = true;
        }
      }
    }

    const double tieEps = 1e-12;
    int bestIdx = 0;
    double bestQ = -std::numeric_limits<double>::infinity();
    bool bestGrounded = false;
    bool bestDirectTb = false;

    for (int i = 0; i < pl.count; ++i) {
      const auto &cm = metas[static_cast<size_t>(i)];
      const double rew = REWARD_NORM_TETRIS[cm.lc];
      double q = rew;
      bool grounded = false;
      bool directTb = false;
      if (cm.inTb) {
        q += gamma * static_cast<double>(cm.tbVal);
        grounded = true;
        directTb = true;
      } else {
        q += gamma * (anyTbGrounded ? cm.ply2.bestIfTbGrounded : cm.ply2.bestIfAllOut);
        grounded = anyTbGrounded && cm.ply2.bestGroundedFromTb;
      }

      const bool strictlyBetter = (i == 0 || q > bestQ + tieEps);
      const bool tieBetter = !strictlyBetter
                             && (std::abs(q - bestQ) <= tieEps)
                             && ((grounded && !bestGrounded)
                                 || (directTb && !bestDirectTb && grounded == bestGrounded));
      if (strictlyBetter || tieBetter) {
        bestIdx = i;
        bestQ = q;
        bestGrounded = grounded;
        bestDirectTb = directTb;
      }
    }

    const auto &cm = metas[static_cast<size_t>(bestIdx)];
    out.found = true;
    out.placement = pl.data[bestIdx];
    out.linesCleared = cm.lc;
    std::memcpy(out.afterRows, cm.rows, sizeof(out.afterRows));
    out.afterKey = cm.key;
    out.afterFitsKey = (cm.fitsKey != 0);
    out.chosenInTb = (cm.inTb != 0);
    out.usedAllOutFallback = !anyTbGrounded;
    out.score = bestQ;
    return out;
  }

  if (!anyInTb) {
    int bestIdx = 0;
    double bestSc = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < pl.count; ++i) {
      const auto &cm = metas[static_cast<size_t>(i)];
      const double sc =
          hasNn ? scoreMoveNn20(cm.rows, cm.lc, nextPiece, gravity, gamma, *nn, rngMode, outCfg)
                : REWARD_NORM_TETRIS[cm.lc];
      if (i == 0 || sc > bestSc) {
        bestSc = sc;
        bestIdx = i;
      }
    }
    const auto &cm = metas[static_cast<size_t>(bestIdx)];
    out.found = true;
    out.placement = pl.data[bestIdx];
    out.linesCleared = cm.lc;
    std::memcpy(out.afterRows, cm.rows, sizeof(out.afterRows));
    out.afterKey = cm.key;
    out.afterFitsKey = (cm.fitsKey != 0);
    out.chosenInTb = false;
    out.usedAllOutFallback = true;
    out.score = bestSc;
    return out;
  }

  const double tieEps = allowPartialOut ? 1e-12 : 0.0;
  int bestIdx = 0;
  double bestQ = -std::numeric_limits<double>::infinity();
  bool bestInTb = false;

  for (int i = 0; i < pl.count; ++i) {
    const auto &cm = metas[static_cast<size_t>(i)];
    const double rew = REWARD_NORM_TETRIS[cm.lc];
    double q = rew;
    if (cm.inTb) {
      q += gamma * static_cast<double>(cm.tbVal);
    } else if (allowPartialOut) {
      double estCont = 0.0;
      if (outCfg.ply >= 3) {
        PlacementList pl2;
        const double est2 =
            detail::bestReplyNn2Ply20(cm.rows, nextPiece, gravity, gamma, *nn, rngMode, pl2);
        if (rew + gamma * std::max(0.0, est2 - clampedMargin) + NN_3PLY_PRESCREEN_SLACK
            < bestInTbQ) {
          estCont = est2;
        } else {
          estCont = detail::bestReplyNn3PlyTopK20(cm.rows, nextPiece, gravity, gamma, *nn, rngMode,
                                                  outCfg.topK);
        }
      } else if (outCfg.ply == 2) {
        PlacementList pl2;
        estCont = detail::bestReplyNn2Ply20(cm.rows, nextPiece, gravity, gamma, *nn, rngMode, pl2);
      } else {
        const Board20 bAfter = Board20::fromRows(cm.rows);
        if (hasAnyLegalMove10Hz(bAfter, nextPiece, gravity)) {
          estCont = std::max(0.0, static_cast<double>(nn->evalPiece(cm.rows, nextPiece)));
        }
      }
      q += gamma * std::max(0.0, estCont - clampedMargin);
    }

    if (i == 0 || q > bestQ + tieEps) {
      bestIdx = i;
      bestQ = q;
      bestInTb = (cm.inTb != 0);
    } else if (std::abs(q - bestQ) <= tieEps && allowPartialOut && cm.inTb && !bestInTb) {
      bestIdx = i;
      bestQ = q;
      bestInTb = true;
    }
  }

  const auto &cm = metas[static_cast<size_t>(bestIdx)];
  out.found = true;
  out.placement = pl.data[bestIdx];
  out.linesCleared = cm.lc;
  std::memcpy(out.afterRows, cm.rows, sizeof(out.afterRows));
  out.afterKey = cm.key;
  out.afterFitsKey = (cm.fitsKey != 0);
  out.chosenInTb = bestInTb;
  out.usedAllOutFallback = false;
  out.score = bestQ;
  return out;
}

// Frozen neural soft-boundary evaluator for solve20MPI:
// Returns conservative B(b', n) = max(0, estCont(b', n) - margin) across all 7 next pieces.
struct NnSoftBoundaryEval20 {
  static constexpr bool enabled = true;
  const TinyDasNet *net = nullptr;
  double margin = 0.6;
  int gravity = GRAVITY_L19;
  double gamma = 0.995;
  RngMode rngMode = RngMode::Nes;
  bool use2Ply = false;

  void evalRow(const uint16_t afterRows[BOARD_ROWS], double outCont[NUM_PIECES]) const noexcept {
    if (net == nullptr || !net->isLoaded() || !std::isfinite(margin)) {
      for (int n = 0; n < NUM_PIECES; ++n)
        outCont[n] = 0.0;
      return;
    }
    const double m = std::max(0.0, margin);
    if (use2Ply) {
      PlacementList pl2;
      for (int n = 0; n < NUM_PIECES; ++n) {
        const double est =
            detail::bestReplyNn2Ply20(afterRows, n, gravity, gamma, *net, rngMode, pl2);
        outCont[n] = (std::isfinite(est) && est > m) ? (est - m) : 0.0;
      }
      return;
    }
    float v[NUM_PIECES];
    net->evalAll(afterRows, v);
    // When rows 0..2 are completely empty (~95% of boards), every piece trivially has a legal spawn
    // placement, avoiding Board20::fromRows + 7x hasAnyLegalMove10Hz.
    const bool topClear = ((afterRows[0] | afterRows[1] | afterRows[2]) & FULL_ROW_MASK) == 0U;
    if (topClear) {
      for (int n = 0; n < NUM_PIECES; ++n) {
        const double pred = static_cast<double>(v[n]);
        outCont[n] = (std::isfinite(pred) && pred > m) ? (pred - m) : 0.0;
      }
      return;
    }
    const Board20 bAfter = Board20::fromRows(afterRows);
    for (int n = 0; n < NUM_PIECES; ++n) {
      const double pred = static_cast<double>(v[n]);
      if (!std::isfinite(pred) || pred <= m || !hasAnyLegalMove10Hz(bAfter, n, gravity)) {
        outCont[n] = 0.0;
      } else {
        outCont[n] = pred - m;
      }
    }
  }
};

// ---------------- On-Policy Distillation Dataset & 4-Part Multi-Task Trainer ----------------

struct LabeledMoveCand20 {
  std::array<uint16_t, BOARD_ROWS> afterRows{};
  uint8_t linesCleared = 0;
  double qStar = 0.0;          // r_m + gamma * V_tb(b', nextPiece) (or r_m + 0.0 for ply-2 top-out)
  double childValTarget = 0.0; // V_tb(b', nextPiece) (or 0.0 for ply-2 top-out)
};

struct LabeledDecision20 {
  std::array<uint16_t, BOARD_ROWS> rows{};
  int curPiece = 0;
  int nextPiece = 0;
  bool boardInTb = false;
  std::array<float, NUM_PIECES> vStar{};
  double bestQStar = 0.0;
  std::vector<LabeledMoveCand20> moves;
};

struct DistillSampleConfig20 {
  int numGames = 4000;
  int maxDecisions = 300000;
  int maxPiecesPerGame = 600;
  double recoverFrac = 0.30; // 70% greedy TB+NN + 30% epsilon-recovery rollouts
  double recoverEpsilon = 0.08;
  int exploreTopK = 4;
  double rolloutMargin = 0.6; // Margin used when guiding out-of-TB rollout steps with prior NN
  bool includeTerminalOutMoves = true;
  uint64_t baseSeed = 20000000ULL;
  int numThreads = 1;
};

struct DistillDataset20 {
  std::vector<LabeledDecision20> decisions;
  uint64_t totalSteps = 0;
  uint64_t skippedIncomplete = 0;
};

// Deterministic (1T == MT) on-policy trajectory sampler for supervised NNUE distillation.
template <typename TbType>
inline DistillDataset20 sampleDistillDataset20(const TbType &tb, const TinyDasNet *guideNn,
                                               const DistillSampleConfig20 &cfg) {
  DistillDataset20 out{};
  if (!tb.isLoaded() || cfg.numGames <= 0 || cfg.maxDecisions <= 0) {
    return out;
  }
  const RngMode rngMode = tb.rngMode();
  const int gravity = tb.gravity();
  const double gamma = tb.gamma();
  const int threads = std::max(1, cfg.numThreads);
  const int recoverMod = std::clamp(static_cast<int>(std::lround(cfg.recoverFrac * 100.0)), 0, 100);
  const int chunkGames = std::max(32, threads * 4);
  const bool hasGuide = (guideNn != nullptr && guideNn->isLoaded());

  out.decisions.reserve(static_cast<size_t>(cfg.maxDecisions));
  std::vector<std::vector<LabeledDecision20>> perGame(static_cast<size_t>(chunkGames));
  std::vector<uint64_t> perSteps(static_cast<size_t>(chunkGames), 0);
  std::vector<uint64_t> perSkipped(static_cast<size_t>(chunkGames), 0);

  for (int baseG = 0;
       baseG < cfg.numGames && static_cast<int>(out.decisions.size()) < cfg.maxDecisions;
       baseG += chunkGames) {
    const int batchCount = std::min(chunkGames, cfg.numGames - baseG);
    for (int i = 0; i < batchCount; ++i) {
      perGame[static_cast<size_t>(i)].clear();
      perSteps[static_cast<size_t>(i)] = 0;
      perSkipped[static_cast<size_t>(i)] = 0;
    }

    std::atomic<int> nextLocal{0};
    detail::runWorkerPool(threads, static_cast<size_t>(batchCount), 8, [&](int /*tid*/) {
      PlacementList pl;
      uint16_t afterRows[BOARD_ROWS];
      float parentRow[NUM_PIECES];
      while (true) {
        const int local = nextLocal.fetch_add(1, std::memory_order_relaxed);
        if (local >= batchCount)
          break;
        const int g = baseG + local;
        const bool isRecover = ((g % 100) < recoverMod);
        const double eps = isRecover ? cfg.recoverEpsilon : 0.0;

        Rng rng(cfg.baseSeed + static_cast<uint64_t>(g) * 1009ULL);
        int curP = rng.sampleOpeningPiece(rngMode);
        int nextP = rng.sampleNextPiece(rngMode, curP);
        Board20 board = Board20::empty();
        auto &gameVec = perGame[static_cast<size_t>(local)];
        uint64_t steps = 0;
        uint64_t skipped = 0;

        for (int step = 0; step < cfg.maxPiecesPerGame; ++step) {
          findPlacements10Hz(board, curP, gravity, pl);
          if (pl.count == 0)
            break;

          // Collect labeled candidate moves (all in-TB moves + exact ply-2 top-out moves).
          double bestQ = -std::numeric_limits<double>::infinity();
          double minQ = std::numeric_limits<double>::infinity();
          std::vector<LabeledMoveCand20> cands;
          cands.reserve(static_cast<size_t>(pl.count));

          struct RolloutCand {
            double inTbScore;
            double rawNnScore;
            uint16_t rows[BOARD_ROWS];
          };
          std::vector<RolloutCand> rCands;
          rCands.reserve(static_cast<size_t>(pl.count));
          bool anyInTb = false;

          for (int m = 0; m < pl.count; ++m) {
            const CompactPlacement &p = pl.data[m];
            const uint8_t lc = applyPlacement20(board, curP, p.rot, p.x, p.y, afterRows);
            const double rew = REWARD_NORM_TETRIS[lc];
            const int startRow = (lc == 0)
                                     ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                            static_cast<int>(p.y)))
                                     : 0;
            BoardKey afterKey;
            const bool fitsKey = encodeBoardKey(afterRows, afterKey, startRow);
            float vNext = 0.0f;
            const bool inTb = fitsKey && tb.lookupPiece(afterKey, nextP, vNext);

            double inTbSc = rew;
            double rawNnSc = rew;
            if (inTb) {
              anyInTb = true;
              const double q = rew + gamma * static_cast<double>(vNext);
              inTbSc = q;
              rawNnSc = q;
              LabeledMoveCand20 cand{};
              std::memcpy(cand.afterRows.data(), afterRows, sizeof(afterRows));
              cand.linesCleared = lc;
              cand.qStar = q;
              cand.childValTarget = static_cast<double>(vNext);
              cands.push_back(cand);
              if (q > bestQ)
                bestQ = q;
              if (q < minQ)
                minQ = q;
            } else {
              const Board20 bAfter = Board20::fromRows(afterRows);
              const bool nextCanPlay = hasAnyLegalMove10Hz(bAfter, nextP, gravity);
              if (!nextCanPlay) {
                if (cfg.includeTerminalOutMoves) {
                  LabeledMoveCand20 cand{};
                  std::memcpy(cand.afterRows.data(), afterRows, sizeof(afterRows));
                  cand.linesCleared = lc;
                  cand.qStar = rew;
                  cand.childValTarget = 0.0;
                  cands.push_back(cand);
                  if (rew > bestQ)
                    bestQ = rew;
                  if (rew < minQ)
                    minQ = rew;
                }
              } else if (hasGuide) {
                const double est =
                    std::max(0.0, static_cast<double>(guideNn->evalPiece(afterRows, nextP)));
                rawNnSc = rew + gamma * est;
                inTbSc = rew + gamma * std::max(0.0, est - cfg.rolloutMargin);
              }
            }

            RolloutCand rc{};
            rc.inTbScore = inTbSc;
            rc.rawNnScore = rawNnSc;
            std::memcpy(rc.rows, afterRows, sizeof(afterRows));
            rCands.push_back(rc);
          }

          if (cands.size() < 2) {
            skipped++;
          } else if (bestQ > minQ + 1e-9) {
            LabeledDecision20 sample{};
            std::memcpy(sample.rows.data(), board.rows(), sizeof(uint16_t) * BOARD_ROWS);
            sample.curPiece = curP;
            sample.nextPiece = nextP;
            BoardKey parentKey;
            if (encodeBoardKey(board.rows(), parentKey, board.topNonEmptyRow)
                && tb.lookup(parentKey, parentRow)) {
              sample.boardInTb = true;
              for (int p = 0; p < NUM_PIECES; ++p) {
                sample.vStar[static_cast<size_t>(p)] = parentRow[p];
              }
            }
            sample.bestQStar = bestQ;
            sample.moves = std::move(cands);
            gameVec.push_back(std::move(sample));
          }

          // Advance rollout state (all-OUT skips rolloutMargin clamping, matching Partial-OUT).
          std::stable_sort(
              rCands.begin(), rCands.end(), [&](const RolloutCand &a, const RolloutCand &b) {
                return anyInTb ? (a.inTbScore > b.inTbScore) : (a.rawNnScore > b.rawNnScore);
              });
          int pick = 0;
          if (eps > 0.0 && rCands.size() > 1 && rng.nextUniform01() < eps) {
            const int hi =
                std::min<int>(static_cast<int>(rCands.size()), std::max(2, cfg.exploreTopK));
            pick = 1 + rng.nextInt(hi - 1);
          }
          board = Board20::fromRows(rCands[static_cast<size_t>(pick)].rows);
          steps++;
          curP = nextP;
          nextP = rng.sampleNextPiece(rngMode, curP);
        }

        perSteps[static_cast<size_t>(local)] = steps;
        perSkipped[static_cast<size_t>(local)] = skipped;
      }
    });

    for (int local = 0; local < batchCount; ++local) {
      out.totalSteps += perSteps[static_cast<size_t>(local)];
      out.skippedIncomplete += perSkipped[static_cast<size_t>(local)];
      for (auto &s : perGame[static_cast<size_t>(local)]) {
        if (static_cast<int>(out.decisions.size()) >= cfg.maxDecisions)
          break;
        out.decisions.push_back(std::move(s));
      }
      if (static_cast<int>(out.decisions.size()) >= cfg.maxDecisions)
        break;
    }
  }
  return out;
}

struct NnTrainConfig20 {
  int h1 = 128;
  int h2 = 64;
  int epochs = 25;
  int batchSize = 256;
  double lr = 2.0e-3;
  double minLr = 1.0e-4;
  double weightDecay = 1.0e-5;
  double valWeight = 1.0;
  double childValWeight = 1.0;
  double rankWeight = 4.0;
  double huberDelta = 0.5;   // Scaled for normalized Tetris-equivalents
  double temperature = 0.35; // Boltzmann move-ranking temperature
  double gamma = 0.995;
  uint64_t seed = 42ULL;
  int numThreads = 1;
  bool verbose = true;
};

struct NnEvalMetrics20 {
  double valMae = 0.0;
  double top1ExactPct = 0.0;
  double meanRegret = 0.0;
};

namespace detail {

// Per-worker reusable scratch eliminating repeated feature extraction, duplicate forward passes,
// and per-batch heap allocations inside computeBatchSliceLossAndGrad.
struct WorkerTrainScratch {
  std::vector<float> parentA1;
  std::vector<float> parentA2;
  std::vector<double> dA1;
  std::vector<double> dA2;
  std::vector<float> candX;
  std::vector<float> candA1;
  std::vector<float> candA2;
  std::vector<double> predV;
  std::vector<double> predQ;
  std::vector<double> trueQ;
  std::vector<double> probP;
  std::vector<double> probQ;

  void ensureHidden(int h1, int h2) {
    if (static_cast<int>(parentA1.size()) != h1) {
      parentA1.resize(static_cast<size_t>(h1));
      dA1.resize(static_cast<size_t>(h1));
    }
    if (static_cast<int>(parentA2.size()) != h2) {
      parentA2.resize(static_cast<size_t>(h2));
      dA2.resize(static_cast<size_t>(h2));
    }
  }

  void ensureCands(size_t mCnt, int h1, int h2) {
    if (predQ.size() < mCnt) {
      predV.resize(mCnt);
      predQ.resize(mCnt);
      trueQ.resize(mCnt);
      probP.resize(mCnt);
      probQ.resize(mCnt);
      candX.resize(mCnt * NN_INPUT_DIM);
      candA1.resize(mCnt * static_cast<size_t>(h1));
      candA2.resize(mCnt * static_cast<size_t>(h2));
    }
  }
};

// Computes 4-part multi-task loss & exact analytical gradient over a slice of decision indices.
// Single feature extraction and single forward pass per parent/candidate with branchless backprop.
inline double computeBatchSliceLossAndGrad(const TinyDasNet &net,
                                           const std::vector<LabeledDecision20> &data,
                                           const int *indices, int count,
                                           const NnTrainConfig20 &cfg, std::vector<double> &gradOut,
                                           WorkerTrainScratch &sc) noexcept {
  const int inD = net.inDim;
  const int h1 = net.h1;
  const int h2 = net.h2;
  const size_t offW1 = net.offW1();
  const size_t offB1 = net.offB1();
  const size_t offW2 = net.offW2();
  const size_t offB2 = net.offB2();
  const size_t offW3 = net.offW3();
  const size_t offB3 = net.offB3();
  const float *__restrict__ w1 = net.params.data() + offW1;
  const float *__restrict__ b1 = net.params.data() + offB1;
  const float *__restrict__ w2 = net.params.data() + offW2;
  const float *__restrict__ b2 = net.params.data() + offB2;
  const float *__restrict__ w3 = net.params.data() + offW3;
  const float *__restrict__ b3 = net.params.data() + offB3;

  sc.ensureHidden(h1, h2);
  double *__restrict__ dA1Ptr = sc.dA1.data();
  double *__restrict__ dA2Ptr = sc.dA2.data();
  double *__restrict__ gPtr = gradOut.data();

  auto forwardHidden = [&](const float *__restrict__ feat, float *__restrict__ a1,
                           float *__restrict__ a2) {
    for (int j = 0; j < h1; ++j)
      a1[j] = b1[j];
    for (int i = 0; i < inD; ++i) {
      const float xi = feat[i];
      if (xi == 0.0f)
        continue;
      const float *__restrict__ row = w1 + static_cast<size_t>(i) * static_cast<size_t>(h1);
      for (int j = 0; j < h1; ++j)
        a1[j] += xi * row[j];
    }
    for (int j = 0; j < h1; ++j)
      a1[j] = (a1[j] > 0.0f) ? a1[j] : 0.0f;

    for (int k = 0; k < h2; ++k)
      a2[k] = b2[k];
    for (int j = 0; j < h1; ++j) {
      const float aj = a1[j];
      const float *__restrict__ row = w2 + static_cast<size_t>(j) * static_cast<size_t>(h2);
      for (int k = 0; k < h2; ++k)
        a2[k] += aj * row[k];
    }
    for (int k = 0; k < h2; ++k)
      a2[k] = (a2[k] > 0.0f) ? a2[k] : 0.0f;
  };

  auto backpropCached = [&](const float *__restrict__ feat, const float *__restrict__ a1,
                            const float *__restrict__ a2, const double dOut[NUM_PIECES],
                            bool onlyPiece, int pieceIdx) {
    for (int k = 0; k < h2; ++k)
      dA2Ptr[k] = 0.0;
    const int pStart = onlyPiece ? pieceIdx : 0;
    const int pEnd = onlyPiece ? (pieceIdx + 1) : NUM_PIECES;
    for (int p = pStart; p < pEnd; ++p) {
      const double dp = dOut[p];
      if (dp == 0.0)
        continue;
      gPtr[offB3 + static_cast<size_t>(p)] += dp;
      const size_t wRowOff = static_cast<size_t>(p) * static_cast<size_t>(h2);
      double *__restrict__ gW3 = gPtr + offW3 + wRowOff;
      const float *__restrict__ w3Row = w3 + wRowOff;
      for (int k = 0; k < h2; ++k) {
        gW3[k] += dp * static_cast<double>(a2[k]);
        dA2Ptr[k] += dp * static_cast<double>(w3Row[k]);
      }
    }

    double *__restrict__ gB2 = gPtr + offB2;
    for (int k = 0; k < h2; ++k) {
      const double dk = (a2[k] > 0.0f) ? dA2Ptr[k] : 0.0;
      dA2Ptr[k] = dk;
      gB2[k] += dk;
    }

    double *__restrict__ gB1 = gPtr + offB1;
    for (int j = 0; j < h1; ++j) {
      const float aj = a1[j];
      if (aj <= 0.0f) {
        dA1Ptr[j] = 0.0;
        continue;
      }
      const double ajD = static_cast<double>(aj);
      const size_t wRowOff = static_cast<size_t>(j) * static_cast<size_t>(h2);
      double *__restrict__ gW2 = gPtr + offW2 + wRowOff;
      const float *__restrict__ w2Row = w2 + wRowOff;
      double sum = 0.0;
      for (int k = 0; k < h2; ++k) {
        const double dk = dA2Ptr[k];
        gW2[k] += ajD * dk;
        sum += static_cast<double>(w2Row[k]) * dk;
      }
      dA1Ptr[j] = sum;
      gB1[j] += sum;
    }

    for (int i = 0; i < inD; ++i) {
      const float xi = feat[i];
      if (xi == 0.0f)
        continue;
      const double xiD = static_cast<double>(xi);
      double *__restrict__ gW1 = gPtr + offW1 + static_cast<size_t>(i) * static_cast<size_t>(h1);
      for (int j = 0; j < h1; ++j) {
        gW1[j] += xiD * dA1Ptr[j];
      }
    }
  };

  double totalLoss = 0.0;
  float parentX[NN_INPUT_DIM];

  for (int idx = 0; idx < count; ++idx) {
    const LabeledDecision20 &d = data[static_cast<size_t>(indices[idx])];

    // 1. Parent 7-output Huber value loss (single extract + single forward)
    if (d.boardInTb && cfg.valWeight > 0.0) {
      extractNnFeatures20(d.rows.data(), parentX);
      forwardHidden(parentX, sc.parentA1.data(), sc.parentA2.data());
      double dOut[NUM_PIECES] = {};
      const double scale = cfg.valWeight / static_cast<double>(NUM_PIECES);
      for (int p = 0; p < NUM_PIECES; ++p) {
        const float pred = b3[p]
                           + dotProduct4(w3 + static_cast<size_t>(p) * static_cast<size_t>(h2),
                                         sc.parentA2.data(), h2);
        double deriv = 0.0;
        const double err =
            static_cast<double>(pred) - static_cast<double>(d.vStar[static_cast<size_t>(p)]);
        totalLoss += scale * huberLossAndGrad(err, cfg.huberDelta, deriv);
        dOut[p] = scale * deriv;
      }
      backpropCached(parentX, sc.parentA1.data(), sc.parentA2.data(), dOut, /*onlyPiece=*/false, 0);
    }

    // 2 & 3 & 4. Candidate-child Huber + Terminal ply-2 top-out + Boltzmann move-ranking CE
    const size_t mCnt = d.moves.size();
    if (mCnt >= 2 && (cfg.childValWeight > 0.0 || cfg.rankWeight > 0.0)) {
      sc.ensureCands(mCnt, h1, h2);
      const int np = d.nextPiece;
      const float *__restrict__ w3Np = w3 + static_cast<size_t>(np) * static_cast<size_t>(h2);
      const float b3Np = b3[np];

      double maxTrue = -std::numeric_limits<double>::infinity();
      double maxPred = -std::numeric_limits<double>::infinity();
      for (size_t m = 0; m < mCnt; ++m) {
        const auto &mv = d.moves[m];
        float *mx = sc.candX.data() + m * NN_INPUT_DIM;
        float *ma1 = sc.candA1.data() + m * static_cast<size_t>(h1);
        float *ma2 = sc.candA2.data() + m * static_cast<size_t>(h2);
        extractNnFeatures20(mv.afterRows.data(), mx);
        forwardHidden(mx, ma1, ma2);
        const double vPred = static_cast<double>(b3Np + dotProduct4(w3Np, ma2, h2));
        sc.predV[m] = vPred;
        sc.predQ[m] = REWARD_NORM_TETRIS[mv.linesCleared] + cfg.gamma * vPred;
        sc.trueQ[m] = mv.qStar;
        if (sc.trueQ[m] > maxTrue)
          maxTrue = sc.trueQ[m];
        if (sc.predQ[m] > maxPred)
          maxPred = sc.predQ[m];
      }

      const double invT = 1.0 / std::max(1e-6, cfg.temperature);
      double sumTrue = 0.0, sumPred = 0.0;
      for (size_t m = 0; m < mCnt; ++m) {
        sc.probP[m] = std::exp((sc.trueQ[m] - maxTrue) * invT);
        sc.probQ[m] = std::exp((sc.predQ[m] - maxPred) * invT);
        sumTrue += sc.probP[m];
        sumPred += sc.probQ[m];
      }
      for (size_t m = 0; m < mCnt; ++m) {
        sc.probP[m] /= sumTrue;
        sc.probQ[m] /= sumPred;
        if (cfg.rankWeight > 0.0 && sc.probP[m] > 1e-15) {
          totalLoss -= cfg.rankWeight * sc.probP[m] * std::log(std::max(1e-15, sc.probQ[m]));
        }
      }

      const double childScale = cfg.childValWeight / static_cast<double>(mCnt);
      for (size_t m = 0; m < mCnt; ++m) {
        const auto &mv = d.moves[m];
        double dPiece = 0.0;
        if (cfg.childValWeight > 0.0) {
          double deriv = 0.0;
          totalLoss +=
              childScale * huberLossAndGrad(sc.predV[m] - mv.childValTarget, cfg.huberDelta, deriv);
          dPiece += childScale * deriv;
        }
        if (cfg.rankWeight > 0.0) {
          dPiece += cfg.rankWeight * (sc.probQ[m] - sc.probP[m]) * invT * cfg.gamma;
        }
        if (dPiece != 0.0) {
          double dOut[NUM_PIECES] = {};
          dOut[np] = dPiece;
          backpropCached(
              sc.candX.data() + m * NN_INPUT_DIM, sc.candA1.data() + m * static_cast<size_t>(h1),
              sc.candA2.data() + m * static_cast<size_t>(h2), dOut, /*onlyPiece=*/true, np);
        }
      }
    }
  }
  return totalLoss;
}

inline double computeBatchSliceLossAndGrad(const TinyDasNet &net,
                                           const std::vector<LabeledDecision20> &data,
                                           const int *indices, int count,
                                           const NnTrainConfig20 &cfg,
                                           std::vector<double> &gradOut) noexcept {
  WorkerTrainScratch sc;
  return computeBatchSliceLossAndGrad(net, data, indices, count, cfg, gradOut, sc);
}

} // namespace detail

inline NnEvalMetrics20
evaluateNnOnDecisions20(const TinyDasNet &net, const std::vector<LabeledDecision20> &valData,
                        double gamma, detail::PersistentWorkerPool *pool = nullptr) noexcept {
  NnEvalMetrics20 m{};
  if (!net.isLoaded() || valData.empty())
    return m;
  const int threads = (pool != nullptr) ? pool->numThreads() : 1;

  struct alignas(128) EvalAccum {
    double maeSum = 0.0;
    uint64_t maeCnt = 0;
    uint64_t exactHits = 0;
    double regretSum = 0.0;
    uint64_t rankCnt = 0;
  };
  std::vector<EvalAccum> accs(static_cast<size_t>(threads));

  auto runRange = [&](int tid, size_t begin, size_t end) {
    EvalAccum &a = accs[static_cast<size_t>(tid)];
    float pred[NUM_PIECES];
    for (size_t idx = begin; idx < end; ++idx) {
      const auto &d = valData[idx];
      if (d.boardInTb) {
        net.evalAll(d.rows.data(), pred);
        for (int p = 0; p < NUM_PIECES; ++p) {
          a.maeSum += std::abs(static_cast<double>(pred[p])
                               - static_cast<double>(d.vStar[static_cast<size_t>(p)]));
          a.maeCnt++;
        }
      }
      if (d.moves.size() >= 2) {
        int bestIdx = 0;
        double bestSc = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < d.moves.size(); ++i) {
          const double sc =
              REWARD_NORM_TETRIS[d.moves[i].linesCleared]
              + gamma
                    * static_cast<double>(net.evalPiece(d.moves[i].afterRows.data(), d.nextPiece));
          if (i == 0 || sc > bestSc) {
            bestSc = sc;
            bestIdx = static_cast<int>(i);
          }
        }
        const double reg = std::max(0.0, d.bestQStar - d.moves[static_cast<size_t>(bestIdx)].qStar);
        if (reg <= 1e-5)
          a.exactHits++;
        a.regretSum += reg;
        a.rankCnt++;
      }
    }
  };

  if (pool != nullptr && valData.size() >= 256) {
    pool->runSubrange(0, valData.size(), 256, runRange);
  } else {
    runRange(0, 0, valData.size());
  }

  double maeSum = 0.0, regretSum = 0.0;
  uint64_t maeCnt = 0, exactHits = 0, rankCnt = 0;
  for (const auto &a : accs) {
    maeSum += a.maeSum;
    maeCnt += a.maeCnt;
    exactHits += a.exactHits;
    regretSum += a.regretSum;
    rankCnt += a.rankCnt;
  }
  m.valMae = maeCnt ? (maeSum / static_cast<double>(maeCnt)) : 0.0;
  m.top1ExactPct =
      rankCnt ? (100.0 * static_cast<double>(exactHits) / static_cast<double>(rankCnt)) : 0.0;
  m.meanRegret = rankCnt ? (regretSum / static_cast<double>(rankCnt)) : 0.0;
  return m;
}

// Deterministic (1T == MT) multi-task Adam trainer for TinyDasNet.
inline TinyDasNet trainTinyDasNet(const std::vector<LabeledDecision20> &trainData,
                                  const std::vector<LabeledDecision20> &valData, RngMode rngMode,
                                  const NnTrainConfig20 &cfg,
                                  const TinyDasNet *warmStart = nullptr) {
  TinyDasNet net;
  if (warmStart != nullptr
      && warmStart->isLoaded()
      && warmStart->h1 == cfg.h1
      && warmStart->h2 == cfg.h2) {
    net = *warmStart;
    net.rngMode = rngMode;
  } else {
    net.initRandom(cfg.h1, cfg.h2, rngMode, cfg.seed);
  }
  if (trainData.empty() || cfg.epochs <= 0) {
    return net;
  }

  const size_t nParams = net.params.size();
  const int threads = std::max(1, cfg.numThreads);
  detail::PersistentWorkerPool pool(threads);

  struct alignas(128) WorkerGradBuf {
    std::vector<double> grad;
    detail::WorkerTrainScratch scratch;
    double loss = 0.0;
  };
  std::vector<WorkerGradBuf> workerBufs(static_cast<size_t>(threads));
  for (auto &wb : workerBufs) {
    wb.grad.assign(nParams, 0.0);
  }

  std::vector<double> mAdam(nParams, 0.0);
  std::vector<double> vAdam(nParams, 0.0);
  std::vector<int> perm(trainData.size());
  for (size_t i = 0; i < perm.size(); ++i)
    perm[i] = static_cast<int>(i);

  Rng shuffleRng(cfg.seed ^ 0xA5A5A5A55A5A5A5AULL);
  const int batchSz = std::max(1, cfg.batchSize);
  const int stepsPerEpoch = (static_cast<int>(trainData.size()) + batchSz - 1) / batchSz;
  const int totalSteps = std::max(1, cfg.epochs * stepsPerEpoch);
  int globalStep = 0;

  constexpr double BETA1 = 0.9;
  constexpr double BETA2 = 0.999;
  constexpr double EPS = 1e-8;
  constexpr double PI = 3.14159265358979323846;

  TinyDasNet bestNet = net;
  double bestValScore = -std::numeric_limits<double>::infinity();

  for (int ep = 1; ep <= cfg.epochs; ++ep) {
    const auto epStart = std::chrono::steady_clock::now();
    // Deterministic Fisher-Yates shuffle
    for (size_t i = perm.size() - 1; i > 0; --i) {
      const size_t j = static_cast<size_t>(shuffleRng.nextU64() % (i + 1));
      std::swap(perm[i], perm[j]);
    }

    double epochLoss = 0.0;
    for (size_t bOff = 0; bOff < perm.size(); bOff += static_cast<size_t>(batchSz)) {
      const int bCnt =
          static_cast<int>(std::min<size_t>(static_cast<size_t>(batchSz), perm.size() - bOff));
      pool.run([&](int tid) {
        auto &wb = workerBufs[static_cast<size_t>(tid)];
        std::fill(wb.grad.begin(), wb.grad.end(), 0.0);
        const int sBegin = (bCnt * tid) / threads;
        const int sEnd = (bCnt * (tid + 1)) / threads;
        wb.loss =
            (sEnd > sBegin)
                ? detail::computeBatchSliceLossAndGrad(net, trainData, perm.data() + bOff + sBegin,
                                                       sEnd - sBegin, cfg, wb.grad, wb.scratch)
                : 0.0;
      });

      double batchLoss = 0.0;
      for (int t = 0; t < threads; ++t)
        batchLoss += workerBufs[static_cast<size_t>(t)].loss;
      epochLoss += batchLoss;

      globalStep++;
      const double prog =
          static_cast<double>(globalStep - 1) / static_cast<double>(std::max(1, totalSteps - 1));
      const double lr = cfg.minLr + 0.5 * (cfg.lr - cfg.minLr) * (1.0 + std::cos(PI * prog));
      const double bc1 = 1.0 - std::pow(BETA1, static_cast<double>(globalStep));
      const double bc2 = 1.0 - std::pow(BETA2, static_cast<double>(globalStep));
      const double invB = 1.0 / static_cast<double>(bCnt);

      const size_t offB1 = net.offB1();
      const size_t offW2 = net.offW2();
      const size_t offB2 = net.offB2();
      const size_t offW3 = net.offW3();
      const size_t offB3 = net.offB3();

      // Parallelize Adam reduction & parameter update across disjoint parameter slices.
      pool.runSubrange(0, nParams, 4096, [&](int /*tid*/, size_t pBegin, size_t pEnd) {
        for (size_t p = pBegin; p < pEnd; ++p) {
          double g = 0.0;
          for (int t = 0; t < threads; ++t) {
            g += workerBufs[static_cast<size_t>(t)].grad[p];
          }
          g *= invB;
          const bool isWeight =
              (p < offB1) || (p >= offW2 && p < offB2) || (p >= offW3 && p < offB3);
          if (isWeight && cfg.weightDecay > 0.0) {
            g += cfg.weightDecay * static_cast<double>(net.params[p]);
          }
          mAdam[p] = BETA1 * mAdam[p] + (1.0 - BETA1) * g;
          vAdam[p] = BETA2 * vAdam[p] + (1.0 - BETA2) * g * g;
          const double mHat = mAdam[p] / bc1;
          const double vHat = vAdam[p] / bc2;
          net.params[p] = static_cast<float>(static_cast<double>(net.params[p])
                                             - lr * mHat / (std::sqrt(vHat) + EPS));
        }
      });
    }

    const double epSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - epStart).count();
    const NnEvalMetrics20 vm = evaluateNnOnDecisions20(net, valData, cfg.gamma, &pool);
    const double valScore = vm.top1ExactPct - 10.0 * vm.meanRegret - 0.5 * vm.valMae;
    if (valData.empty() || valScore > bestValScore) {
      bestValScore = valScore;
      bestNet = net;
    }
    if (cfg.verbose && (ep == 1 || ep == cfg.epochs || ep % 5 == 0)) {
      printf("  [NN ep=%2d/%d] trainLoss=%.4f | valMAE=%.4f top1=%.2f%% regret=%.4f (%.2fs)\n", ep,
             cfg.epochs, epochLoss / static_cast<double>(trainData.size()), vm.valMae,
             vm.top1ExactPct, vm.meanRegret, epSec);
      fflush(stdout);
    }
  }
  return valData.empty() ? net : bestNet;
}

} // namespace das

#endif // DAS_TABLEBASE_NNUE_20ROW_HPP
