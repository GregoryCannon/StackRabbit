#ifndef DAS_TABLEBASE_SOLVER_RT_20ROW_HPP
#define DAS_TABLEBASE_SOLVER_RT_20ROW_HPP

// Reaction-time + DAS-bucket Modified Policy Iteration solver for V_RT(b, c, d) (SRTBV004).
//
// Bellman equation (d = starting DAS bucket of the current piece c, n = next piece):
//   V(b, c, d) = max over actions a committed before the reaction time of
//                sum_n P(n | c) * max_{e in Edges(a)} [ r(e) + delta(lc_e) * V(b'_e, n, bucket(e))
//                ]
// where an action is either
//   - a phantom phi (a distinct mid-air state at frame == reactionTime) whose edges are every lock
//     reachable from phi once n is known, or
//   - an early lock (locks strictly before the reaction time) with a single fixed edge.
// Each edge carries the ending DAS charge, mapped to the next piece's starting bucket.

#include "das_rt_move_gen.hpp"
#include "solver_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace das {

// Packed 32-bit RT policy word:
//   - Bit 31 == 0: bits 0..25 childId (RT_ACTION_OUT_CHILD = out-of-set / top-out),
//                  bits 26..27 next-piece DAS bucket, bits 28..30 linesCleared.
//   - Bit 31 == 1: fixed-point terminal soft-boundary payoff (same encoding as solve20MPI).
inline constexpr uint32_t RT_ACTION_CHILD_MASK = 0x03FFFFFFU;
inline constexpr uint32_t RT_ACTION_OUT_CHILD = 0x03FFFFFFU;
inline constexpr int RT_ACTION_BUCKET_SHIFT = 26;
inline constexpr uint32_t RT_ACTION_BUCKET_MASK = 0x3U;
inline constexpr size_t RT_MAX_BOARDS = static_cast<size_t>(RT_ACTION_OUT_CHILD);

static_assert((RT_ACTION_CHILD_MASK >> RT_ACTION_BUCKET_SHIFT) == 0U, "RT child/bucket overlap");
static_assert(RT_ACTION_BUCKET_SHIFT + 2 == ACTION_REWARD_SHIFT, "RT bucket/lines overlap");

constexpr uint32_t packRtPolicyAction(uint32_t childId, uint8_t linesCleared, int bucket) noexcept {
  const uint32_t packedChild = (childId == INVALID_BOARD_ID) ? RT_ACTION_OUT_CHILD : childId;
  return (static_cast<uint32_t>(linesCleared) << ACTION_REWARD_SHIFT)
         | ((static_cast<uint32_t>(bucket) & RT_ACTION_BUCKET_MASK) << RT_ACTION_BUCKET_SHIFT)
         | (packedChild & RT_ACTION_CHILD_MASK);
}

constexpr uint32_t unpackRtPolicyChild(uint32_t packed) noexcept {
  if (isTerminalPayoff(packed)) {
    return INVALID_BOARD_ID;
  }
  const uint32_t raw = packed & RT_ACTION_CHILD_MASK;
  return (raw == RT_ACTION_OUT_CHILD) ? INVALID_BOARD_ID : raw;
}

constexpr int unpackRtPolicyBucket(uint32_t packed) noexcept {
  return static_cast<int>((packed >> RT_ACTION_BUCKET_SHIFT) & RT_ACTION_BUCKET_MASK);
}

inline uint32_t moveConfigToTbV4Flags(const MoveSearchConfig &cfg) noexcept {
  return (cfg.enableSlowTaps ? TB_V4_MOVE_SLOW_TAPS : 0U)
         | (cfg.enablePoleVaults ? TB_V4_MOVE_POLE_VAULTS : 0U)
         | (cfg.enableQuicktaps ? TB_V4_MOVE_QUICKTAPS : 0U)
         | (cfg.enableTucks ? TB_V4_MOVE_TUCKS : 0U);
}

inline void fillHeaderV4MoveFields(TablebaseHeaderV4 &hdr, const MoveSearchConfig &cfg) noexcept {
  hdr.reactionTimeFrames = static_cast<uint32_t>(cfg.reactionTime);
  hdr.moveFlags = moveConfigToTbV4Flags(cfg);
  hdr.dpadGapFrames = static_cast<uint32_t>(cfg.dpadGapFrames);
  hdr.rotGapFrames = static_cast<uint32_t>(cfg.rotGapFrames);
  hdr.quicktapDelayFrames = static_cast<uint32_t>(cfg.quicktapDelayFrames);
}

// Reconstructs the move-search config a SRTBV004 tablebase was solved under.
inline MoveSearchConfig moveConfigFromHeaderV4(const TablebaseHeaderV4 &hdr) noexcept {
  MoveSearchConfig cfg;
  cfg.gravity = getGravity20(static_cast<int>(hdr.level));
  cfg.reactionTime = static_cast<int>(hdr.reactionTimeFrames);
  cfg.dpadGapFrames = static_cast<int>(hdr.dpadGapFrames);
  cfg.rotGapFrames = static_cast<int>(hdr.rotGapFrames);
  cfg.quicktapDelayFrames = static_cast<int>(hdr.quicktapDelayFrames);
  cfg.enableSlowTaps = (hdr.moveFlags & TB_V4_MOVE_SLOW_TAPS) != 0U;
  cfg.enablePoleVaults = (hdr.moveFlags & TB_V4_MOVE_POLE_VAULTS) != 0U;
  cfg.enableQuicktaps = (hdr.moveFlags & TB_V4_MOVE_QUICKTAPS) != 0U;
  cfg.enableTucks = (hdr.moveFlags & TB_V4_MOVE_TUCKS) != 0U;
  return cfg;
}

// Hard-boundary evaluator for the RT solver (zero continuation outside the tablebase).
struct NoOpRtBoundaryEval20 {
  static constexpr bool enabled = false;
  void evalRowRt(const uint16_t /*afterRows*/[BOARD_ROWS], int /*numBuckets*/,
                 double /*outCont*/[MAX_RT_VALUES_PER_BOARD]) const noexcept {}
};

namespace detail {

// Per-worker direct-mapped cache of RT soft-boundary continuations keyed by BoardKey. Stores the
// exact doubles produced by BoundaryFn so hits are bit-identical to misses.
class SoftBoundaryCacheRt20 {
public:
  static constexpr size_t NUM_SLOTS = 131072; // ~24 MB/worker
  static constexpr size_t MASK = NUM_SLOTS - 1;
  static_assert((NUM_SLOTS & (NUM_SLOTS - 1)) == 0, "NUM_SLOTS must be a power of two");

  struct Entry {
    BoardKey key = EMPTY_KEY;
    double cont[MAX_RT_VALUES_PER_BOARD] = {};
  };

  SoftBoundaryCacheRt20() : slots_(NUM_SLOTS) {}

  bool lookup(BoardKey key, double *outCont, int numValues) const noexcept {
    const Entry &e = slots_[static_cast<size_t>(hashBoardKeySlot(key)) & MASK];
    if (e.key != key) {
      return false;
    }
    std::memcpy(outCont, e.cont, static_cast<size_t>(numValues) * sizeof(double));
    return true;
  }

  void store(BoardKey key, const double *cont, int numValues) noexcept {
    Entry &e = slots_[static_cast<size_t>(hashBoardKeySlot(key)) & MASK];
    e.key = key;
    std::memcpy(e.cont, cont, static_cast<size_t>(numValues) * sizeof(double));
  }

private:
  std::vector<Entry> slots_;
};

} // namespace detail

