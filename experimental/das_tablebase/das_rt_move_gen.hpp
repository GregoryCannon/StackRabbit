#ifndef DAS_TABLEBASE_DAS_RT_MOVE_GEN_HPP
#define DAS_TABLEBASE_DAS_RT_MOVE_GEN_HPP

// Fast zero-allocation bitboard move & reaction-time phantom generator for NES Tetris DAS.
//
// Input model (unified human 7.5 Hz independent-thumb limit):
//   - D-pad thumb: >= dpadGapFrames (7) frames between any two D-pad button-down events
//     (period 8 = 7.5 Hz; bans quick L->R / R->L reversals; continuous DAS hold is one
//     button-down; holding through ARE starts with dpadCooldown = 0 at spawn).
//   - Rotate thumb (independent): >= rotGapFrames (7) frames between any two A/B button-down
//     events (period 8 = 7.5 Hz; bans quick A->B / B->A combos).
//
// Search architecture:
//   - Phase A: Precomputed SoA swept-path tables (16 B WalkMeta + 40 B BoardMask5 in flat CSR
//     arrays) across DAS, native 7.5 Hz slow-tap (das < 15 / dasWillReset), and optional X.X
//     quicktap schedules, with a 32-bit (4 rots x 8 rotCooldowns) pole-vault DP fallback on
//     collisions.
//   - Phase B: Shared per-(board, piece) O(1) surface lock-height & wall-charge depth tables +
//     exact closed-form hold-through-fall DAS + NES rising-edge wall-charge check.
//   - Phase C: Pure real overhang tucks & spins (skipped on 0-hole boards when all basic cells
//     are reached), respecting independent dpadCooldown / rotCooldown and wall-charge tuck DAS.

#include "core_20row.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace das {

inline constexpr int DAS_MAX_CHARGE = 16;
inline constexpr int DAS_FULL_THRESHOLD = 15;
inline constexpr int DAS_POST_SHIFT_CHARGE = 10;
inline constexpr int DAS_RESET_THRESHOLD = 5;
inline constexpr int DAS_MAX_GAP_FRAMES = 7;  // cooldown in [0..7] fits in 8-bit bitmask
inline constexpr int DAS_MAX_SIM_FRAMES = 64; // L18 (gravity=3) max empty-board lock frame is 59
inline constexpr int DAS_MAX_SHIFTS = 6;
inline constexpr int DAS_MAX_ADJ_STATES = 96;
inline constexpr int DAS_MAX_LOCK_MOVES = 256;
inline constexpr int MIN_PIECE_Y = -2;
inline constexpr int MAX_PIECE_Y = 18;
inline constexpr int MIN_PIECE_X = -2;
inline constexpr int MAX_PIECE_X = 7;
inline constexpr int Y_ROW_OFFSET = 2;
inline constexpr int INVALID_ROW_Y = -10;
inline constexpr int NO_PREV_FRAME = -100;
inline constexpr uint8_t ALL_ROTS_MASK = 0x0FU;
inline constexpr uint16_t INVALID_CELL_IDX = 0xFFFFU;

// Move provenance flags on DasLockMove::flags. Bit ordering also serves as the secondary
// tie-break priority when two paths reach the same cell with identical ending DAS (lower wins).
inline constexpr uint8_t MOVE_FLAG_SLOW_TAP = 1U << 0;
inline constexpr uint8_t MOVE_FLAG_QUICKTAP = 1U << 1;
inline constexpr uint8_t MOVE_FLAG_POLE_VAULT = 1U << 2;
inline constexpr uint8_t MOVE_FLAG_TUCK = 1U << 3;
inline constexpr uint8_t MOVE_FLAG_WALL_CHARGE = 1U << 4;

inline constexpr int dasBucket3(int dasCharge) noexcept {
  return (dasCharge >= DAS_FULL_THRESHOLD) ? 2 : ((dasCharge >= DAS_POST_SHIFT_CHARGE) ? 1 : 0);
}

inline constexpr int dasBucket2(int dasCharge) noexcept {
  return (dasCharge >= DAS_FULL_THRESHOLD) ? 1 : 0;
}

inline constexpr int dasBucketToRepresentative(int bucket, int numBuckets) noexcept {
  assert(numBuckets == 2 || numBuckets == 3);
  if (numBuckets == 2) {
    return (bucket >= 1) ? DAS_FULL_THRESHOLD : DAS_RESET_THRESHOLD;
  }
  return (bucket >= 2) ? DAS_FULL_THRESHOLD
                       : ((bucket == 1) ? DAS_POST_SHIFT_CHARGE : DAS_RESET_THRESHOLD);
}

struct MoveSearchConfig {
  int gravity = GRAVITY_L19;    // 1 (L29), 2 (L19), 3 (L18)
  int reactionTime = 18;        // 0 = no reaction-time split; 18 = standard L19
  int dpadGapFrames = 7;        // empty frames between D-pad down-presses (0..7; 7 -> 7.5 Hz)
  int rotGapFrames = 7;         // empty frames between A/B down-presses (0..7; 7 -> 7.5 Hz)
  int quicktapDelayFrames = 2;  // X.X = 1 release frame + tap on 2nd frame after last DAS shift
  bool enableSlowTaps = true;   // native 7.5 Hz slow-tap track on das < 15 / dasWillReset
  bool enablePoleVaults = true; // 32-bit (rot x rotCd) DP fallback when swept-path collides
  bool enableQuicktaps = false; // final-shift quicktap track (off for pure-DAS training)
  bool enableTucks = true;      // real overhang tucks & spins (Phase C)

  bool isValid() const noexcept {
    return gravity >= 1
           && gravity <= 3
           && reactionTime >= 0
           && reactionTime < DAS_MAX_SIM_FRAMES
           && dpadGapFrames >= 0
           && dpadGapFrames <= DAS_MAX_GAP_FRAMES
           && rotGapFrames >= 0
           && rotGapFrames <= DAS_MAX_GAP_FRAMES
           && quicktapDelayFrames >= 2
           && quicktapDelayFrames <= 5;
  }
};

struct DasStartState {
  int8_t x = SPAWN_COL;
  int8_t y = -1;
  uint8_t rot = 0;
  uint8_t frame = 0;
  uint8_t dasCharge = DAS_FULL_THRESHOLD; // 0..16
  int8_t heldDir = 0;                     // -1 left, 0 none, +1 right (held on frame-1)
  uint8_t dpadCooldown = 0;               // 0..7: frames until next fresh D-pad down-press
  uint8_t rotCooldown = 0;                // 0..7: frames until next A/B down-press
  uint8_t shiftCooldown = 0;              // 0..7: frames since last shift (no-quicktap RT guard)
  bool isAdjustment = false;

  static DasStartState spawn(int pieceIdx, int dasCharge) noexcept {
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    DasStartState s;
    s.x = SPAWN_COL;
    s.y = PIECES_20[pieceIdx].initialY;
    s.rot = 0;
    s.frame = 0;
    s.dasCharge = static_cast<uint8_t>(std::clamp(dasCharge, 0, DAS_MAX_CHARGE));
    s.heldDir = 0;
    s.dpadCooldown = 0;
    s.rotCooldown = 0;
    s.shiftCooldown = 0;
    s.isAdjustment = false;
    return s;
  }

  static DasStartState makeAdjustment(int x, int y, int rot, int frame, int das, int heldDir,
                                      int dpadCd, int rotCd, int shiftCd) noexcept {
    DasStartState s;
    s.x = static_cast<int8_t>(x);
    s.y = static_cast<int8_t>(y);
    s.rot = static_cast<uint8_t>(rot);
    s.frame = static_cast<uint8_t>(frame);
    s.dasCharge = static_cast<uint8_t>(das);
    s.heldDir = static_cast<int8_t>(heldDir);
    s.dpadCooldown = static_cast<uint8_t>(dpadCd);
    s.rotCooldown = static_cast<uint8_t>(rotCd);
    s.shiftCooldown = static_cast<uint8_t>(shiftCd);
    s.isAdjustment = true;
    return s;
  }

  uint64_t packKey() const noexcept {
    static_assert(DAS_MAX_GAP_FRAMES <= 7, "Cooldowns must fit in 3 bits");
    return (static_cast<uint64_t>(static_cast<uint8_t>(x + 16)) << 0)
           | (static_cast<uint64_t>(static_cast<uint8_t>(y + 16)) << 8)
           | (static_cast<uint64_t>(rot & 3U) << 16)
           | (static_cast<uint64_t>(frame) << 20)
           | (static_cast<uint64_t>(dasCharge) << 28)
           | (static_cast<uint64_t>(static_cast<uint8_t>(heldDir + 1)) << 36)
           | (static_cast<uint64_t>(dpadCooldown & 7U) << 40)
           | (static_cast<uint64_t>(rotCooldown & 7U) << 44)
           | (static_cast<uint64_t>(isAdjustment ? 1U : 0U) << 48)
           | (static_cast<uint64_t>(shiftCooldown & 7U) << 49);
  }
};

struct DasLockMove {
  int8_t x;
  int8_t y;
  uint8_t rot;
  uint8_t dasChargeAfter; // 0..16
  uint8_t flags;
  char tuckInput; // '.' for basic, L/R/A/B/E/F/I/G for overhang tuck/spin
};
static_assert(sizeof(DasLockMove) == 6, "DasLockMove should stay compact");

struct DasMoveList {
  DasLockMove data[DAS_MAX_LOCK_MOVES];
  int count = 0;
  void clear() noexcept { count = 0; }
};

struct AdjEdge {
  uint8_t cellIdx;
  uint8_t dasAfter;
};
static_assert(sizeof(AdjEdge) == 2, "AdjEdge must be 2 bytes");
static_assert(DAS_MAX_LOCK_MOVES <= 256, "AdjEdge::cellIdx requires DAS_MAX_LOCK_MOVES <= 256");

// Output of a full 2-half reaction-time search for one (board, piece, startDas).
// Invariants:
//   - uniqueCells[0..numUniqueCells-1] is deduplicated by (rot, x, y); tuckInput prefers '.'
//     if any branch reaches that cell without a tuck, and callers should key board transitions
//     solely by (rot, x, y).
//   - earlyMoves[0..numEarlyMoves-1] holds unadjustable moves locking strictly before RT, with
//     unique (rot, x, y) cells carrying max achievable dasAfter.
//   - adjStates[0..numAdjStates-1] holds distinct reachable mid-air states at frame == RT
//     (or {spawnSt} when reactionTime == 0, in which case numEarlyMoves == 0).
//   - adjEdges[adjMoveOffset[i]..adjMoveOffset[i+1]) holds unique (cellIdx, max dasAfter) edges
//     reachable from adjStates[i].
struct alignas(64) DasRtSearchResult {
  CompactPlacement uniqueCells[DAS_MAX_LOCK_MOVES];
  int numUniqueCells = 0;

  AdjEdge earlyMoves[DAS_MAX_LOCK_MOVES];
  int numEarlyMoves = 0;

  DasStartState adjStates[DAS_MAX_ADJ_STATES];
  int numAdjStates = 0;

  static constexpr int MAX_ADJ_EDGES = DAS_MAX_ADJ_STATES * 96;
  uint16_t adjMoveOffset[DAS_MAX_ADJ_STATES + 1];
  AdjEdge adjEdges[MAX_ADJ_EDGES];
  int numAdjEdges = 0;

  void clear() noexcept {
    numUniqueCells = 0;
    numEarlyMoves = 0;
    numAdjStates = 0;
    numAdjEdges = 0;
    adjMoveOffset[0] = 0;
  }
};

namespace rt_detail {

inline constexpr int pieceMaxX(const PieceDef20 &piece, int rot) noexcept {
  return static_cast<int>(piece.minX[rot]) + static_cast<int>(piece.xSpan[rot]);
}

inline constexpr bool inPieceXBounds(const PieceDef20 &piece, int rot, int x) noexcept {
  return x >= static_cast<int>(piece.minX[rot]) && x <= pieceMaxX(piece, rot);
}

inline constexpr uint16_t validXBitsForRot(const PieceDef20 &piece, int rot) noexcept {
  const int loX = piece.minX[rot];
  const int span = static_cast<int>(piece.xSpan[rot]) + 1;
  return static_cast<uint16_t>(((1U << span) - 1U) << (loX + detail::PIECE_MASK_X_OFFSET));
}

inline constexpr int trackFlagsToIndex(uint8_t flags) noexcept {
  return (flags == MOVE_FLAG_SLOW_TAP) ? 1 : ((flags == MOVE_FLAG_QUICKTAP) ? 2 : 0);
}

// Fixed-capacity stack buffer for frame-RT adjustment states (zero heap allocations).
struct AdjStateBuf {
  DasStartState data[DAS_MAX_ADJ_STATES];
  int count = 0;

  void clear() noexcept { count = 0; }
  void push(const DasStartState &st) noexcept {
    if (count < DAS_MAX_ADJ_STATES) {
      data[count++] = st;
    }
  }
};

// 5 x uint64_t mirrors Board20::rows()[0..19] directly (4 rows per uint64_t, 16 bits/row).
struct BoardMask5 {
  uint64_t w[5] = {};

  void clear() noexcept { w[0] = w[1] = w[2] = w[3] = w[4] = 0ULL; }

  int orPiece(int pieceIdx, int rot, int x, int y) noexcept {
    const uint64_t pm = detail::PIECE_MASK_64.mask[pieceIdx][rot][x + detail::PIECE_MASK_X_OFFSET];
    int maxRow = -1;
    for (int r = 0; r < 4; ++r) {
      const int rowIdx = y + r;
      if (rowIdx >= 0 && rowIdx < BOARD_ROWS) {
        const uint64_t rowBits = (pm >> (r * 16)) & FULL_ROW_MASK;
        if (rowBits != 0ULL) {
          w[rowIdx >> 2] |= rowBits << ((rowIdx & 3) * 16);
          if (rowIdx > maxRow) {
            maxRow = rowIdx;
          }
        }
      }
    }
    return maxRow;
  }

  bool intersects(const uint64_t b[5]) const noexcept {
    return ((b[0] & w[0]) | (b[1] & w[1]) | (b[2] & w[2]) | (b[3] & w[3]) | (b[4] & w[4])) != 0ULL;
  }
};

inline void loadBoard5(const Board20 &board, uint64_t out[5]) noexcept {
  std::memcpy(out, board.rows(), BOARD_ROWS * sizeof(uint16_t));
}

// Precomputed 64-bit left/right neighbor masks and screen-wall touch flags per (piece, rot, x).
struct WallChargeTables {
  uint64_t sideMask[NUM_PIECES][4][detail::PIECE_MASK_X_COUNT];
  uint64_t leftMask[NUM_PIECES][4][detail::PIECE_MASK_X_COUNT];
  uint64_t rightMask[NUM_PIECES][4][detail::PIECE_MASK_X_COUNT];
  uint8_t touchesWall[NUM_PIECES][4][detail::PIECE_MASK_X_COUNT];
  uint8_t touchesLeftWall[NUM_PIECES][4][detail::PIECE_MASK_X_COUNT];
  uint8_t touchesRightWall[NUM_PIECES][4][detail::PIECE_MASK_X_COUNT];
};

constexpr WallChargeTables buildWallChargeTables() noexcept {
  WallChargeTables t = {};
  for (int p = 0; p < NUM_PIECES; ++p) {
    const PieceDef20 &pc = PIECES_20[p];
    for (int rot = 0; rot < pc.numOrientations; ++rot) {
      const int loX = pc.minX[rot];
      const int hiX = pieceMaxX(pc, rot);
      for (int x = loX; x <= hiX; ++x) {
        const int idx = x + detail::PIECE_MASK_X_OFFSET;
        const bool touchL = (x == loX);
        const bool touchR = (x == hiX);
        t.touchesLeftWall[p][rot][idx] = static_cast<uint8_t>(touchL ? 1U : 0U);
        t.touchesRightWall[p][rot][idx] = static_cast<uint8_t>(touchR ? 1U : 0U);
        t.touchesWall[p][rot][idx] = static_cast<uint8_t>((touchL || touchR) ? 1U : 0U);
        const uint64_t lm =
            touchL ? 0ULL
                   : detail::PIECE_MASK_64.mask[p][rot][(x - 1) + detail::PIECE_MASK_X_OFFSET];
        const uint64_t rm =
            touchR ? 0ULL
                   : detail::PIECE_MASK_64.mask[p][rot][(x + 1) + detail::PIECE_MASK_X_OFFSET];
        t.leftMask[p][rot][idx] = lm;
        t.rightMask[p][rot][idx] = rm;
        t.sideMask[p][rot][idx] = lm | rm;
      }
    }
  }
  return t;
}

inline constexpr WallChargeTables WALL_CHARGE_TABLES = buildWallChargeTables();

// Per-(board, piece) precomputed surface drop heights, deepest wall-charge rows, and under-surface
// empty row bitmasks, built ONCE per searchRt / searchRtMultiDas call and reused across all phis.
struct BoardPieceCache {
  int8_t surfLockY[4][detail::PIECE_MASK_X_COUNT];
  int8_t maxSurfLockYAtX[detail::PIECE_MASK_X_COUNT];
  int8_t lastWcY[4][detail::PIECE_MASK_X_COUNT];
  // Valid only when board.numHoles > 0: bit (y + Y_ROW_OFFSET) is 1 iff
  // y in [max(MIN_PIECE_Y, surfLockY + 1) .. maxY] and !collides20(board, piece, x, y, rot).
  uint32_t underSurfEmptyMask[4][detail::PIECE_MASK_X_COUNT];

