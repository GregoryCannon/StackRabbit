#ifndef DAS_TABLEBASE_CORE_20ROW_HPP
#define DAS_TABLEBASE_CORE_20ROW_HPP

#include "board_key.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace das {

inline constexpr int NUM_PIECES = 7;
inline constexpr char PIECE_CHARS[NUM_PIECES + 1] = "IOLJTSZ";
inline constexpr int SPAWN_COL = 3;
inline constexpr int GRAVITY_L18 = 3;
inline constexpr int GRAVITY_L19 = 2;
inline constexpr int GRAVITY_L29 = 1;
inline constexpr int TIMELINE_10HZ_PERIOD = 6; // "X.....": input on frame % 6 == 0
inline constexpr uint16_t PIECE_NONE = 0xFFFFU;
inline constexpr int MAX_PLACEMENTS = 256;

enum class RngMode : uint32_t {
  Uniform = 0,
  Nes = 1,
};

// Exact NES Tetris transition numerators out of 64, indexed by [prevPiece][nextPiece] in
// PIECE_CHARS (I, O, L, J, T, S, Z) order (remapped from src/backend/piece_rng.cpp).
inline constexpr int NES_TRANSITION_DENOM = 64;
static_assert((NES_TRANSITION_DENOM & (NES_TRANSITION_DENOM - 1)) == 0,
              "NES_TRANSITION_DENOM must be power of two");
inline constexpr int NES_TRANSITION_NUM[NUM_PIECES][NUM_PIECES] = {
    {2, 10, 10, 10, 10, 12, 10}, // I
    {10, 4, 10, 10, 10, 10, 10}, // O
    {10, 10, 2, 10, 12, 10, 10}, // L
    {10, 10, 10, 2, 12, 10, 10}, // J
    {10, 10, 10, 10, 2, 10, 12}, // T
    {10, 10, 10, 10, 10, 4, 10}, // S
    {10, 10, 10, 12, 10, 10, 2}, // Z
};

// Exact stationary distribution of NES_TRANSITION_NUM (numerators over 335916): pi * P = pi.
inline constexpr uint64_t NES_STATIONARY_DENOM = 335916ULL;
inline constexpr uint64_t NES_STATIONARY_NUM[NUM_PIECES] = {
    46655ULL, 47988ULL, 46655ULL, 47989ULL, 49284ULL, 49321ULL, 48024ULL,
};

inline constexpr double NES_STATIONARY_PROB[NUM_PIECES] = {
    static_cast<double>(NES_STATIONARY_NUM[0]) / static_cast<double>(NES_STATIONARY_DENOM),
    static_cast<double>(NES_STATIONARY_NUM[1]) / static_cast<double>(NES_STATIONARY_DENOM),
    static_cast<double>(NES_STATIONARY_NUM[2]) / static_cast<double>(NES_STATIONARY_DENOM),
    static_cast<double>(NES_STATIONARY_NUM[3]) / static_cast<double>(NES_STATIONARY_DENOM),
    static_cast<double>(NES_STATIONARY_NUM[4]) / static_cast<double>(NES_STATIONARY_DENOM),
    static_cast<double>(NES_STATIONARY_NUM[5]) / static_cast<double>(NES_STATIONARY_DENOM),
    static_cast<double>(NES_STATIONARY_NUM[6]) / static_cast<double>(NES_STATIONARY_DENOM),
};

constexpr double pieceTransitionProb(RngMode mode, int prevPiece, int nextPiece) noexcept {
  assert(prevPiece >= 0 && prevPiece < NUM_PIECES);
  assert(nextPiece >= 0 && nextPiece < NUM_PIECES);
  if (mode == RngMode::Nes) {
    return static_cast<double>(NES_TRANSITION_NUM[prevPiece][nextPiece])
           / static_cast<double>(NES_TRANSITION_DENOM);
  }
  return 1.0 / static_cast<double>(NUM_PIECES);
}

constexpr double openingPieceProb(RngMode mode, int piece) noexcept {
  assert(piece >= 0 && piece < NUM_PIECES);
  return (mode == RngMode::Nes) ? NES_STATIONARY_PROB[piece]
                                : (1.0 / static_cast<double>(NUM_PIECES));
}

template <typename T>
inline double expectedValueFromRow(const T row[NUM_PIECES], RngMode mode) noexcept {
  double ev = 0.0;
  for (int c = 0; c < NUM_PIECES; ++c) {
    ev += openingPieceProb(mode, c) * static_cast<double>(row[c]);
  }
  return ev;
}

constexpr const char *rngModeName(RngMode mode) noexcept {
  return (mode == RngMode::Nes) ? "nes" : "uniform";
}

constexpr bool parseRngMode(std::string_view s, RngMode &out) noexcept {
  if (s == "uniform") {
    out = RngMode::Uniform;
    return true;
  }
  if (s == "nes") {
    out = RngMode::Nes;
    return true;
  }
  return false;
}

// Normalized NES line-clear score in Tetris-equivalents (40 / 100 / 300 / 1200 ratio).
inline constexpr double REWARD_NORM_TETRIS[5] = {
    0.0, 40.0 / 1200.0, 100.0 / 1200.0, 300.0 / 1200.0, 1.0,
};