// Reusable per-thread workspace for one (board, piece) reaction-time decision: runs the RT move
// search for 1..3 starting DAS charges, collects the union of reachable lock cells, and picks the
// max-expected-value action (phantom or early lock) once callers have filled per-cell values.
//
// Per-cell value layout: q(cell)[n * numBuckets + k'] = value of locking at `cell` when the next
// piece is n and the lock leaves the next piece in DAS bucket k'.
class RtDecisionWorkspace {
public:
  static constexpr int MAX_CELLS = rt_detail::LockMoveTable::TOTAL;

  struct Cell {
    int8_t x;
    int8_t y;
    uint8_t rot;
    uint8_t lc;
  };

  struct Choice {
    bool any = false;     // false when the piece cannot spawn (top-out)
    bool isEarly = false; // early lock (true) or phantom (false)
    int index = -1;       // early-move index or phantom index within result(start)
    double value = 0.0;   // expected value over the next piece
    int cellForNext[NUM_PIECES] = {};
    int bucketForNext[NUM_PIECES] = {};
    int dasAfterForNext[NUM_PIECES] = {};
  };

  RtDecisionWorkspace()
      : search_(std::make_unique<RtSearchScratch>()),
        res_(std::make_unique<DasRtSearchResult[]>(MAX_DAS_BUCKETS)),
        q_(static_cast<size_t>(MAX_CELLS) * MAX_RT_VALUES_PER_BOARD, 0.0),
        word_(static_cast<size_t>(MAX_CELLS) * MAX_RT_VALUES_PER_BOARD, 0U) {
    std::fill(std::begin(cellSlot_), std::end(cellSlot_), static_cast<int16_t>(-1));
  }

  RtDecisionWorkspace(const RtDecisionWorkspace &) = delete;
  RtDecisionWorkspace &operator=(const RtDecisionWorkspace &) = delete;

  // Runs the RT search from `numStarts` starting DAS charges and collects all unique lock cells.
  // When `forceZeroRt` is true, runs a 0-frame spawn move search even if `tables` was configured
  // with `reactionTime > 0`. Cell line-clear counts are filled lazily by computeAfterRows().
  void search(const DasRtMoveTables &tables, const Board20 &board, int piece,
              const int *startDasValues, int numStarts, bool forceZeroRt = false) noexcept {
    assert(numStarts >= 1 && numStarts <= MAX_DAS_BUCKETS);
    for (int i = 0; i < numCells_; ++i) {
      const Cell &c = cells_[i];
      cellSlot_[rt_detail::LockMoveTable::cellIndex(c.rot, c.x, c.y)] = -1;
    }
    numCells_ = 0;
    numStarts_ = numStarts;
    if (forceZeroRt && tables.config().reactionTime > 0) {
      DasMoveList mList;
      for (int s = 0; s < numStarts; ++s) {
        DasRtSearchResult &r = res_[s];
        r.clear();
        const DasStartState sp = DasStartState::spawn(piece, startDasValues[s]);
        if (collides20(board, piece, sp.x, sp.y, sp.rot)) {
          continue;
        }
        tables.searchMoves(board, piece, sp, mList);
        r.adjStates[0] = sp;
        r.numAdjStates = 1;
        r.adjMoveOffset[0] = 0;
        r.adjMoveOffset[1] = mList.count;
        r.numUniqueCells = mList.count;
        for (int i = 0; i < mList.count; ++i) {
          const DasLockMove &m = mList.data[i];
          r.uniqueCells[i] = {m.x, m.y, m.rot, '-'};
          r.adjEdges[i] = {static_cast<uint8_t>(i), m.dasChargeAfter};
        }
      }
    } else {
      tables.searchRtMultiDas(board, piece, startDasValues, numStarts, res_.get(), search_.get());
    }
    for (int s = 0; s < numStarts; ++s) {
      const DasRtSearchResult &r = res_[s];
      for (int u = 0; u < r.numUniqueCells; ++u) {
        const CompactPlacement &cp = r.uniqueCells[u];
        const int idx = rt_detail::LockMoveTable::cellIndex(cp.rot, cp.x, cp.y);
        int16_t slot = cellSlot_[idx];
        if (slot < 0) {
          slot = static_cast<int16_t>(numCells_);
          cellSlot_[idx] = slot;
          cells_[numCells_++] = {cp.x, cp.y, cp.rot, 0U};
        }
        localOf_[s][u] = static_cast<uint16_t>(slot);
      }
    }
  }

  int numCells() const noexcept { return numCells_; }
  int numStarts() const noexcept { return numStarts_; }
  const Cell &cell(int i) const noexcept { return cells_[i]; }
  const DasRtSearchResult &result(int s) const noexcept { return res_[s]; }
  int localCellOf(int start, int uniqueCellIdx) const noexcept {
    return localOf_[start][uniqueCellIdx];
  }
  RtSearchScratch *scratch() noexcept { return search_.get(); }

  // Applies cell i to board, stores the line-clear count on the cell, and returns it.
  uint8_t computeAfterRows(const Board20 &board, int piece, int i,
                           uint16_t outRows[BOARD_ROWS]) noexcept {
    Cell &c = cells_[i];
    c.lc = applyPlacement20(board, piece, c.rot, c.x, c.y, outRows);
    return c.lc;
  }

  double *q(int i) noexcept { return &q_[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD]; }
  const double *q(int i) const noexcept {
    return &q_[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD];
  }
  uint32_t *word(int i) noexcept {
    return &word_[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD];
  }
  const uint32_t *word(int i) const noexcept {
    return &word_[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD];
  }

  // Picks the action with the highest expected value for search start `s` (ties keep the first
  // early move, then the first phantom, in search order).
  Choice chooseAction(int s, int numBuckets,
                      const std::array<double, NUM_PIECES> &probs) const noexcept {
    Choice ch;
    const DasRtSearchResult &r = res_[s];
    double best = 0.0;

    for (int i = 0; i < r.numEarlyMoves; ++i) {
      const int cl = localOf_[s][r.earlyMoves[i].cellIdx];
      const int kb = dasChargeToBucket(r.earlyMoves[i].dasAfter, numBuckets);
      const double *qc = q(cl);
      double v = 0.0;
      for (int n = 0; n < NUM_PIECES; ++n) {
        v += probs[static_cast<size_t>(n)] * qc[n * numBuckets + kb];
      }
      if (!ch.any || v > best) {
        best = v;
        ch.any = true;
        ch.isEarly = true;
        ch.index = i;
      }
    }

    double bestPerNext[NUM_PIECES];
    for (int p = 0; p < r.numAdjStates; ++p) {
      const int lo = r.adjMoveOffset[p];
      const int hi = r.adjMoveOffset[p + 1];
      if (lo == hi) {
        continue;
      }
      bestEdgesForPhantom(s, numBuckets, lo, hi, bestPerNext, nullptr, nullptr);
      double v = 0.0;
      for (int n = 0; n < NUM_PIECES; ++n) {
        v += probs[static_cast<size_t>(n)] * bestPerNext[n];
      }
      if (!ch.any || v > best) {
        best = v;
        ch.any = true;
        ch.isEarly = false;
        ch.index = p;
      }
    }

    if (!ch.any) {
      return ch;
    }
    ch.value = best;
    if (ch.isEarly) {
      const AdjEdge &e = r.earlyMoves[ch.index];
      const int cl = localOf_[s][e.cellIdx];
      const int dasAfter = static_cast<int>(e.dasAfter);
      const int kb = dasChargeToBucket(dasAfter, numBuckets);
      for (int n = 0; n < NUM_PIECES; ++n) {
        ch.cellForNext[n] = cl;
        ch.bucketForNext[n] = kb;
        ch.dasAfterForNext[n] = dasAfter;
      }
    } else {
      bestEdgesForPhantom(s, numBuckets, r.adjMoveOffset[ch.index], r.adjMoveOffset[ch.index + 1],
                          bestPerNext, ch.cellForNext, ch.bucketForNext, ch.dasAfterForNext);
    }
    return ch;
  }

private:
  std::unique_ptr<RtSearchScratch> search_;
  std::unique_ptr<DasRtSearchResult[]> res_;
  std::vector<double> q_;
  std::vector<uint32_t> word_;
  int16_t cellSlot_[rt_detail::LockMoveTable::TOTAL];
  Cell cells_[MAX_CELLS];
  uint16_t localOf_[MAX_DAS_BUCKETS][DAS_MAX_LOCK_MOVES];
  int numCells_ = 0;
  int numStarts_ = 0;