  void build(const Board20 &board, int pieceIdx) noexcept {
    const PieceDef20 &piece = PIECES_20[pieceIdx];
    const int minStackY = static_cast<int>(board.topNonEmptyRow) - 3;
    const bool hasHoles = (board.numHoles > 0);
    for (int xi = 0; xi < detail::PIECE_MASK_X_COUNT; ++xi) {
      maxSurfLockYAtX[xi] = INVALID_ROW_Y;
    }

    for (int rot = 0; rot < piece.numOrientations; ++rot) {
      const int8_t *bot = piece.bottomSurface[rot];
      const int loX = piece.minX[rot];
      const int hiX = pieceMaxX(piece, rot);
      const int maxY = piece.maxY[rot];
      for (int x = loX; x <= hiX; ++x) {
        const int xi = x + detail::PIECE_MASK_X_OFFSET;
        int ly = 99;
        for (int c = 0; c < 4; ++c) {
          if (bot[c] >= 0) {
            const int d = (BOARD_ROWS - bot[c]) - board.surface[x + c];
            if (d < ly) {
              ly = d;
            }
          }
        }
        surfLockY[rot][xi] = static_cast<int8_t>(ly);
        if (ly > maxSurfLockYAtX[xi]) {
          maxSurfLockYAtX[xi] = static_cast<int8_t>(ly);
        }

        if (WALL_CHARGE_TABLES.touchesWall[pieceIdx][rot][xi] != 0U) {
          lastWcY[rot][xi] = static_cast<int8_t>(ly);
        } else if (minStackY > ly || ly < MIN_PIECE_Y) {
          lastWcY[rot][xi] = INVALID_ROW_Y;
        } else {
          const uint64_t sm = WALL_CHARGE_TABLES.sideMask[pieceIdx][rot][xi];
          int deepest = INVALID_ROW_Y;
          const int scanStop = std::max(MIN_PIECE_Y, minStackY);
          for (int y = ly; y >= scanStop; --y) {
            uint64_t b4;
            std::memcpy(&b4, &board.paddedRows[y + Y_ROW_OFFSET], sizeof(uint64_t));
            if ((b4 & sm) != 0ULL) {
              deepest = y;
              break;
            }
          }
          lastWcY[rot][xi] = static_cast<int8_t>(deepest);
        }

        if (hasHoles) {
          uint32_t um = 0U;
          const uint64_t pm = detail::PIECE_MASK_64.mask[pieceIdx][rot][xi];
          // Clamp start to MIN_PIECE_Y so row-0 blocks (ly == -4 on vertical I) stay in bounds.
          const int scanStart = std::max(MIN_PIECE_Y, ly + 1);
          for (int y = scanStart; y <= maxY; ++y) {
            uint64_t b4;
            std::memcpy(&b4, &board.paddedRows[y + Y_ROW_OFFSET], sizeof(uint64_t));
            if ((b4 & pm) == 0ULL) {
              um |= (1U << (y + Y_ROW_OFFSET));
            }
          }
          underSurfEmptyMask[rot][xi] = um;
        }
      }
    }
  }

  int dropY(const Board20 &board, int pieceIdx, int rot, int x, int yStart) const noexcept {
    const int sy = surfLockY[rot][x + detail::PIECE_MASK_X_OFFSET];
    if (yStart <= sy) {
      return sy;
    }
    int curY = yStart;
    while (!collides20(board, pieceIdx, x, curY + 1, rot)) {
      curY++;
    }
    return curY;
  }

  bool canWallChargeAny(const Board20 &board, int pieceIdx, int rot, int x, int yStart,
                        int lockY) const noexcept {
    if (yStart > lockY) {
      return false;
    }
    const int xi = x + detail::PIECE_MASK_X_OFFSET;
    if (lockY == surfLockY[rot][xi]) {
      return yStart <= lastWcY[rot][xi];
    }
    if (WALL_CHARGE_TABLES.touchesWall[pieceIdx][rot][xi] != 0U) {
      return true;
    }
    const int scanLo =
        std::max(std::max(MIN_PIECE_Y, yStart), static_cast<int>(board.topNonEmptyRow) - 3);
    if (scanLo > lockY) {
      return false;
    }
    const uint64_t sm = WALL_CHARGE_TABLES.sideMask[pieceIdx][rot][xi];
    for (int y = scanLo; y <= lockY; ++y) {
      uint64_t b4;
      std::memcpy(&b4, &board.paddedRows[y + Y_ROW_OFFSET], sizeof(uint64_t));
      if ((b4 & sm) != 0ULL) {
        return true;
      }
    }
    return false;
  }
};

// Single-row directional wall-charge check (-1 = left, +1 = right).
inline bool wallChargeHitsRowDir(const Board20 &board, int pieceIdx, int rot, int x, int y,
                                 int dir) noexcept {
  assert(y >= MIN_PIECE_Y && y <= MAX_PIECE_Y);
  const int xi = x + detail::PIECE_MASK_X_OFFSET;
  if (dir < 0) {
    if (WALL_CHARGE_TABLES.touchesLeftWall[pieceIdx][rot][xi] != 0U) {
      return true;
    }
    uint64_t b4;
    std::memcpy(&b4, &board.paddedRows[y + Y_ROW_OFFSET], sizeof(uint64_t));
    return (b4 & WALL_CHARGE_TABLES.leftMask[pieceIdx][rot][xi]) != 0ULL;
  }
  if (WALL_CHARGE_TABLES.touchesRightWall[pieceIdx][rot][xi] != 0U) {
    return true;
  }
  uint64_t b4;
  std::memcpy(&b4, &board.paddedRows[y + Y_ROW_OFFSET], sizeof(uint64_t));
  return (b4 & WALL_CHARGE_TABLES.rightMask[pieceIdx][rot][xi]) != 0ULL;
}

inline constexpr int countGravityDrops(int fStart, int fEnd, int gravity) noexcept {
  if (fEnd <= fStart) {
    return 0;
  }
  if (gravity == 2) {
    return (fEnd >> 1) - (fStart >> 1);
  }
  if (gravity == 1) {
    return fEnd - fStart;
  }
  return (fEnd / 3) - (fStart / 3);
}

inline constexpr int computeLockFrame(int fStart, int y, int lockY, int gravity) noexcept {
  const int drops = lockY - y;
  assert(drops >= 0);
  if (gravity == 2) {
    return fStart + (((fStart & 1) == 0) ? 1 : 0) + 2 * drops;
  }
  if (gravity == 1) {
    return fStart + drops;
  }
  const int rem = (2 - (fStart % 3) + 3) % 3;
  return fStart + rem + 3 * drops;
}

inline constexpr int frameEnteringRow(int fStart, int yStart, int targetY, int gravity) noexcept {
  if (targetY <= yStart) {
    return fStart;
  }
  return computeLockFrame(fStart, yStart, targetY - 1, gravity) + 1;
}

// Unified NES rising-edge fresh wall-charge test over [f0 .. lockFrame] when heldDirPrev was held
// on frame f0 - 1 (same-direction fresh re-press requires 1 release frame).
inline bool canFreshWallChargeFrom(const Board20 &board, const BoardPieceCache &bpCache,
                                   int pieceIdx, int rot, int x, int yStart, int firstFallFrame,
                                   int lockY, int lockFrame, int f0, int dpadReady, int heldDirPrev,
                                   int gravity) noexcept {
  const int baseF = std::max(f0, dpadReady);
  if (baseF > lockFrame) {
    return false;
  }
  if (heldDirPrev == 0 || baseF > f0) {
    const int yW = yStart + countGravityDrops(firstFallFrame, baseF, gravity);
    return bpCache.canWallChargeAny(board, pieceIdx, rot, x, yW, lockY);
  }
  const int y0 = yStart + countGravityDrops(firstFallFrame, f0, gravity);
  if (wallChargeHitsRowDir(board, pieceIdx, rot, x, y0, -heldDirPrev)) {
    return true;
  }
  if (f0 + 1 <= lockFrame) {
    const int y1 = yStart + countGravityDrops(firstFallFrame, f0 + 1, gravity);
    return bpCache.canWallChargeAny(board, pieceIdx, rot, x, y1, lockY);
  }
  return false;
}

enum class TrackKind : uint8_t {
  Das = 0,
  SlowTap = 1,
  Quicktap = 2,
};

struct ShiftSchedule {
  TrackKind track = TrackKind::Das;
  int8_t dir = 0;        // -1, 0, +1
  uint8_t numShifts = 0; // 0..6
  uint8_t shiftFrames[DAS_MAX_SHIFTS] = {};
  uint8_t shiftResetsDas[DAS_MAX_SHIFTS] = {};
  uint8_t holdsThroughAre = 0;
  uint8_t holdsAfterLastShift = 0;
  uint8_t trackFlags = 0;
};

// Shared D-pad hold decision for all schedule simulation loops (including stationary frame-18
// hold-DAS when start.isAdjustment && start.heldDir != 0 && sched.dir == 0).
inline bool shouldHoldDpad(const ShiftSchedule &sched, const DasStartState &start, int nextShiftIdx,
                           int heldDir, int das, int &outHoldDir) noexcept {
  if (sched.dir == 0) {
    if (start.isAdjustment
        && start.heldDir != 0
        && heldDir == start.heldDir
        && das < DAS_FULL_THRESHOLD) {
      outHoldDir = start.heldDir;
      return true;
    }
    outHoldDir = 0;
    return false;
  }
  if (sched.track == TrackKind::Das || sched.track == TrackKind::Quicktap) {
    if (nextShiftIdx < sched.numShifts) {
      const bool inQtRel =
          (sched.track == TrackKind::Quicktap && nextShiftIdx == sched.numShifts - 1);
      const bool waitReset = (nextShiftIdx == 0 && sched.shiftResetsDas[0] != 0U);
      if (!inQtRel && !waitReset) {
        outHoldDir = sched.dir;
        return true;
      }
    } else if (sched.holdsAfterLastShift != 0U
               && heldDir == sched.dir
               && das < DAS_FULL_THRESHOLD) {
      outHoldDir = sched.dir;
      return true;
    }
  }
  outHoldDir = 0;
  return false;
}

inline int computeTuckDpadReadyFrame(int nextFrame, int dpadCd, int shiftCd,
                                     const MoveSearchConfig &cfg) noexcept {
  const int cdReady = nextFrame + dpadCd;
  if (cfg.enableQuicktaps) {
    return cdReady;
  }
  return std::max(cdReady, nextFrame + shiftCd);
}

inline bool buildShiftSchedule(const DasStartState &start, int goalOffsetX, TrackKind track,
                               const MoveSearchConfig &cfg, ShiftSchedule &out) noexcept {
  out = {};
  out.track = track;
  const int absDx = std::abs(goalOffsetX);
  if (absDx > DAS_MAX_SHIFTS) {
    return false;
  }
  const int dir = (goalOffsetX > 0) ? 1 : ((goalOffsetX < 0) ? -1 : 0);
  out.dir = static_cast<int8_t>(dir);
  out.numShifts = static_cast<uint8_t>(absDx);

  if (absDx == 0) {
    return track == TrackKind::Das;
  }

  const bool dasWillReset = (!start.isAdjustment && start.dasCharge <= DAS_RESET_THRESHOLD)
                            || (start.isAdjustment && start.heldDir != dir);
  const int dpadPeriod = cfg.dpadGapFrames + 1;

  if (track == TrackKind::Das) {
    out.holdsAfterLastShift = 1;
    out.holdsThroughAre = (!start.isAdjustment && !dasWillReset) ? 1U : 0U;
    out.trackFlags = 0;

    int f = start.frame;
    int das = start.dasCharge;
    int cd = start.dpadCooldown;
    bool pendingReset = dasWillReset;
    int sIdx = 0;

    while (sIdx < absDx) {
      if (f >= DAS_MAX_SIM_FRAMES) {
        return false;
      }
      if (pendingReset) {
        if (cd == 0) {
          out.shiftFrames[sIdx] = static_cast<uint8_t>(f);
          out.shiftResetsDas[sIdx] = 1;
          sIdx++;
          das = 0;
          pendingReset = false;
          cd = cfg.dpadGapFrames;
          f++;
          continue;
        }
        cd--;
        f++;
        continue;
      }
      if (das >= DAS_FULL_THRESHOLD) {
        out.shiftFrames[sIdx] = static_cast<uint8_t>(f);
        out.shiftResetsDas[sIdx] = 0;
        sIdx++;
        das = DAS_POST_SHIFT_CHARGE;
      } else {
        das++;
      }
      if (cd > 0) {
        cd--;
      }
      f++;
    }
    return true;
  }

  if (track == TrackKind::SlowTap) {
    if (!cfg.enableSlowTaps) {
      return false;
    }
    const bool allowed =
        (!start.isAdjustment && start.dasCharge < DAS_FULL_THRESHOLD) || dasWillReset;
    if (!allowed || (absDx == 1 && dasWillReset)) {
      return false;
    }
    out.holdsAfterLastShift = 0;
    out.holdsThroughAre = 0;
    out.trackFlags = MOVE_FLAG_SLOW_TAP;

    int f = static_cast<int>(start.frame) + static_cast<int>(start.dpadCooldown);
    for (int s = 0; s < absDx; ++s) {
      if (f >= DAS_MAX_SIM_FRAMES) {
        return false;
      }
      out.shiftFrames[s] = static_cast<uint8_t>(f);
      out.shiftResetsDas[s] = 1;
      f += dpadPeriod;
    }
    return true;
  }

  if (!cfg.enableQuicktaps) {
    return false;
  }
  ShiftSchedule dasSched;
  if (!buildShiftSchedule(start, goalOffsetX, TrackKind::Das, cfg, dasSched)) {
    return false;
  }

  int qtFrame = -1;
  if (absDx >= 2) {
    const int prevShiftF = dasSched.shiftFrames[absDx - 2];
    const int dasLastF = dasSched.shiftFrames[absDx - 1];
    int lastManualDown =
        start.dpadCooldown > 0
            ? (static_cast<int>(start.frame) + static_cast<int>(start.dpadCooldown) - dpadPeriod)
            : NO_PREV_FRAME;
    for (int s = 0; s < absDx - 1; ++s) {
      if (dasSched.shiftResetsDas[s] != 0U) {
        lastManualDown = dasSched.shiftFrames[s];
      }
    }
    const int earliest =
        std::max(prevShiftF + cfg.quicktapDelayFrames, lastManualDown + dpadPeriod);
    if (earliest < dasLastF && earliest < DAS_MAX_SIM_FRAMES) {
      qtFrame = earliest;
    }
  } else if (absDx == 1
             && start.isAdjustment
             && !dasWillReset
             && start.dasCharge < DAS_FULL_THRESHOLD) {
    const int dasLastF = dasSched.shiftFrames[0];
    // Respect both 1 release frame (start.frame) and quicktapDelayFrames since last shift.
    const int prevShiftF =
        (start.shiftCooldown > 0)
            ? (static_cast<int>(start.frame) + static_cast<int>(start.shiftCooldown) - dpadPeriod)
            : NO_PREV_FRAME;
    const int earliest =
        std::max(std::max(static_cast<int>(start.frame) + 1, prevShiftF + cfg.quicktapDelayFrames),
                 static_cast<int>(start.frame) + static_cast<int>(start.dpadCooldown));
    if (earliest < dasLastF && earliest < DAS_MAX_SIM_FRAMES) {
      qtFrame = earliest;
    }
  }

  if (qtFrame < 0) {
    return false;
  }

  out = dasSched;
  out.track = TrackKind::Quicktap;
  out.shiftFrames[absDx - 1] = static_cast<uint8_t>(qtFrame);
  out.shiftResetsDas[absDx - 1] = 1;
  out.holdsAfterLastShift = 0;
  out.trackFlags = MOVE_FLAG_QUICKTAP;
  return true;
}

// SoA swept-path metadata: exact 16 bytes (4 entries per 64-byte cache line!).
// Parallel BoardMask5 array is only touched when maxArriveRow >= board.topNonEmptyRow.
struct WalkMeta {
  static constexpr uint8_t BIT_ARRIVE_ON_GRAVITY = 1U << 0;
  static constexpr uint8_t BIT_HOLD_ACTIVE = 1U << 1;

  int8_t goalX = 0;
  int8_t yArrive = 0;
  int8_t yAtArriveInput = 0;
  int8_t dir = 0;
  int8_t heldDirAtArrive = 0;
  int8_t maxArriveRow = -1;
  int8_t yAtLastShift = MIN_PIECE_Y;
  uint8_t goalRot = 0;
  uint8_t arriveFrame = 0;
  uint8_t dasAfterArrive = 0;
  uint8_t dpadReadyFrame = 0;
  uint8_t tuckDpadReadyFrame = 0;
  uint8_t rotReadyFrame = 0;
  uint8_t trackFlags = 0;
  uint8_t numShifts = 0;
  uint8_t bitFlags = 0;

