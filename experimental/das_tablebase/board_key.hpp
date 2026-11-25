#ifndef DAS_TABLEBASE_BOARD_KEY_HPP
#define DAS_TABLEBASE_BOARD_KEY_HPP

#include <cassert>
#include <cstdint>

namespace das {

inline constexpr int BOARD_WIDTH = 10;
inline constexpr int BOARD_ROWS = 20;
inline constexpr uint16_t FULL_ROW_MASK = 0x3FFU;
inline constexpr int KEY_MAX_HOLES = 9;
inline constexpr int KEY_MAX_HOLE_INDEX =
    (BOARD_ROWS - 1) * BOARD_WIDTH; // 190 (rows 1..19, cols 0..9)

inline constexpr uint16_t colBitMask(int col) noexcept {
  return static_cast<uint16_t>(1U << ((BOARD_WIDTH - 1) - col));
}

// Lossless 16-byte canonical board key for any 20x10 board with <= 9 empty cells under the
// surface (sealed holes + overhangs).
// Bit layout (126 bits used of 128):
//   lo bits  0..49 : surface[0..9] (5 bits/col, 0..20, col c at bit c*5)
//   lo bits 50..53 : numHoles (4 bits, 0..9)
//   lo bits 54..61 : hole[0] (8 bits, (r-1)*10 + c in 0..189; 0 when numHoles == 0)
//   lo bits 62..63 : always 0 for valid keys
//   hi bits  0..63 : hole[1..8] (8 bits each; unused slots zeroed)
struct BoardKey {
  uint64_t lo;
  uint64_t hi;