  void bestEdgesForPhantom(int s, int numBuckets, int lo, int hi, double bestPerNext[NUM_PIECES],
                           int *cellOut, int *bucketOut,
                           int *dasAfterOut = nullptr) const noexcept {
    const DasRtSearchResult &r = res_[s];
    int bestDas[NUM_PIECES] = {};
    for (int e = lo; e < hi; ++e) {
      const int cl = localOf_[s][r.adjEdges[e].cellIdx];
      const int dasAfter = static_cast<int>(r.adjEdges[e].dasAfter);
      const int kb = dasChargeToBucket(dasAfter, numBuckets);
      const double *qc = q(cl);
      for (int n = 0; n < NUM_PIECES; ++n) {
        const double qq = qc[n * numBuckets + kb];
        if (e == lo || qq > bestPerNext[n] || (qq == bestPerNext[n] && dasAfter > bestDas[n])) {
          bestPerNext[n] = qq;
          bestDas[n] = dasAfter;
          if (cellOut != nullptr) {
            cellOut[n] = cl;
            bucketOut[n] = kb;
          }
          if (dasAfterOut != nullptr) {
            dasAfterOut[n] = dasAfter;
          }
        }
      }
    }
  }
};

struct SolveRtConfig20 {
  SolveConfig20 base;        // level, rngMode, gamma, discount mode, tolerances, threads, ...
  int numDasBuckets = 3;     // 2 or 3
  MoveSearchConfig move;     // move.gravity must match base.level
  bool exactResidual = true; // run one extra full Bellman backup to measure the residual
};

struct SolveRtResult20 {
  bool ok = false;
  std::string error;
  std::vector<double> values; // Flat numBoards * 7 * numDasBuckets ([board][c][k])
  int outerIterations = 0;
  int totalInnerSweeps = 0;
  double finalOuterDiff = 0.0;
  double bellmanResidual = 0.0;
  double emptyBoardValue = MISSING_BOARD_VALUE; // E_c[V(0, c, highest bucket)]
  double maxValue = 0.0;
  double elapsedSeconds = 0.0;
  double improvementSeconds = 0.0;
  size_t warmStartMatched = 0;
  bool converged = false;
  bool usedDiskPolicyCache = false;
};

namespace detail {

inline const char *validateSolveRtConfig(const SolveRtConfig20 &cfg) noexcept {
  const SolveConfig20 &b = cfg.base;
  if (!std::isfinite(b.gamma) || b.gamma <= 0.0 || b.gamma > 1.0)
    return "Invalid gamma (must be in (0, 1])";
  if (!std::isfinite(b.tolerance) || b.tolerance <= 0.0)
    return "Invalid tolerance (must be > 0)";
  if (!std::isfinite(b.maxValueDivergenceGuard) || b.maxValueDivergenceGuard <= 0.0)
    return "Invalid maxValueDivergenceGuard";
  if (b.level != 18 && b.level != 19 && b.level != 29)
    return "Unsupported level (expected 18, 19, or 29)";
  if (b.maxOuterIterations <= 0 || b.innerEvalSweeps < 0)
    return "Invalid iteration limits";
  if (!isValidDasBucketCount(cfg.numDasBuckets))
    return "numDasBuckets must be 2 or 3";
  if (!cfg.move.isValid())
    return "Invalid MoveSearchConfig";
  if (cfg.move.gravity != getGravity20(b.level))
    return "MoveSearchConfig gravity does not match level";
  return nullptr;
}

// Warm-starts vCur ([board][c][k] doubles) from a SRTBV003 TB (broadcast across buckets) or a
// SRTBV004 TB with the same bucket count, matching keys by a sorted merge.
template <typename InitTbType>
inline bool initializeRtValuesFromWarmStart(const std::vector<BoardKey> &sortedKeys,
                                            const InitTbType &initTb, int numBuckets,
                                            std::vector<double> &vCur, size_t &outMatched,
                                            std::string &errOut) {
  outMatched = 0;
  const uint64_t initCount = initTb.size();
  const int vpb = NUM_PIECES * numBuckets;
  constexpr bool IS_V4 = std::is_same_v<InitTbType, MappedDasRtTablebase>;
  if constexpr (IS_V4) {
    if (initTb.numDasBuckets() != numBuckets) {
      errOut = "Warm-start SRTBV004 numDasBuckets does not match solver";
      return false;
    }
  }
  float row[MAX_RT_VALUES_PER_BOARD];
  uint64_t j = 0;
  for (size_t i = 0; i < sortedKeys.size() && j < initCount; ++i) {
    const BoardKey target = sortedKeys[i];
    while (j < initCount && initTb.keyAt(j) < target) {
      if (j > 0 && initTb.keyAt(j) <= initTb.keyAt(j - 1)) {
        errOut = "Warm-start tablebase keys are not strictly sorted ascending";
        return false;
      }
      ++j;
    }
    if (j < initCount && initTb.keyAt(j) == target) {
      initTb.copyValuesAt(j, row);
      double *dst = &vCur[i * static_cast<size_t>(vpb)];
      for (int c = 0; c < NUM_PIECES; ++c) {
        for (int k = 0; k < numBuckets; ++k) {
          const float v = IS_V4 ? row[rtValueIndex(c, k, numBuckets)] : row[c];
          if (!std::isfinite(v) || v < 0.0f) {
            errOut = "Warm-start tablebase contains non-finite or negative value";
            return false;
          }
          dst[rtValueIndex(c, k, numBuckets)] = static_cast<double>(v);
        }
      }
      ++outMatched;
      ++j;
    }
  }
  return true;
}

// Per-worker state for RT Bellman backups.
struct RtBackupWorker {
  RtDecisionWorkspace ws;
  std::unique_ptr<SoftBoundaryCacheRt20> cache;
  std::vector<uint32_t> depScratch;
};

inline constexpr uint8_t ALL_PIECES_MASK = static_cast<uint8_t>((1U << NUM_PIECES) - 1U);

// Fills q/word for every cell of `ws` given current values vCur (in-TB children), the soft
// boundary (out-of-TB, fitsKey children), or zero continuation. Sets sawOverflow = true on payoff
// overflow. Optionally records whether any cell landed in the tablebase and appends its childId.
template <typename BoundaryFn>
inline void fillRtCellValues(const Board20 &board, int piece, int numBuckets,
                             const FlatBoardIndex &index, const std::vector<double> &vCur,
                             const StepDiscountTable20 &disc, const BoundaryFn &boundaryFn,
                             SoftBoundaryCacheRt20 *cache, RtDecisionWorkspace &ws,
                             bool &sawOverflow, bool *outHasInTb = nullptr,
                             std::vector<uint32_t> *outInTbChildren = nullptr) {
  const int vpb = NUM_PIECES * numBuckets;
  uint16_t afterRows[BOARD_ROWS];
  double cont[MAX_RT_VALUES_PER_BOARD];
  if (outHasInTb != nullptr) {
    *outHasInTb = false;
  }
  for (int i = 0; i < ws.numCells(); ++i) {
    const uint8_t lc = ws.computeAfterRows(board, piece, i, afterRows);
    const RtDecisionWorkspace::Cell &cl = ws.cell(i);
    const double rew = REWARD_NORM_TETRIS[lc];
    const double dFac = disc.f[lc];
    const int startRow =
        (lc == 0)
            ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow), static_cast<int>(cl.y)))
            : 0;
    BoardKey afterKey;
    const bool fitsKey = encodeBoardKey(afterRows, afterKey, startRow);
    const uint32_t cid = fitsKey ? index.find(afterKey) : INVALID_BOARD_ID;
    double *qc = ws.q(i);
    uint32_t *wc = ws.word(i);

    if (cid != INVALID_BOARD_ID) {
      if (outHasInTb != nullptr) {
        *outHasInTb = true;
      }
      if (outInTbChildren != nullptr) {
        outInTbChildren->push_back(cid);
      }
      const double *childRow = &vCur[static_cast<size_t>(cid) * static_cast<size_t>(vpb)];
      for (int n = 0; n < NUM_PIECES; ++n) {
        for (int k = 0; k < numBuckets; ++k) {
          const int o = n * numBuckets + k;
          qc[o] = rew + dFac * childRow[o];
          wc[o] = packRtPolicyAction(cid, lc, k);
        }
      }
      continue;
    }
    if constexpr (BoundaryFn::enabled) {
      if (fitsKey) {
        if (cache == nullptr || !cache->lookup(afterKey, cont, vpb)) {
          boundaryFn.evalRowRt(afterRows, numBuckets, cont);
          if (cache != nullptr) {
            cache->store(afterKey, cont, vpb);
          }
        }
        for (int o = 0; o < vpb; ++o) {
          const double c = (std::isfinite(cont[o]) && cont[o] > 0.0) ? cont[o] : 0.0;
          const double qRaw = rew + dFac * c;
          if (qRaw > TERMINAL_MAX_PAYOFF) {
            sawOverflow = true;
          }
          wc[o] = packTerminalPayoff(qRaw);
          qc[o] = unpackTerminalPayoff(wc[o]);
        }
      } else {
        // >= 10-hole children can never enter the tablebase: zero continuation (no NN sink).
        const uint32_t termWord = packTerminalPayoff(rew);
        const double qQuant = unpackTerminalPayoff(termWord);
        for (int o = 0; o < vpb; ++o) {
          wc[o] = termWord;
          qc[o] = qQuant;
        }
      }
    } else {
      const uint32_t outWord = packRtPolicyAction(INVALID_BOARD_ID, lc, 0);
      for (int o = 0; o < vpb; ++o) {
        wc[o] = outWord;
        qc[o] = rew;
      }
    }
  }
}