  bool arriveOnGravity() const noexcept { return (bitFlags & BIT_ARRIVE_ON_GRAVITY) != 0U; }
  bool holdActiveAtArrive() const noexcept { return (bitFlags & BIT_HOLD_ACTIVE) != 0U; }
};
static_assert(sizeof(WalkMeta) == 16, "WalkMeta must be exactly 16 bytes");

struct SpawnRtMeta {
  BoardMask5 rtMask;
  int16_t rtAdjPoolIdx = -1;
  int8_t maxRtRow = -1;
  uint8_t hasRtCapture = 0;
};

inline bool precomputeSweptWalk(int pieceIdx, const DasStartState &start, int goalRot,
                                int goalOffsetX, TrackKind track, const MoveSearchConfig &cfg,
                                WalkMeta &outMeta, BoardMask5 &outMask,
                                SpawnRtMeta *outRtMeta = nullptr,
                                DasStartState *outRtState = nullptr) noexcept {
  outMeta = {};
  outMask.clear();
  if (outRtMeta != nullptr) {
    *outRtMeta = {};
  }
  const PieceDef20 &piece = PIECES_20[pieceIdx];
  const int goalX = static_cast<int>(start.x) + goalOffsetX;
  if (goalRot < 0 || goalRot >= piece.numOrientations || !inPieceXBounds(piece, goalRot, goalX)) {
    return false;
  }

  ShiftSchedule sched;
  if (!buildShiftSchedule(start, goalOffsetX, track, cfg, sched)) {
    return false;
  }

  const int rotMask = piece.numOrientations - 1;
  int rotInc = 0;
  if (start.rot != goalRot) {
    rotInc = (piece.numOrientations == 2)
                 ? 1
                 : ((((static_cast<int>(start.rot) - 1) & 3) == goalRot) ? -1 : 1);
  }

  int x = start.x;
  int y = start.y;
  int rot = start.rot;
  int f = start.frame;
  int das = start.dasCharge;
  int dpadCd = start.dpadCooldown;
  int rotCd = start.rotCooldown;
  int shiftCd = start.shiftCooldown;
  int heldDir = start.heldDir;
  int nextShiftIdx = 0;
  int yAtLastShift = start.y;
  bool arrived = (x == goalX && rot == goalRot);
  bool capturedRt = false;
  bool validEntry = false;

  const int rt =
      (!start.isAdjustment && cfg.reactionTime > 0 && outRtMeta != nullptr) ? cfg.reactionTime : -1;

  if (y > piece.maxY[rot]) {
    return false;
  }
  auto markArrive = [&](int r, int px, int py) {
    const int mr = outMask.orPiece(pieceIdx, r, px, py);
    if (mr > outMeta.maxArriveRow) {
      outMeta.maxArriveRow = static_cast<int8_t>(mr);
    }
  };
  auto markRt = [&](int r, int px, int py) {
    if (outRtMeta != nullptr) {
      const int mr = outRtMeta->rtMask.orPiece(pieceIdx, r, px, py);
      if (mr > outRtMeta->maxRtRow) {
        outRtMeta->maxRtRow = static_cast<int8_t>(mr);
      }
    }
  };

  markArrive(rot, x, y);
  if (rt > 0) {
    markRt(rot, x, y);
  }

  if (arrived) {
    const bool keepHoldingStationary =
        (start.isAdjustment && start.heldDir != 0 && das < DAS_FULL_THRESHOLD);
    outMeta.goalX = static_cast<int8_t>(goalX);
    outMeta.goalRot = static_cast<uint8_t>(goalRot);
    outMeta.yArrive = static_cast<int8_t>(y);
    outMeta.yAtArriveInput = static_cast<int8_t>(y);
    outMeta.arriveFrame = static_cast<uint8_t>(f);
    outMeta.dasAfterArrive = static_cast<uint8_t>(das);
    outMeta.heldDirAtArrive = start.heldDir;
    outMeta.dpadReadyFrame = static_cast<uint8_t>(f + dpadCd);
    outMeta.tuckDpadReadyFrame =
        static_cast<uint8_t>(computeTuckDpadReadyFrame(f, dpadCd, shiftCd, cfg));
    outMeta.rotReadyFrame = static_cast<uint8_t>(f + rotCd);
    outMeta.trackFlags = sched.trackFlags;
    outMeta.dir = sched.dir;
    outMeta.numShifts = sched.numShifts;
    outMeta.bitFlags = keepHoldingStationary ? WalkMeta::BIT_HOLD_ACTIVE : 0U;
    outMeta.yAtLastShift = static_cast<int8_t>(y);
    validEntry = true;
    if (rt <= 0) {
      return true;
    }
  }

  while (f < DAS_MAX_SIM_FRAMES) {
    if (y > piece.maxY[rot]) {
      return false;
    }
    const bool isGrav = detail::isGravityFrame(f, cfg.gravity);
    const bool willShift = (nextShiftIdx < sched.numShifts && sched.shiftFrames[nextShiftIdx] == f);
    int holdDir = 0;
    const bool willHold =
        !willShift && shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, holdDir);

    if (willShift) {
      const int nx = x + sched.dir;
      if (!inPieceXBounds(piece, rot, nx)) {
        return false;
      }
      x = nx;
      if (!arrived) {
        markArrive(rot, x, y);
      }
      if (rt > 0 && !capturedRt) {
        markRt(rot, x, y);
      }
      if (sched.shiftResetsDas[nextShiftIdx] != 0U) {
        das = 0;
        dpadCd = cfg.dpadGapFrames;
      } else {
        das = DAS_POST_SHIFT_CHARGE;
        if (dpadCd > 0) {
          dpadCd--;
        }
      }
      shiftCd = cfg.dpadGapFrames;
      heldDir = sched.dir;
      yAtLastShift = y;
      nextShiftIdx++;
    } else if (willHold) {
      if (das < DAS_FULL_THRESHOLD) {
        das++;
      }
      heldDir = holdDir;
      if (dpadCd > 0) {
        dpadCd--;
      }
      if (shiftCd > 0) {
        shiftCd--;
      }
    } else {
      heldDir = 0;
      if (dpadCd > 0) {
        dpadCd--;
      }
      if (shiftCd > 0) {
        shiftCd--;
      }
    }

    const bool willRot = (rot != goalRot && rotCd == 0);
    if (willRot) {
      const int nrot = (rot + rotInc + 4) & rotMask;
      if (inPieceXBounds(piece, nrot, x) && y <= piece.maxY[nrot]) {
        rot = nrot;
        rotCd = cfg.rotGapFrames;
        if (!arrived) {
          markArrive(rot, x, y);
        }
        if (rt > 0 && !capturedRt) {
          markRt(rot, x, y);
        }
      } else if (x == goalX) {
        return false;
      }
    } else if (rotCd > 0) {
      rotCd--;
    }

    const int yBeforeGrav = y;
    const bool justArrived = (!arrived && x == goalX && rot == goalRot);
    if (justArrived) {
      arrived = true;
      validEntry = true;
      outMeta.goalX = static_cast<int8_t>(goalX);
      outMeta.goalRot = static_cast<uint8_t>(goalRot);
      outMeta.yAtArriveInput = static_cast<int8_t>(yBeforeGrav);
      outMeta.arriveFrame = static_cast<uint8_t>(f + 1);
      outMeta.yArrive = static_cast<int8_t>(yBeforeGrav + (isGrav ? 1 : 0));
      outMeta.dasAfterArrive = static_cast<uint8_t>(das);
      outMeta.heldDirAtArrive = static_cast<int8_t>(heldDir);
      outMeta.dpadReadyFrame = static_cast<uint8_t>(f + 1 + dpadCd);
      outMeta.tuckDpadReadyFrame =
          static_cast<uint8_t>(computeTuckDpadReadyFrame(f + 1, dpadCd, shiftCd, cfg));
      outMeta.rotReadyFrame = static_cast<uint8_t>(f + 1 + rotCd);
      outMeta.trackFlags = sched.trackFlags;
      outMeta.dir = sched.dir;
      outMeta.numShifts = sched.numShifts;
      int dummyDir = 0;
      const bool holdAct = shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, dummyDir);
      outMeta.bitFlags = static_cast<uint8_t>((isGrav ? WalkMeta::BIT_ARRIVE_ON_GRAVITY : 0U)
                                              | (holdAct ? WalkMeta::BIT_HOLD_ACTIVE : 0U));
      outMeta.yAtLastShift = static_cast<int8_t>(yAtLastShift);
    }

    if (isGrav) {
      y++;
      if (y > piece.maxY[rot]) {
        if (justArrived) {
          outMeta.yArrive = static_cast<int8_t>(yBeforeGrav);
        }
        break;
      }
      if (!arrived) {
        markArrive(rot, x, y);
      }
      if (rt > 0 && !capturedRt && (f + 1) < rt) {
        markRt(rot, x, y);
      }
    }

    f++;

    if (rt > 0 && !capturedRt && f == rt) {
      capturedRt = true;
      outRtMeta->hasRtCapture = 1;
      markRt(rot, x, y);
      if (outRtState != nullptr) {
        *outRtState =
            DasStartState::makeAdjustment(x, y, rot, f, das, heldDir, dpadCd, rotCd, shiftCd);
      }
    }

    if (arrived && (rt <= 0 || capturedRt)) {
      break;
    }
  }

  return validEntry;
}

// Sparse-reset per-cell best move accumulator (zero full memset between phi states).
struct LockMoveTable {
  static constexpr int X_OFF = detail::PIECE_MASK_X_OFFSET;
  static constexpr int X_DIM = detail::PIECE_MASK_X_COUNT;
  static constexpr int Y_OFF = Y_ROW_OFFSET;
  static constexpr int Y_DIM = 22;
  static constexpr int TOTAL = 4 * X_DIM * Y_DIM;

  uint16_t slot[TOTAL] = {}; // index into out.data[] + 1 (0 = empty)
  uint16_t touchedSlots[DAS_MAX_LOCK_MOVES];
  int numTouched = 0;
  int8_t basicLockY[4][X_DIM];
  uint16_t basicFoundBits[4];

  void initOnce() noexcept {
    std::memset(slot, 0, sizeof(slot));
    numTouched = 0;
    resetBasic();
  }

  void resetBasic() noexcept {
    for (int r = 0; r < 4; ++r) {
      basicFoundBits[r] = 0U;
      for (int x = 0; x < X_DIM; ++x) {
        basicLockY[r][x] = INVALID_ROW_Y;
      }
    }
  }

  void sparseReset(bool resetBasicTracking = true) noexcept {
    for (int i = 0; i < numTouched; ++i) {
      slot[touchedSlots[i]] = 0;
    }
    numTouched = 0;
    if (resetBasicTracking) {
      resetBasic();
    }
  }

  static int cellIndex(int rot, int x, int y) noexcept {
    return ((rot * X_DIM) + (x + X_OFF)) * Y_DIM + (y + Y_OFF);
  }

  void recordBasic(int rot, int x, int y, uint8_t dasAfter, uint8_t flags, bool trackBasic,
                   DasMoveList &out) noexcept {
    if (trackBasic) {
      const int xi = x + X_OFF;
      if (y > basicLockY[rot][xi]) {
        basicLockY[rot][xi] = static_cast<int8_t>(y);
      }
      basicFoundBits[rot] |= static_cast<uint16_t>(1U << xi);
    }
    recordMove(rot, x, y, dasAfter, flags, '.', out);
  }

  void recordMove(int rot, int x, int y, uint8_t dasAfter, uint8_t flags, char tuckInput,
                  DasMoveList &out) noexcept {
    const int idx = cellIndex(rot, x, y);
    const uint16_t existing = slot[idx];
    if (existing == 0U) {
      assert(out.count < DAS_MAX_LOCK_MOVES);
      if (out.count >= DAS_MAX_LOCK_MOVES) {
        return;
      }
      out.data[out.count] = {static_cast<int8_t>(x),
                             static_cast<int8_t>(y),
                             static_cast<uint8_t>(rot),
                             dasAfter,
                             flags,
                             tuckInput};
      out.count++;
      slot[idx] = static_cast<uint16_t>(out.count);
      touchedSlots[numTouched++] = static_cast<uint16_t>(idx);
      return;
    }
    DasLockMove &cur = out.data[existing - 1];
    if (dasAfter > cur.dasChargeAfter
        || (dasAfter == cur.dasChargeAfter && cur.tuckInput != '.' && tuckInput == '.')
        || (dasAfter == cur.dasChargeAfter && cur.tuckInput == tuckInput && flags < cur.flags)) {
      cur.dasChargeAfter = dasAfter;
      cur.flags = flags;
      cur.tuckInput = tuckInput;
    }
  }
};

// Compact 8-byte Phase B fall track descriptor for Phase C overhang tucks/spins.
struct TuckColumnTrack {
  int8_t lockY = INVALID_ROW_Y;
  int8_t yStart = BOARD_ROWS;
  uint8_t firstFallFrame = 0;
  uint8_t dpadReadyFrame = UINT8_MAX;
  uint8_t tuckDpadReadyFrame = UINT8_MAX;
  uint8_t rotReadyFrame = UINT8_MAX;
  uint8_t spinDasBase = 0;
  int8_t initHeldDir = 0;
};
static_assert(sizeof(TuckColumnTrack) == 8, "TuckColumnTrack must be exactly 8 bytes");

struct TuckColumnTable {
  static constexpr int X_OFF = detail::PIECE_MASK_X_OFFSET;
  static constexpr int X_DIM = detail::PIECE_MASK_X_COUNT;
  static constexpr int MAX_PER_COL = 10;
  TuckColumnTrack entries[4][X_DIM][MAX_PER_COL];
  uint8_t counts[4][X_DIM];

  void reset() noexcept { std::memset(counts, 0, sizeof(counts)); }

  void add(int rot, int x, int yStart, int lockY, int firstFallFrame, int dpadReadyFrame,
           int tuckDpadReadyFrame, int rotReadyFrame, int spinDasBase, int initHeldDir) noexcept {
    const int xi = x + X_OFF;
    uint8_t &cnt = counts[rot][xi];
    const uint8_t ff = static_cast<uint8_t>(firstFallFrame);
    const uint8_t dr = static_cast<uint8_t>(std::min(dpadReadyFrame, 255));
    const uint8_t tdr = static_cast<uint8_t>(std::min(tuckDpadReadyFrame, 255));
    const uint8_t rr = static_cast<uint8_t>(std::min(rotReadyFrame, 255));
    if (cnt == 0U) {
      entries[rot][xi][0] = {
          static_cast<int8_t>(lockY),        static_cast<int8_t>(yStart),     ff, dr, tdr, rr,
          static_cast<uint8_t>(spinDasBase), static_cast<int8_t>(initHeldDir)};
      cnt = 1U;
      return;
    }
    for (int i = 0; i < cnt; ++i) {
      TuckColumnTrack &e = entries[rot][xi][i];
      if (e.lockY == lockY && e.initHeldDir == initHeldDir) {
        if (e.yStart <= yStart
            && e.firstFallFrame <= ff
            && e.dpadReadyFrame <= dr
            && e.tuckDpadReadyFrame <= tdr
            && e.rotReadyFrame <= rr
            && e.spinDasBase >= spinDasBase) {
          return;
        }
        if (yStart <= e.yStart
            && ff <= e.firstFallFrame
            && dr <= e.dpadReadyFrame
            && tdr <= e.tuckDpadReadyFrame
            && rr <= e.rotReadyFrame
            && spinDasBase >= e.spinDasBase) {
          e = {static_cast<int8_t>(lockY),        static_cast<int8_t>(yStart),     ff, dr, tdr, rr,
               static_cast<uint8_t>(spinDasBase), static_cast<int8_t>(initHeldDir)};
          return;
        }
      }
    }
    if (cnt < MAX_PER_COL) {
      entries[rot][xi][cnt++] = {
          static_cast<int8_t>(lockY),        static_cast<int8_t>(yStart),     ff, dr, tdr, rr,
          static_cast<uint8_t>(spinDasBase), static_cast<int8_t>(initHeldDir)};
    }
  }
};

// O(1) Phase B using precomputed BoardPieceCache. Templated on CollectTucks so unobstructed
// 0-hole searches pay zero tuck-tracking overhead.
template <bool CollectTucks>
inline void evaluateFallPhaseB(const Board20 &board, const BoardPieceCache &bpCache, int pieceIdx,
                               int rot, int x, int yStart, int firstFallFrame, int dasAtStart,
                               int dpadReadyFrame, int tuckDpadReadyFrame, int rotReadyFrame,
                               bool holdActive, int heldDirAtArrive, bool lockedOnArrive,
                               uint8_t flags, const MoveSearchConfig &cfg, LockMoveTable &table,
                               TuckColumnTable &tuckCols, DasMoveList &out) noexcept {
  if (lockedOnArrive) {
    table.recordBasic(rot, x, yStart, static_cast<uint8_t>(dasAtStart), flags, CollectTucks, out);
    return;
  }

  const int lockY = bpCache.dropY(board, pieceIdx, rot, x, yStart);
  const int lockFrame = computeLockFrame(firstFallFrame, yStart, lockY, cfg.gravity);
  const int fallFrames = lockFrame - firstFallFrame + 1;

  int bestDas = dasAtStart;
  uint8_t bestFlags = flags;

  // Held wall-charge right on firstFallFrame when arriving already at das == 15 with D-pad held.
  if (bestDas == DAS_FULL_THRESHOLD
      && heldDirAtArrive != 0
      && wallChargeHitsRowDir(board, pieceIdx, rot, x, yStart, heldDirAtArrive)) {
    bestDas = DAS_MAX_CHARGE;
    bestFlags |= MOVE_FLAG_WALL_CHARGE;
  }

  // Variant 1: Keep holding heldDirAtArrive until das == 15 (or lock).
  if (holdActive && heldDirAtArrive != 0 && dasAtStart < DAS_FULL_THRESHOLD) {
    const int heldFrames = std::min(DAS_FULL_THRESHOLD - dasAtStart, fallFrames);
    bestDas = dasAtStart + heldFrames;
    const int postHoldF = firstFallFrame + heldFrames;
    if (postHoldF <= lockFrame) {
      const int yPost = yStart + countGravityDrops(firstFallFrame, postHoldF, cfg.gravity);
      if (wallChargeHitsRowDir(board, pieceIdx, rot, x, yPost, heldDirAtArrive)
          || canFreshWallChargeFrom(board, bpCache, pieceIdx, rot, x, yStart, firstFallFrame, lockY,
                                    lockFrame, postHoldF, dpadReadyFrame, heldDirAtArrive,
                                    cfg.gravity)) {
        bestDas = DAS_MAX_CHARGE;
        bestFlags |= MOVE_FLAG_WALL_CHARGE;
      }
    }
  }

  // Variant 2: Release immediately after arrival and wall-charge when rising-edge & cooldown allow.
  if (bestDas < DAS_MAX_CHARGE
      && canFreshWallChargeFrom(board, bpCache, pieceIdx, rot, x, yStart, firstFallFrame, lockY,
                                lockFrame, firstFallFrame, dpadReadyFrame, heldDirAtArrive,
                                cfg.gravity)) {
    bestDas = DAS_MAX_CHARGE;
    bestFlags |= MOVE_FLAG_WALL_CHARGE;
  }

  table.recordBasic(rot, x, lockY, static_cast<uint8_t>(bestDas), bestFlags, CollectTucks, out);

  if constexpr (CollectTucks) {
    tuckCols.add(rot, x, yStart, lockY, firstFallFrame, dpadReadyFrame, tuckDpadReadyFrame,
                 rotReadyFrame, dasAtStart, heldDirAtArrive);
    if (holdActive && heldDirAtArrive != 0 && dasAtStart < DAS_FULL_THRESHOLD) {
      const int needed = DAS_FULL_THRESHOLD - dasAtStart;
      if (needed < fallFrames) {
        const int postF = firstFallFrame + needed;
        const int postY = yStart + countGravityDrops(firstFallFrame, postF, cfg.gravity);
        tuckCols.add(rot, x, postY, lockY, postF, std::max(postF, dpadReadyFrame),
                     std::max(postF, tuckDpadReadyFrame), std::max(postF, rotReadyFrame),
                     DAS_FULL_THRESHOLD, heldDirAtArrive);
      }
    }
  }
}

