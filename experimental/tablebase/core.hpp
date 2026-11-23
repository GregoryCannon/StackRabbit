#ifndef TABLEBASE_CORE_HPP
#define TABLEBASE_CORE_HPP

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace tb {

inline constexpr int BOARD_WIDTH = 10;
inline constexpr int BOARD_ROWS = 4;
inline constexpr int BOARD_CELLS = BOARD_WIDTH * BOARD_ROWS;
inline constexpr uint64_t FULL_ROW_MASK = (1ULL << BOARD_WIDTH) - 1ULL; // 0x3FF
inline constexpr uint64_t VALID_BOARD_MASK = (1ULL << BOARD_CELLS) - 1ULL;

inline constexpr int NUM_PIECES = 7;
inline constexpr char PIECE_CHARS[NUM_PIECES + 1] = "IOLJTSZ";

// Coordinate conventions used during move generation:
//   - Board coordinates use (rowFromBottom in [0..3], col in [0..9]), where rowFromBottom = 0 is
//     the bottom floor and col = 0 is the left wall.
//   - Move search operates in an 8-row vertical frame indexed by gridY in [0..7] top-to-bottom:
//       gridY = 0..3 : 4 always-empty entry rows above the ceiling (enough for any 4x4 piece box)
//       gridY = 4..7 : the 4 playable bottom rows (rowFromBottom = 7 - gridY)
//   - A piece state (rot, x, y) places the top-left corner of the piece's 4x4 bounding box at
//     (col = x, gridY = y), so mino (r, c) inside the 4x4 box lands at (col = x + c, gridY = y +
//     r). y ranges over [0..8], where y = 8 is the sentinel slot used to test floor collision at y
//     + 1.
inline constexpr int ENTRY_ROWS = 4;
inline constexpr int SEARCH_GRID_ROWS = ENTRY_ROWS + BOARD_ROWS; // 8
inline constexpr int MAX_SEARCH_Y = SEARCH_GRID_ROWS;            // 8
inline constexpr int NUM_Y_SLOTS = MAX_SEARCH_Y + 1;             // 9

// Across all 7 NES tetrominoes and rotations, in-bounds piece-box x origins lie in [-2, +7].
inline constexpr int MIN_PIECE_X = -2;
inline constexpr int MAX_PIECE_X = 7;
inline constexpr int NUM_X_SLOTS = MAX_PIECE_X - MIN_PIECE_X + 1; // 10

inline constexpr char NO_TUCK = '.';

// Exact upper bound on legal placements for any piece on any 4x10 board is 50:
// in any single (rot, x), adjacent heights y and y + 1 cannot both lock, so 2-tall orientations
// lock at <= 2 heights and 3/4-tall orientations lock at <= 1 height (L/J/T: 2*8*2 + 2*9*1 = 50).
inline constexpr int MAX_PLACEMENTS = 64;
inline constexpr int MAX_TRANSITIONS = 64;

constexpr int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_popcountll(x);
#else
  int c = 0;
  while (x) {
    x &= (x - 1);
    ++c;
  }
  return c;
#endif
}

// Bit layout: rowFromBottom in [0..3] occupies bits [10*r .. 10*r + 9].
// Within each 10-bit row, col 0 (left wall) is bit 9 and col 9 (right wall) is bit 0.
constexpr uint64_t cellBit(int rowFromBottom, int col) {
  assert(rowFromBottom >= 0 && rowFromBottom < BOARD_ROWS);
  assert(col >= 0 && col < BOARD_WIDTH);
  return 1ULL << (rowFromBottom * BOARD_WIDTH + (BOARD_WIDTH - 1 - col));
}

constexpr bool getCell(uint64_t board, int rowFromBottom, int col) {
  return (board & cellBit(rowFromBottom, col)) != 0;
}

constexpr uint64_t setCell(uint64_t board, int rowFromBottom, int col) {
  return board | cellBit(rowFromBottom, col);
}

constexpr uint16_t getRow(uint64_t board, int rowFromBottom) {
  assert(rowFromBottom >= 0 && rowFromBottom < BOARD_ROWS);
  return static_cast<uint16_t>((board >> (rowFromBottom * BOARD_WIDTH)) & FULL_ROW_MASK);
}

struct ClearResult {
  uint64_t board;
  int linesCleared;
};