// Evaluates one piece's K bucket values from already-recorded packed RT policy words.
inline void evalRtPieceFromPolicy20(int piece, int numBuckets, size_t zeroRowBase,
                                    const double *__restrict__ vCurPtr,
                                    const TransitionProbMatrix &transProb,
                                    const StepDiscountTable20 &disc, const uint32_t *boardActs,
                                    double *newRow) noexcept {
  const size_t vpb = static_cast<size_t>(NUM_PIECES * numBuckets);
  const auto &probs = transProb[static_cast<size_t>(piece)];
  for (int k = 0; k < numBuckets; ++k) {
    const int vi = rtValueIndex(piece, k, numBuckets);
    const uint32_t *w = &boardActs[static_cast<size_t>(vi) * NUM_PIECES];
    double v = 0.0;
    for (int nx = 0; nx < NUM_PIECES; ++nx) {
      const uint32_t packed = w[nx];
      double q = 0.0;
      if (isTerminalPayoff(packed)) {
        q = unpackTerminalPayoff(packed);
      } else {
        const uint8_t lc = unpackPolicyLines(packed);
        const uint32_t raw = packed & RT_ACTION_CHILD_MASK;
        const size_t rowBase =
            (raw == RT_ACTION_OUT_CHILD) ? zeroRowBase : static_cast<size_t>(raw) * vpb;
        const int nextBucket = unpackRtPolicyBucket(packed);
        q = REWARD_NORM_TETRIS[lc]
            + disc.f[lc] * vCurPtr[rowBase + static_cast<size_t>(nx * numBuckets + nextBucket)];
      }
      v += probs[static_cast<size_t>(nx)] * q;
    }
    newRow[vi] = v;
  }
}

// Full or partial RT Bellman backup for one board: writes 7 * numBuckets values into newRow and
// (when policyActions != nullptr) updates the 7 policy words per (c, k) for dirty pieces.
// Pieces with bit 0 in `dirtyPieceMask` copy their bit-identical values from `oldRow` without
// running move generation, NN calls, or modifying `policyActions`.
// Preconditions:
//   - When dirtyPieceMask != ALL_PIECES_MASK: oldRow != nullptr, outHasInTbMask == nullptr, and
//     outInTbChildren == nullptr.
//   - When outInTbChildren != nullptr, it is cleared and filled with sorted unique childIds.
template <typename BoundaryFn>
inline void computeRtBellmanBackup20(
    const Board20 &board, const DasRtMoveTables &tables, int numBuckets,
    const FlatBoardIndex &index, const std::vector<double> &vCur,
    const TransitionProbMatrix &transProb, const StepDiscountTable20 &disc,
    const BoundaryFn &boundaryFn, RtBackupWorker &worker, const double *oldRow, double *newRow,
    uint32_t *policyActions, bool &sawOverflow, uint8_t dirtyPieceMask = ALL_PIECES_MASK,
    uint8_t *outHasInTbMask = nullptr, std::vector<uint32_t> *outInTbChildren = nullptr) {
  assert(dirtyPieceMask == ALL_PIECES_MASK
         || (oldRow != nullptr && outHasInTbMask == nullptr && outInTbChildren == nullptr));
  int reps[MAX_DAS_BUCKETS];
  for (int k = 0; k < numBuckets; ++k) {
    reps[k] = dasBucketRepresentativeCharge(k, numBuckets);
  }
  const uint32_t emptyAct = packRtPolicyAction(INVALID_BOARD_ID, 0, 0);
  uint8_t inTbMask = 0U;
  if (outInTbChildren != nullptr) {
    outInTbChildren->clear();
  }
  for (int c = 0; c < NUM_PIECES; ++c) {
    if (((dirtyPieceMask >> c) & 1U) == 0U) {
      for (int k = 0; k < numBuckets; ++k) {
        const int vi = rtValueIndex(c, k, numBuckets);
        newRow[vi] = oldRow[vi];
      }
      continue;
    }
    worker.ws.search(tables, board, c, reps, numBuckets);
    bool pieceHasInTb = false;
    fillRtCellValues(board, c, numBuckets, index, vCur, disc, boundaryFn, worker.cache.get(),
                     worker.ws, sawOverflow, outHasInTbMask != nullptr ? &pieceHasInTb : nullptr,
                     outInTbChildren);
    if (pieceHasInTb) {
      inTbMask = static_cast<uint8_t>(inTbMask | (1U << c));
    }
    const auto &probs = transProb[static_cast<size_t>(c)];
    for (int k = 0; k < numBuckets; ++k) {
      const RtDecisionWorkspace::Choice ch = worker.ws.chooseAction(k, numBuckets, probs);
      newRow[rtValueIndex(c, k, numBuckets)] = ch.any ? ch.value : 0.0;
      if (policyActions != nullptr) {
        uint32_t *dst =
            &policyActions[static_cast<size_t>(rtValueIndex(c, k, numBuckets)) * NUM_PIECES];
        for (int n = 0; n < NUM_PIECES; ++n) {
          dst[n] = ch.any ? worker.ws.word(ch.cellForNext[n])[n * numBuckets + ch.bucketForNext[n]]
                          : emptyAct;
        }
      }
    }
  }
  if (outHasInTbMask != nullptr) {
    *outHasInTbMask = inTbMask;
  }
  if (outInTbChildren != nullptr && outInTbChildren->size() > 1) {
    std::sort(outInTbChildren->begin(), outInTbChildren->end());
    outInTbChildren->erase(std::unique(outInTbChildren->begin(), outInTbChildren->end()),
                           outInTbChildren->end());
  }
}

} // namespace detail