// Single frame of the 32-bit (4 rots x 8 rotCooldowns) rotation bitmask DP.
inline void stepRotationMask(const Board20 &board, int pieceIdx, int numOrients, int x, int y,
                             int rotGapFrames, uint8_t m[4]) noexcept {
  const int rotMask = numOrients - 1;
  uint8_t nextM[4] = {};
  for (int r = 0; r < numOrients; ++r) {
    if (m[r] == 0U) {
      continue;
    }
    nextM[r] |= static_cast<uint8_t>((m[r] >> 1) | (m[r] & 1U));
    if ((m[r] & 1U) != 0U && numOrients > 1) {
      const uint8_t cdBit = static_cast<uint8_t>(1U << rotGapFrames);
      const int cw = (r + 1) & rotMask;
      if (!collides20(board, pieceIdx, x, y, cw)) {
        nextM[cw] |= cdBit;
      }
      if (numOrients == 4) {
        const int ccw = (r + 3) & 3;
        if (!collides20(board, pieceIdx, x, y, ccw)) {
          nextM[ccw] |= cdBit;
        }
      }
    }
  }
  for (int r = 0; r < numOrients; ++r) {
    m[r] = nextM[r];
  }
}

inline void emitAdjStatesFromRotMask(const uint8_t m[4], int numOrients, int x, int y, int frame,
                                     int das, int heldDir, int dpadCd, int shiftCd,
                                     uint8_t allowedRotMask, AdjStateBuf &outAdj) noexcept {
  for (int r = 0; r < numOrients; ++r) {
    if (m[r] == 0U || ((allowedRotMask & (1U << r)) == 0U)) {
      continue;
    }
    outAdj.push(DasStartState::makeAdjustment(x, y, r, frame, das, heldDir, dpadCd,
                                              __builtin_ctz(static_cast<unsigned>(m[r])), shiftCd));
  }
}

// In-column rotation closure at a reached column goalX starting at frame f's gravity step
// (or at frame f's shift/rotate step when runFirstFrameInputs == true, used for k == 0).
template <bool CollectTucks>
inline void finishPoleVaultAtColumn(const Board20 &board, const BoardPieceCache &bpCache,
                                    int pieceIdx, const DasStartState &start,
                                    const ShiftSchedule &sched, const uint8_t initM[4], int goalX,
                                    int y, int f, int das, int dpadCd, int shiftCd, int heldDir,
                                    uint8_t neededRotMask, bool preEmittedStartRot,
                                    bool runFirstFrameInputs, int rt, const MoveSearchConfig &cfg,
                                    LockMoveTable &table, TuckColumnTable &tuckCols,
                                    DasMoveList &out, AdjStateBuf *outAdjStates) noexcept {
  const PieceDef20 &piece = PIECES_20[pieceIdx];
  const int numOrients = piece.numOrientations;
  uint8_t m[4] = {initM[0], initM[1], initM[2], initM[3]};
  bool emittedAtGoal[4] = {};
  const bool zeroHoles = (board.numHoles == 0);
  for (int r = 0; r < numOrients; ++r) {
    if (!inPieceXBounds(piece, r, goalX) || ((neededRotMask & (1U << r)) == 0U)) {
      emittedAtGoal[r] = true;
    } else if (zeroHoles && collides20(board, pieceIdx, goalX, y, r)) {
      emittedAtGoal[r] = true;
    }
  }
  if (preEmittedStartRot) {
    emittedAtGoal[start.rot] = true;
  }
  const uint8_t mvFlags = static_cast<uint8_t>(sched.trackFlags | MOVE_FLAG_POLE_VAULT);

  if (rt <= 0) {
    bool allDone = true;
    for (int r = 0; r < numOrients; ++r) {
      if (!emittedAtGoal[r]) {
        allDone = false;
        break;
      }
    }
    if (allDone) {
      return;
    }
  }

  bool skipInputsThisFrame = !runFirstFrameInputs;
  while (f < DAS_MAX_SIM_FRAMES) {
    if ((m[0] | m[1] | m[2] | m[3]) == 0U) {
      break;
    }
    const bool isGrav = detail::isGravityFrame(f, cfg.gravity);

    if (!skipInputsThisFrame) {
      int holdDir = 0;
      if (shouldHoldDpad(sched, start, sched.numShifts, heldDir, das, holdDir)) {
        das++;
        heldDir = holdDir;
      } else {
        heldDir = 0;
      }
      if (dpadCd > 0) {
        dpadCd--;
      }
      if (shiftCd > 0) {
        shiftCd--;
      }
      stepRotationMask(board, pieceIdx, numOrients, goalX, y, cfg.rotGapFrames, m);
    }
    skipInputsThisFrame = false;

    uint8_t lockedHere = 0;
    if (isGrav) {
      for (int r = 0; r < numOrients; ++r) {
        if (m[r] != 0U && collides20(board, pieceIdx, goalX, y + 1, r)) {
          lockedHere |= static_cast<uint8_t>(1U << r);
        }
      }
    }

    int dummyDir = 0;
    const bool holdActiveNow =
        shouldHoldDpad(sched, start, sched.numShifts, heldDir, das, dummyDir);
    const int tuckDpadReady = computeTuckDpadReadyFrame(f + 1, dpadCd, shiftCd, cfg);
    for (int r = 0; r < numOrients; ++r) {
      if (m[r] == 0U) {
        continue;
      }
      const bool isLocked = ((lockedHere & (1U << r)) != 0U);
      if (!emittedAtGoal[r]) {
        emittedAtGoal[r] = true;
        const int bestRotCd = __builtin_ctz(static_cast<unsigned>(m[r]));
        const int yNext = isLocked ? y : (y + (isGrav ? 1 : 0));
        evaluateFallPhaseB<CollectTucks>(board, bpCache, pieceIdx, r, goalX, yNext, f + 1, das,
                                         f + 1 + dpadCd, tuckDpadReady, f + 1 + bestRotCd,
                                         !isLocked && holdActiveNow, heldDir, isLocked, mvFlags,
                                         cfg, table, tuckCols, out);
      }
      if (isLocked) {
        m[r] = 0;
      }
    }

    if (isGrav) {
      y++;
    }
    f++;

    if (rt > 0 && f == rt && outAdjStates != nullptr) {
      emitAdjStatesFromRotMask(m, numOrients, goalX, y, f, das, heldDir, dpadCd, shiftCd,
                               ALL_ROTS_MASK, *outAdjStates);
    }

    if (rt <= 0 || f >= rt) {
      bool allDone = true;
      for (int r = 0; r < numOrients; ++r) {
        if (!emittedAtGoal[r]) {
          if (zeroHoles && collides20(board, pieceIdx, goalX, y, r)) {
            emittedAtGoal[r] = true;
          } else {
            allDone = false;
            break;
          }
        }
      }
      if (allDone) {
        break;
      }
    }
  }
}

// Merged single-pass multi-column 32-bit pole-vault DP for DAS or SlowTap along one direction.
template <bool CollectTucks>
inline void runPoleVaultDpMultiK(const Board20 &board, const BoardPieceCache &bpCache, int pieceIdx,
                                 const DasStartState &start, int dir, TrackKind tk,
                                 const uint8_t neededRotByK[DAS_MAX_SHIFTS + 1], uint8_t rtKMask,
                                 const MoveSearchConfig &cfg, LockMoveTable &table,
                                 TuckColumnTable &tuckCols, DasMoveList &out,
                                 AdjStateBuf *outAdjStates = nullptr,
                                 const ShiftSchedule *preSched = nullptr) noexcept {
  assert(tk != TrackKind::Quicktap && dir != 0);
  assert(start.rotCooldown <= DAS_MAX_GAP_FRAMES);
  if (collides20(board, pieceIdx, start.x, start.y, start.rot)) {
    return;
  }

  int maxK = 0;
  for (int k = 1; k <= DAS_MAX_SHIFTS; ++k) {
    if (neededRotByK[k] != 0U || ((rtKMask & (1U << k)) != 0U)) {
      maxK = k;
    }
  }
  if (maxK == 0) {
    return;
  }
  ShiftSchedule sched;
  if (preSched != nullptr && preSched->numShifts > 0) {
    sched = *preSched;
    if (sched.numShifts > maxK) {
      sched.numShifts = static_cast<uint8_t>(maxK);
    }
  } else if (!buildShiftSchedule(start, dir * maxK, tk, cfg, sched)) {
    int validK = maxK - 1;
    while (validK >= 1 && !buildShiftSchedule(start, dir * validK, tk, cfg, sched)) {
      validK--;
    }
    if (validK < 1) {
      return;
    }
  }

  const PieceDef20 &piece = PIECES_20[pieceIdx];
  const int numOrients = piece.numOrientations;
  const int baseRt = (!start.isAdjustment && cfg.reactionTime > 0 && outAdjStates != nullptr)
                         ? cfg.reactionTime
                         : -1;

  // Exact 0-hole pre-loop reachability truncation.
  if (board.numHoles == 0 && baseRt <= 0) {
    int reachableShifts = 0;
    int cx = start.x;
    for (int s = 0; s < sched.numShifts; ++s) {
      const int nx = cx + dir;
      if (nx < MIN_PIECE_X || nx > MAX_PIECE_X) {
        break;
      }
      const int ys = static_cast<int>(start.y)
                     + countGravityDrops(start.frame, sched.shiftFrames[s], cfg.gravity);
      if (ys > bpCache.maxSurfLockYAtX[cx + detail::PIECE_MASK_X_OFFSET]
          || ys > bpCache.maxSurfLockYAtX[nx + detail::PIECE_MASK_X_OFFSET]) {
        break;
      }
      cx = nx;
      reachableShifts = s + 1;
    }
    int newMaxK = 0;
    for (int k = 1; k <= reachableShifts; ++k) {
      if (neededRotByK[k] != 0U) {
        newMaxK = k;
      }
    }
    if (newMaxK == 0) {
      return;
    }
    sched.numShifts = static_cast<uint8_t>(newMaxK);
  }

  uint8_t m[4] = {};
  m[start.rot] = static_cast<uint8_t>(1U << start.rotCooldown);

  int x = start.x;
  int y = start.y;
  int f = start.frame;
  int das = start.dasCharge;
  int dpadCd = start.dpadCooldown;
  int shiftCd = start.shiftCooldown;
  int heldDir = start.heldDir;
  int nextShiftIdx = 0;

  while (f < DAS_MAX_SIM_FRAMES && nextShiftIdx < sched.numShifts) {
    if ((m[0] | m[1] | m[2] | m[3]) == 0U) {
      break;
    }
    const bool isGrav = detail::isGravityFrame(f, cfg.gravity);
    const bool willShift = (sched.shiftFrames[nextShiftIdx] == f);
    int holdDir = 0;
    const bool willHold =
        !willShift && shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, holdDir);

    if (willShift) {
      x += dir;
      for (int r = 0; r < numOrients; ++r) {
        if (m[r] != 0U && collides20(board, pieceIdx, x, y, r)) {
          m[r] = 0;
        }
      }
      if ((m[0] | m[1] | m[2] | m[3]) == 0U) {
        break;
      }
      if (sched.shiftResetsDas[nextShiftIdx] != 0U) {
        das = 0;
        dpadCd = cfg.dpadGapFrames;
      } else {
        das = DAS_POST_SHIFT_CHARGE;
        if (dpadCd > 0) {
          dpadCd--;
        }
      }
      shiftCd = cfg.dpadGapFrames;
      heldDir = dir;
      nextShiftIdx++;
    } else if (willHold) {
      if (das < DAS_FULL_THRESHOLD) {
        das++;
      }
      heldDir = holdDir;
      if (dpadCd > 0) {
        dpadCd--;
      }
      if (shiftCd > 0) {
        shiftCd--;
      }
    } else {
      heldDir = 0;
      if (dpadCd > 0) {
        dpadCd--;
      }
      if (shiftCd > 0) {
        shiftCd--;
      }
    }

    stepRotationMask(board, pieceIdx, numOrients, x, y, cfg.rotGapFrames, m);

    if (willShift) {
      const uint8_t needRots = neededRotByK[nextShiftIdx];
      const int colRt = ((rtKMask & (1U << nextShiftIdx)) != 0U) ? baseRt : -1;
      if (needRots != 0U || colRt > 0) {
        ShiftSchedule colSched = sched;
        colSched.numShifts = static_cast<uint8_t>(nextShiftIdx);
        finishPoleVaultAtColumn<CollectTucks>(
            board, bpCache, pieceIdx, start, colSched, m, x, y, f, das, dpadCd, shiftCd, heldDir,
            colRt > 0 ? ALL_ROTS_MASK : needRots, /*preEmittedStartRot=*/false,
            /*runFirstFrameInputs=*/false, colRt, cfg, table, tuckCols, out,
            colRt > 0 ? outAdjStates : nullptr);
      }
    }

    if (isGrav) {
      for (int r = 0; r < numOrients; ++r) {
        if (m[r] != 0U && collides20(board, pieceIdx, x, y + 1, r)) {
          m[r] = 0;
        }
      }
      y++;
    }
    f++;

    if (baseRt > 0 && f == baseRt && (rtKMask >> (nextShiftIdx + 1)) != 0U) {
      emitAdjStatesFromRotMask(m, numOrients, x, y, f, das, heldDir, dpadCd, shiftCd, ALL_ROTS_MASK,
                               *outAdjStates);
    }
  }
}

// 32-bit (4 rots x 8 rotCooldowns) DP along a single ShiftSchedule (used for k=0, Quicktap,
// dynamic).
template <bool CollectTucks>
inline void runPoleVaultDpForSchedule(const Board20 &board, const BoardPieceCache &bpCache,
                                      int pieceIdx, const DasStartState &start,
                                      const ShiftSchedule &sched, const MoveSearchConfig &cfg,
                                      LockMoveTable &table, TuckColumnTable &tuckCols,
                                      DasMoveList &out,
                                      AdjStateBuf *outAdjStates = nullptr) noexcept {
  assert(start.rotCooldown <= DAS_MAX_GAP_FRAMES);
  if (collides20(board, pieceIdx, start.x, start.y, start.rot)) {
    return;
  }

  const PieceDef20 &piece = PIECES_20[pieceIdx];
  const int numOrients = piece.numOrientations;
  const int rt = (!start.isAdjustment && cfg.reactionTime > 0 && outAdjStates != nullptr)
                     ? cfg.reactionTime
                     : -1;
  const int goalX = static_cast<int>(start.x) + static_cast<int>(sched.dir) * sched.numShifts;

  uint8_t m[4] = {};
  m[start.rot] = static_cast<uint8_t>(1U << start.rotCooldown);

  if (sched.numShifts == 0) {
    const bool keepHoldingStationary =
        (start.isAdjustment && start.heldDir != 0 && start.dasCharge < DAS_FULL_THRESHOLD);
    evaluateFallPhaseB<CollectTucks>(
        board, bpCache, pieceIdx, start.rot, start.x, start.y, start.frame, start.dasCharge,
        start.frame + start.dpadCooldown,
        computeTuckDpadReadyFrame(start.frame, start.dpadCooldown, start.shiftCooldown, cfg),
        start.frame + start.rotCooldown, keepHoldingStationary, start.heldDir, false,
        static_cast<uint8_t>(sched.trackFlags | MOVE_FLAG_POLE_VAULT), cfg, table, tuckCols, out);
    finishPoleVaultAtColumn<CollectTucks>(board, bpCache, pieceIdx, start, sched, m, goalX, start.y,
                                          start.frame, start.dasCharge, start.dpadCooldown,
                                          start.shiftCooldown, start.heldDir, ALL_ROTS_MASK,
                                          /*preEmittedStartRot=*/true, /*runFirstFrameInputs=*/true,
                                          rt, cfg, table, tuckCols, out, outAdjStates);
    return;
  }

  // On Quicktap schedules whose final shift occurs at or after frame RT, restrict RT captures to
  // orientations that are horizontally in bounds at goalX (consistent with swept-walk tables).
  uint8_t qtRtRotMask = ALL_ROTS_MASK;
  if (sched.track == TrackKind::Quicktap && rt > 0) {
    qtRtRotMask = 0U;
    for (int r = 0; r < numOrients; ++r) {
      if (inPieceXBounds(piece, r, goalX)) {
        qtRtRotMask |= static_cast<uint8_t>(1U << r);
      }
    }
  }

  int x = start.x;
  int y = start.y;
  int f = start.frame;
  int das = start.dasCharge;
  int dpadCd = start.dpadCooldown;
  int shiftCd = start.shiftCooldown;
  int heldDir = start.heldDir;
  int nextShiftIdx = 0;

  while (f < DAS_MAX_SIM_FRAMES && nextShiftIdx < sched.numShifts) {
    if ((m[0] | m[1] | m[2] | m[3]) == 0U) {
      break;
    }
    const bool isGrav = detail::isGravityFrame(f, cfg.gravity);
    const bool willShift = (sched.shiftFrames[nextShiftIdx] == f);
    int holdDir = 0;
    const bool willHold =
        !willShift && shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, holdDir);

    if (willShift) {
      x += sched.dir;
      for (int r = 0; r < numOrients; ++r) {
        if (m[r] != 0U && collides20(board, pieceIdx, x, y, r)) {
          m[r] = 0;
        }
      }
      if ((m[0] | m[1] | m[2] | m[3]) == 0U) {
        break;
      }
      if (sched.shiftResetsDas[nextShiftIdx] != 0U) {
        das = 0;
        dpadCd = cfg.dpadGapFrames;
      } else {
        das = DAS_POST_SHIFT_CHARGE;
        if (dpadCd > 0) {
          dpadCd--;
        }
      }
      shiftCd = cfg.dpadGapFrames;
      heldDir = sched.dir;
      nextShiftIdx++;
    } else if (willHold) {
      if (das < DAS_FULL_THRESHOLD) {
        das++;
      }
      heldDir = holdDir;
      if (dpadCd > 0) {
        dpadCd--;
      }
      if (shiftCd > 0) {
        shiftCd--;
      }
    } else {
      heldDir = 0;
      if (dpadCd > 0) {
        dpadCd--;
      }
      if (shiftCd > 0) {
        shiftCd--;
      }
    }

    stepRotationMask(board, pieceIdx, numOrients, x, y, cfg.rotGapFrames, m);

    if (nextShiftIdx == sched.numShifts) {
      // Arrived at goalX on frame f: hand off to shared in-column closure.
      finishPoleVaultAtColumn<CollectTucks>(
          board, bpCache, pieceIdx, start, sched, m, goalX, y, f, das, dpadCd, shiftCd, heldDir,
          ALL_ROTS_MASK, /*preEmittedStartRot=*/false, /*runFirstFrameInputs=*/false, rt, cfg,
          table, tuckCols, out, outAdjStates);
      return;
    }

    if (isGrav) {
      for (int r = 0; r < numOrients; ++r) {
        if (m[r] != 0U && collides20(board, pieceIdx, x, y + 1, r)) {
          m[r] = 0;
        }
      }
      y++;
    }
    f++;

    if (rt > 0 && f == rt && outAdjStates != nullptr) {
      emitAdjStatesFromRotMask(m, numOrients, x, y, f, das, heldDir, dpadCd, shiftCd, qtRtRotMask,
                               *outAdjStates);
    }
  }
}