constexpr ClearResult clearFullRows(uint64_t placed) {
  uint64_t compacted = 0;
  int writeShift = 0;
  int cleared = 0;
  for (int r = 0; r < BOARD_ROWS; ++r) {
    const uint64_t row = getRow(placed, r);
    if (row == FULL_ROW_MASK) {
      ++cleared;
    } else {
      compacted |= (row << writeShift);
      writeShift += BOARD_WIDTH;
    }
  }
  return {compacted, cleared};
}

// Returns a 40-bit mask marking every empty cell that has at least one filled cell above it
// in the same column.
constexpr uint64_t computeHoleMask(uint64_t board) {
  const uint64_t r0 = getRow(board, 0);
  const uint64_t r1 = getRow(board, 1);
  const uint64_t r2 = getRow(board, 2);
  const uint64_t r3 = getRow(board, 3);

  const uint64_t above2 = r3;
  const uint64_t above1 = above2 | r2;
  const uint64_t above0 = above1 | r1;

  return (above0 & ~r0) | ((above1 & ~r1) << BOARD_WIDTH) | ((above2 & ~r2) << (2 * BOARD_WIDTH));
}

constexpr int countHoles(uint64_t board) { return popcount64(computeHoleMask(board)); }

// Returns a 40-bit mask of hole cells that are horizontally adjacent (in the same row) to a
// column completely open from the ceiling down through that row.
constexpr uint64_t computeOverhangMask(uint64_t board) {
  const uint64_t holes = computeHoleMask(board);
  const uint64_t r0 = getRow(board, 0);
  const uint64_t r1 = getRow(board, 1);
  const uint64_t r2 = getRow(board, 2);
  const uint64_t r3 = getRow(board, 3);

  const uint64_t above2 = r3;
  const uint64_t above1 = above2 | r2;
  const uint64_t above0 = above1 | r1;

  // Columns with zero filled cells at or above row k (reachable straight down from the ceiling).
  const uint64_t open2 = FULL_ROW_MASK & ~(above2 | r2);
  const uint64_t open1 = FULL_ROW_MASK & ~(above1 | r1);
  const uint64_t open0 = FULL_ROW_MASK & ~(above0 | r0);

  const auto adj = [](uint64_t openCols) constexpr {
    return ((openCols << 1) | (openCols >> 1)) & FULL_ROW_MASK;
  };

  const uint64_t adjacentToOpen =
      adj(open0) | (adj(open1) << BOARD_WIDTH) | (adj(open2) << (2 * BOARD_WIDTH));
  return holes & adjacentToOpen;
}

constexpr void getColumnHeights(uint64_t board, int heights[BOARD_WIDTH]) {
  for (int col = 0; col < BOARD_WIDTH; ++col) {
    const uint16_t colMask = static_cast<uint16_t>(1U << (BOARD_WIDTH - 1 - col));
    int h = 0;
    for (int r = BOARD_ROWS - 1; r >= 0; --r) {
      if (getRow(board, r) & colMask) {
        h = r + 1;
        break;
      }
    }
    heights[col] = h;
  }
}

constexpr int getMaxHeight(uint64_t board) {
  for (int r = BOARD_ROWS - 1; r >= 0; --r) {
    if (getRow(board, r) != 0) {
      return r + 1;
    }
  }
  return 0;
}

// Checks all structural invariants of a valid post-clear 4x10 Tetris board:
//   1. No bits set outside bits 0..39.
//   2. No un-cleared full rows (0x3FF).
//   3. No completely empty row below a non-empty row.
//   4. Even cell count (pieces add +4 cells, line clears remove -10 cells).
constexpr bool isCanonicalBoard(uint64_t board) {
  if ((board & ~VALID_BOARD_MASK) != 0) {
    return false;
  }
  bool seenEmptyRow = false;
  for (int r = 0; r < BOARD_ROWS; ++r) {
    const uint16_t row = getRow(board, r);
    if (row == FULL_ROW_MASK) {
      return false;
    }
    if (row == 0) {
      seenEmptyRow = true;
    } else if (seenEmptyRow) {
      return false;
    }
  }
  return (popcount64(board) & 1) == 0;
}

// Each uint16_t in rows[rot][r] encodes a 10-bit playfield row with the 4x4 piece box anchored at
// x = 0 (columns 0..3 correspond to bits 9..6 = 512, 256, 128, 64). Shifting right by x places the
// box at column x; negative x in [-2, -1] shifts empty left columns of the 4x4 box off-board.
struct PieceDef {
  char id;
  int numRots;
  uint16_t rows[4][4];
};