// Stationary Bellman discount semantics:
//   - PerPiece: factor(lc) = gamma (classic infinite-horizon per-piece discount).
//   - PerLine:  factor(lc) = PER_LINE_STEP_TIEBREAK * gamma^lc (models an expected
//               1/(1-gamma) line horizon where clearing lines consumes line budget and
//               waiting for a Tetris carries negligible 0.01% per-piece tie-break decay).
enum class DiscountMode : uint32_t {
  PerPiece = 0U,
  PerLine = 1U,
};

inline constexpr double PER_LINE_STEP_TIEBREAK = 0.9999;

constexpr const char *discountModeName(DiscountMode mode) noexcept {
  return (mode == DiscountMode::PerLine) ? "line" : "piece";
}

constexpr bool parseDiscountMode(std::string_view s, DiscountMode &out) noexcept {
  if (s == "piece") {
    out = DiscountMode::PerPiece;
    return true;
  }
  if (s == "line") {
    out = DiscountMode::PerLine;
    return true;
  }
  return false;
}

struct StepDiscountTable20 {
  double f[5] = {1.0, 1.0, 1.0, 1.0, 1.0};

  static StepDiscountTable20 make(double gamma, DiscountMode mode) noexcept {
    StepDiscountTable20 t{};
    if (mode == DiscountMode::PerPiece) {
      for (int lc = 0; lc <= 4; ++lc) {
        t.f[lc] = gamma;
      }
    } else {
      double g = PER_LINE_STEP_TIEBREAK;
      for (int lc = 0; lc <= 4; ++lc) {
        t.f[lc] = g;
        g *= gamma;
      }
    }
    return t;
  }
};

inline double stepDiscount20(double gamma, DiscountMode mode, uint8_t linesCleared) noexcept {
  if (mode == DiscountMode::PerPiece) {
    return gamma;
  }
  double g = PER_LINE_STEP_TIEBREAK;
  for (uint8_t i = 0; i < linesCleared; ++i) {
    g *= gamma;
  }
  return g;
}

inline constexpr int NES_LINE_BASE_POINTS[5] = {0, 40, 100, 300, 1200};

inline constexpr int nesRawScore(int linesCleared, int levelAfter) noexcept {
  if (linesCleared < 0 || linesCleared > 4) {
    return 0;
  }
  return NES_LINE_BASE_POINTS[linesCleared] * (levelAfter + 1);
}

// Exact parity with src/backend/utils.hpp::getLevelAfterLineClears.
inline constexpr int getLevelAfterLineClears20(int level, int lines, int numLinesCleared) noexcept {
  if (level == 18 && lines < 126) {
    return 18;
  }
  if (level == 19 && lines < 136) {
    return 19;
  }
  if (level == 29 && lines < 196) {
    return 29;
  }
  if ((lines % 10) + numLinesCleared >= 10) {
    return level + 1;
  }
  return level;
}

inline constexpr int getGravity20(int level) noexcept {
  if (level <= 18) {
    return GRAVITY_L18;
  }
  if (level < 29) {
    return GRAVITY_L19;
  }
  return GRAVITY_L29;
}

// Deterministic splitmix64 PRNG.
struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) noexcept : state(seed) {}

  uint64_t nextU64() noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    return detail::splitmix64Mix(state);
  }

  double nextUniform01() noexcept {
    return static_cast<double>(nextU64() >> 11) * (1.0 / 9007199254740992.0);
  }

  int nextInt(int bound) noexcept {
    assert(bound > 0);
    if (bound <= 1) {
      return 0;
    }
    return static_cast<int>(nextU64() % static_cast<uint64_t>(bound));
  }

  int sampleOpeningPiece(RngMode mode) noexcept {
    if (mode == RngMode::Uniform) {
      return nextInt(NUM_PIECES);
    }
    const uint64_t r = nextU64() % NES_STATIONARY_DENOM;
    uint64_t acc = 0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      acc += NES_STATIONARY_NUM[p];
      if (r < acc) {
        return p;
      }
    }
    return NUM_PIECES - 1;
  }

  int sampleNextPiece(RngMode mode, int prevPiece) noexcept {
    assert(prevPiece >= 0 && prevPiece < NUM_PIECES);
    if (mode == RngMode::Uniform) {
      return nextInt(NUM_PIECES);
    }
    const int r = static_cast<int>(nextU64() & static_cast<uint64_t>(NES_TRANSITION_DENOM - 1));
    int acc = 0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      acc += NES_TRANSITION_NUM[prevPiece][p];
      if (r < acc) {
        return p;
      }
    }
    return NUM_PIECES - 1;
  }
};

// Cache-line-aligned piece geometry (64 B = power-of-2 indexing, L1D-resident).
// Matches src/data/tetrominoes.hpp bit-for-bit.
struct alignas(64) PieceDef20 {
  char id;
  int8_t numOrientations;
  int8_t initialY;
  int8_t maxY[4];
  int8_t minX[4];
  uint8_t xSpan[4];
  int8_t bottomSurface[4][4]; // -1 = column unoccupied
  uint16_t rows[4][4];
};
static_assert(sizeof(PieceDef20) == 64, "PieceDef20 must be exactly 64 bytes");