// Shared Phase C single-row tuck/spin evaluator (used by both 0-hole and holey loops).
inline bool evalPhaseCTuckRow(const Board20 &board, const BoardPieceCache &bpCache, int pieceIdx,
                              const DasStartState &start, const TuckColumnTrack &tr,
                              const TuckInput20 &ti, int preRot, int newRot, int newX, int y,
                              int baseY, int readyFrame, int knownTargetSurfY,
                              const MoveSearchConfig &cfg, LockMoveTable &table,
                              DasMoveList &out) noexcept {
  const int fEnterY = frameEnteringRow(tr.firstFallFrame, tr.yStart, y, cfg.gravity);
  const int fExitY = computeLockFrame(tr.firstFallFrame, tr.yStart, y, cfg.gravity);
  const int tuckF = std::max(fEnterY, readyFrame);
  if (tuckF > fExitY) {
    return false;
  }

  const bool gravOnTuckF = detail::isGravityFrame(tuckF, cfg.gravity);
  const bool locksOnTuckF = gravOnTuckF && collides20(board, pieceIdx, newX, y + 1, newRot);
  const int postY = locksOnTuckF ? y : (y + (gravOnTuckF ? 1 : 0));
  const int tuckLockY = locksOnTuckF ? y
                                     : ((knownTargetSurfY >= MIN_PIECE_Y)
                                            ? knownTargetSurfY
                                            : bpCache.dropY(board, pieceIdx, newRot, newX, postY));
  const int postStartF = tuckF + 1;

  int dasAfter = 0;
  uint8_t mvFlags = MOVE_FLAG_TUCK;

  if (ti.xChange == 0) {
    dasAfter = tr.spinDasBase;
    if (!locksOnTuckF) {
      const int tuckLockF = computeLockFrame(postStartF, postY, tuckLockY, cfg.gravity);
      const int postDpadReadyF = std::max(postStartF, static_cast<int>(tr.dpadReadyFrame));
      if (dasAfter < DAS_MAX_CHARGE && postDpadReadyF <= tuckLockF) {
        const int yWall = postY + countGravityDrops(postStartF, postDpadReadyF, cfg.gravity);
        if (bpCache.canWallChargeAny(board, pieceIdx, newRot, newX, yWall, tuckLockY)) {
          dasAfter = DAS_MAX_CHARGE;
          mvFlags |= MOVE_FLAG_WALL_CHARGE;
        }
      }
    }
  } else {
    const int wcMinPrevF =
        std::max(static_cast<int>(tr.dpadReadyFrame),
                 static_cast<int>(tr.firstFallFrame) + ((tr.initHeldDir == ti.xChange) ? 1 : 0));
    const bool canWallChargeTuck = (board.numHoles > 0)
                                   && (y > MIN_PIECE_Y)
                                   && (tuckF > static_cast<int>(tr.firstFallFrame))
                                   && (tuckF > start.frame)
                                   && (tuckF - 1 >= wcMinPrevF)
                                   && detail::isGravityFrame(tuckF - 1, cfg.gravity)
                                   && collides20(board, pieceIdx, newX, y - 1, preRot);

    if (y == baseY && !canWallChargeTuck) {
      return false;
    }

    if (canWallChargeTuck) {
      dasAfter = DAS_POST_SHIFT_CHARGE;
      mvFlags |= MOVE_FLAG_WALL_CHARGE;
      if (!locksOnTuckF) {
        const int tuckLockF = computeLockFrame(postStartF, postY, tuckLockY, cfg.gravity);
        const int remFrames = tuckLockF - postStartF + 1;
        dasAfter = std::min(DAS_FULL_THRESHOLD, dasAfter + remFrames);
        const int holdUsed = dasAfter - DAS_POST_SHIFT_CHARGE;
        const int postHoldF = postStartF + holdUsed;
        const int wcFreshF = (tuckF - 1) + cfg.dpadGapFrames + 1;
        const bool heldWc =
            (postHoldF <= tuckLockF)
            && wallChargeHitsRowDir(board, pieceIdx, newRot, newX,
                                    postY + countGravityDrops(postStartF, postHoldF, cfg.gravity),
                                    ti.xChange);
        if (heldWc
            || canFreshWallChargeFrom(board, bpCache, pieceIdx, newRot, newX, postY, postStartF,
                                      tuckLockY, tuckLockF, postStartF, wcFreshF, ti.xChange,
                                      cfg.gravity)) {
          dasAfter = DAS_MAX_CHARGE;
        }
      }
    } else {
      dasAfter = 0;
      if (!locksOnTuckF) {
        const int tuckLockF = computeLockFrame(postStartF, postY, tuckLockY, cfg.gravity);
        const int wcReadyF = tuckF + cfg.dpadGapFrames + 1;
        if (canFreshWallChargeFrom(board, bpCache, pieceIdx, newRot, newX, postY, postStartF,
                                   tuckLockY, tuckLockF, postStartF, wcReadyF, ti.xChange,
                                   cfg.gravity)) {
          dasAfter = DAS_MAX_CHARGE;
          mvFlags |= MOVE_FLAG_WALL_CHARGE;
        }
      }
    }
  }

  table.recordMove(newRot, newX, tuckLockY, static_cast<uint8_t>(dasAfter), mvFlags, ti.notation,
                   out);
  return true;
}

// Phase C: Pure real overhang tucks & spins (depth 1).
inline void runPhaseCOverhangTucks(const Board20 &board, const BoardPieceCache &bpCache,
                                   int pieceIdx, const DasStartState &start,
                                   const MoveSearchConfig &cfg, const TuckColumnTable &tuckCols,
                                   LockMoveTable &table, DasMoveList &out) noexcept {
  const PieceDef20 &piece = PIECES_20[pieceIdx];
  const int numOrients = piece.numOrientations;
  const int rotMask = numOrients - 1;
  static constexpr int INPUT_COUNT_BY_ORIENT[5] = {0, 2, 5, 0, 8};
  static constexpr int INPUT_IDX_2[5] = {0, 1, 2, 4, 5};
  const int nInputs = INPUT_COUNT_BY_ORIENT[numOrients];

  auto computeReadyFrame = [](const TuckColumnTrack &tr, const TuckInput20 &ti) noexcept -> int {
    const int dpadMinF = (ti.xChange != 0)
                             ? std::max(static_cast<int>(tr.tuckDpadReadyFrame),
                                        static_cast<int>(tr.firstFallFrame)
                                            + ((tr.initHeldDir == ti.xChange) ? 1 : 0))
                             : 0;
    return std::max(static_cast<int>(tr.firstFallFrame),
                    std::max(dpadMinF, ti.rotChange != 0 ? static_cast<int>(tr.rotReadyFrame) : 0));
  };

  // Fast 0-hole path: ONLY unreached (newRot, newX) cells can receive a depth-1 tuck/spin.
  if (board.numHoles == 0) {
    for (int newRot = 0; newRot < numOrients; ++newRot) {
      uint16_t unreached =
          static_cast<uint16_t>(validXBitsForRot(piece, newRot) & ~table.basicFoundBits[newRot]);
      while (unreached != 0U) {
        const int newXi = __builtin_ctz(static_cast<unsigned>(unreached));
        unreached &= static_cast<uint16_t>(unreached - 1U);
        const int newX = newXi - LockMoveTable::X_OFF;
        const int targetSurfY = bpCache.surfLockY[newRot][newXi];
        if (targetSurfY < static_cast<int>(start.y)) {
          continue;
        }

        for (int tiIdx = 0; tiIdx < nInputs; ++tiIdx) {
          const TuckInput20 &ti = TUCK_INPUTS_20[(numOrients == 2) ? INPUT_IDX_2[tiIdx] : tiIdx];
          const int preX = newX - ti.xChange;
          const int preRot = (numOrients == 1) ? newRot : ((newRot - ti.rotChange + 4) & rotMask);
          if (!inPieceXBounds(piece, preRot, preX)) {
            continue;
          }
          const int preXi = preX + TuckColumnTable::X_OFF;
          const int cnt = tuckCols.counts[preRot][preXi];
          if (cnt == 0) {
            continue;
          }

          for (int eIdx = 0; eIdx < cnt; ++eIdx) {
            const TuckColumnTrack &tr = tuckCols.entries[preRot][preXi][eIdx];
            const int lockFrame =
                computeLockFrame(tr.firstFallFrame, tr.yStart, tr.lockY, cfg.gravity);
            const int readyFrame = computeReadyFrame(tr, ti);
            if (readyFrame > lockFrame) {
              continue;
            }
            const int minReadyY =
                tr.yStart + countGravityDrops(tr.firstFallFrame, readyFrame, cfg.gravity);
            const int maxValidY = std::min<int>(tr.lockY, targetSurfY);
            for (int y = minReadyY; y <= maxValidY; ++y) {
              if (ti.xChange != 0 && collides20(board, pieceIdx, newX, y, preRot)) {
                break;
              }
              if (ti.rotChange != 0 && collides20(board, pieceIdx, newX, y, newRot)) {
                break;
              }
              if (evalPhaseCTuckRow(board, bpCache, pieceIdx, start, tr, ti, preRot, newRot, newX,
                                    y, INVALID_ROW_Y, readyFrame, targetSurfY, cfg, table, out)) {
                break; // earliest legal y maximizes post-tuck fall frames on a 0-hole board
              }
            }
          }
        }
      }
    }
    return;
  }

  // Holey-board path: uses precomputed underSurfEmptyMask bitmasks to skip solid stack rows.
  for (int preRot = 0; preRot < numOrients; ++preRot) {
    const int loX = piece.minX[preRot];
    const int hiX = pieceMaxX(piece, preRot);
    for (int preX = loX; preX <= hiX; ++preX) {
      const int xi = preX + TuckColumnTable::X_OFF;
      const int cnt = tuckCols.counts[preRot][xi];
      if (cnt == 0) {
        continue;
      }

      for (int tiIdx = 0; tiIdx < nInputs; ++tiIdx) {
        const TuckInput20 &ti = TUCK_INPUTS_20[(numOrients == 2) ? INPUT_IDX_2[tiIdx] : tiIdx];
        const int newX = preX + ti.xChange;
        const int newRot = (numOrients == 1) ? preRot : ((preRot + ti.rotChange + 4) & rotMask);
        if (!inPieceXBounds(piece, newRot, newX)) {
          continue;
        }
        const int newXi = newX + LockMoveTable::X_OFF;
        const int baseY = table.basicLockY[newRot][newXi];
        const int minTargetY = (baseY >= MIN_PIECE_Y) ? baseY : MIN_PIECE_Y;
        const uint32_t underMask =
            (baseY >= MIN_PIECE_Y) ? bpCache.underSurfEmptyMask[newRot][newXi] : 0U;

        for (int eIdx = 0; eIdx < cnt; ++eIdx) {
          const TuckColumnTrack &tr = tuckCols.entries[preRot][xi][eIdx];
          if (tr.lockY < minTargetY) {
            continue;
          }
          const int lockFrame =
              computeLockFrame(tr.firstFallFrame, tr.yStart, tr.lockY, cfg.gravity);
          const int readyFrame = computeReadyFrame(tr, ti);
          if (readyFrame > lockFrame) {
            continue;
          }
          const int minReadyY =
              tr.yStart + countGravityDrops(tr.firstFallFrame, readyFrame, cfg.gravity);
          const int scanLoY = std::max(minReadyY, minTargetY);
          if (scanLoY > tr.lockY) {
            continue;
          }
          assert(scanLoY >= MIN_PIECE_Y && tr.lockY <= MAX_PIECE_Y);

          const uint32_t loMask = ~((1U << (scanLoY + Y_ROW_OFFSET)) - 1U);
          const uint32_t hiMask = (tr.lockY + Y_ROW_OFFSET + 1 < 32)
                                      ? ((1U << (tr.lockY + Y_ROW_OFFSET + 1)) - 1U)
                                      : 0xFFFFFFFFU;
          uint32_t yBits = loMask & hiMask;
          if (baseY >= MIN_PIECE_Y) {
            uint32_t cand = underMask;
            if (baseY >= scanLoY && baseY <= tr.lockY) {
              cand |= (1U << (baseY + Y_ROW_OFFSET));
            }
            yBits &= cand;
            if (yBits == 0U) {
              continue;
            }
          }

          while (yBits != 0U) {
            const int y = __builtin_ctz(yBits) - Y_ROW_OFFSET;
            yBits &= (yBits - 1U);
            if (y == baseY) {
              const bool overhangAbove =
                  (y > MIN_PIECE_Y)
                  && ((ti.xChange != 0) ? collides20(board, pieceIdx, newX, y - 1, preRot)
                                        : collides20(board, pieceIdx, newX, y - 1, newRot));
              if (!overhangAbove) {
                continue;
              }
            }
            if (ti.xChange != 0 && collides20(board, pieceIdx, newX, y, preRot)) {
              continue;
            }
            if (ti.rotChange != 0 && collides20(board, pieceIdx, newX, y, newRot)) {
              continue;
            }
            evalPhaseCTuckRow(board, bpCache, pieceIdx, start, tr, ti, preRot, newRot, newX, y,
                              baseY, readyFrame, INVALID_ROW_Y, cfg, table, out);
          }
        }
      }
    }
  }
}

// Flat open-addressed lookup table for immutable adjPool keys (zero node chasing, ~2 ns lookup).
struct FlatPoolLookup {
  struct Slot {
    uint64_t key = UINT64_MAX;
    int32_t idx = -1;
  };
  std::vector<Slot> slots;
  size_t mask = 0;

  void build(const std::vector<DasStartState> &pool) {
    size_t cap = 16;
    while (cap < pool.size() * 4) {
      cap <<= 1;
    }
    slots.assign(cap, Slot{});
    mask = cap - 1;
    for (size_t i = 0; i < pool.size(); ++i) {
      const uint64_t k = pool[i].packKey();
      size_t pos = static_cast<size_t>(detail::splitmix64Mix(k)) & mask;
      while (slots[pos].key != UINT64_MAX) {
        pos = (pos + 1) & mask;
      }
      slots[pos] = {k, static_cast<int32_t>(i)};
    }
  }

  int find(uint64_t key) const noexcept {
    if (slots.empty()) {
      return -1;
    }
    size_t pos = static_cast<size_t>(detail::splitmix64Mix(key)) & mask;
    while (true) {
      const Slot &s = slots[pos];
      if (s.key == key) {
        return s.idx;
      }
      if (s.key == UINT64_MAX) {
        return -1;
      }
      pos = (pos + 1) & mask;
    }
  }
};

} // namespace rt_detail

// Reusable per-thread scratch workspace for DasRtMoveTables::searchRt / searchRtMultiDas.
// Keeps hot working memory off macOS 512 KB std::thread stacks and avoids per-call initOnce()
// zeroing of LockMoveTable / cellMap / poolToSlot across consecutive solver queries.
struct alignas(64) RtSearchScratch {
  static constexpr int MAX_CACHED_PHIS = DAS_MAX_ADJ_STATES * 3;
  static constexpr int MAX_CACHED_MOVES = 6144;
  static constexpr int MAX_POOLED_PHIS = 2048;

  rt_detail::LockMoveTable moveTable;
  rt_detail::TuckColumnTable tuckCols;
  DasMoveList scratchMl;
  uint16_t cellMap[rt_detail::LockMoveTable::TOTAL];
  int16_t poolToSlot[MAX_POOLED_PHIS];
  int cachedPoolIdx[MAX_CACHED_PHIS];
  uint16_t cachedOff[MAX_CACHED_PHIS];
  uint16_t cachedCnt[MAX_CACHED_PHIS];
  DasLockMove cachedBuf[MAX_CACHED_MOVES];