// Exact NES Tetris 4x4 rotation masks matching src/data/tetrominoes.hpp.
// clang-format off
inline constexpr PieceDef PIECES[NUM_PIECES] = {
    {'I', 2, {{0, 0, 960, 0},   {128, 128, 128, 128}, {0, 0, 0, 0},     {0, 0, 0, 0}}},
    {'O', 1, {{0, 384, 384, 0}, {0, 0, 0, 0},         {0, 0, 0, 0},     {0, 0, 0, 0}}},
    {'L', 4, {{0, 448, 256, 0}, {384, 128, 128, 0},   {64, 448, 0, 0},  {128, 128, 192, 0}}},
    {'J', 4, {{0, 448, 64, 0},  {128, 128, 384, 0},   {256, 448, 0, 0}, {192, 128, 128, 0}}},
    {'T', 4, {{0, 448, 128, 0}, {128, 384, 128, 0},   {128, 448, 0, 0}, {128, 192, 128, 0}}},
    {'S', 2, {{0, 192, 384, 0}, {128, 192, 64, 0},    {0, 0, 0, 0},     {0, 0, 0, 0}}},
    {'Z', 2, {{0, 384, 192, 0}, {64, 192, 128, 0},    {0, 0, 0, 0},     {0, 0, 0, 0}}},
};
// clang-format on

constexpr int parsePieceChar(char c) {
  for (int i = 0; i < NUM_PIECES; ++i) {
    if (PIECE_CHARS[i] == c) {
      return i;
    }
  }
  return -1;
}

struct TuckInput {
  char notation;
  int dx;
  int dRot;
};

// Single-frame NES tuck/spin inputs resolved in hardware order (Shift -> Rotate):
//   L/R: shift left/right; A/B: CW (+1) / CCW (-1) spin;
//   E: L+A, I: R+A, F: L+B, G: R+B.
inline constexpr TuckInput TUCK_INPUTS[8] = {
    {'L', -1, 0}, {'R', 1, 0}, {'A', 0, 1},   {'B', 0, -1},
    {'E', -1, 1}, {'I', 1, 1}, {'F', -1, -1}, {'G', 1, -1},
};

struct CellPlacementGeometry {
  uint64_t boardMask = 0;   // 40-bit intersection of the piece with playable rows (gridY 4..7)
  bool inBounds = false;    // True if rot is valid and no mino hits a wall or the floor
  bool fitsCeiling = false; // True if inBounds and all 4 minos lie in gridY 4..7 (pre-clear rule)
};

using GeometryTable = std::array<
    std::array<std::array<std::array<CellPlacementGeometry, NUM_Y_SLOTS>, NUM_X_SLOTS>, 4>,
    NUM_PIECES>;

namespace detail {

constexpr GeometryTable buildGeometryTable() {
  GeometryTable table{};
  for (int p = 0; p < NUM_PIECES; ++p) {
    const PieceDef &piece = PIECES[p];
    for (int rot = 0; rot < piece.numRots; ++rot) {
      for (int xIdx = 0; xIdx < NUM_X_SLOTS; ++xIdx) {
        const int x = MIN_PIECE_X + xIdx;
        uint16_t shiftedRows[4] = {0, 0, 0, 0};
        bool wallsOk = true;
        for (int r = 0; r < 4; ++r) {
          const uint16_t raw = piece.rows[rot][r];
          if (raw == 0) {
            continue;
          }
          if (x >= 0) {
            const uint16_t s = static_cast<uint16_t>(raw >> x);
            if (static_cast<uint16_t>(s << x) != raw) {
              wallsOk = false; // Mino shifted off the right wall
              break;
            }
            shiftedRows[r] = s;
          } else {
            const int leftShift = -x;
            const uint32_t s = static_cast<uint32_t>(raw) << leftShift;
            if (s > FULL_ROW_MASK) {
              wallsOk = false; // Mino shifted off the left wall
              break;
            }
            shiftedRows[r] = static_cast<uint16_t>(s);
          }
        }
        if (!wallsOk) {
          continue;
        }

        for (int y = 0; y < NUM_Y_SLOTS; ++y) {
          uint64_t mask = 0;
          bool floorOk = true;
          bool ceilingOk = true;
          for (int r = 0; r < 4; ++r) {
            const uint16_t s = shiftedRows[r];
            if (s == 0) {
              continue;
            }
            const int gridY = y + r;
            if (gridY >= SEARCH_GRID_ROWS) {
              floorOk = false;
              break;
            }
            if (gridY < ENTRY_ROWS) {
              ceilingOk = false;
            } else {
              const int rowFromBottom = (SEARCH_GRID_ROWS - 1) - gridY;
              mask |= (static_cast<uint64_t>(s) << (rowFromBottom * BOARD_WIDTH));
            }
          }
          if (floorOk) {
            table[p][rot][xIdx][y] = {mask, true, ceilingOk};
          }
        }
      }
    }
  }
  return table;
}

inline constexpr GeometryTable GEOMETRY = buildGeometryTable();
inline constexpr CellPlacementGeometry OUT_OF_BOUNDS_GEOM{0ULL, false, false};

} // namespace detail