// Solves V_RT(b, c, d) with Modified Policy Iteration. Optional warm start from a SRTBV003
// (broadcast across buckets) or SRTBV004 tablebase, and optional frozen soft boundary.
template <typename BoundaryFn = NoOpRtBoundaryEval20>
inline SolveRtResult20 solve20RtMPI(const std::vector<BoardKey> &sortedKeys,
                                    const SolveRtConfig20 &cfg,
                                    const MappedDasTablebase *initTbV3 = nullptr,
                                    const MappedDasRtTablebase *initTbV4 = nullptr,
                                    const BoundaryFn &boundaryFn = BoundaryFn{}) {
  SolveRtResult20 res{};
  const auto t0 = std::chrono::steady_clock::now();
  if (const char *err = detail::validateSolveRtConfig(cfg)) {
    res.error = err;
    return res;
  }
  if (initTbV3 != nullptr && initTbV4 != nullptr && initTbV3->isLoaded() && initTbV4->isLoaded()) {
    res.error = "Specify at most one warm-start tablebase (SRTBV003 or SRTBV004)";
    return res;
  }
  const SolveConfig20 &bc = cfg.base;
  const int K = cfg.numDasBuckets;
  const size_t vpb = static_cast<size_t>(NUM_PIECES * K);
  const size_t wordsPerBoard = vpb * NUM_PIECES;
  const size_t n = sortedKeys.size();
  if (n >= RT_MAX_BOARDS) {
    res.error = "Board count exceeds 26-bit packed RT childId capacity";
    return res;
  }
  if (!detail::validateSortedKeys(sortedKeys, &res.error)) {
    return res;
  }
  if (n == 0) {
    res.ok = true;
    res.converged = true;
    return res;
  }

  FlatBoardIndex index;
  if (!index.build(sortedKeys, &res.error)) {
    return res;
  }

  // Extra trailing zero row at index n is the branchless sentinel for RT_ACTION_OUT_CHILD.
  std::vector<double> vCur((n + 1) * vpb, 0.0);
  std::vector<double> vNext((n + 1) * vpb, 0.0);
  if (initTbV3 != nullptr && initTbV3->isLoaded()) {
    initTbV3->adviseSequential();
    const bool ok = detail::initializeRtValuesFromWarmStart(sortedKeys, *initTbV3, K, vCur,
                                                            res.warmStartMatched, res.error);
    initTbV3->adviseDontNeed();
    if (!ok)
      return res;
  } else if (initTbV4 != nullptr && initTbV4->isLoaded()) {
    initTbV4->adviseSequential();
    const bool ok = detail::initializeRtValuesFromWarmStart(sortedKeys, *initTbV4, K, vCur,
                                                            res.warmStartMatched, res.error);
    initTbV4->adviseDontNeed();
    if (!ok)
      return res;
  }

  const size_t policyBytes = n * wordsPerBoard * sizeof(uint32_t);
  const size_t autoDiskBytes = bc.autoDiskThresholdBoards * ACTIONS_PER_BOARD * sizeof(uint32_t);
  const bool useDisk =
      (bc.policyCacheMode == PolicyCacheMode::Disk)
      || (bc.policyCacheMode == PolicyCacheMode::Auto && policyBytes > autoDiskBytes);
  res.usedDiskPolicyCache = useDisk;
  detail::PolicyActionStore20 policyStore;
  if (!policyStore.init(n, useDisk, &res.error, wordsPerBoard)) {
    return res;
  }

  const int threads = std::max(1, bc.numThreads);
  detail::PersistentWorkerPool pool(threads);
  const DasRtMoveTables tables(cfg.move);
  const auto transProb = detail::buildTransitionProbMatrix(bc.rngMode);
  const StepDiscountTable20 disc = StepDiscountTable20::make(bc.gamma, bc.discountMode);
  const uint32_t emptyBoardId = index.find(EMPTY_BOARD_KEY);
  std::vector<std::unique_ptr<detail::RtBackupWorker>> workers(static_cast<size_t>(threads));
  for (auto &w : workers) {
    w = std::make_unique<detail::RtBackupWorker>();
    if constexpr (BoundaryFn::enabled) {
      w->cache = std::make_unique<detail::SoftBoundaryCacheRt20>();
    }
  }

  const size_t chunkStride = useDisk ? detail::PolicyActionStore20::CHUNK_BOARDS : n;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);
  std::vector<double> threadMaxVal(static_cast<size_t>(threads), 0.0);

  auto emptyValue = [&](const std::vector<double> &v) {
    if (emptyBoardId == INVALID_BOARD_ID)
      return MISSING_BOARD_VALUE;
    double ev = 0.0;
    for (int c = 0; c < NUM_PIECES; ++c) {
      ev += openingPieceProb(bc.rngMode, c)
            * v[static_cast<size_t>(emptyBoardId) * vpb
                + static_cast<size_t>(rtValueIndex(c, K - 1, K))];
    }
    return ev;
  };

  // Exact dependency tracking built on outer == 1 so outer >= 2 skips every (board, piece) that
  // has zero in-TB children or whose in-TB children were bit-identical since the previous backup.
  enum class BackupMode { CollectDeps, Incremental, Residual };
  constexpr size_t MAX_IN_TB_CHILD_EDGES = 450'000'000ULL; // 1.8 GB cap (covers 15M–20M live-RT)
  static_assert(MAX_IN_TB_CHILD_EDGES <= std::numeric_limits<uint32_t>::max(),
                "MAX_IN_TB_CHILD_EDGES must fit in uint32_t offsets");
  std::vector<uint8_t> hasInTbPieceMask(n, detail::ALL_PIECES_MASK);
  std::vector<uint64_t> prevBoardDirtyBits((n + 63) / 64, ~uint64_t{0});
  std::vector<uint8_t> curBoardDirty(n, 0U);
  std::vector<uint32_t> boardInTbOffset;
  std::vector<uint32_t> boardInTbChildren;
  bool useChildDeps = true;

  auto isPrevBoardDirty = [&](uint32_t cid) noexcept -> bool {
    return ((prevBoardDirtyBits[cid >> 6] >> (cid & 63U)) & 1ULL) != 0ULL;
  };
  auto computeBoardDirtyMask = [&](size_t i) noexcept -> uint8_t {
    uint8_t mask = hasInTbPieceMask[i];
    if (mask != 0U && useChildDeps) {
      const uint32_t lo = boardInTbOffset[i];
      const uint32_t hi = boardInTbOffset[i + 1];
      bool anyChildDirty = false;
      for (uint32_t e = lo; e < hi; ++e) {
        if (isPrevBoardDirty(boardInTbChildren[e])) {
          anyChildDirty = true;
          break;
        }
      }
      if (!anyChildDirty) {
        mask = 0U;
      }
    }
    return mask;
  };

  // One full or incremental Bellman backup over all boards.
  auto runImprovement = [&](BackupMode mode, double &outDiff, double &outMaxV,
                            uint64_t *outSkippedPieces = nullptr) -> bool {
    const bool recordActions = (mode != BackupMode::Residual);
    const bool collectDeps = (mode == BackupMode::CollectDeps);
    const bool allowSkip = (mode == BackupMode::Incremental);
    std::atomic<bool> sawNonFinite{false};
    std::atomic<bool> sawOverflow{false};
    std::atomic<uint64_t> skippedPieces{0};
    outDiff = 0.0;
    outMaxV = 0.0;
    if (collectDeps && useChildDeps) {
      boardInTbOffset.assign(n + 1, 0U);
      boardInTbChildren.clear();
    }
    // Always bound chunk size during CollectDeps so temporary per-chunk edge storage stays small
    // even in RAM policy-cache mode.
    const size_t loopStride =
        (useDisk || collectDeps) ? detail::PolicyActionStore20::CHUNK_BOARDS : chunkStride;
    std::vector<uint8_t> chunkMasks;
    if (allowSkip) {
      chunkMasks.resize(std::min(n, loopStride));
    }

    for (size_t chunkBegin = 0; chunkBegin < n; chunkBegin += loopStride) {
      const size_t chunkEnd = std::min(n, chunkBegin + loopStride);
      const size_t chunkCount = chunkEnd - chunkBegin;

      size_t dirtyBoardsInChunk = chunkCount;
      bool allFullPiecesDirty = true;
      if (allowSkip) {
        dirtyBoardsInChunk = 0;
        for (size_t j = 0; j < chunkCount; ++j) {
          const uint8_t m = computeBoardDirtyMask(chunkBegin + j);
          chunkMasks[j] = m;
          if (m != 0U) {
            dirtyBoardsInChunk++;
          }
          if (m != detail::ALL_PIECES_MASK) {
            allFullPiecesDirty = false;
          }
        }
        if (dirtyBoardsInChunk == 0) {
          // Entire chunk is bit-identical to the previous backup: zero move gen and zero disk I/O.
          std::memcpy(&vNext[chunkBegin * vpb], &vCur[chunkBegin * vpb],
                      chunkCount * vpb * sizeof(double));
          for (size_t j = 0; j < chunkCount; ++j) {
            const double *row = &vCur[(chunkBegin + j) * vpb];
            for (size_t v = 0; v < vpb; ++v) {
              outMaxV = std::max(outMaxV, row[v]);
            }
          }
          skippedPieces.fetch_add(static_cast<uint64_t>(chunkCount) * NUM_PIECES,
                                  std::memory_order_relaxed);
          continue;
        }
      }

      uint32_t *chunkActions = nullptr;
      if (recordActions) {
        if (allowSkip && !allFullPiecesDirty) {
          chunkActions = policyStore.beginModifyChunk(chunkBegin, chunkCount, &res.error);
          if (chunkActions == nullptr) {
            return false;
          }
        } else {
          chunkActions = policyStore.beginWriteChunk(chunkBegin, chunkCount);
        }
      }

      std::vector<std::vector<uint32_t>> chunkDeps((collectDeps && useChildDeps) ? chunkCount : 0U);
      std::atomic<size_t> depEdgeBudget{collectDeps && useChildDeps ? boardInTbChildren.size()
                                                                    : 0U};
      std::atomic<size_t> nextBoard{chunkBegin};
      std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
      std::fill(threadMaxVal.begin(), threadMaxVal.end(), 0.0);
      const size_t grabSize = allowSkip ? 256U : std::max<size_t>(64U, bc.minParallelBoards / 16U);

      pool.run([&](int tid) {
        detail::RtBackupWorker &w = *workers[static_cast<size_t>(tid)];
        double localDiff = 0.0;
        double localMax = 0.0;
        bool localOverflow = false;
        uint64_t localSkipped = 0;
        double row[MAX_RT_VALUES_PER_BOARD];
        while (true) {
          const size_t subBegin = nextBoard.fetch_add(grabSize, std::memory_order_relaxed);
          if (subBegin >= chunkEnd) {
            break;
          }
          const size_t subEnd = std::min(chunkEnd, subBegin + grabSize);
          for (size_t i = subBegin; i < subEnd; ++i) {
            const uint8_t dirtyMask =
                allowSkip ? chunkMasks[i - chunkBegin] : detail::ALL_PIECES_MASK;
            const double *oldRow = &vCur[i * vpb];
            double *dst = &vNext[i * vpb];
            if (dirtyMask == 0U) {
              std::memcpy(dst, oldRow, vpb * sizeof(double));
              for (size_t v = 0; v < vpb; ++v) {
                localMax = std::max(localMax, oldRow[v]);
              }
              localSkipped += NUM_PIECES;
              continue;
            }
            for (int c = 0; c < NUM_PIECES; ++c) {
              if (((dirtyMask >> c) & 1U) == 0U)
                localSkipped++;
            }
            const Board20 board = Board20::fromKey(sortedKeys[i]);
            uint32_t *acts = (chunkActions != nullptr)
                                 ? &chunkActions[(i - chunkBegin) * wordsPerBoard]
                                 : nullptr;
            const bool wantDeps =
                collectDeps
                && useChildDeps
                && (depEdgeBudget.load(std::memory_order_relaxed) <= MAX_IN_TB_CHILD_EDGES);
            std::vector<uint32_t> *depTarget = wantDeps ? &w.depScratch : nullptr;
            detail::computeRtBellmanBackup20(
                board, tables, K, index, vCur, transProb, disc, boundaryFn, w, oldRow, row, acts,
                localOverflow, dirtyMask, collectDeps ? &hasInTbPieceMask[i] : nullptr, depTarget);
            if (wantDeps && !w.depScratch.empty()) {
              const size_t prevTot =
                  depEdgeBudget.fetch_add(w.depScratch.size(), std::memory_order_relaxed);
              if (prevTot + w.depScratch.size() <= MAX_IN_TB_CHILD_EDGES) {
                chunkDeps[i - chunkBegin].assign(w.depScratch.begin(), w.depScratch.end());
              }
            }
            bool boardChanged = false;
            for (size_t v = 0; v < vpb; ++v) {
              const double nv = row[v];
              if (!std::isfinite(nv)) {
                sawNonFinite.store(true, std::memory_order_relaxed);
                continue;
              }
              if (recordActions) {
                dst[v] = nv;
                if (nv != oldRow[v]) {
                  boardChanged = true;
                }
              }
              localMax = std::max(localMax, nv);
              localDiff = std::max(localDiff, std::abs(nv - oldRow[v]));
            }
            if (recordActions && boardChanged) {
              curBoardDirty[i] = 1U;
            }
          }
        }
        if (localOverflow)
          sawOverflow.store(true, std::memory_order_relaxed);
        if (localSkipped > 0)
          skippedPieces.fetch_add(localSkipped, std::memory_order_relaxed);
        threadMaxDiff[static_cast<size_t>(tid)] = localDiff;
        threadMaxVal[static_cast<size_t>(tid)] = localMax;
      });
      if (sawNonFinite.load()) {
        res.error = "Non-finite value encountered during RT Bellman backup";
        return false;
      }
      if (sawOverflow.load()) {
        res.error = "Soft-boundary terminal payoff exceeded TERMINAL_MAX_PAYOFF";
        return false;
      }
      if (collectDeps && useChildDeps) {
        if (depEdgeBudget.load(std::memory_order_relaxed) > MAX_IN_TB_CHILD_EDGES) {
          useChildDeps = false;
          std::vector<uint32_t>().swap(boardInTbChildren);
          std::vector<uint32_t>().swap(boardInTbOffset);
          if (bc.verbose) {
            printf("  [RT-MPI] in-TB dependency graph exceeded %zu edges; falling back to "
                   "zero-child piece skipping\n",
                   MAX_IN_TB_CHILD_EDGES);
            fflush(stdout);
          }
        } else {
          for (size_t j = 0; j < chunkCount; ++j) {
            const auto &deps = chunkDeps[j];
            boardInTbChildren.insert(boardInTbChildren.end(), deps.begin(), deps.end());
            boardInTbOffset[chunkBegin + j + 1] = static_cast<uint32_t>(boardInTbChildren.size());
          }
        }
      }
      outDiff = std::max(outDiff, *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end()));
      outMaxV = std::max(outMaxV, *std::max_element(threadMaxVal.begin(), threadMaxVal.end()));
      if (recordActions && !policyStore.finishWriteChunk(chunkBegin, chunkCount, &res.error)) {
        return false;
      }
    }
    if (outSkippedPieces != nullptr) {
      *outSkippedPieces = skippedPieces.load();
    }
    return true;
  };

  // One policy-evaluation sweep under the stored policy.
  auto runEval = [&](double &outDiff, double &outMaxV) -> bool {
    std::atomic<bool> sawNonFinite{false};
    outDiff = 0.0;
    outMaxV = 0.0;
    for (size_t chunkBegin = 0; chunkBegin < n; chunkBegin += chunkStride) {
      const size_t chunkEnd = std::min(n, chunkBegin + chunkStride);
      const size_t chunkCount = chunkEnd - chunkBegin;
      const uint32_t *chunkActions = policyStore.readChunk(chunkBegin, chunkCount, &res.error);
      if (chunkActions == nullptr)
        return false;
      std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
      std::fill(threadMaxVal.begin(), threadMaxVal.end(), 0.0);
      pool.runSubrange(chunkBegin, chunkEnd, bc.minParallelBoards,
                       [&](int tid, size_t subBegin, size_t subEnd) {
                         double localDiff = 0.0;
                         double localMax = 0.0;
                         const double *__restrict__ vCurPtr = vCur.data();
                         const size_t zeroRowBase = n * vpb;
                         for (size_t i = subBegin; i < subEnd; ++i) {
                           const uint32_t *acts = &chunkActions[(i - chunkBegin) * wordsPerBoard];
                           const double *oldRow = &vCurPtr[i * vpb];
                           double *__restrict__ dst = &vNext[i * vpb];
                           for (int c = 0; c < NUM_PIECES; ++c) {
                             detail::evalRtPieceFromPolicy20(c, K, zeroRowBase, vCurPtr, transProb,
                                                             disc, acts, dst);
                           }
                           bool boardChanged = false;
                           for (size_t vi = 0; vi < vpb; ++vi) {
                             const double v = dst[vi];
                             if (!std::isfinite(v)) {
                               sawNonFinite.store(true, std::memory_order_relaxed);
                             } else {
                               if (v != oldRow[vi]) {
                                 boardChanged = true;
                               }
                               localMax = std::max(localMax, v);
                               localDiff = std::max(localDiff, std::abs(v - oldRow[vi]));
                             }
                           }
                           if (boardChanged && curBoardDirty[i] == 0U) {
                             curBoardDirty[i] = 1U;
                           }
                         }
                         threadMaxDiff[static_cast<size_t>(tid)] = localDiff;
                         threadMaxVal[static_cast<size_t>(tid)] = localMax;
                       });
      if (sawNonFinite.load()) {
        res.error = "Non-finite value encountered during RT policy-evaluation sweep";
        return false;
      }
      outDiff = std::max(outDiff, *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end()));
      outMaxV = std::max(outMaxV, *std::max_element(threadMaxVal.begin(), threadMaxVal.end()));
    }
    return true;
  };

  for (int outer = 1; outer <= bc.maxOuterIterations; ++outer) {
    const auto iterStart = std::chrono::steady_clock::now();
    std::fill(curBoardDirty.begin(), curBoardDirty.end(), static_cast<uint8_t>(0U));
    double outerDiff = 0.0;
    double outerMaxV = 0.0;
    uint64_t skippedPieces = 0;
    const BackupMode mode = (outer == 1) ? BackupMode::CollectDeps : BackupMode::Incremental;
    if (!runImprovement(mode, outerDiff, outerMaxV, &skippedPieces)) {
      return res;
    }
    const double impSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - iterStart).count();
    res.improvementSeconds += impSec;
    vCur.swap(vNext);
    res.outerIterations = outer;
    res.finalOuterDiff = outerDiff;
    res.maxValue = outerMaxV;
    if (outerMaxV > bc.maxValueDivergenceGuard) {
      res.error = "Solver diverged: maxV=" + std::to_string(outerMaxV);
      return res;
    }

    const double innerTol =
        std::max(bc.tolerance * bc.innerStopRatio, outerDiff * bc.innerStopRatio);
    int innerDone = 0;
    double innerDiff = 0.0;
    if (outerDiff > bc.tolerance && bc.innerEvalSweeps > 0) {
      for (int it = 0; it < bc.innerEvalSweeps; ++it) {
        double innerMaxV = 0.0;
        if (!runEval(innerDiff, innerMaxV)) {
          return res;
        }
        vCur.swap(vNext);
        innerDone++;
        res.totalInnerSweeps++;
        res.maxValue = innerMaxV;
        if (innerMaxV > bc.maxValueDivergenceGuard) {
          res.error = "Solver diverged in inner sweep: maxV=" + std::to_string(innerMaxV);
          return res;
        }
        if (innerDiff <= innerTol) {
          break;
        }
      }
    }
    // Pack curBoardDirty into compact 1-bit/board prevBoardDirtyBits for L2-friendly child checks.
    std::fill(prevBoardDirtyBits.begin(), prevBoardDirtyBits.end(), uint64_t{0});
    for (size_t i = 0; i < n; ++i) {
      if (curBoardDirty[i] != 0U) {
        prevBoardDirtyBits[i >> 6] |= (uint64_t{1} << (i & 63U));
      }
    }

    if (bc.verbose) {
      const double iterSec =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - iterStart).count();
      const double skipPct =
          100.0 * static_cast<double>(skippedPieces) / static_cast<double>(n * NUM_PIECES);
      printf("  [RT-MPI outer=%2d] outerDiff=%.3e inner=%d(%.2e) E[V(0,full)]=%.5f maxV=%.3f "
             "skip=%.1f%% (improve %.1fs, total %.1fs)\n",
             outer, outerDiff, innerDone, innerDiff, emptyValue(vCur), res.maxValue, skipPct,
             impSec, iterSec);
      fflush(stdout);
    }
    if (outerDiff <= bc.tolerance) {
      res.converged = true;
      break;
    }
  }

  policyStore.close();
  std::vector<uint32_t>().swap(boardInTbChildren);
  std::vector<uint32_t>().swap(boardInTbOffset);
  std::vector<uint8_t>().swap(hasInTbPieceMask);
  std::vector<uint64_t>().swap(prevBoardDirtyBits);
  std::vector<uint8_t>().swap(curBoardDirty);

  if (cfg.exactResidual) {
    double resid = 0.0;
    double maxV = 0.0;
    if (!runImprovement(BackupMode::Residual, resid, maxV)) {
      return res;
    }
    res.bellmanResidual = resid;
  } else {
    res.bellmanResidual = res.finalOuterDiff;
  }
  std::vector<double>().swap(vNext);

  res.emptyBoardValue = emptyValue(vCur);
  vCur.resize(n * vpb);
  res.values = std::move(vCur);
  res.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  res.ok = true;
  return res;
}