  RtSearchScratch() noexcept {
    moveTable.initOnce();
    std::memset(cellMap, 0, sizeof(cellMap));
    std::memset(poolToSlot, -1, sizeof(poolToSlot));
  }
};

// Precomputed SoA swept-path & empty-board frame-RT tables for a fixed MoveSearchConfig.
// Immutable after construction -> 100% thread-safe for concurrent solver/eval queries.
class DasRtMoveTables {
public:
  // Exact 17 classes covering every valid starting dasCharge in [0..16] without losing
  // stationary (dx == 0) charge fidelity for 0..4 or 16.
  static constexpr int NUM_DAS_CLASSES = DAS_MAX_CHARGE + 1;

  static int dasToClass(int dasCharge) noexcept { return std::clamp(dasCharge, 0, DAS_MAX_CHARGE); }

  explicit DasRtMoveTables(const MoveSearchConfig &cfg = MoveSearchConfig()) : cfg_(cfg) {
    if (!cfg_.isValid()) {
      std::fprintf(stderr,
                   "FATAL: Invalid MoveSearchConfig (g=%d rt=%d dpadGap=%d rotGap=%d qt=%d)\n",
                   cfg_.gravity, cfg_.reactionTime, cfg_.dpadGapFrames, cfg_.rotGapFrames,
                   cfg_.quicktapDelayFrames);
      std::abort();
    }
    buildAll();
  }

  const MoveSearchConfig &config() const noexcept { return cfg_; }

  void searchMoves(const Board20 &board, int pieceIdx, const DasStartState &start,
                   DasMoveList &out) const noexcept {
    out.clear();
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    if (collides20(board, pieceIdx, start.x, start.y, start.rot)) {
      return;
    }

    uint64_t b5[5];
    rt_detail::loadBoard5(board, b5);
    rt_detail::BoardPieceCache bpCache;
    bpCache.build(board, pieceIdx);

    rt_detail::LockMoveTable table;
    table.initOnce();
    rt_detail::TuckColumnTable tuckCols;

    const rt_detail::WalkMeta *metas = nullptr;
    const rt_detail::BoardMask5 *masks = nullptr;
    int walkCount = 0;
    const PreSchedQuad *preScheds = nullptr;
    bool allCovered = false;
    int maxWalkRow = -1;

    if (!start.isAdjustment
        && start.x == SPAWN_COL
        && start.y == PIECES_20[pieceIdx].initialY
        && start.rot == 0
        && start.frame == 0
        && start.heldDir == 0
        && start.dpadCooldown == 0
        && start.rotCooldown == 0
        && start.shiftCooldown == 0) {
      const int cls = dasToClass(start.dasCharge);
      metas = spawnWalkMetas_[pieceIdx][cls].data();
      masks = spawnWalkMasks_[pieceIdx][cls].data();
      walkCount = static_cast<int>(spawnWalkMetas_[pieceIdx][cls].size());
      preScheds = &spawnPreScheds_[pieceIdx][cls];
      allCovered = spawnAllCellsCovered_[pieceIdx][cls];
      maxWalkRow = spawnMaxWalkRow_[pieceIdx][cls];
    } else if (start.isAdjustment) {
      const int poolIdx = adjPoolLookup_[pieceIdx].find(start.packKey());
      if (poolIdx >= 0) {
        const size_t lo = adjWalkOffset_[pieceIdx][static_cast<size_t>(poolIdx)];
        const size_t hi = adjWalkOffset_[pieceIdx][static_cast<size_t>(poolIdx) + 1];
        metas = adjWalkMetaData_[pieceIdx].data() + lo;
        masks = adjWalkMaskData_[pieceIdx].data() + lo;
        walkCount = static_cast<int>(hi - lo);
        preScheds = &adjPreScheds_[pieceIdx][static_cast<size_t>(poolIdx)];
        allCovered = adjAllCellsCovered_[pieceIdx][static_cast<size_t>(poolIdx)] != 0U;
        maxWalkRow = adjMaxWalkRow_[pieceIdx][static_cast<size_t>(poolIdx)];
      }
    }

    searchStateInternal(board, bpCache, b5, pieceIdx, start, metas, masks, walkCount, preScheds,
                        allCovered, maxWalkRow, table, tuckCols, out);
  }

  void searchRt(const Board20 &board, int pieceIdx, int startDas, DasRtSearchResult &res,
                RtSearchScratch *scratch = nullptr) const noexcept {
    searchRtMultiDas(board, pieceIdx, &startDas, 1, &res, scratch);
  }

  // Batch 2-half reaction-time search across `numBuckets` starting DAS values (e.g. {5, 10, 15})
  // on one (board, pieceIdx). Shares BoardPieceCache, b5, and O(1)-cached second-half adjustment
  // move lists across buckets while populating each outResults[b] with its own independent
  // uniqueCells[] and CSR tables (identical to calling searchRt per bucket).
  void searchRtMultiDas(const Board20 &board, int pieceIdx, const int *startDasValues,
                        int numBuckets, DasRtSearchResult *outResults,
                        RtSearchScratch *externalScratch = nullptr) const noexcept {
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    assert(numBuckets >= 1 && startDasValues != nullptr && outResults != nullptr);
    for (int b = 0; b < numBuckets; ++b) {
      outResults[b].clear();
    }
    const DasStartState baseSpawn = DasStartState::spawn(pieceIdx, startDasValues[0]);
    if (collides20(board, pieceIdx, baseSpawn.x, baseSpawn.y, baseSpawn.rot)) {
      return;
    }

    if (cfg_.reactionTime <= 0) {
      DasMoveList ml;
      for (int b = 0; b < numBuckets; ++b) {
        const DasStartState sp = DasStartState::spawn(pieceIdx, startDasValues[b]);
        searchMoves(board, pieceIdx, sp, ml);
        DasRtSearchResult &res = outResults[b];
        res.adjStates[0] = sp;
        res.numAdjStates = 1;
        res.adjMoveOffset[0] = 0;
        for (int i = 0; i < ml.count; ++i) {
          const DasLockMove &m = ml.data[i];
          const uint8_t cIdx = static_cast<uint8_t>(res.numUniqueCells);
          res.uniqueCells[res.numUniqueCells++] = {m.x, m.y, m.rot, m.tuckInput};
          res.adjEdges[res.numAdjEdges++] = {cIdx, m.dasChargeAfter};
        }
        res.adjMoveOffset[1] = static_cast<uint16_t>(res.numAdjEdges);
      }
      return;
    }

    uint64_t b5[5];
    rt_detail::loadBoard5(board, b5);
    rt_detail::BoardPieceCache bpCache;
    bpCache.build(board, pieceIdx);

    // Use caller-provided thread-local scratch when available; otherwise allocate a local one.
    // Note: when numBuckets == 1, CSR cache is bypassed completely.
    RtSearchScratch localScratchHolder;
    RtSearchScratch &sc = (externalScratch != nullptr) ? *externalScratch : localScratchHolder;

    int numCached = 0;
    int cachedBufUsed = 0;
    const int boardTopRow = static_cast<int>(board.topNonEmptyRow);
    const int emptyRtMaxRow = maxEmptyRtBottomRow_[pieceIdx];

    auto getOrComputeAdjMoves = [&](int bucketIdx, const DasStartState &phi, int poolIdx,
                                    const DasLockMove *&outPtr, int &outCnt) {
      const bool canUsePoolSlot = (poolIdx >= 0 && poolIdx < RtSearchScratch::MAX_POOLED_PHIS);
      // Probe O(1) cache only for b > 0 (b == 0 phis are already unique).
      if (bucketIdx > 0 && canUsePoolSlot) {
        const int16_t slot = sc.poolToSlot[poolIdx];
        if (slot >= 0) {
          outPtr = &sc.cachedBuf[sc.cachedOff[slot]];
          outCnt = sc.cachedCnt[slot];
          return;
        }
      }

      sc.scratchMl.clear();
      if (!collides20(board, pieceIdx, phi.x, phi.y, phi.rot)) {
        const rt_detail::WalkMeta *metas = nullptr;
        const rt_detail::BoardMask5 *masks = nullptr;
        int walkCount = 0;
        const PreSchedQuad *preScheds = nullptr;
        bool allCov = false;
        int maxWRow = -1;
        if (poolIdx >= 0) {
          const size_t lo = adjWalkOffset_[pieceIdx][static_cast<size_t>(poolIdx)];
          const size_t hi = adjWalkOffset_[pieceIdx][static_cast<size_t>(poolIdx) + 1];
          metas = adjWalkMetaData_[pieceIdx].data() + lo;
          masks = adjWalkMaskData_[pieceIdx].data() + lo;
          walkCount = static_cast<int>(hi - lo);
          preScheds = &adjPreScheds_[pieceIdx][static_cast<size_t>(poolIdx)];
          allCov = adjAllCellsCovered_[pieceIdx][static_cast<size_t>(poolIdx)] != 0U;
          maxWRow = adjMaxWalkRow_[pieceIdx][static_cast<size_t>(poolIdx)];
        }
        searchStateInternal(board, bpCache, b5, pieceIdx, phi, metas, masks, walkCount, preScheds,
                            allCov, maxWRow, sc.moveTable, sc.tuckCols, sc.scratchMl);
      }

      // Store in CSR cache only when a subsequent bucket may reuse it (zero overhead for last
      // bucket and for single-DAS searchRt calls!).
      if (bucketIdx + 1 < numBuckets
          && canUsePoolSlot
          && numCached < RtSearchScratch::MAX_CACHED_PHIS
          && cachedBufUsed + sc.scratchMl.count <= RtSearchScratch::MAX_CACHED_MOVES) {
        const uint16_t off = static_cast<uint16_t>(cachedBufUsed);
        if (sc.scratchMl.count > 0) {
          std::memcpy(&sc.cachedBuf[off], sc.scratchMl.data,
                      static_cast<size_t>(sc.scratchMl.count) * sizeof(DasLockMove));
        }
        sc.poolToSlot[poolIdx] = static_cast<int16_t>(numCached);
        sc.cachedPoolIdx[numCached] = poolIdx;
        sc.cachedOff[numCached] = off;
        sc.cachedCnt[numCached] = static_cast<uint16_t>(sc.scratchMl.count);
        numCached++;
        cachedBufUsed += sc.scratchMl.count;
        outPtr = &sc.cachedBuf[off];
        outCnt = sc.scratchMl.count;
        return;
      }
      outPtr = sc.scratchMl.data;
      outCnt = sc.scratchMl.count;
    };

    for (int b = 0; b < numBuckets; ++b) {
      DasRtSearchResult &res = outResults[b];
      const int startDas = startDasValues[b];
      const DasStartState spawnSt = DasStartState::spawn(pieceIdx, startDas);
      const int dasCls = dasToClass(startDas);

      auto internCell = [&](int rot, int x, int y, char tuckChar) -> uint16_t {
        const int idx = rt_detail::LockMoveTable::cellIndex(rot, x, y);
        uint16_t v = sc.cellMap[idx];
        if (v == 0U) {
          assert(res.numUniqueCells < DAS_MAX_LOCK_MOVES);
          if (res.numUniqueCells >= DAS_MAX_LOCK_MOVES) {
            return INVALID_CELL_IDX;
          }
          res.uniqueCells[res.numUniqueCells] = {static_cast<int8_t>(x), static_cast<int8_t>(y),
                                                 static_cast<uint8_t>(rot), tuckChar};
          res.numUniqueCells++;
          v = static_cast<uint16_t>(res.numUniqueCells);
          sc.cellMap[idx] = v;
        } else if (tuckChar == '.') {
          res.uniqueCells[v - 1].tuckInput = '.';
        }
        return static_cast<uint16_t>(v - 1);
      };

      int adjPoolIndices[DAS_MAX_ADJ_STATES];
      if (boardTopRow > emptyRtMaxRow) {
        const auto &prePhi = spawnEmptyRtAdjIndices_[pieceIdx][dasCls];
        for (int idx : prePhi) {
          if (res.numAdjStates >= DAS_MAX_ADJ_STATES) {
            break;
          }
          adjPoolIndices[res.numAdjStates] = idx;
          res.adjStates[res.numAdjStates] = adjPool_[pieceIdx][static_cast<size_t>(idx)];
          res.numAdjStates++;
        }
      } else {
        collectHighBoardSpawnRt(board, bpCache, b5, pieceIdx, spawnSt, dasCls, internCell,
                                adjPoolIndices, sc.moveTable, sc.tuckCols, sc.scratchMl, res);
      }

      for (int pIdx = 0; pIdx < res.numAdjStates; ++pIdx) {
        res.adjMoveOffset[pIdx] = static_cast<uint16_t>(res.numAdjEdges);
        const DasLockMove *mlPtr = nullptr;
        int mlCnt = 0;
        getOrComputeAdjMoves(b, res.adjStates[pIdx], adjPoolIndices[pIdx], mlPtr, mlCnt);
        for (int i = 0; i < mlCnt; ++i) {
          assert(res.numAdjEdges < DasRtSearchResult::MAX_ADJ_EDGES);
          if (res.numAdjEdges >= DasRtSearchResult::MAX_ADJ_EDGES) {
            break;
          }
          const DasLockMove &m = mlPtr[i];
          const uint16_t cIdx = internCell(m.rot, m.x, m.y, m.tuckInput);
          if (cIdx == INVALID_CELL_IDX) {
            continue;
          }
          res.adjEdges[res.numAdjEdges++] = {static_cast<uint8_t>(cIdx), m.dasChargeAfter};
        }
      }
      res.adjMoveOffset[res.numAdjStates] = static_cast<uint16_t>(res.numAdjEdges);

      // Sparse-reset cellMap (only ~25-40 stores instead of 2,112 B memset!).
      for (int i = 0; i < res.numUniqueCells; ++i) {
        const CompactPlacement &c = res.uniqueCells[i];
        sc.cellMap[rt_detail::LockMoveTable::cellIndex(c.rot, c.x, c.y)] = 0;
      }
    }

    // Sparse-reset O(1) poolToSlot lookup before returning.
    for (int i = 0; i < numCached; ++i) {
      sc.poolToSlot[sc.cachedPoolIdx[i]] = -1;
    }
  }

  size_t totalPrecomputedWalks() const noexcept {
    size_t n = 0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      for (int c = 0; c < NUM_DAS_CLASSES; ++c) {
        n += spawnWalkMetas_[p][c].size();
      }
      n += adjWalkMetaData_[p].size();
    }
    return n;
  }

  size_t totalPooledAdjStates() const noexcept {
    size_t n = 0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      n += adjPool_[p].size();
    }
    return n;
  }

  size_t memoryFootprintBytes() const noexcept {
    size_t bytes = sizeof(*this);
    for (int p = 0; p < NUM_PIECES; ++p) {
      for (int c = 0; c < NUM_DAS_CLASSES; ++c) {
        bytes += spawnWalkMetas_[p][c].capacity() * sizeof(rt_detail::WalkMeta);
        bytes += spawnWalkMasks_[p][c].capacity() * sizeof(rt_detail::BoardMask5);
        bytes += spawnRtMetas_[p][c].capacity() * sizeof(rt_detail::SpawnRtMeta);
        bytes += spawnEmptyRtAdjIndices_[p][c].capacity() * sizeof(int);
      }
      bytes += adjPool_[p].capacity() * sizeof(DasStartState);
      bytes += adjWalkMetaData_[p].capacity() * sizeof(rt_detail::WalkMeta);
      bytes += adjWalkMaskData_[p].capacity() * sizeof(rt_detail::BoardMask5);
      bytes += adjWalkOffset_[p].capacity() * sizeof(uint32_t);
      bytes += adjPreScheds_[p].capacity() * sizeof(PreSchedQuad);
      bytes += adjAllCellsCovered_[p].capacity() * sizeof(uint8_t);
      bytes += adjMaxWalkRow_[p].capacity() * sizeof(int8_t);
      bytes += adjPoolLookup_[p].slots.capacity() * sizeof(rt_detail::FlatPoolLookup::Slot);
    }
    return bytes;
  }