inline constexpr PieceDef20 PIECES_20[NUM_PIECES] = {
    {'I',
     2,
     -2,
     {17, 16, -1, -1},
     {0, -2, 0, 0},
     {6, 9, 0, 0},
     {{3, 3, 3, 3}, {-1, -1, 4, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}},
     {{0, 0, 960, 0},
      {128, 128, 128, 128},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE}}},
    {'O',
     1,
     -1,
     {17, -1, -1, -1},
     {-1, 0, 0, 0},
     {8, 0, 0, 0},
     {{-1, 3, 3, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}},
     {{0, 384, 384, 0},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE}}},
    {'L',
     4,
     -1,
     {17, 17, 18, 17},
     {-1, -1, -1, -2},
     {7, 8, 7, 8},
     {{-1, 3, 2, 2}, {-1, 1, 3, -1}, {-1, 2, 2, 2}, {-1, -1, 3, 3}},
     {{0, 448, 256, 0}, {384, 128, 128, 0}, {64, 448, 0, 0}, {128, 128, 192, 0}}},
    {'J',
     4,
     -1,
     {17, 17, 18, 17},
     {-1, -1, -1, -2},
     {7, 8, 7, 8},
     {{-1, 2, 2, 3}, {-1, 3, 3, -1}, {-1, 2, 2, 2}, {-1, -1, 3, 1}},
     {{0, 448, 64, 0}, {128, 128, 384, 0}, {256, 448, 0, 0}, {192, 128, 128, 0}}},
    {'T',
     4,
     -1,
     {17, 17, 18, 17},
     {-1, -1, -1, -2},
     {7, 8, 7, 8},
     {{-1, 2, 3, 2}, {-1, 2, 3, -1}, {-1, 2, 2, 2}, {-1, -1, 3, 2}},
     {{0, 448, 128, 0}, {128, 384, 128, 0}, {128, 448, 0, 0}, {128, 192, 128, 0}}},
    {'S',
     2,
     -1,
     {17, 17, -1, -1},
     {-1, -2, 0, 0},
     {7, 8, 0, 0},
     {{-1, 3, 3, 2}, {-1, -1, 2, 3}, {-1, -1, -1, -1}, {-1, -1, -1, -1}},
     {{0, 192, 384, 0},
      {128, 192, 64, 0},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE}}},
    {'Z',
     2,
     -1,
     {17, 17, -1, -1},
     {-1, -2, 0, 0},
     {7, 8, 0, 0},
     {{-1, 2, 3, 3}, {-1, -1, 3, 2}, {-1, -1, -1, -1}, {-1, -1, -1, -1}},
     {{0, 384, 192, 0},
      {64, 192, 128, 0},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE},
      {PIECE_NONE, PIECE_NONE, PIECE_NONE, PIECE_NONE}}},
};

struct TuckOriginSpot20 {
  int8_t orientation;
  int8_t x;
  int8_t y;
};

struct TuckSpotList20 {
  int8_t count;
  TuckOriginSpot20 spots[8];
};

inline constexpr TuckSpotList20 TUCK_SPOTS_20[NUM_PIECES] = {
    {3, {{0, 0, 2}, {0, 3, 2}, {1, 2, 0}}},
    {2, {{0, 1, 1}, {0, 2, 1}}},
    {8, {{0, 1, 1}, {0, 3, 1}, {1, 1, 0}, {1, 2, 0}, {2, 1, 1}, {2, 3, 0}, {3, 2, 0}, {3, 3, 2}}},
    {8, {{0, 1, 1}, {0, 3, 1}, {1, 1, 2}, {1, 2, 0}, {2, 1, 0}, {2, 3, 1}, {3, 2, 0}, {3, 3, 0}}},
    {8, {{0, 1, 1}, {0, 3, 1}, {1, 1, 1}, {1, 2, 0}, {2, 1, 1}, {2, 3, 1}, {3, 2, 0}, {3, 3, 1}}},
    {4, {{0, 1, 2}, {0, 3, 1}, {1, 2, 0}, {1, 3, 1}}},
    {4, {{0, 1, 1}, {0, 3, 2}, {1, 3, 0}, {1, 2, 1}}},
};

struct TuckInput20 {
  char notation;
  int8_t xChange;
  int8_t rotChange;
};

inline constexpr TuckInput20 TUCK_INPUTS_20[8] = {
    {'L', -1, 0}, {'R', 1, 0}, {'A', 0, 1},   {'B', 0, -1},
    {'E', -1, 1}, {'I', 1, 1}, {'F', -1, -1}, {'G', 1, -1},
};