// Exact RT Soft-Boundary Monte Carlo check: plays the greedy V_RT policy from the empty board
// (opening piece sampled, highest DAS bucket), committing each action before sampling the next
// piece, and compares the mean discounted return against E_c[V(0, c, highest bucket)].
template <typename TbType, typename BoundaryFn = NoOpRtBoundaryEval20>
inline SoftMcResult20 evaluateSoftBoundaryMcRt20(const TbType &tb, const DasRtMoveTables &tables,
                                                 int numGames, uint64_t seed, int numThreads,
                                                 const BoundaryFn &boundaryFn = BoundaryFn{},
                                                 int maxSteps = 25000) {
  SoftMcResult20 out{};
  if (!tb.isLoaded() || numGames <= 0) {
    return out;
  }
  const int K = tb.numDasBuckets();
  const int vpb = NUM_PIECES * K;
  const int threads = std::max(1, std::min(numThreads, numGames));
  const StepDiscountTable20 disc = StepDiscountTable20::make(tb.gamma(), tb.discountMode());
  const RngMode rngMode = tb.rngMode();
  const auto transProb = detail::buildTransitionProbMatrix(rngMode);

  std::vector<double> gameReturns(static_cast<size_t>(numGames), 0.0);
  std::vector<uint64_t> gameSteps(static_cast<size_t>(numGames), 0);
  std::vector<uint8_t> gameExited(static_cast<size_t>(numGames), 0);
  std::atomic<int> nextG{0};

  detail::runWorkerPool(threads, static_cast<size_t>(numGames), 8, [&](int /*tid*/) {
    auto ws = std::make_unique<RtDecisionWorkspace>();
    std::vector<uint8_t> inTb(RtDecisionWorkspace::MAX_CELLS, 0);
    std::vector<BoardKey> keys(RtDecisionWorkspace::MAX_CELLS);
    uint16_t afterRows[BOARD_ROWS];
    float row[MAX_RT_VALUES_PER_BOARD];
    double cont[MAX_RT_VALUES_PER_BOARD];
    while (true) {
      const int g = nextG.fetch_add(1, std::memory_order_relaxed);
      if (g >= numGames)
        break;
      Rng rng(seed + static_cast<uint64_t>(g) * 1009ULL);
      int curP = rng.sampleOpeningPiece(rngMode);
      int bucket = K - 1;
      Board20 board = Board20::empty();
      double total = 0.0;
      double discount = 1.0;
      uint64_t steps = 0;
      bool exited = false;
      for (int step = 0; step < maxSteps && discount >= 1e-12; ++step) {
        const int rep = dasBucketRepresentativeCharge(bucket, K);
        ws->search(tables, board, curP, &rep, 1);
        for (int i = 0; i < ws->numCells(); ++i) {
          const uint8_t lc = ws->computeAfterRows(board, curP, i, afterRows);
          const double rew = REWARD_NORM_TETRIS[lc];
          const double dFac = disc.f[lc];
          const int sr = (lc == 0) ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                          static_cast<int>(ws->cell(i).y)))
                                   : 0;
          BoardKey k;
          const bool fits = encodeBoardKey(afterRows, k, sr);
          keys[static_cast<size_t>(i)] = k;
          double *qc = ws->q(i);
          if (fits && tb.lookup(k, row)) {
            inTb[static_cast<size_t>(i)] = 1;
            for (int o = 0; o < vpb; ++o) {
              qc[o] = rew + dFac * static_cast<double>(row[o]);
            }
            continue;
          }
          inTb[static_cast<size_t>(i)] = 0;
          if constexpr (BoundaryFn::enabled) {
            if (fits) {
              boundaryFn.evalRowRt(afterRows, K, cont);
              for (int o = 0; o < vpb; ++o) {
                const double c = (std::isfinite(cont[o]) && cont[o] > 0.0) ? cont[o] : 0.0;
                qc[o] = unpackTerminalPayoff(packTerminalPayoff(rew + dFac * c));
              }
            } else {
              const double qQuant = unpackTerminalPayoff(packTerminalPayoff(rew));
              for (int o = 0; o < vpb; ++o)
                qc[o] = qQuant;
            }
          } else {
            for (int o = 0; o < vpb; ++o)
              qc[o] = rew;
          }
        }
        const RtDecisionWorkspace::Choice ch =
            ws->chooseAction(0, K, transProb[static_cast<size_t>(curP)]);
        if (!ch.any) {
          break;
        }
        const int nextP = rng.sampleNextPiece(rngMode, curP);
        const int cl = ch.cellForNext[nextP];
        const int kb = ch.bucketForNext[nextP];
        steps++;
        if (inTb[static_cast<size_t>(cl)] != 0U) {
          const uint8_t lc = ws->cell(cl).lc;
          total += discount * REWARD_NORM_TETRIS[lc];
          discount *= disc.f[lc];
          board = Board20::fromKey(keys[static_cast<size_t>(cl)]);
          curP = nextP;
          bucket = kb;
        } else {
          total += discount * ws->q(cl)[nextP * K + kb];
          exited = true;
          break;
        }
      }
      gameReturns[static_cast<size_t>(g)] = total;
      gameSteps[static_cast<size_t>(g)] = steps;
      gameExited[static_cast<size_t>(g)] = exited ? 1 : 0;
    }
  });

  double sum = 0.0;
  uint64_t sumSteps = 0;
  int exits = 0;
  for (int g = 0; g < numGames; ++g) {
    sum += gameReturns[static_cast<size_t>(g)];
    sumSteps += gameSteps[static_cast<size_t>(g)];
    exits += gameExited[static_cast<size_t>(g)];
  }
  const double nG = static_cast<double>(numGames);
  const double mean = sum / nG;
  double varAcc = 0.0;
  for (double r : gameReturns) {
    varAcc += (r - mean) * (r - mean);
  }
  const double se = (numGames > 1) ? std::sqrt((varAcc / (nG - 1.0)) / nG) : 0.0;
  out.numGames = numGames;
  out.meanReturn = mean;
  out.stdError = se;
  out.meanSteps = static_cast<double>(sumSteps) / nG;
  out.exitRate = 100.0 * static_cast<double>(exits) / nG;
  out.zScore = (se > 1e-12 && tb.emptyBoardValue() >= 0.0)
                   ? std::abs(mean - tb.emptyBoardValue()) / se
                   : 0.0;
  return out;
}

} // namespace das

#endif // DAS_TABLEBASE_SOLVER_RT_20ROW_HPP