private:
  struct PreSchedQuad {
    rt_detail::ShiftSchedule s[2][2]; // [dir < 0 ? 0 : 1][trackIdx: 0=Das, 1=SlowTap]
    const rt_detail::ShiftSchedule &get(int dir, rt_detail::TrackKind tk) const noexcept {
      return s[dir < 0 ? 0 : 1][static_cast<int>(tk)];
    }
  };

  MoveSearchConfig cfg_;
  std::vector<rt_detail::WalkMeta> spawnWalkMetas_[NUM_PIECES][NUM_DAS_CLASSES];
  std::vector<rt_detail::BoardMask5> spawnWalkMasks_[NUM_PIECES][NUM_DAS_CLASSES];
  std::vector<rt_detail::SpawnRtMeta> spawnRtMetas_[NUM_PIECES][NUM_DAS_CLASSES];
  PreSchedQuad spawnPreScheds_[NUM_PIECES][NUM_DAS_CLASSES];
  bool spawnAllCellsCovered_[NUM_PIECES][NUM_DAS_CLASSES] = {};
  int8_t spawnMaxWalkRow_[NUM_PIECES][NUM_DAS_CLASSES] = {};
  std::vector<int> spawnEmptyRtAdjIndices_[NUM_PIECES][NUM_DAS_CLASSES];

  // Flat contiguous SoA CSR tables for pooled frame-RT adjustment states.
  std::vector<DasStartState> adjPool_[NUM_PIECES];
  std::vector<rt_detail::WalkMeta> adjWalkMetaData_[NUM_PIECES];
  std::vector<rt_detail::BoardMask5> adjWalkMaskData_[NUM_PIECES];
  std::vector<uint32_t> adjWalkOffset_[NUM_PIECES];
  std::vector<PreSchedQuad> adjPreScheds_[NUM_PIECES];
  std::vector<uint8_t> adjAllCellsCovered_[NUM_PIECES];
  std::vector<int8_t> adjMaxWalkRow_[NUM_PIECES];
  rt_detail::FlatPoolLookup adjPoolLookup_[NUM_PIECES];
  int maxEmptyRtBottomRow_[NUM_PIECES] = {};

  void buildPreSchedQuad(const DasStartState &st, PreSchedQuad &q) const noexcept {
    for (int di = 0; di < 2; ++di) {
      const int dir = (di == 0) ? -1 : 1;
      for (int ti = 0; ti < 2; ++ti) {
        q.s[di][ti] = {};
        for (int k = DAS_MAX_SHIFTS; k >= 1; --k) {
          if (rt_detail::buildShiftSchedule(st, dir * k, static_cast<rt_detail::TrackKind>(ti),
                                            cfg_, q.s[di][ti])) {
            break;
          }
        }
      }
    }
  }

  static int maxWalkRowOf(const rt_detail::WalkMeta *metas, int count) noexcept {
    int m = -1;
    for (int i = 0; i < count; ++i) {
      if (metas[i].maxArriveRow > m) {
        m = metas[i].maxArriveRow;
      }
    }
    return m;
  }

  static bool checkAllCellsCovered(int pieceIdx, const rt_detail::WalkMeta *metas,
                                   int count) noexcept {
    uint16_t cov[4] = {};
    for (int i = 0; i < count; ++i) {
      cov[metas[i].goalRot] |=
          static_cast<uint16_t>(1U << (metas[i].goalX + rt_detail::LockMoveTable::X_OFF));
    }
    const PieceDef20 &piece = PIECES_20[pieceIdx];
    for (int rot = 0; rot < piece.numOrientations; ++rot) {
      const uint16_t valid = rt_detail::validXBitsForRot(piece, rot);
      if ((cov[rot] & valid) != valid) {
        return false;
      }
    }
    return true;
  }

  static bool hasUnreachedBasicCell(int pieceIdx, const rt_detail::LockMoveTable &table) noexcept {
    const PieceDef20 &piece = PIECES_20[pieceIdx];
    for (int rot = 0; rot < piece.numOrientations; ++rot) {
      const uint16_t valid = rt_detail::validXBitsForRot(piece, rot);
      if ((table.basicFoundBits[rot] & valid) != valid) {
        return true;
      }
    }
    return false;
  }

  void searchStateInternal(const Board20 &board, const rt_detail::BoardPieceCache &bpCache,
                           const uint64_t b5[5], int pieceIdx, const DasStartState &start,
                           const rt_detail::WalkMeta *metas, const rt_detail::BoardMask5 *masks,
                           int walkCount, const PreSchedQuad *preScheds, bool allCovered,
                           int maxWalkRow, rt_detail::LockMoveTable &table,
                           rt_detail::TuckColumnTable &tuckCols, DasMoveList &out) const noexcept {
    const int boardTopRow = static_cast<int>(board.topNonEmptyRow);
    if (metas != nullptr) {
      const bool mayNeedTucks =
          cfg_.enableTucks && (board.numHoles > 0 || !allCovered || boardTopRow <= maxWalkRow);
      table.sparseReset(mayNeedTucks);
      if (mayNeedTucks) {
        tuckCols.reset();
        const bool anyCollided =
            runFromPrecomputedWalks<true>(board, bpCache, b5, pieceIdx, start, metas, masks,
                                          walkCount, maxWalkRow, preScheds, table, tuckCols, out);
        if (board.numHoles > 0 || anyCollided || !allCovered) {
          if (board.numHoles > 0 || hasUnreachedBasicCell(pieceIdx, table)) {
            rt_detail::runPhaseCOverhangTucks(board, bpCache, pieceIdx, start, cfg_, tuckCols,
                                              table, out);
          }
        }
      } else {
        runFromPrecomputedWalks<false>(board, bpCache, b5, pieceIdx, start, metas, masks, walkCount,
                                       maxWalkRow, preScheds, table, tuckCols, out);
      }
    } else {
      const bool collectTucks = cfg_.enableTucks;
      table.sparseReset(collectTucks);
      if (collectTucks) {
        tuckCols.reset();
        runDynamic<true>(board, bpCache, pieceIdx, start, table, tuckCols, out);
        if (board.numHoles > 0 || hasUnreachedBasicCell(pieceIdx, table)) {
          rt_detail::runPhaseCOverhangTucks(board, bpCache, pieceIdx, start, cfg_, tuckCols, table,
                                            out);
        }
      } else {
        runDynamic<false>(board, bpCache, pieceIdx, start, table, tuckCols, out);
      }
    }
  }

  template <typename InternFn>
  void collectHighBoardSpawnRt(const Board20 &board, const rt_detail::BoardPieceCache &bpCache,
                               const uint64_t b5[5], int pieceIdx, const DasStartState &spawnSt,
                               int dasCls, InternFn &&internCell,
                               int adjPoolIndices[DAS_MAX_ADJ_STATES],
                               rt_detail::LockMoveTable &earlyTable,
                               rt_detail::TuckColumnTable &earlyTucks, DasMoveList &earlyMl,
                               DasRtSearchResult &res) const noexcept {
    // O(1) bitset dedup for pooled phis + compact key array only for rare unpooled dynamic phis.
    constexpr int MAX_POOL_BITS = 2048;
    uint64_t seenPoolBits[MAX_POOL_BITS / 64] = {};
    uint64_t seenDynKeys[DAS_MAX_ADJ_STATES];
    int numDynSeen = 0;

    auto addAdjState = [&](const DasStartState &st, int knownPoolIdx) {
      int pIdx = knownPoolIdx;
      if (pIdx < 0) {
        pIdx = adjPoolLookup_[pieceIdx].find(st.packKey());
      }
      if (pIdx >= 0 && pIdx < MAX_POOL_BITS) {
        const uint64_t bit = 1ULL << (pIdx & 63);
        if ((seenPoolBits[pIdx >> 6] & bit) != 0ULL) {
          return;
        }
        seenPoolBits[pIdx >> 6] |= bit;
      } else {
        const uint64_t k = st.packKey();
        for (int i = 0; i < numDynSeen; ++i) {
          if (seenDynKeys[i] == k) {
            return;
          }
        }
        if (numDynSeen < DAS_MAX_ADJ_STATES) {
          seenDynKeys[numDynSeen++] = k;
        }
      }
      if (res.numAdjStates < DAS_MAX_ADJ_STATES) {
        adjPoolIndices[res.numAdjStates] = pIdx;
        res.adjStates[res.numAdjStates] = st;
        res.numAdjStates++;
      }
    };

    const bool collectEarlyTucks = cfg_.enableTucks;
    earlyTable.sparseReset(collectEarlyTucks);
    if (collectEarlyTucks) {
      earlyTucks.reset();
    }
    earlyMl.clear();

    const auto &metas = spawnWalkMetas_[pieceIdx][dasCls];
    const auto &masks = spawnWalkMasks_[pieceIdx][dasCls];
    const auto &rtMetas = spawnRtMetas_[pieceIdx][dasCls];
    const PreSchedQuad &preScheds = spawnPreScheds_[pieceIdx][dasCls];
    uint8_t rtCollided[3][3][DAS_MAX_SHIFTS + 1] = {};
    uint8_t arrCollided[3][3][DAS_MAX_SHIFTS + 1] = {};
    const int boardTopRow = static_cast<int>(board.topNonEmptyRow);

    for (size_t wi = 0; wi < metas.size(); ++wi) {
      const rt_detail::WalkMeta &w = metas[wi];
      const rt_detail::SpawnRtMeta &rm = rtMetas[wi];
      const int dIdx = w.dir + 1;
      const int tIdx = rt_detail::trackFlagsToIndex(w.trackFlags);
      if (rm.maxRtRow < boardTopRow || !rm.rtMask.intersects(b5)) {
        if (rm.hasRtCapture && rm.rtAdjPoolIdx >= 0) {
          addAdjState(adjPool_[pieceIdx][static_cast<size_t>(rm.rtAdjPoolIdx)], rm.rtAdjPoolIdx);
        }
      } else {
        rtCollided[dIdx][tIdx][w.numShifts] = 1;
      }
      if (w.maxArriveRow < boardTopRow || !masks[wi].intersects(b5)) {
        const int sy = bpCache.surfLockY[w.goalRot][w.goalX + detail::PIECE_MASK_X_OFFSET];
        const bool lockedOnArrive =
            w.arriveOnGravity()
            && (w.yAtArriveInput == sy
                || (w.yAtArriveInput > sy
                    && collides20(board, pieceIdx, w.goalX, w.yAtArriveInput + 1, w.goalRot)));
        const int yStart = lockedOnArrive ? w.yAtArriveInput : w.yArrive;
        if (collectEarlyTucks) {
          rt_detail::evaluateFallPhaseB<true>(
              board, bpCache, pieceIdx, w.goalRot, w.goalX, yStart, w.arriveFrame, w.dasAfterArrive,
              w.dpadReadyFrame, w.tuckDpadReadyFrame, w.rotReadyFrame, w.holdActiveAtArrive(),
              w.heldDirAtArrive, lockedOnArrive, w.trackFlags, cfg_, earlyTable, earlyTucks,
              earlyMl);
        } else {
          rt_detail::evaluateFallPhaseB<false>(
              board, bpCache, pieceIdx, w.goalRot, w.goalX, yStart, w.arriveFrame, w.dasAfterArrive,
              w.dpadReadyFrame, w.tuckDpadReadyFrame, w.rotReadyFrame, w.holdActiveAtArrive(),
              w.heldDirAtArrive, lockedOnArrive, w.trackFlags, cfg_, earlyTable, earlyTucks,
              earlyMl);
        }
      } else {
        arrCollided[dIdx][tIdx][w.numShifts] |= static_cast<uint8_t>(1U << w.goalRot);
      }
    }

    rt_detail::AdjStateBuf dpAdj;
    if (cfg_.enablePoleVaults) {
      if (rtCollided[1][0][0] != 0U || arrCollided[1][0][0] != 0U) {
        rt_detail::ShiftSchedule sched;
        if (rt_detail::buildShiftSchedule(spawnSt, 0, rt_detail::TrackKind::Das, cfg_, sched)) {
          dpAdj.clear();
          if (collectEarlyTucks) {
            rt_detail::runPoleVaultDpForSchedule<true>(board, bpCache, pieceIdx, spawnSt, sched,
                                                       cfg_, earlyTable, earlyTucks, earlyMl,
                                                       rtCollided[1][0][0] ? &dpAdj : nullptr);
          } else {
            rt_detail::runPoleVaultDpForSchedule<false>(board, bpCache, pieceIdx, spawnSt, sched,
                                                        cfg_, earlyTable, earlyTucks, earlyMl,
                                                        rtCollided[1][0][0] ? &dpAdj : nullptr);
          }
          for (int i = 0; i < dpAdj.count; ++i) {
            addAdjState(dpAdj.data[i], -1);
          }
        }
      }
      for (int dir : {-1, 1}) {
        const int dIdx = dir + 1;
        for (int tIdx = 0; tIdx < 2; ++tIdx) {
          const auto tk = static_cast<rt_detail::TrackKind>(tIdx);
          uint8_t neededByK[DAS_MAX_SHIFTS + 1] = {};
          uint8_t rtMaskBits = 0U;
          bool anyNeed = false;
          for (int k = 1; k <= DAS_MAX_SHIFTS; ++k) {
            if (rtCollided[dIdx][tIdx][k] != 0U) {
              neededByK[k] = ALL_ROTS_MASK;
              rtMaskBits |= static_cast<uint8_t>(1U << k);
              anyNeed = true;
            } else if (arrCollided[dIdx][tIdx][k] != 0U) {
              neededByK[k] = arrCollided[dIdx][tIdx][k];
              anyNeed = true;
            }
          }
          if (anyNeed) {
            dpAdj.clear();
            if (collectEarlyTucks) {
              rt_detail::runPoleVaultDpMultiK<true>(
                  board, bpCache, pieceIdx, spawnSt, dir, tk, neededByK, rtMaskBits, cfg_,
                  earlyTable, earlyTucks, earlyMl, rtMaskBits != 0U ? &dpAdj : nullptr,
                  &preScheds.get(dir, tk));
            } else {
              rt_detail::runPoleVaultDpMultiK<false>(
                  board, bpCache, pieceIdx, spawnSt, dir, tk, neededByK, rtMaskBits, cfg_,
                  earlyTable, earlyTucks, earlyMl, rtMaskBits != 0U ? &dpAdj : nullptr,
                  &preScheds.get(dir, tk));
            }
            for (int i = 0; i < dpAdj.count; ++i) {
              addAdjState(dpAdj.data[i], -1);
            }
          }
        }
        if (cfg_.enableQuicktaps) {
          for (int k = 1; k <= DAS_MAX_SHIFTS; ++k) {
            const bool needRt = (rtCollided[dIdx][2][k] != 0U);
            if (!needRt && arrCollided[dIdx][2][k] == 0U) {
              continue;
            }
            rt_detail::ShiftSchedule sched;
            if (!rt_detail::buildShiftSchedule(spawnSt, dir * k, rt_detail::TrackKind::Quicktap,
                                               cfg_, sched)) {
              continue;
            }
            dpAdj.clear();
            if (collectEarlyTucks) {
              rt_detail::runPoleVaultDpForSchedule<true>(board, bpCache, pieceIdx, spawnSt, sched,
                                                         cfg_, earlyTable, earlyTucks, earlyMl,
                                                         needRt ? &dpAdj : nullptr);
            } else {
              rt_detail::runPoleVaultDpForSchedule<false>(board, bpCache, pieceIdx, spawnSt, sched,
                                                          cfg_, earlyTable, earlyTucks, earlyMl,
                                                          needRt ? &dpAdj : nullptr);
            }
            for (int i = 0; i < dpAdj.count; ++i) {
              addAdjState(dpAdj.data[i], -1);
            }
          }
        }
      }
    } else {
      for (int dIdx = 0; dIdx < 3; ++dIdx) {
        const int dir = dIdx - 1;
        for (int tIdx = 0; tIdx < 3; ++tIdx) {
          const auto tk = static_cast<rt_detail::TrackKind>(tIdx);
          for (int k = 0; k <= DAS_MAX_SHIFTS; ++k) {
            if (rtCollided[dIdx][tIdx][k] == 0U || (dir == 0 && k > 0) || (dir != 0 && k == 0)) {
              continue;
            }
            rt_detail::ShiftSchedule sched;
            if (!rt_detail::buildShiftSchedule(spawnSt, dir * k, tk, cfg_, sched)) {
              continue;
            }
            dpAdj.clear();
            if (collectEarlyTucks) {
              runDirectScheduleNoPoleVault<true>(board, bpCache, pieceIdx, spawnSt, sched,
                                                 earlyTable, earlyTucks, earlyMl, &dpAdj);
            } else {
              runDirectScheduleNoPoleVault<false>(board, bpCache, pieceIdx, spawnSt, sched,
                                                  earlyTable, earlyTucks, earlyMl, &dpAdj);
            }
            for (int i = 0; i < dpAdj.count; ++i) {
              addAdjState(dpAdj.data[i], -1);
            }
          }
        }
      }
    }

    if (collectEarlyTucks && (board.numHoles > 0 || hasUnreachedBasicCell(pieceIdx, earlyTable))) {
      rt_detail::runPhaseCOverhangTucks(board, bpCache, pieceIdx, spawnSt, cfg_, earlyTucks,
                                        earlyTable, earlyMl);
    }

    const int minRtY = static_cast<int>(spawnSt.y)
                       + rt_detail::countGravityDrops(0, cfg_.reactionTime, cfg_.gravity);
    for (int i = 0; i < earlyMl.count; ++i) {
      const DasLockMove &m = earlyMl.data[i];
      if (m.y < minRtY) {
        assert(res.numEarlyMoves < DAS_MAX_LOCK_MOVES);
        if (res.numEarlyMoves >= DAS_MAX_LOCK_MOVES) {
          break;
        }
        const uint16_t cIdx = internCell(m.rot, m.x, m.y, m.tuckInput);
        if (cIdx == INVALID_CELL_IDX) {
          continue;
        }
        res.earlyMoves[res.numEarlyMoves++] = {static_cast<uint8_t>(cIdx), m.dasChargeAfter};
      }
    }
  }

  void buildWalkListForState(int pieceIdx, const DasStartState &start,
                             std::vector<rt_detail::WalkMeta> &outMetas,
                             std::vector<rt_detail::BoardMask5> &outMasks,
                             std::vector<rt_detail::SpawnRtMeta> *outRtMetas,
                             std::vector<int> *outRtIndices, std::vector<uint64_t> *poolKeys) {
    const PieceDef20 &piece = PIECES_20[pieceIdx];
    const int tracks = cfg_.enableQuicktaps ? 3 : (cfg_.enableSlowTaps ? 2 : 1);
    std::vector<uint8_t> seenRtIdx;

    for (int goalRot = 0; goalRot < piece.numOrientations; ++goalRot) {
      const int loX = piece.minX[goalRot];
      const int hiX = rt_detail::pieceMaxX(piece, goalRot);
      for (int goalX = loX; goalX <= hiX; ++goalX) {
        const int dx = goalX - static_cast<int>(start.x);
        for (int t = 0; t < tracks; ++t) {
          const auto tk = static_cast<rt_detail::TrackKind>(t);
          rt_detail::WalkMeta meta;
          rt_detail::BoardMask5 mask;
          rt_detail::SpawnRtMeta rtMeta;
          DasStartState rtState;
          if (!rt_detail::precomputeSweptWalk(pieceIdx, start, goalRot, dx, tk, cfg_, meta, mask,
                                              outRtMetas != nullptr ? &rtMeta : nullptr,
                                              outRtIndices != nullptr ? &rtState : nullptr)) {
            continue;
          }
          if (outRtIndices != nullptr && outRtMetas != nullptr && rtMeta.hasRtCapture) {
            const uint64_t k = rtState.packKey();
            int poolIdx = -1;
            for (size_t i = 0; i < poolKeys->size(); ++i) {
              if ((*poolKeys)[i] == k) {
                poolIdx = static_cast<int>(i);
                break;
              }
            }
            if (poolIdx < 0) {
              poolIdx = static_cast<int>(adjPool_[pieceIdx].size());
              adjPool_[pieceIdx].push_back(rtState);
              poolKeys->push_back(k);
            }
            assert(poolIdx < std::numeric_limits<int16_t>::max());
            rtMeta.rtAdjPoolIdx = static_cast<int16_t>(poolIdx);
            if (static_cast<size_t>(poolIdx) >= seenRtIdx.size()) {
              seenRtIdx.resize(static_cast<size_t>(poolIdx) + 1, 0);
            }
            if (seenRtIdx[static_cast<size_t>(poolIdx)] == 0U) {
              seenRtIdx[static_cast<size_t>(poolIdx)] = 1;
              outRtIndices->push_back(poolIdx);
            }
          }
          outMetas.push_back(meta);
          outMasks.push_back(mask);
          if (outRtMetas != nullptr) {
            outRtMetas->push_back(rtMeta);
          }
        }
      }
    }
  }

  void buildAll() {
    // Build hot DAS buckets {5, 10, 15} first so their pooled phis are densely packed at low
    // indices [0..~120] for optimal L2/TLB locality and O(1) poolToSlot indexing.
    static constexpr int CLASS_ORDER[NUM_DAS_CLASSES] = {5, 10, 15, 0,  1,  2,  3,  4, 6,
                                                         7, 8,  9,  11, 12, 13, 14, 16};

    for (int p = 0; p < NUM_PIECES; ++p) {
      const PieceDef20 &piece = PIECES_20[p];
      const int rtY = static_cast<int>(piece.initialY)
                      + ((cfg_.reactionTime > 0)
                             ? rt_detail::countGravityDrops(0, cfg_.reactionTime, cfg_.gravity)
                             : 0);
      int maxBottomOffset = 0;
      for (int rot = 0; rot < piece.numOrientations; ++rot) {
        for (int r = 0; r < 4; ++r) {
          if (piece.rows[rot][r] != 0 && piece.rows[rot][r] != PIECE_NONE) {
            maxBottomOffset = std::max(maxBottomOffset, r);
          }
        }
      }
      maxEmptyRtBottomRow_[p] = rtY + maxBottomOffset;

      std::vector<uint64_t> poolKeys;
      for (int cls : CLASS_ORDER) {
        const DasStartState sp = DasStartState::spawn(p, cls);
        buildWalkListForState(p, sp, spawnWalkMetas_[p][cls], spawnWalkMasks_[p][cls],
                              cfg_.reactionTime > 0 ? &spawnRtMetas_[p][cls] : nullptr,
                              cfg_.reactionTime > 0 ? &spawnEmptyRtAdjIndices_[p][cls] : nullptr,
                              &poolKeys);
        const int cnt = static_cast<int>(spawnWalkMetas_[p][cls].size());
        spawnAllCellsCovered_[p][cls] =
            checkAllCellsCovered(p, spawnWalkMetas_[p][cls].data(), cnt);
        spawnMaxWalkRow_[p][cls] =
            static_cast<int8_t>(maxWalkRowOf(spawnWalkMetas_[p][cls].data(), cnt));
        buildPreSchedQuad(sp, spawnPreScheds_[p][cls]);
      }

      adjPoolLookup_[p].build(adjPool_[p]);

      const size_t nAdj = adjPool_[p].size();
      adjWalkOffset_[p].resize(nAdj + 1, 0U);
      adjPreScheds_[p].resize(nAdj);
      adjAllCellsCovered_[p].resize(nAdj, 0U);
      adjMaxWalkRow_[p].resize(nAdj, -1);

      std::vector<rt_detail::WalkMeta> tmpMetas;
      std::vector<rt_detail::BoardMask5> tmpMasks;
      for (size_t i = 0; i < nAdj; ++i) {
        tmpMetas.clear();
        tmpMasks.clear();
        buildWalkListForState(p, adjPool_[p][i], tmpMetas, tmpMasks, nullptr, nullptr, nullptr);
        adjWalkOffset_[p][i] = static_cast<uint32_t>(adjWalkMetaData_[p].size());
        adjWalkMetaData_[p].insert(adjWalkMetaData_[p].end(), tmpMetas.begin(), tmpMetas.end());
        adjWalkMaskData_[p].insert(adjWalkMaskData_[p].end(), tmpMasks.begin(), tmpMasks.end());
        const int cnt = static_cast<int>(tmpMetas.size());
        adjAllCellsCovered_[p][i] = checkAllCellsCovered(p, tmpMetas.data(), cnt) ? 1U : 0U;
        adjMaxWalkRow_[p][i] = static_cast<int8_t>(maxWalkRowOf(tmpMetas.data(), cnt));
        buildPreSchedQuad(adjPool_[p][i], adjPreScheds_[p][i]);
      }
      adjWalkOffset_[p][nAdj] = static_cast<uint32_t>(adjWalkMetaData_[p].size());
    }
  }

  template <bool CollectTucks>
  bool runFromPrecomputedWalks(const Board20 &board, const rt_detail::BoardPieceCache &bpCache,
                               const uint64_t b5[5], int pieceIdx, const DasStartState &start,
                               const rt_detail::WalkMeta *metas, const rt_detail::BoardMask5 *masks,
                               int walkCount, int maxWalkRow, const PreSchedQuad *preScheds,
                               rt_detail::LockMoveTable &table,
                               rt_detail::TuckColumnTable &tuckCols,
                               DasMoveList &out) const noexcept {
    const int boardTopRow = static_cast<int>(board.topNonEmptyRow);

    // Unobstructed fast path: streams only 16 B WalkMeta (4x fewer cache lines than AoS!).
    if (boardTopRow > maxWalkRow) {
      for (int i = 0; i < walkCount; ++i) {
        const rt_detail::WalkMeta &w = metas[i];
        const int sy = bpCache.surfLockY[w.goalRot][w.goalX + detail::PIECE_MASK_X_OFFSET];
        const bool lockedOnArrive = w.arriveOnGravity() && (w.yAtArriveInput == sy);
        const int yStart = lockedOnArrive ? w.yAtArriveInput : w.yArrive;
        rt_detail::evaluateFallPhaseB<CollectTucks>(
            board, bpCache, pieceIdx, w.goalRot, w.goalX, yStart, w.arriveFrame, w.dasAfterArrive,
            w.dpadReadyFrame, w.tuckDpadReadyFrame, w.rotReadyFrame, w.holdActiveAtArrive(),
            w.heldDirAtArrive, lockedOnArrive, w.trackFlags, cfg_, table, tuckCols, out);
      }
      return false;
    }

    uint8_t collidedRot[3][3][DAS_MAX_SHIFTS + 1];
    bool anyCollided = false;
    bool anyPvNeed = false;

    for (int i = 0; i < walkCount; ++i) {
      const rt_detail::WalkMeta &w = metas[i];
      if (w.maxArriveRow < boardTopRow || !masks[i].intersects(b5)) {
        const int sy = bpCache.surfLockY[w.goalRot][w.goalX + detail::PIECE_MASK_X_OFFSET];
        const bool lockedOnArrive =
            w.arriveOnGravity()
            && (w.yAtArriveInput == sy
                || (w.yAtArriveInput > sy
                    && collides20(board, pieceIdx, w.goalX, w.yAtArriveInput + 1, w.goalRot)));
        const int yStart = lockedOnArrive ? w.yAtArriveInput : w.yArrive;
        rt_detail::evaluateFallPhaseB<CollectTucks>(
            board, bpCache, pieceIdx, w.goalRot, w.goalX, yStart, w.arriveFrame, w.dasAfterArrive,
            w.dpadReadyFrame, w.tuckDpadReadyFrame, w.rotReadyFrame, w.holdActiveAtArrive(),
            w.heldDirAtArrive, lockedOnArrive, w.trackFlags, cfg_, table, tuckCols, out);
      } else {
        if (!anyCollided) {
          std::memset(collidedRot, 0, sizeof(collidedRot));
          anyCollided = true;
        }
        if (cfg_.enablePoleVaults) {
          if (board.numHoles > 0
              || w.yAtLastShift
                     <= bpCache.surfLockY[w.goalRot][w.goalX + detail::PIECE_MASK_X_OFFSET]) {
            const int dIdx = w.dir + 1;
            const int tIdx = rt_detail::trackFlagsToIndex(w.trackFlags);
            collidedRot[dIdx][tIdx][w.numShifts] |= static_cast<uint8_t>(1U << w.goalRot);
            anyPvNeed = true;
          }
        }
      }
    }

    if (anyPvNeed && PIECES_20[pieceIdx].numOrientations > 1) {
      if (collidedRot[1][0][0] != 0U) {
        rt_detail::ShiftSchedule sched;
        if (rt_detail::buildShiftSchedule(start, 0, rt_detail::TrackKind::Das, cfg_, sched)) {
          rt_detail::runPoleVaultDpForSchedule<CollectTucks>(board, bpCache, pieceIdx, start, sched,
                                                             cfg_, table, tuckCols, out, nullptr);
        }
      }
      for (int dir : {-1, 1}) {
        const int dIdx = dir + 1;
        for (int tIdx = 0; tIdx < 2; ++tIdx) {
          bool anyNeed = false;
          for (int k = 1; k <= DAS_MAX_SHIFTS; ++k) {
            if (collidedRot[dIdx][tIdx][k] != 0U) {
              anyNeed = true;
              break;
            }
          }
          if (anyNeed) {
            const auto tk = static_cast<rt_detail::TrackKind>(tIdx);
            rt_detail::runPoleVaultDpMultiK<CollectTucks>(
                board, bpCache, pieceIdx, start, dir, tk, collidedRot[dIdx][tIdx], 0U, cfg_, table,
                tuckCols, out, nullptr, preScheds != nullptr ? &preScheds->get(dir, tk) : nullptr);
          }
        }
        if (cfg_.enableQuicktaps) {
          for (int k = 1; k <= DAS_MAX_SHIFTS; ++k) {
            if (collidedRot[dIdx][2][k] == 0U) {
              continue;
            }
            rt_detail::ShiftSchedule sched;
            if (!rt_detail::buildShiftSchedule(start, dir * k, rt_detail::TrackKind::Quicktap, cfg_,
                                               sched)) {
              continue;
            }
            rt_detail::runPoleVaultDpForSchedule<CollectTucks>(
                board, bpCache, pieceIdx, start, sched, cfg_, table, tuckCols, out, nullptr);
          }
        }
      }
    }
    return anyCollided;
  }

  template <bool CollectTucks>
  void runDynamic(const Board20 &board, const rt_detail::BoardPieceCache &bpCache, int pieceIdx,
                  const DasStartState &start, rt_detail::LockMoveTable &table,
                  rt_detail::TuckColumnTable &tuckCols, DasMoveList &out) const noexcept {
    const PieceDef20 &piece = PIECES_20[pieceIdx];
    const int tracks = cfg_.enableQuicktaps ? 3 : (cfg_.enableSlowTaps ? 2 : 1);
    int minGoalX = 99;
    int maxGoalX = -99;
    for (int rot = 0; rot < piece.numOrientations; ++rot) {
      minGoalX = std::min<int>(minGoalX, piece.minX[rot]);
      maxGoalX = std::max<int>(maxGoalX, rt_detail::pieceMaxX(piece, rot));
    }
    for (int goalX = minGoalX; goalX <= maxGoalX; ++goalX) {
      const int dx = goalX - static_cast<int>(start.x);
      for (int t = 0; t < tracks; ++t) {
        rt_detail::ShiftSchedule sched;
        if (!rt_detail::buildShiftSchedule(start, dx, static_cast<rt_detail::TrackKind>(t), cfg_,
                                           sched)) {
          continue;
        }
        if (cfg_.enablePoleVaults) {
          rt_detail::runPoleVaultDpForSchedule<CollectTucks>(board, bpCache, pieceIdx, start, sched,
                                                             cfg_, table, tuckCols, out, nullptr);
        } else {
          runDirectScheduleNoPoleVault<CollectTucks>(board, bpCache, pieceIdx, start, sched, table,
                                                     tuckCols, out, nullptr);
        }
      }
    }
  }

  template <bool CollectTucks>
  void runDirectScheduleNoPoleVault(const Board20 &board, const rt_detail::BoardPieceCache &bpCache,
                                    int pieceIdx, const DasStartState &start,
                                    const rt_detail::ShiftSchedule &sched,
                                    rt_detail::LockMoveTable &table,
                                    rt_detail::TuckColumnTable &tuckCols, DasMoveList &out,
                                    rt_detail::AdjStateBuf *outAdjStates) const noexcept {
    const PieceDef20 &piece = PIECES_20[pieceIdx];
    const int numOrients = piece.numOrientations;
    const int rotMask = numOrients - 1;
    const int goalX = static_cast<int>(start.x) + static_cast<int>(sched.dir) * sched.numShifts;
    const int rt = (!start.isAdjustment && cfg_.reactionTime > 0 && outAdjStates != nullptr)
                       ? cfg_.reactionTime
                       : -1;

    for (int goalRot = 0; goalRot < numOrients; ++goalRot) {
      if (!rt_detail::inPieceXBounds(piece, goalRot, goalX)) {
        continue;
      }
      int rotInc = 0;
      if (start.rot != goalRot) {
        rotInc =
            (numOrients == 2) ? 1 : ((((static_cast<int>(start.rot) - 1) & 3) == goalRot) ? -1 : 1);
      }

      int x = start.x;
      int y = start.y;
      int rot = start.rot;
      int f = start.frame;
      int das = start.dasCharge;
      int dpadCd = start.dpadCooldown;
      int rotCd = start.rotCooldown;
      int shiftCd = start.shiftCooldown;
      int heldDir = start.heldDir;
      int nextShiftIdx = 0;
      bool arrived = (x == goalX && rot == goalRot);
      bool capturedRt = false;

      if (arrived) {
        const bool keepHoldingStationary =
            (start.isAdjustment && start.heldDir != 0 && das < DAS_FULL_THRESHOLD);
        rt_detail::evaluateFallPhaseB<CollectTucks>(
            board, bpCache, pieceIdx, rot, x, y, f, das, f + dpadCd,
            rt_detail::computeTuckDpadReadyFrame(f, dpadCd, shiftCd, cfg_), f + rotCd,
            keepHoldingStationary, start.heldDir, false, sched.trackFlags, cfg_, table, tuckCols,
            out);
        if (rt <= 0) {
          continue;
        }
      }

      while (f < DAS_MAX_SIM_FRAMES) {
        const bool isGrav = detail::isGravityFrame(f, cfg_.gravity);
        const bool willShift =
            (nextShiftIdx < sched.numShifts && sched.shiftFrames[nextShiftIdx] == f);
        int holdDir = 0;
        const bool willHold =
            !willShift
            && rt_detail::shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, holdDir);

        if (willShift) {
          if (collides20(board, pieceIdx, x + sched.dir, y, rot)) {
            break;
          }
          x += sched.dir;
          if (sched.shiftResetsDas[nextShiftIdx] != 0U) {
            das = 0;
            dpadCd = cfg_.dpadGapFrames;
          } else {
            das = DAS_POST_SHIFT_CHARGE;
            if (dpadCd > 0) {
              dpadCd--;
            }
          }
          shiftCd = cfg_.dpadGapFrames;
          heldDir = sched.dir;
          nextShiftIdx++;
        } else if (willHold) {
          if (das < DAS_FULL_THRESHOLD) {
            das++;
          }
          heldDir = holdDir;
          if (dpadCd > 0) {
            dpadCd--;
          }
          if (shiftCd > 0) {
            shiftCd--;
          }
        } else {
          heldDir = 0;
          if (dpadCd > 0) {
            dpadCd--;
          }
          if (shiftCd > 0) {
            shiftCd--;
          }
        }

        if (rot != goalRot && rotCd == 0) {
          const int nrot = (rot + rotInc + 4) & rotMask;
          if (!rt_detail::inPieceXBounds(piece, nrot, x)) {
            if (x == goalX) {
              break;
            }
          } else {
            if (collides20(board, pieceIdx, x, y, nrot)) {
              break;
            }
            rot = nrot;
            rotCd = cfg_.rotGapFrames;
          }
        } else if (rotCd > 0) {
          rotCd--;
        }

        const bool locksHere = isGrav && collides20(board, pieceIdx, x, y + 1, rot);
        const bool justArrived = (!arrived && x == goalX && rot == goalRot);
        if (justArrived) {
          arrived = true;
          int dummyDir = 0;
          const bool holdActive =
              rt_detail::shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, dummyDir);
          const int tuckDpadReady =
              rt_detail::computeTuckDpadReadyFrame(f + 1, dpadCd, shiftCd, cfg_);
          rt_detail::evaluateFallPhaseB<CollectTucks>(
              board, bpCache, pieceIdx, rot, x, locksHere ? y : (y + (isGrav ? 1 : 0)), f + 1, das,
              f + 1 + dpadCd, tuckDpadReady, f + 1 + rotCd, holdActive, heldDir, locksHere,
              sched.trackFlags, cfg_, table, tuckCols, out);
        }

        if (locksHere) {
          break;
        }
        if (isGrav) {
          y++;
        }
        f++;

        if (rt > 0 && !capturedRt && f == rt) {
          capturedRt = true;
          outAdjStates->push(
              DasStartState::makeAdjustment(x, y, rot, f, das, heldDir, dpadCd, rotCd, shiftCd));
        }

        if (arrived && (rt <= 0 || capturedRt)) {
          break;
        }
      }
    }
  }
};

} // namespace das

#endif // DAS_TABLEBASE_DAS_RT_MOVE_GEN_HPP