namespace detail {

inline constexpr int PIECE_MASK_X_OFFSET = 2; // legal x in [-2, 7] -> index in [0, 9]
inline constexpr int PIECE_MASK_X_COUNT = 12;

constexpr uint32_t shiftRowBits(uint32_t row, int x) noexcept {
  return (x > 0) ? (row >> x) : (row << (-x));
}

struct PieceMask64Table {
  uint64_t mask[NUM_PIECES][4][PIECE_MASK_X_COUNT];
};

constexpr PieceMask64Table buildPieceMask64Table() noexcept {
  PieceMask64Table t = {};
  for (int p = 0; p < NUM_PIECES; ++p) {
    for (int rot = 0; rot < PIECES_20[p].numOrientations; ++rot) {
      const int loX = PIECES_20[p].minX[rot];
      const int hiX = loX + static_cast<int>(PIECES_20[p].xSpan[rot]);
      for (int x = loX; x <= hiX; ++x) {
        uint64_t packed = 0;
        for (int r = 0; r < 4; ++r) {
          const uint32_t pr = PIECES_20[p].rows[rot][r];
          const uint32_t shifted = shiftRowBits(pr, x) & FULL_ROW_MASK;
          packed |= static_cast<uint64_t>(shifted) << (r * 16);
        }
        t.mask[p][rot][x + PIECE_MASK_X_OFFSET] = packed;
      }
    }
  }
  return t;
}

inline constexpr PieceMask64Table PIECE_MASK_64 = buildPieceMask64Table();

constexpr bool isGravityFrame(int frameIndex, int gravity) noexcept {
  assert(gravity >= 1 && gravity <= 3);
  if (gravity == 2) {
    return (frameIndex & 1) != 0;
  }
  if (gravity == 3) {
    return (frameIndex % 3) == 2;
  }
  return true;
}

constexpr std::array<int8_t, 7> buildShiftYTable10Hz(int gravity, int initialY) noexcept {
  std::array<int8_t, 7> res = {};
  int inputsPerformed = 0;
  int y = initialY;
  int frame = 0;
  while (inputsPerformed <= 5) {
    if ((frame % TIMELINE_10HZ_PERIOD) == 0) {
      res[inputsPerformed + 1] = static_cast<int8_t>(y);
      inputsPerformed++;
    }
    if (isGravityFrame(frame, gravity)) {
      y++;
    }
    frame++;
  }
  return res;
}

inline constexpr std::array<int8_t, 7> SHIFT_Y_10HZ[4][2] = {
    {},
    {buildShiftYTable10Hz(1, -1), buildShiftYTable10Hz(1, -2)},
    {buildShiftYTable10Hz(2, -1), buildShiftYTable10Hz(2, -2)},
    {buildShiftYTable10Hz(3, -1), buildShiftYTable10Hz(3, -2)},
};

inline int rotateTowardsGoal(int curRot, int goalRot) noexcept {
  if (curRot == goalRot) {
    return curRot;
  }
  if (goalRot == curRot - 1 || goalRot == curRot + 3) {
    return goalRot;
  }
  return (curRot + 1) & 3;
}

inline int tuckColIndex(int rot, int x) noexcept { return rot * 10 + x + 2; }

} // namespace detail

// Cache-line-sized 20x10 board (64 B) with 2 leading and 2 trailing zero padding rows so
// 64-bit 4-row loads at paddedRows[y + 2] are always in-bounds for y in [-2, 18].
struct alignas(64) Board20 {
  uint16_t paddedRows[BOARD_ROWS + 4];
  int8_t surface[BOARD_WIDTH];
  uint8_t numHoles;
  uint8_t topNonEmptyRow; // 0..20 (20 = empty board)

  uint16_t *rows() noexcept { return &paddedRows[2]; }
  const uint16_t *rows() const noexcept { return &paddedRows[2]; }

  static Board20 empty() noexcept {
    Board20 b = {};
    b.topNonEmptyRow = BOARD_ROWS;
    return b;
  }

  void recomputeSurfaceAndHoles() noexcept {
    paddedRows[0] = 0;
    paddedRows[1] = 0;
    paddedRows[BOARD_ROWS + 2] = 0;
    paddedRows[BOARD_ROWS + 3] = 0;
    std::memset(surface, 0, sizeof(surface));

    uint32_t covered = 0;
    int holes = 0;
    uint8_t topRow = BOARD_ROWS;
    const uint16_t *rPtr = rows();

    for (int r = 0; r < BOARD_ROWS; ++r) {
      const uint32_t row = static_cast<uint32_t>(rPtr[r]) & FULL_ROW_MASK;
      if (row != 0U && topRow == BOARD_ROWS) {
        topRow = static_cast<uint8_t>(r);
      }
      const uint32_t newTop = row & ~covered;
      if (newTop) {
        uint32_t nt = newTop << 22;
        while (nt) {
          const int col = __builtin_clz(nt);
          nt ^= (0x80000000U >> col);
          surface[col] = static_cast<int8_t>(BOARD_ROWS - r);
        }
      }
      holes += __builtin_popcount((~row) & covered);
      covered |= row;
    }
    numHoles = static_cast<uint8_t>(std::min(holes, 255));
    topNonEmptyRow = topRow;
  }

  static Board20 fromKey(BoardKey key) noexcept {
    Board20 b;
    b.paddedRows[0] = 0;
    b.paddedRows[1] = 0;
    b.paddedRows[BOARD_ROWS + 2] = 0;
    b.paddedRows[BOARD_ROWS + 3] = 0;
    decodeBoardKey(key, b.rows(), b.surface);
    b.numHoles = static_cast<uint8_t>(boardKeyNumHoles(key));
    int maxH = 0;
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      if (b.surface[c] > maxH) {
        maxH = b.surface[c];
      }
    }
    b.topNonEmptyRow = static_cast<uint8_t>(BOARD_ROWS - maxH);
    return b;
  }

  template <typename RowT> static Board20 fromRows(const RowT srcRows[BOARD_ROWS]) noexcept {
    Board20 b;
    uint16_t *dst = b.rows();
    for (int r = 0; r < BOARD_ROWS; ++r) {
      dst[r] = static_cast<uint16_t>(srcRows[r] & FULL_ROW_MASK);
    }
    b.recomputeSurfaceAndHoles();
    return b;
  }
};
static_assert(sizeof(Board20) == 64, "Board20 should fit in one 64-byte cache line");