  constexpr bool operator==(const BoardKey &o) const noexcept { return lo == o.lo && hi == o.hi; }
  constexpr bool operator!=(const BoardKey &o) const noexcept { return !(*this == o); }
  constexpr bool operator<(const BoardKey &o) const noexcept {
    return (lo != o.lo) ? (lo < o.lo) : (hi < o.hi);
  }
  constexpr bool operator>(const BoardKey &o) const noexcept { return o < *this; }
  constexpr bool operator<=(const BoardKey &o) const noexcept { return !(o < *this); }
  constexpr bool operator>=(const BoardKey &o) const noexcept { return !(*this < o); }
};

static_assert(sizeof(BoardKey) == 16, "BoardKey must be exactly 16 bytes");
static_assert(alignof(BoardKey) == 8, "BoardKey must be 8-byte aligned");

// Collision-free sentinels: EMPTY_KEY has surface[0]=31 (>20) and top bits 11 (!=0).
inline constexpr BoardKey EMPTY_KEY = {~0ULL, ~0ULL};
inline constexpr BoardKey EMPTY_BOARD_KEY = {0ULL, 0ULL};

namespace detail {

inline constexpr uint64_t splitmix64Mix(uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

inline uint64_t wymix(uint64_t a, uint64_t b) noexcept {
  const unsigned __int128 prod = static_cast<unsigned __int128>(a) * b;
  return static_cast<uint64_t>(prod) ^ static_cast<uint64_t>(prod >> 64);
}

inline void writeHoleSlot(int holeIdx, uint8_t coord, uint64_t &lo, uint64_t &hi) noexcept {
  assert(holeIdx >= 0 && holeIdx < KEY_MAX_HOLES);
  if (holeIdx == 0) {
    lo |= static_cast<uint64_t>(coord) << 54;
  } else {
    hi |= static_cast<uint64_t>(coord) << ((holeIdx - 1) * 8);
  }
}

inline uint8_t readHoleSlot(BoardKey key, int holeIdx) noexcept {
  assert(holeIdx >= 0 && holeIdx < KEY_MAX_HOLES);
  if (holeIdx == 0) {
    return static_cast<uint8_t>((key.lo >> 54) & 0xFFULL);
  }
  return static_cast<uint8_t>((key.hi >> ((holeIdx - 1) * 8)) & 0xFFULL);
}

} // namespace detail

// Independent 2-round 128-bit multiply-xor-fold hashes for shard selection and open-addressing
// slot probing; both preserve full 64-bit avalanche even when k.hi == 0.
inline uint64_t hashBoardKeyShard(BoardKey k) noexcept {
  return detail::wymix(detail::wymix(k.lo ^ 0x243F6A8885A308D3ULL, k.hi ^ 0x13198A2E03707344ULL)
                           ^ 0x9E3779B97F4A7C15ULL,
                       0xC6BC279692B5C323ULL);
}

inline uint64_t hashBoardKeySlot(BoardKey k) noexcept {
  return detail::wymix(detail::wymix(k.lo ^ 0xA0761D6478BD642FULL, k.hi ^ 0xE7037ED1A0B428DBULL)
                           ^ 0x5851F42D4C957F2DULL,
                       0x8EBC6AF09C88C6E3ULL);
}

inline int boardKeySurfaceCol(BoardKey key, int col) noexcept {
  assert(col >= 0 && col < BOARD_WIDTH);
  return static_cast<int>((key.lo >> (col * 5)) & 0x1FULL);
}

inline int boardKeyNumHoles(BoardKey key) noexcept {
  return static_cast<int>((key.lo >> 50) & 0xFULL);
}

inline int boardKeyMaxHeight(BoardKey key) noexcept {
  int maxH = 0;
  uint64_t s = key.lo;
  for (int c = 0; c < BOARD_WIDTH; ++c) {
    const int h = static_cast<int>(s & 0x1FULL);
    if (h > maxH) {
      maxH = h;
    }
    s >>= 5;
  }
  return maxH;
}

// Encodes a canonical 20x10 board (no uncleared full rows; bit 9-c = col c, row 0 = top) into
// a 16-byte BoardKey. Returns false and writes EMPTY_KEY if >= 10 empty cells sit below surface.
template <typename RowT>
inline bool encodeBoardKey(const RowT rows[BOARD_ROWS], BoardKey &out, int startRow = 0) noexcept {
  uint32_t covered = 0;
  uint64_t lo = 0;
  uint64_t hi = 0;
  int numHoles = 0;

  int r = (startRow > 0) ? startRow : 0;
  while (r < BOARD_ROWS && (static_cast<uint32_t>(rows[r]) & FULL_ROW_MASK) == 0U) {
    ++r;
  }

  for (; r < BOARD_ROWS; ++r) {
    const uint32_t row = static_cast<uint32_t>(rows[r]) & FULL_ROW_MASK;
    const uint32_t diff = row ^ covered;
    if (diff == 0U) {
      continue;
    }

    const uint32_t newTop = diff & row;
    if (newTop) {
      const uint64_t h = static_cast<uint64_t>(BOARD_ROWS - r);
      uint32_t nt = newTop << 22;
      while (nt) {
        const int col = __builtin_clz(nt);
        nt ^= (0x80000000U >> col);
        lo |= h << (col * 5);
      }
    }

    const uint32_t holes = diff & covered;
    if (holes) {
      if (numHoles + __builtin_popcount(holes) > KEY_MAX_HOLES) {
        out = EMPTY_KEY;
        return false;
      }
      const int rowBase = (r - 1) * BOARD_WIDTH;
      uint32_t hMask = holes << 22;
      while (hMask) {
        const int col = __builtin_clz(hMask);
        hMask ^= (0x80000000U >> col);
        const uint8_t coord = static_cast<uint8_t>(rowBase + col);
        detail::writeHoleSlot(numHoles, coord, lo, hi);
        numHoles++;
      }
    }
    covered |= row;
  }

  lo |= static_cast<uint64_t>(numHoles) << 50;
  out.lo = lo;
  out.hi = hi;
  return true;
}

// Prefix-OR decode into 20x10 rows (and optional surface[10]). Clamps bounds defensively so
// unvalidated keys cannot trigger OOB writes or shift UB under -DNDEBUG.
template <typename RowT>
inline void decodeBoardKey(BoardKey key, RowT outRows[BOARD_ROWS],
                           int8_t outSurface[BOARD_WIDTH] = nullptr) noexcept {
  for (int r = 0; r < BOARD_ROWS; ++r) {
    outRows[r] = static_cast<RowT>(0);
  }

  uint64_t s = key.lo;
  for (int c = 0; c < BOARD_WIDTH; ++c) {
    int h = static_cast<int>(s & 0x1FULL);
    s >>= 5;
    assert(h <= BOARD_ROWS);
    if (h > BOARD_ROWS) {
      h = BOARD_ROWS;
    }
    if (outSurface) {
      outSurface[c] = static_cast<int8_t>(h);
    }
    if (h > 0) {
      outRows[BOARD_ROWS - h] = static_cast<RowT>(outRows[BOARD_ROWS - h] | colBitMask(c));
    }
  }

  RowT running = static_cast<RowT>(0);
  for (int r = 0; r < BOARD_ROWS; ++r) {
    running = static_cast<RowT>(running | outRows[r]);
    outRows[r] = running;
  }

  int numHoles = boardKeyNumHoles(key);
  assert(numHoles <= KEY_MAX_HOLES);
  if (numHoles > KEY_MAX_HOLES) {
    numHoles = KEY_MAX_HOLES;
  }
  if (numHoles > 0) {
    const unsigned coord0 = static_cast<unsigned>((key.lo >> 54) & 0xFFULL);
    assert(coord0 < static_cast<unsigned>(KEY_MAX_HOLE_INDEX));
    if (coord0 < static_cast<unsigned>(KEY_MAX_HOLE_INDEX)) {
      const unsigned r = 1U + coord0 / static_cast<unsigned>(BOARD_WIDTH);
      const unsigned c = coord0 % static_cast<unsigned>(BOARD_WIDTH);
      outRows[r] = static_cast<RowT>(outRows[r] & ~colBitMask(static_cast<int>(c)));
    }
    uint64_t hiHoles = key.hi;
    for (int i = 1; i < numHoles; ++i) {
      const unsigned coord = static_cast<unsigned>(hiHoles & 0xFFULL);
      hiHoles >>= 8;
      assert(coord < static_cast<unsigned>(KEY_MAX_HOLE_INDEX));
      if (coord < static_cast<unsigned>(KEY_MAX_HOLE_INDEX)) {
        const unsigned r = 1U + coord / static_cast<unsigned>(BOARD_WIDTH);
        const unsigned c = coord % static_cast<unsigned>(BOARD_WIDTH);
        outRows[r] = static_cast<RowT>(outRows[r] & ~colBitMask(static_cast<int>(c)));
      }
    }
  }
}

// Strict structural validation for untrusted or file-loaded 16-byte BoardKeys.
inline bool isValidBoardKey(BoardKey key) noexcept {
  if ((key.lo >> 62) != 0ULL) {
    return false;
  }
  int surface[BOARD_WIDTH];
  for (int c = 0; c < BOARD_WIDTH; ++c) {
    surface[c] = boardKeySurfaceCol(key, c);
    if (surface[c] > BOARD_ROWS) {
      return false;
    }
  }
  const int numHoles = boardKeyNumHoles(key);
  if (numHoles > KEY_MAX_HOLES) {
    return false;
  }

  int prevCoord = -1;
  for (int i = 0; i < numHoles; ++i) {
    const int coord = static_cast<int>(detail::readHoleSlot(key, i));
    if (coord >= KEY_MAX_HOLE_INDEX || coord <= prevCoord) {
      return false;
    }
    const int r = 1 + coord / BOARD_WIDTH;
    const int c = coord % BOARD_WIDTH;
    if (r <= BOARD_ROWS - surface[c]) {
      return false;
    }
    prevCoord = coord;
  }
  for (int i = numHoles; i < KEY_MAX_HOLES; ++i) {
    if (detail::readHoleSlot(key, i) != 0U) {
      return false;
    }
  }

  uint16_t rows[BOARD_ROWS];
  decodeBoardKey(key, rows);
  for (int r = 0; r < BOARD_ROWS; ++r) {
    if (rows[r] == FULL_ROW_MASK) {
      return false;
    }
  }
  return true;
}

} // namespace das

#endif // DAS_TABLEBASE_BOARD_KEY_HPP