constexpr const CellPlacementGeometry &getGeometry(int pieceIdx, int rot, int x, int y) {
  if (pieceIdx < 0
      || pieceIdx >= NUM_PIECES
      || rot < 0
      || rot >= 4
      || x < MIN_PIECE_X
      || x > MAX_PIECE_X
      || y < 0
      || y > MAX_SEARCH_Y) {
    return detail::OUT_OF_BOUNDS_GEOM;
  }
  return detail::GEOMETRY[pieceIdx][rot][x - MIN_PIECE_X][y];
}

constexpr bool collides(uint64_t board, const CellPlacementGeometry &g) {
  return !g.inBounds || ((board & g.boardMask) != 0);
}

constexpr bool collidesAt(uint64_t board, int pieceIdx, int rot, int x, int y) {
  return collides(board, getGeometry(pieceIdx, rot, x, y));
}

struct Placement {
  uint64_t afterBoard;  // Resulting 40-bit board after line clears
  uint64_t lockedMask;  // 40-bit locked-piece footprint prior to line clears
  uint8_t linesCleared; // Number of full rows cleared (0..4)
  uint8_t rot;          // Final piece rotation index (0..numRots-1)
  int8_t x;             // Final 4x4 piece-box column origin in [-2, +7]
  uint8_t lockY;        // Final 4x4 piece-box vertical origin in [0, 6]
  char tuckInput;       // '.' for straight drop, else single-frame tuck/spin code
};

struct PlacementList {
  Placement data[MAX_PLACEMENTS];
  int count = 0;

  void clear() { count = 0; }
  int size() const { return count; }
  bool empty() const { return count == 0; }
  Placement *begin() { return data; }
  Placement *end() { return data + count; }
  const Placement *begin() const { return data; }
  const Placement *end() const { return data + count; }
  Placement &operator[](int i) {
    assert(i >= 0 && i < count);
    return data[i];
  }
  const Placement &operator[](int i) const {
    assert(i >= 0 && i < count);
    return data[i];
  }
};

struct Transition {
  uint64_t afterBoard;
  uint8_t linesCleared;
};

struct TransitionList {
  Transition data[MAX_TRANSITIONS];
  int count = 0;

  void clear() { count = 0; }
  int size() const { return count; }
  bool empty() const { return count == 0; }
  Transition *begin() { return data; }
  Transition *end() { return data + count; }
  const Transition *begin() const { return data; }
  const Transition *end() const { return data + count; }
  Transition &operator[](int i) {
    assert(i >= 0 && i < count);
    return data[i];
  }
  const Transition &operator[](int i) const {
    assert(i >= 0 && i < count);
    return data[i];
  }
};