// Precomputed per-board tuck setup bitmasks (independent of piece and gravity).
struct TuckSetupInfo {
  uint16_t mask[BOARD_ROWS];
  uint32_t nonZeroRowsMask;
  bool any;
};

// Computes exact per-row tuck-setup bitmasks matching src/backend/move_result.cpp::analyzeHole +
// updateSurfaceAndHoles (CAN_TUCK == 1). Short-circuits in 1 cycle on 0-hole boards.
inline TuckSetupInfo computeTuckSetupInfo(const Board20 &board) noexcept {
  TuckSetupInfo info;
  if (board.numHoles == 0) {
    info.nonZeroRowsMask = 0U;
    info.any = false;
    return info;
  }
  std::memset(info.mask, 0, sizeof(info.mask));
  info.nonZeroRowsMask = 0U;

  const uint16_t *rPtr = board.rows();
  uint32_t covered = 0U;
  for (int r = board.topNonEmptyRow; r < BOARD_ROWS; ++r) {
    const uint32_t row = rPtr[r];
    uint32_t holes = (~row) & covered;
    if (holes) {
      uint32_t hMask = holes << 22;
      while (hMask) {
        const int c = __builtin_clz(hMask);
        hMask ^= (0x80000000U >> c);
        const uint16_t bit = colBitMask(c);

        if (c >= 2
            && ((row >> (9 - c)) & 0x7U) == 0
            && (BOARD_ROWS - board.surface[c - 1] >= r + 1)
            && (BOARD_ROWS - board.surface[c - 2] >= r + 1)) {
          info.mask[r] |= bit;
          info.nonZeroRowsMask |= (1U << r);
        } else if (c <= 7 && ((row >> (7 - c)) & 0x7U) == 0) {
          info.mask[r] |= bit;
          info.nonZeroRowsMask |= (1U << r);
        } else if (c == 8
                   && r <= 16
                   && (rPtr[r] & 0x3U) == 0
                   && (rPtr[r + 1] & 0x3U) == 0
                   && (rPtr[r + 2] & 0x3U) == 0
                   && (rPtr[r + 3] & 0x3U) == 0) {
          info.mask[r] |= bit;
          info.mask[r + 1] |= bit;
          info.mask[r + 2] |= bit;
          info.mask[r + 3] |= bit;
          info.nonZeroRowsMask |= (0xFU << r);
        }
      }
    }
    covered |= row;
  }
  info.any = (info.nonZeroRowsMask != 0U);
  return info;
}

struct CompactPlacement {
  int8_t x;
  int8_t y;
  uint8_t rot;
  char tuckInput; // '.' for non-tuck placements
};
static_assert(sizeof(CompactPlacement) == 4, "CompactPlacement should stay 4 bytes");

struct PlacementList {
  CompactPlacement data[MAX_PLACEMENTS];
  int count = 0;

  void clear() noexcept { count = 0; }
  void push(int x, int y, int rot, char tuck) noexcept {
    assert(count < MAX_PLACEMENTS);
    if (count < MAX_PLACEMENTS) {
      data[count++] = {static_cast<int8_t>(x), static_cast<int8_t>(y), static_cast<uint8_t>(rot),
                       tuck};
    }
  }
};

struct BoardTransition {
  BoardKey afterKey; // EMPTY_KEY when !fitsKey
  uint8_t linesCleared;
  uint8_t fitsKey;
  CompactPlacement placement;
};

struct TransitionList {
  BoardTransition data[MAX_PLACEMENTS];
  int count = 0;
  void clear() noexcept { count = 0; }
};

// Branchless 64-bit collision check against padded Board20.
inline bool collides20(const Board20 &board, int pieceIdx, int x, int y, int rot) noexcept {
  const PieceDef20 &piece = PIECES_20[pieceIdx];
  if (rot >= piece.numOrientations || y > piece.maxY[rot] || y < -2) {
    return true;
  }
  if (static_cast<unsigned>(x - piece.minX[rot]) > static_cast<unsigned>(piece.xSpan[rot])) {
    return true;
  }
  uint64_t board4;
  std::memcpy(&board4, &board.paddedRows[y + 2], sizeof(uint64_t));
  return (board4 & detail::PIECE_MASK_64.mask[pieceIdx][rot][x + detail::PIECE_MASK_X_OFFSET])
         != 0ULL;
}

// Applies placement to board.rows(), writing into non-aliasing outRows[20]. Returns lines cleared
// (0..4). Matches src/backend/move_result.cpp::getNewBoardAndLinesCleared bit-for-bit on bits 0..9.
inline uint8_t applyPlacement20(const Board20 &board, int pieceIdx, int rot, int x, int y,
                                uint16_t outRows[BOARD_ROWS]) noexcept {
  const uint16_t *srcRows = board.rows();
  assert(outRows != srcRows);

  const uint64_t pieceMask =
      detail::PIECE_MASK_64.mask[pieceIdx][rot][x + detail::PIECE_MASK_X_OFFSET];
  uint64_t board4;
  std::memcpy(&board4, &board.paddedRows[y + 2], sizeof(uint64_t));
  const uint64_t stamped = board4 | pieceMask;

  // Valid rows use only bits 0..9, so +1 carries into bit 10 iff the row is 0x3FF (full).
  const uint64_t fullMask = (stamped + 0x0001000100010001ULL) & 0x0400040004000400ULL;
  if (fullMask == 0ULL) {
    std::memcpy(outRows, srcRows, BOARD_ROWS * sizeof(uint16_t));
    if (y >= 0 && y <= BOARD_ROWS - 4) {
      std::memcpy(&outRows[y], &stamped, sizeof(uint64_t));
    } else {
      for (int i = 0; i < 4; ++i) {
        const int r = y + i;
        if (r >= 0 && r < BOARD_ROWS) {
          outRows[r] = static_cast<uint16_t>((stamped >> (i * 16)) & FULL_ROW_MASK);
        }
      }
    }
    return 0;
  }

  if (y + 4 < BOARD_ROWS) {
    std::memcpy(&outRows[y + 4], &srcRows[y + 4],
                static_cast<size_t>(BOARD_ROWS - (y + 4)) * sizeof(uint16_t));
  }
  uint8_t cleared = 0;
  for (int i = 3; i >= 0; --i) {
    const int rowIdx = y + i;
    if (rowIdx < 0 || rowIdx >= BOARD_ROWS) {
      continue;
    }
    const uint16_t newRow = static_cast<uint16_t>((stamped >> (i * 16)) & FULL_ROW_MASK);
    if (newRow == FULL_ROW_MASK) {
      cleared++;
      continue;
    }
    outRows[rowIdx + cleared] = newRow;
  }
  if (y > 0) {
    std::memcpy(&outRows[cleared], srcRows, static_cast<size_t>(y) * sizeof(uint16_t));
  }
  outRows[0] = 0;
  if (cleared > 1)
    outRows[1] = 0;
  if (cleared > 2)
    outRows[2] = 0;
  if (cleared > 3)
    outRows[3] = 0;
  return cleared;
}

namespace detail {

inline void emitDroppedPlacement(const Board20 &board, const PieceDef20 &piece, int x, int y,
                                 int rot, bool trackTuckCols, int8_t availableTuckCols[40],
                                 PlacementList &out) noexcept {
  const int8_t *bot = piece.bottomSurface[rot];
  int rowsToShift = 99999;
  for (int c = 0; c < 4; ++c) {
    if (bot[c] < 0) {
      continue;
    }
    const int diff = (BOARD_ROWS - bot[c] - y) - board.surface[x + c];
    if (diff < rowsToShift) {
      rowsToShift = diff;
    }
    if (rowsToShift < 0) {
      return;
    }
  }
  const int lockY = y + rowsToShift;
  if (trackTuckCols) {
    availableTuckCols[tuckColIndex(rot, x)] = static_cast<int8_t>(lockY);
  }
  out.push(x, lockY, rot, '.');
}

// Steps 1 full 6-frame 10Hz input cycle per iteration (exact parity with frame-by-frame
// exploreHorizontally for gravity in {1, 2, 3} starting at frame=0, arr=0).
template <int Gravity>
inline void exploreHorizontally10HzCycle(const Board20 &board, const PieceDef20 &piece,
                                         int pieceIdx, int shiftInc, int maxOrMinX, int goalRot,
                                         bool trackTuckCols, int8_t availableTuckCols[40],
                                         PlacementList &out) noexcept {
  int x = SPAWN_COL;
  int y = piece.initialY;
  int rot = 0;

  constexpr int DROPS_AFTER_INPUT = (Gravity == 3) ? 2 : ((Gravity == 2) ? 3 : 5);

  while (x != maxOrMinX || rot != goalRot) {
    if (x != maxOrMinX) {
      if (collides20(board, pieceIdx, x + shiftInc, y, rot)) {
        return;
      }
      x += shiftInc;
    }
    if (rot != goalRot) {
      const int nextRot = rotateTowardsGoal(rot, goalRot);
      if (collides20(board, pieceIdx, x, y, nextRot)) {
        return;
      }
      rot = nextRot;
    }
    const bool foundNew = (rot == goalRot);

    if constexpr (Gravity == 1) {
      if (collides20(board, pieceIdx, x, y + 1, rot)) {
        if (foundNew) {
          emitDroppedPlacement(board, piece, x, y, rot, trackTuckCols, availableTuckCols, out);
        }
        return;
      }
      y++;
    }

    if (foundNew) {
      emitDroppedPlacement(board, piece, x, y, rot, trackTuckCols, availableTuckCols, out);
    }

    if (x == maxOrMinX && rot == goalRot) {
      return;
    }

    const int8_t *bot = piece.bottomSurface[rot];
    int lockY = 99;
    for (int c = 0; c < 4; ++c) {
      if (bot[c] >= 0) {
        const int colLockY = BOARD_ROWS - bot[c] - board.surface[x + c];
        if (colLockY < lockY) {
          lockY = colLockY;
        }
      }
    }
    if (y + DROPS_AFTER_INPUT <= lockY) {
      y += DROPS_AFTER_INPUT;
    } else {
      for (int d = 0; d < DROPS_AFTER_INPUT; ++d) {
        if (collides20(board, pieceIdx, x, y + 1, rot)) {
          return;
        }
        y++;
      }
    }
  }
}

template <int Gravity>
inline void runBasicSearch10Hz(const Board20 &board, const PieceDef20 &piece, int pieceIdx,
                               bool trackTuckCols, int8_t availableTuckCols[40],
                               PlacementList &out) noexcept {
  emitDroppedPlacement(board, piece, SPAWN_COL, piece.initialY, 0, trackTuckCols, availableTuckCols,
                       out);

  for (int goalRot = 0; goalRot < piece.numOrientations; ++goalRot) {
    exploreHorizontally10HzCycle<Gravity>(board, piece, pieceIdx, -1, -99, goalRot, trackTuckCols,
                                          availableTuckCols, out);
    exploreHorizontally10HzCycle<Gravity>(board, piece, pieceIdx, 1, 99, goalRot, trackTuckCols,
                                          availableTuckCols, out);
    if (goalRot > 0) {
      const int rStart = (goalRot == 2) ? -1 : 0;
      const int rEnd = (goalRot == 2) ? 1 : 0;
      for (int xOff = rStart; xOff <= rEnd; ++xOff) {
        exploreHorizontally10HzCycle<Gravity>(board, piece, pieceIdx, xOff, SPAWN_COL + xOff,
                                              goalRot, trackTuckCols, availableTuckCols, out);
      }
    }
  }
}

inline char findTuckInput20(const Board20 &board, int pieceIdx, int x, int y, int rot,
                            const int8_t availableTuckCols[40],
                            const std::array<int8_t, 7> &minTuckY) noexcept {
  const int numOrients = PIECES_20[pieceIdx].numOrientations;
  const int rotMask = (numOrients == 4) ? 3 : 1;
  for (const TuckInput20 &ti : TUCK_INPUTS_20) {
    const int preX = x - ti.xChange;
    const int preRot = (numOrients == 1) ? rot : ((rot - ti.rotChange + 4) & rotMask);
    const int idx = tuckColIndex(preRot, preX);
    if (idx < 0 || idx >= 40) {
      continue;
    }
    const int numRots = (preRot == 3) ? 1 : preRot;
    const int numInputs = std::max(numRots, std::abs(preX - SPAWN_COL));
    if (numInputs + 1 >= 7) {
      continue;
    }
    const int minY = minTuckY[numInputs + 1];
    const int maxY = static_cast<int>(availableTuckCols[idx]);
    if (y < minY || y > maxY) {
      continue;
    }
    if (ti.rotChange != 0 && collides20(board, pieceIdx, x, y, preRot)) {
      continue;
    }
    if (ti.xChange != 0 && collides20(board, pieceIdx, preX, y, preRot)) {
      continue;
    }
    return ti.notation;
  }
  return '.';
}

} // namespace detail