namespace detail {

// Core move discovery shared by generatePlacements and generateTransitions:
//   1. Straight drops from y = 0 in every in-bounds (rot, x), recording maxDropY[rot][xIdx].
//   2. 1-input tucks/spins across the overhang window postMaxY < y <= preMaxY using NES
//      Shift -> Rotate collision order, followed by gravity drop to lock.
//   3. Pre-clear 4-row ceiling filter (fitsCeiling) at lock.
template <typename EmitFn>
inline void forEachLegalMove(uint64_t board, int pieceIdx, EmitFn &&emit) {
  if (pieceIdx < 0 || pieceIdx >= NUM_PIECES) {
    return;
  }
  const int numRots = PIECES[pieceIdx].numRots;

  int maxDropY[4][NUM_X_SLOTS];
  for (int rot = 0; rot < numRots; ++rot) {
    for (int xIdx = 0; xIdx < NUM_X_SLOTS; ++xIdx) {
      const auto &colGeom = GEOMETRY[pieceIdx][rot][xIdx];
      if (!colGeom[0].inBounds) {
        maxDropY[rot][xIdx] = -1;
        continue;
      }
      int y = 0;
      while (!collides(board, colGeom[y + 1])) {
        ++y;
      }
      maxDropY[rot][xIdx] = y;

      const CellPlacementGeometry &lockGeom = colGeom[y];
      if (lockGeom.fitsCeiling) {
        emit(lockGeom.boardMask, rot, MIN_PIECE_X + xIdx, y, NO_TUCK);
      }
    }
  }

  // Explore 1-input tucks and spins. Iterating TUCK_INPUTS outermost prioritizes single-button
  // tucks ('L','R') and spins ('A','B') ahead of 2-button shift+rotate combos ('E','I','F','G').
  for (const TuckInput &tuck : TUCK_INPUTS) {
    if (numRots == 1 && tuck.dRot != 0) {
      continue;
    }
    // On 2-orientation pieces (I/S/Z), +1 and -1 toggle to the exact same orientation (rot ^ 1).
    if (numRots == 2 && tuck.dRot < 0) {
      continue;
    }

    for (int preRot = 0; preRot < numRots; ++preRot) {
      const int postRot = (preRot + tuck.dRot + numRots) % numRots;
      for (int preXIdx = 0; preXIdx < NUM_X_SLOTS; ++preXIdx) {
        const int preMaxY = maxDropY[preRot][preXIdx];
        if (preMaxY < 0) {
          continue;
        }
        const int preX = MIN_PIECE_X + preXIdx;
        const int postX = preX + tuck.dx;
        if (postX < MIN_PIECE_X || postX > MAX_PIECE_X) {
          continue;
        }
        const int postXIdx = postX - MIN_PIECE_X;

        const int postMaxY = maxDropY[postRot][postXIdx];
        if (postMaxY < 0) {
          continue; // (postRot, postX) hits a horizontal wall
        }

        // Any y <= postMaxY is already reachable by a straight drop in (postRot, postX).
        const auto &shiftedPreRotCol = GEOMETRY[pieceIdx][preRot][postXIdx];
        const auto &postCol = GEOMETRY[pieceIdx][postRot][postXIdx];

        for (int y = postMaxY + 1; y <= preMaxY; ++y) {
          // NES frame resolution order: Shift first (in preRot), then Rotate (to postRot).
          if (tuck.dx != 0 && collides(board, shiftedPreRotCol[y])) {
            continue;
          }
          if (collides(board, postCol[y])) {
            continue;
          }

          int lockY = y;
          while (!collides(board, postCol[lockY + 1])) {
            ++lockY;
          }
          const CellPlacementGeometry &lockGeom = postCol[lockY];
          if (lockGeom.fitsCeiling) {
            emit(lockGeom.boardMask, postRot, postX, lockY, tuck.notation);
          }
          // Every y' in [y + 1, lockY] drops to the same lockY, and postCol[lockY + 1] collides.
          y = lockY + 1;
        }
      }
    }
  }
}

} // namespace detail

// Enumerates all legal placements deduplicated by locked 4-mino footprint (for UI & move analysis).
// Straight drops are visited first, so non-tuck notation '.' always wins ties over tucks.
inline int generatePlacements(uint64_t board, int pieceIdx, PlacementList &out) {
  out.clear();
  detail::forEachLegalMove(board, pieceIdx,
                           [&](uint64_t lockedMask, int rot, int x, int lockY, char tuckInput) {
                             for (int i = 0; i < out.count; ++i) {
                               if (out.data[i].lockedMask == lockedMask) {
                                 return;
                               }
                             }
                             assert(out.count < MAX_PLACEMENTS);
                             const ClearResult res = clearFullRows(board | lockedMask);
                             out.data[out.count++] = {
                                 res.board,
                                 lockedMask,
                                 static_cast<uint8_t>(res.linesCleared),
                                 static_cast<uint8_t>(rot),
                                 static_cast<int8_t>(x),
                                 static_cast<uint8_t>(lockY),
                                 tuckInput,
                             };
                           });
  return out.count;
}

// Enumerates unique post-clear (afterBoard, linesCleared) transitions (for search & Value
// Iteration).
inline int generateTransitions(uint64_t board, int pieceIdx, TransitionList &out) {
  out.clear();
  detail::forEachLegalMove(
      board, pieceIdx,
      [&](uint64_t lockedMask, int /*rot*/, int /*x*/, int /*lockY*/, char /*tuckInput*/) {
        const ClearResult res = clearFullRows(board | lockedMask);
        for (int i = 0; i < out.count; ++i) {
          if (out.data[i].afterBoard == res.board) {
            return;
          }
        }
        assert(out.count < MAX_TRANSITIONS);
        out.data[out.count++] = {res.board, static_cast<uint8_t>(res.linesCleared)};
      });
  return out.count;
}