// Zero-allocation 10 Hz ("X.....") move generator matching
// src/backend/move_search.cpp::fastMoveSearch bit-for-bit (including CAN_TUCK == 1 tucks) for
// gravity in {1, 2, 3}.
inline int findPlacements10Hz(const Board20 &board, const TuckSetupInfo &tucks, int pieceIdx,
                              int gravity, PlacementList &out, bool enableTucks = true) noexcept {
  assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
  assert(gravity >= 1 && gravity <= 3);
  const int safeGrav = (gravity >= 1 && gravity <= 3) ? gravity : GRAVITY_L19;
  out.clear();

  const PieceDef20 &piece = PIECES_20[pieceIdx];
  if (collides20(board, pieceIdx, SPAWN_COL, piece.initialY, 0)) {
    return 0;
  }

  const bool doTucks = enableTucks && tucks.any;
  int8_t availableTuckCols[40];
  if (doTucks) {
    std::memset(availableTuckCols, 0, sizeof(availableTuckCols));
  }

  if (safeGrav == 2) {
    detail::runBasicSearch10Hz<2>(board, piece, pieceIdx, doTucks, availableTuckCols, out);
  } else if (safeGrav == 3) {
    detail::runBasicSearch10Hz<3>(board, piece, pieceIdx, doTucks, availableTuckCols, out);
  } else {
    detail::runBasicSearch10Hz<1>(board, piece, pieceIdx, doTucks, availableTuckCols, out);
  }

  if (doTucks) {
    const auto &minTuckY = detail::SHIFT_Y_10HZ[safeGrav][piece.initialY == -2 ? 1 : 0];
    const TuckSpotList20 &spotList = TUCK_SPOTS_20[pieceIdx];
    uint64_t seenTuck[BOARD_ROWS] = {};

    uint32_t rowBits = tucks.nonZeroRowsMask;
    while (rowBits) {
      const int oy = __builtin_ctz(rowBits);
      rowBits &= rowBits - 1;
      uint32_t colBits = static_cast<uint32_t>(tucks.mask[oy]) << 22;
      while (colBits) {
        const int ox = __builtin_clz(colBits);
        colBits ^= (0x80000000U >> ox);
        for (int s = 0; s < spotList.count; ++s) {
          const TuckOriginSpot20 &spot = spotList.spots[s];
          const int px = ox - spot.x;
          const int postY = oy - spot.y;
          if (collides20(board, pieceIdx, px, postY, spot.orientation)) {
            continue;
          }
          int lockY = postY;
          while (!collides20(board, pieceIdx, px, lockY + 1, spot.orientation)) {
            lockY++;
          }
          const int xIdx = px + 3;
          if (lockY >= 0 && lockY < BOARD_ROWS && xIdx >= 0 && xIdx < 16) {
            const uint64_t seenBit = 1ULL << ((xIdx << 2) | spot.orientation);
            if ((seenTuck[lockY] & seenBit) == 0ULL) {
              seenTuck[lockY] |= seenBit;
              const char c = detail::findTuckInput20(board, pieceIdx, px, postY, spot.orientation,
                                                     availableTuckCols, minTuckY);
              if (c != '.') {
                out.push(px, lockY, spot.orientation, c);
              }
            }
          }
        }
      }
    }
  }

  return out.count;
}

inline int findPlacements10Hz(const Board20 &board, int pieceIdx, int gravity, PlacementList &out,
                              bool enableTucks = true) noexcept {
  const TuckSetupInfo tucks =
      enableTucks ? computeTuckSetupInfo(board) : TuckSetupInfo{{}, 0U, false};
  return findPlacements10Hz(board, tucks, pieceIdx, gravity, out, enableTucks);
}

inline bool hasAnyLegalMove10Hz(const Board20 &board, int pieceIdx, int gravity,
                                bool enableTucks = true) noexcept {
  assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
  assert(gravity >= 1 && gravity <= 3);
  const PieceDef20 &piece = PIECES_20[pieceIdx];
  if (collides20(board, pieceIdx, SPAWN_COL, piece.initialY, 0)) {
    return false;
  }
  const int8_t *bot = piece.bottomSurface[0];
  bool spawnOk = true;
  for (int c = 0; c < 4; ++c) {
    if (bot[c] >= 0 && (BOARD_ROWS - bot[c] - piece.initialY - board.surface[SPAWN_COL + c]) < 0) {
      spawnOk = false;
      break;
    }
  }
  if (spawnOk) {
    return true;
  }
  PlacementList tmp;
  return findPlacements10Hz(board, pieceIdx, gravity, tmp, enableTucks) > 0;
}

inline int generateTransitions10Hz(const Board20 &board, const TuckSetupInfo &tucks, int pieceIdx,
                                   int gravity, TransitionList &out,
                                   bool enableTucks = true) noexcept {
  PlacementList placements;
  findPlacements10Hz(board, tucks, pieceIdx, gravity, placements, enableTucks);
  out.clear();
  uint16_t afterRows[BOARD_ROWS];
  for (int i = 0; i < placements.count; ++i) {
    const CompactPlacement &p = placements.data[i];
    const uint8_t cleared = applyPlacement20(board, pieceIdx, p.rot, p.x, p.y, afterRows);
    const int startRow =
        (cleared == 0)
            ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow), static_cast<int>(p.y)))
            : 0;
    BoardTransition &tr = out.data[out.count++];
    tr.fitsKey = static_cast<uint8_t>(encodeBoardKey(afterRows, tr.afterKey, startRow) ? 1 : 0);
    tr.linesCleared = cleared;
    tr.placement = p;
  }
  return out.count;
}

inline int generateTransitions10Hz(const Board20 &board, int pieceIdx, int gravity,
                                   TransitionList &out, bool enableTucks = true) noexcept {
  const TuckSetupInfo tucks =
      enableTucks ? computeTuckSetupInfo(board) : TuckSetupInfo{{}, 0U, false};
  return generateTransitions10Hz(board, tucks, pieceIdx, gravity, out, enableTucks);
}

// Mid-air piece state at `reactionTimeFrames` under 10 Hz ("X.....") input — O(1) closed form
// matching src/backend/utils.hpp::predictStateAtAdjustmentTime.
struct AdjState10Hz {
  bool valid;
  int8_t x;
  int8_t y;
  uint8_t rot;
  uint8_t arrReset;
};

inline AdjState10Hz predictAdjState10Hz(const CompactPlacement &p, int pieceIdx, int gravity,
                                        int reactionTimeFrames) noexcept {
  assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
  assert(gravity >= 1 && gravity <= 3);
  assert(reactionTimeFrames >= 0);
  const int safeGrav = (gravity >= 1 && gravity <= 3) ? gravity : GRAVITY_L19;
  const int inputsPerformed =
      (reactionTimeFrames > 0)
          ? ((reactionTimeFrames + TIMELINE_10HZ_PERIOD - 1) / TIMELINE_10HZ_PERIOD)
          : 0;
  const int adjY = PIECES_20[pieceIdx].initialY
                   + ((reactionTimeFrames > 0) ? (reactionTimeFrames / safeGrav) : 0);
  if (adjY > p.y) {
    return {false, 0, 0, 0, 0};
  }
  const int endXOff = static_cast<int>(p.x) - SPAWN_COL;
  const int shiftsNeeded = std::abs(endXOff);
  const int shiftsDone = std::min(shiftsNeeded, inputsPerformed);
  const int adjX = SPAWN_COL + ((endXOff < 0) ? -shiftsDone : shiftsDone);

  const int rotsNeeded = (p.rot == 3) ? 1 : static_cast<int>(p.rot);
  const int adjRot = (rotsNeeded <= inputsPerformed) ? static_cast<int>(p.rot) : inputsPerformed;
  const bool arrReset = (shiftsNeeded < inputsPerformed) && (rotsNeeded < inputsPerformed);

  return {true, static_cast<int8_t>(adjX), static_cast<int8_t>(adjY), static_cast<uint8_t>(adjRot),
          static_cast<uint8_t>(arrReset ? 1 : 0)};
}

} // namespace das

#endif // DAS_TABLEBASE_CORE_20ROW_HPP