// Deterministic, fast, zero-global-state PRNG (Xoshiro256** seeded via SplitMix64).
class Rng {
public:
  explicit constexpr Rng(uint64_t seed = 0x9E3779B97F4A7C15ULL) : s_{0, 0, 0, 0} {
    uint64_t z = seed;
    for (int i = 0; i < 4; ++i) {
      z += 0x9E3779B97F4A7C15ULL;
      uint64_t x = z;
      x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
      x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
      s_[i] = x ^ (x >> 31);
    }
  }

  constexpr uint64_t nextU64() {
    const uint64_t result = rotl(s_[1] * 5ULL, 7) * 9ULL;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Unbiased uniform piece draw in [0, 6].
  constexpr int nextPiece() {
    constexpr uint64_t limit = UINT64_MAX - (UINT64_MAX % static_cast<uint64_t>(NUM_PIECES));
    while (true) {
      const uint64_t r = nextU64();
      if (r < limit) {
        return static_cast<int>(r % static_cast<uint64_t>(NUM_PIECES));
      }
    }
  }

  // Uniform double in [0.0, 1.0).
  constexpr double nextDouble() { return static_cast<double>(nextU64() >> 11) * 0x1.0p-53; }

private:
  uint64_t s_[4];

  static constexpr uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
};

// Renders the 4 playable rows top-to-bottom (rowFromBottom = 3 down to 0) as 4 lines of 10 chars.
inline std::string boardToAscii(uint64_t board) {
  std::string out;
  out.reserve(BOARD_ROWS * (BOARD_WIDTH + 1));
  for (int r = BOARD_ROWS - 1; r >= 0; --r) {
    for (int col = 0; col < BOARD_WIDTH; ++col) {
      out.push_back(getCell(board, r, col) ? 'X' : '.');
    }
    if (r > 0) {
      out.push_back('\n');
    }
  }
  return out;
}

// Parses a 4x10 ASCII diagram (top row = rowFromBottom 3, bottom row = rowFromBottom 0).
// Whitespace (' ', '\n', '\r', '\t') is ignored; accepts '.'/'0' for empty and 'X'/'x'/'1'/'#' for
// filled.
inline bool parseBoardAscii(std::string_view input, uint64_t &outBoard) {
  uint64_t board = 0;
  int cellIdx = 0;
  for (char c : input) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      continue;
    }
    if (cellIdx >= BOARD_CELLS) {
      return false;
    }
    const int rowFromTop = cellIdx / BOARD_WIDTH;
    const int rowFromBottom = (BOARD_ROWS - 1) - rowFromTop;
    const int col = cellIdx % BOARD_WIDTH;
    if (c == 'X' || c == 'x' || c == '1' || c == '#') {
      board = setCell(board, rowFromBottom, col);
    } else if (c != '.' && c != '0') {
      return false;
    }
    ++cellIdx;
  }
  if (cellIdx != BOARD_CELLS) {
    return false;
  }
  outBoard = board;
  return true;
}

// Formats a 40-bit board as 10 zero-padded lowercase hexadecimal digits.
inline std::string formatBoardHex(uint64_t board) {
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  const uint64_t masked = board & VALID_BOARD_MASK;
  std::string out(10, '0');
  for (int i = 0; i < 10; ++i) {
    const int shift = (9 - i) * 4;
    out[i] = HEX_DIGITS[(masked >> shift) & 0xFULL];
  }
  return out;
}

// Strictly parses 1..10 hexadecimal digits (lowercase or uppercase, no "0x" prefix or whitespace)
// into a 40-bit board value.
inline bool parseBoardHex(std::string_view hex, uint64_t &outBoard) {
  if (hex.empty() || hex.size() > 10) {
    return false;
  }
  uint64_t val = 0;
  for (char c : hex) {
    uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint64_t>(10 + (c - 'a'));
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint64_t>(10 + (c - 'A'));
    } else {
      return false;
    }
    val = (val << 4) | digit;
  }
  if ((val & ~VALID_BOARD_MASK) != 0) {
    return false;
  }
  outBoard = val;
  return true;
}

} // namespace tb

#endif // TABLEBASE_CORE_HPP
