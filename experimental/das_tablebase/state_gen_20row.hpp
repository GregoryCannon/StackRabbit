#ifndef DAS_TABLEBASE_STATE_GEN_20ROW_HPP
#define DAS_TABLEBASE_STATE_GEN_20ROW_HPP

#include "core_20row.hpp"
#include "nnue_20row.hpp"
#include "rt_nn_boundary_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace das {

enum class BoardPriorityMode {
  Visits,
  NnError,
};

constexpr bool parseBoardPriorityMode(std::string_view s, BoardPriorityMode &out) noexcept {
  if (s == "visits") {
    out = BoardPriorityMode::Visits;
    return true;
  }
  if (s == "nn-error") {
    out = BoardPriorityMode::NnError;
    return true;
  }
  return false;
}

constexpr const char *boardPriorityModeName(BoardPriorityMode mode) noexcept {
  return (mode == BoardPriorityMode::NnError) ? "nn-error" : "visits";
}

// SoA open-addressing visit map: BoardKey -> uint32_t visits (20 B/slot, <=70% load factor).
class FlatVisitMap20 {
public:
  explicit FlatVisitMap20(size_t initialCap = 16384) { rehash(initialCap); }

  void clearAndRelease() noexcept {
    std::vector<BoardKey>().swap(keys_);
    std::vector<uint32_t>().swap(counts_);
    mask_ = 0;
    size_ = 0;
  }

  void increment(BoardKey key, uint32_t delta = 1U) {
    assert(key != EMPTY_KEY);
    if ((size_ + 1) * 10 > keys_.size() * 7) {
      rehash(std::max<size_t>(16, keys_.size() * 2));
    }
    size_t slot = static_cast<size_t>(hashBoardKeySlot(key)) & mask_;
    while (true) {
      const BoardKey cur = keys_[slot];
      if (cur == EMPTY_KEY) {
        keys_[slot] = key;
        counts_[slot] = delta;
        size_++;
        return;
      }
      if (cur == key) {
        const uint64_t sum = static_cast<uint64_t>(counts_[slot]) + delta;
        counts_[slot] = (sum > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(sum);
        return;
      }
      slot = (slot + 1) & mask_;
    }
  }

  size_t size() const noexcept { return size_; }

  template <typename Fn> void forEach(Fn &&fn) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] != EMPTY_KEY) {
        fn(keys_[i], counts_[i]);
      }
    }
  }

private:
  std::vector<BoardKey> keys_;
  std::vector<uint32_t> counts_;
  size_t mask_ = 0;
  size_t size_ = 0;

  void rehash(size_t newCap) {
    size_t cap = 16;
    while (cap < newCap) {
      cap <<= 1;
    }
    std::vector<BoardKey> nextKeys(cap, EMPTY_KEY);
    std::vector<uint32_t> nextCounts(cap, 0U);
    const size_t nextMask = cap - 1;
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] != EMPTY_KEY) {
        size_t slot = static_cast<size_t>(hashBoardKeySlot(keys_[i])) & nextMask;
        while (nextKeys[slot] != EMPTY_KEY) {
          slot = (slot + 1) & nextMask;
        }
        nextKeys[slot] = keys_[i];
        nextCounts[slot] = counts_[i];
      }
    }
    keys_.swap(nextKeys);
    counts_.swap(nextCounts);
    mask_ = nextMask;
  }
};

// 64-way sharded concurrent visit counter using uncorrelated shard & slot hashes (safe to 50M+).
class ShardedVisitCounter20 {
public:
  static constexpr size_t NUM_SHARDS = 64;
  using ShardBuckets = std::array<std::vector<BoardKey>, NUM_SHARDS>;

  void addBatch(const std::vector<BoardKey> &batch, ShardBuckets &byShard) {
    for (auto &b : byShard)
      b.clear();
    for (BoardKey k : batch) {
      const size_t sh = static_cast<size_t>(hashBoardKeyShard(k)) & (NUM_SHARDS - 1);
      byShard[sh].push_back(k);
    }
    for (size_t sh = 0; sh < NUM_SHARDS; ++sh) {
      if (byShard[sh].empty())
        continue;
      std::lock_guard<std::mutex> lk(shards_[sh].mu);
      for (BoardKey k : byShard[sh]) {
        shards_[sh].map.increment(k, 1U);
      }
    }
  }

  void addBatch(const std::vector<BoardKey> &batch) {
    ShardBuckets byShard;
    addBatch(batch, byShard);
  }

  // Drains all shards into a flat vector while immediately releasing each shard's hash table.
  std::vector<std::pair<BoardKey, uint32_t>> drainAndRelease() {
    size_t total = 0;
    for (size_t sh = 0; sh < NUM_SHARDS; ++sh) {
      total += shards_[sh].map.size();
    }
    std::vector<std::pair<BoardKey, uint32_t>> out;
    out.reserve(total);
    for (size_t sh = 0; sh < NUM_SHARDS; ++sh) {
      shards_[sh].map.forEach([&](BoardKey k, uint32_t v) { out.emplace_back(k, v); });
      shards_[sh].map.clearAndRelease();
    }
    return out;
  }

private:
  struct alignas(128) Shard {
    std::mutex mu;
    FlatVisitMap20 map;
  };
  std::array<Shard, NUM_SHARDS> shards_;
};

struct StateGenConfig20 {
  RngMode rngMode = RngMode::Nes;
  int level = 19;
  double gamma = 0.995;
  int numGames = 30000;
  int maxPiecesPerGame = 500;
  int maxLinesPerGame = 100; // 100 for L19, 130 for L18, 0 = uncapped
  uint64_t baseSeed = 10000000ULL;
  double epsilon = 0.12;         // 50/50 exploration split rate
  int exploreTopK = 6;           // Runner-up / frontier probe beam
  double outMargin = 0.6;        // Partial-OUT safety margin during TB+NN rollouts
  uint32_t minVisits = 2;        // Prune rare tail states below this count
  size_t maxBoards = 5000000;    // Output cap (0 = unlimited)
  uint32_t siblingMinVisits = 4; // Expand core sibling replies on >= M-visit boards (0 = off)
  BoardPriorityMode priorityMode = BoardPriorityMode::Visits;
  double nnErrorWeight = 2.0;    // Multiplier weight on |E[V_tb] - E[V_nn]|
  double priorityErrCap = 3.0;   // Scaled for normalized Tetris-equivalents
  double frontierBoost = 2.0;    // Bonus for newly discovered out-of-TB frontier boards
  size_t protectedCore = 500000; // Top-K most-visited boards immune to nn-error eviction
  int numThreads = 1;
  bool verbose = true;
};

struct StateGenStats20 {
  uint64_t totalMoves = 0;
  uint64_t inKeyMoves = 0;
  size_t rawUniqueBoards = 0;
  size_t siblingBoardsAdded = 0;
  size_t outputBoards = 0;
  double elapsedSeconds = 0.0;
};

namespace detail {

// Collects unique greedy TB + NN sibling children on core boards (visits >= siblingMinVisits)
// with geometric per-worker dedup watermarks and pairwise set_union to prevent RAM spikes & O(N^2).
inline std::vector<BoardKey>
expandCoreSiblings20(const std::vector<std::pair<BoardKey, uint32_t>> &rawEntries,
                     const StateGenConfig20 &cfg, const IndexedDasTablebase *fastTb,
                     const TinyDasNet *nn) {
  if (cfg.siblingMinVisits == 0) {
    return {};
  }
  const bool tbActive = (fastTb != nullptr && fastTb->isLoaded());
  const bool nnActive = (nn != nullptr && nn->isLoaded());
  if (!tbActive && !nnActive) {
    return {};
  }

  std::vector<size_t> coreIdx;
  for (size_t i = 0; i < rawEntries.size(); ++i) {
    if (rawEntries[i].second >= cfg.siblingMinVisits) {
      coreIdx.push_back(i);
    }
  }
  if (coreIdx.empty()) {
    return {};
  }

  const int threads = std::max(1, cfg.numThreads);
  const int gravity = getGravity20(cfg.level);
  const StepDiscountTable20 disc = StepDiscountTable20::make(
      cfg.gamma, tbActive ? fastTb->discountMode() : DiscountMode::PerPiece);
  constexpr size_t LOCAL_DEDUP_STEP = 1000000;

  std::vector<std::vector<BoardKey>> perThread(static_cast<size_t>(threads));
  std::atomic<size_t> nextPos{0};

  runWorkerPool(threads, coreIdx.size(), 256, [&](int tid) {
    auto &out = perThread[static_cast<size_t>(tid)];
    size_t nextDedupAt = LOCAL_DEDUP_STEP;
    PlacementList pl;
    uint16_t afterRows[BOARD_ROWS];
    float childRow[NUM_PIECES];

    while (true) {
      const size_t start = nextPos.fetch_add(256, std::memory_order_relaxed);
      if (start >= coreIdx.size())
        break;
      const size_t end = std::min(coreIdx.size(), start + 256);

      for (size_t idx = start; idx < end; ++idx) {
        const BoardKey parentKey = rawEntries[coreIdx[idx]].first;
        const Board20 board = Board20::fromKey(parentKey);
        const TuckSetupInfo tucks = computeTuckSetupInfo(board);

        for (int c = 0; c < NUM_PIECES; ++c) {
          findPlacements10Hz(board, tucks, c, gravity, pl);
          if (pl.count == 0)
            continue;

          bool anyInTb = false;
          double bestTbQ[NUM_PIECES] = {};
          BoardKey bestTbKey[NUM_PIECES];
          for (int n = 0; n < NUM_PIECES; ++n)
            bestTbKey[n] = EMPTY_KEY;

          bool haveNnBest = false;
          double bestNnQ = -std::numeric_limits<double>::infinity();
          BoardKey bestNnKey = EMPTY_KEY;

          for (int m = 0; m < pl.count; ++m) {
            const CompactPlacement &p = pl.data[m];
            const uint8_t lc = applyPlacement20(board, c, p.rot, p.x, p.y, afterRows);
            const int startRow = (lc == 0)
                                     ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                            static_cast<int>(p.y)))
                                     : 0;
            BoardKey childKey;
            if (!encodeBoardKey(afterRows, childKey, startRow)) {
              continue;
            }
            const double rew = REWARD_NORM_TETRIS[lc];
            const double dFac = disc.f[lc];
            if (tbActive && fastTb->lookup(childKey, childRow)) {
              anyInTb = true;
              for (int n = 0; n < NUM_PIECES; ++n) {
                const double q = rew + dFac * static_cast<double>(childRow[n]);
                if (bestTbKey[n] == EMPTY_KEY || q > bestTbQ[n]) {
                  bestTbQ[n] = q;
                  bestTbKey[n] = childKey;
                }
              }
            }
            if (nnActive) {
              const double qNn = rew + dFac * nn->evalNextPieceExpected(afterRows, cfg.rngMode, c);
              if (!haveNnBest || qNn > bestNnQ) {
                haveNnBest = true;
                bestNnQ = qNn;
                bestNnKey = childKey;
              }
            }
          }

          BoardKey emitted[NUM_PIECES + 1];
          int emitCnt = 0;
          if (haveNnBest && bestNnKey != EMPTY_KEY) {
            emitted[emitCnt++] = bestNnKey;
            out.push_back(bestNnKey);
          }
          if (anyInTb) {
            for (int n = 0; n < NUM_PIECES; ++n) {
              const BoardKey k = bestTbKey[n];
              if (k != EMPTY_KEY && std::find(emitted, emitted + emitCnt, k) == emitted + emitCnt) {
                emitted[emitCnt++] = k;
                out.push_back(k);
              }
            }
          }
        }

        if (out.size() >= nextDedupAt) {
          std::sort(out.begin(), out.end());
          out.erase(std::unique(out.begin(), out.end()), out.end());
          nextDedupAt = std::max(out.size() * 2, out.size() + LOCAL_DEDUP_STEP);
        }
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  });

  // Pairwise set_union frees thread vectors incrementally, avoiding concat RAM spikes at 20M-50M.
  while (perThread.size() > 1) {
    std::vector<std::vector<BoardKey>> nextLevel;
    nextLevel.reserve((perThread.size() + 1) / 2);
    for (size_t i = 0; i < perThread.size(); i += 2) {
      if (i + 1 == perThread.size()) {
        nextLevel.push_back(std::move(perThread[i]));
      } else {
        std::vector<BoardKey> u;
        u.reserve(std::max(perThread[i].size(), perThread[i + 1].size()));
        std::set_union(perThread[i].begin(), perThread[i].end(), perThread[i + 1].begin(),
                       perThread[i + 1].end(), std::back_inserter(u));
        std::vector<BoardKey>().swap(perThread[i]);
        std::vector<BoardKey>().swap(perThread[i + 1]);
        nextLevel.push_back(std::move(u));
      }
    }
    perThread.swap(nextLevel);
  }
  return perThread.empty() ? std::vector<BoardKey>{} : std::move(perThread[0]);
}

struct PrioritizedBoard20 {
  static constexpr uint8_t TIER_EMPTY = 0;
  static constexpr uint8_t TIER_PRIOR = 1;
  static constexpr uint8_t TIER_CORE = 2;
  static constexpr uint8_t TIER_SCORED = 3;

  BoardKey key;
  double score;
  uint32_t visits;
  uint8_t tier;
};
static_assert(sizeof(PrioritizedBoard20) == 32,
              "PrioritizedBoard20 should stay packed at 32 bytes");

inline int resolveRtDasBuckets(const IndexedDasRtTablebase *rtTb, const TinyDasNet *nn) noexcept {
  if (rtTb != nullptr && rtTb->isLoaded()) {
    return rtTb->numDasBuckets();
  }
  if (nn != nullptr && nn->isLoaded() && nn->numDasBuckets() >= 2) {
    return nn->numDasBuckets();
  }
  return MAX_DAS_BUCKETS;
}

// Deterministic 3-way merge (rawEntries + priorBoards + coreSiblings), prioritized eviction under
// maxBoards, and strict ascending sort. Thread-count invariant (1T == MT).
// Supports both IndexedDasTablebase (SRTBV003) and IndexedDasRtTablebase (SRTBV004).
template <typename TbType = IndexedDasTablebase>
inline std::vector<BoardKey>
mergeFilterAndCap20(std::vector<std::pair<BoardKey, uint32_t>> &rawEntries,
                    const std::vector<BoardKey> &priorBoards, std::vector<BoardKey> &coreSiblings,
                    const StateGenConfig20 &cfg, const TbType *fastTb, const TinyDasNet *nn) {
  static_assert(std::is_same_v<TbType, IndexedDasTablebase>
                    || std::is_same_v<TbType, IndexedDasRtTablebase>,
                "mergeFilterAndCap20 supports IndexedDasTablebase or IndexedDasRtTablebase");
  std::sort(rawEntries.begin(), rawEntries.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  // Pass 1: count exact filtered union size so items.reserve never over-allocates at 20M-50M.
  size_t exactCount = 0;
  bool sawEmpty = false;
  {
    size_t ir = 0, ip = 0, is = 0;
    while (ir < rawEntries.size() || ip < priorBoards.size() || is < coreSiblings.size()) {
      BoardKey cur = EMPTY_KEY;
      if (ir < rawEntries.size() && rawEntries[ir].first < cur)
        cur = rawEntries[ir].first;
      if (ip < priorBoards.size() && priorBoards[ip] < cur)
        cur = priorBoards[ip];
      if (is < coreSiblings.size() && coreSiblings[is] < cur)
        cur = coreSiblings[is];

      uint32_t visits = 0;
      bool inPrior = false, inSib = false;
      if (ir < rawEntries.size() && rawEntries[ir].first == cur) {
        visits = rawEntries[ir].second;
        ir++;
      }
      if (ip < priorBoards.size() && priorBoards[ip] == cur) {
        inPrior = true;
        ip++;
      }
      if (is < coreSiblings.size() && coreSiblings[is] == cur) {
        inSib = true;
        is++;
      }
      const bool isEmptyBoard = (cur == EMPTY_BOARD_KEY);
      if (isEmptyBoard)
        sawEmpty = true;
      if (!isEmptyBoard && !inPrior && !inSib && visits < cfg.minVisits)
        continue;
      exactCount++;
    }
    if (!sawEmpty)
      exactCount++;
  }

  // Fast path: when no eviction is required, emit sorted BoardKeys directly without allocating the
  // 32 B/item PrioritizedBoard20 buffer or re-sorting.
  if (cfg.maxBoards == 0 || exactCount <= cfg.maxBoards) {
    std::vector<BoardKey> out;
    out.reserve(exactCount);
    if (!sawEmpty) {
      out.push_back(EMPTY_BOARD_KEY);
    }
    size_t ir = 0, ip = 0, is = 0;
    while (ir < rawEntries.size() || ip < priorBoards.size() || is < coreSiblings.size()) {
      BoardKey cur = EMPTY_KEY;
      if (ir < rawEntries.size() && rawEntries[ir].first < cur)
        cur = rawEntries[ir].first;
      if (ip < priorBoards.size() && priorBoards[ip] < cur)
        cur = priorBoards[ip];
      if (is < coreSiblings.size() && coreSiblings[is] < cur)
        cur = coreSiblings[is];

      uint32_t visits = 0;
      bool inPrior = false, inSib = false;
      if (ir < rawEntries.size() && rawEntries[ir].first == cur) {
        visits = rawEntries[ir].second;
        ir++;
      }
      if (ip < priorBoards.size() && priorBoards[ip] == cur) {
        inPrior = true;
        ip++;
      }
      if (is < coreSiblings.size() && coreSiblings[is] == cur) {
        inSib = true;
        is++;
      }
      const bool isEmptyBoard = (cur == EMPTY_BOARD_KEY);
      if (!isEmptyBoard && !inPrior && !inSib && visits < cfg.minVisits)
        continue;
      out.push_back(cur);
    }
    std::vector<std::pair<BoardKey, uint32_t>>().swap(rawEntries);
    std::vector<BoardKey>().swap(coreSiblings);
    return out;
  }

  std::vector<PrioritizedBoard20> items;
  items.reserve(exactCount);
  sawEmpty = false;
  {
    size_t ir = 0, ip = 0, is = 0;
    while (ir < rawEntries.size() || ip < priorBoards.size() || is < coreSiblings.size()) {
      BoardKey cur = EMPTY_KEY;
      if (ir < rawEntries.size() && rawEntries[ir].first < cur)
        cur = rawEntries[ir].first;
      if (ip < priorBoards.size() && priorBoards[ip] < cur)
        cur = priorBoards[ip];
      if (is < coreSiblings.size() && coreSiblings[is] < cur)
        cur = coreSiblings[is];

      uint32_t visits = 0;
      bool inPrior = false, inSib = false;
      if (ir < rawEntries.size() && rawEntries[ir].first == cur) {
        visits = rawEntries[ir].second;
        ir++;
      }
      if (ip < priorBoards.size() && priorBoards[ip] == cur) {
        inPrior = true;
        ip++;
      }
      if (is < coreSiblings.size() && coreSiblings[is] == cur) {
        inSib = true;
        is++;
      }
      const bool isEmptyBoard = (cur == EMPTY_BOARD_KEY);
      if (isEmptyBoard)
        sawEmpty = true;
      if (!isEmptyBoard && !inPrior && !inSib && visits < cfg.minVisits)
        continue;

      uint8_t tier = PrioritizedBoard20::TIER_SCORED;
      if (isEmptyBoard) {
        tier = PrioritizedBoard20::TIER_EMPTY;
      } else if (inPrior) {
        tier = PrioritizedBoard20::TIER_PRIOR;
      }
      items.push_back({cur, static_cast<double>(visits), visits, tier});
    }
    if (!sawEmpty) {
      items.push_back({EMPTY_BOARD_KEY, 0.0, 0U, PrioritizedBoard20::TIER_EMPTY});
    }
  }
  // Free rawEntries & coreSiblings immediately to keep peak RAM tight.
  std::vector<std::pair<BoardKey, uint32_t>>().swap(rawEntries);
  std::vector<BoardKey>().swap(coreSiblings);

  // Promote top `protectedCore` highest-visit TIER_SCORED boards to TIER_CORE.
  if (cfg.priorityMode == BoardPriorityMode::NnError && cfg.protectedCore > 0) {
    std::vector<uint32_t> eligible;
    eligible.reserve(items.size() > priorBoards.size() ? (items.size() - priorBoards.size()) : 0U);
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].tier == PrioritizedBoard20::TIER_SCORED) {
        eligible.push_back(static_cast<uint32_t>(i));
      }
    }
    const size_t kCore = std::min(cfg.protectedCore, eligible.size());
    if (kCore > 0) {
      auto byVisits = [&](uint32_t a, uint32_t b) noexcept {
        if (items[a].visits != items[b].visits)
          return items[a].visits > items[b].visits;
        return items[a].key < items[b].key;
      };
      if (kCore < eligible.size()) {
        std::nth_element(eligible.begin(), eligible.begin() + static_cast<std::ptrdiff_t>(kCore),
                         eligible.end(), byVisits);
      }
      for (size_t i = 0; i < kCore; ++i) {
        items[eligible[i]].tier = PrioritizedBoard20::TIER_CORE;
      }
    }
  }

  // Score remaining TIER_SCORED items in parallel (deterministic per-item pure function).
  // For SRTBV004, nn-error averages |E_c[V_tb(b, c, k)] - E_c[V_nn(b, c, k)]| across DAS buckets k.
  constexpr bool IS_RT_TB = std::is_same_v<TbType, IndexedDasRtTablebase>;
  const bool tbActive = (fastTb != nullptr && fastTb->isLoaded());
  const int rtK = [&]() {
    if constexpr (IS_RT_TB) {
      return resolveRtDasBuckets(fastTb, nn);
    } else {
      return 1;
    }
  }();
  const bool useNnErr = [&]() {
    if (cfg.priorityMode != BoardPriorityMode::NnError || nn == nullptr || !nn->isLoaded()) {
      return false;
    }
    if constexpr (IS_RT_TB) {
      return nnSupportsDasBuckets(*nn, rtK);
    } else {
      return true;
    }
  }();
  if (useNnErr) {
    const int threads = std::max(1, cfg.numThreads);
    std::atomic<size_t> nextIdx{0};
    runWorkerPool(threads, items.size(), 4096, [&](int /*tid*/) {
      float tbRow[MAX_RT_VALUES_PER_BOARD];
      float nnRow[MAX_RT_VALUES_PER_BOARD];
      uint16_t rows[BOARD_ROWS];
      while (true) {
        const size_t s = nextIdx.fetch_add(1024, std::memory_order_relaxed);
        if (s >= items.size())
          break;
        const size_t e = std::min(items.size(), s + 1024);
        for (size_t i = s; i < e; ++i) {
          if (items[i].tier != PrioritizedBoard20::TIER_SCORED)
            continue;
          const double vCount = static_cast<double>(std::max<uint32_t>(1U, items[i].visits));
          if (tbActive && fastTb->lookup(items[i].key, tbRow)) {
            double err = 0.0;
            if constexpr (IS_RT_TB) {
              decodeBoardKey(items[i].key, rows);
              evalNnRtRow(*nn, rows, rtK, nnRow);
              double sumErr = 0.0;
              for (int k = 0; k < rtK; ++k) {
                double tbExp = 0.0;
                double nnExp = 0.0;
                for (int c = 0; c < NUM_PIECES; ++c) {
                  const double p = openingPieceProb(cfg.rngMode, c);
                  const int idx = rtValueIndex(c, k, rtK);
                  tbExp += p * static_cast<double>(tbRow[idx]);
                  const double nv = std::isfinite(nnRow[idx])
                                        ? std::max(0.0, static_cast<double>(nnRow[idx]))
                                        : 0.0;
                  nnExp += p * nv;
                }
                if (std::isfinite(tbExp) && std::isfinite(nnExp)) {
                  sumErr += std::abs(tbExp - nnExp);
                }
              }
              err = std::min(cfg.priorityErrCap, sumErr / static_cast<double>(rtK));
            } else {
              const double tbExp = expectedValueFromRow(tbRow, cfg.rngMode);
              decodeBoardKey(items[i].key, rows);
              const double nnExp = nn->evalExpected(rows, cfg.rngMode);
              err = (std::isfinite(tbExp) && std::isfinite(nnExp))
                        ? std::min(cfg.priorityErrCap, std::abs(tbExp - nnExp))
                        : 0.0;
            }
            items[i].score = vCount * (1.0 + cfg.nnErrorWeight * err);
          } else {
            items[i].score = vCount * (1.0 + cfg.frontierBoost);
          }
        }
      }
    });
  }

  auto cmpKeep = [](const PrioritizedBoard20 &a, const PrioritizedBoard20 &b) noexcept {
    if (a.tier != b.tier)
      return a.tier < b.tier;
    if (a.score != b.score)
      return a.score > b.score;
    if (a.visits != b.visits)
      return a.visits > b.visits;
    return a.key < b.key;
  };
  std::nth_element(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(cfg.maxBoards),
                   items.end(), cmpKeep);
  items.resize(cfg.maxBoards);

  std::vector<BoardKey> out;
  out.reserve(items.size());
  for (const auto &it : items)
    out.push_back(it.key);
  std::vector<PrioritizedBoard20>().swap(items);
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace detail

// Runs TB+NN 50/50 exploration rollouts (half TB runner-ups, half NN frontier probes excluding
// the greedy TB move) and merges/evicts down to cfg.maxBoards deterministically.
inline std::vector<BoardKey> generateStateSetTbNn20(const StateGenConfig20 &cfg,
                                                    const IndexedDasTablebase *fastTb,
                                                    const TinyDasNet *nn,
                                                    const std::vector<BoardKey> &priorBoards,
                                                    StateGenStats20 &statsOut) {
  const auto t0 = std::chrono::steady_clock::now();
  statsOut = {};

  const bool tbActive = (fastTb != nullptr && fastTb->isLoaded());
  const bool nnActive = (nn != nullptr && nn->isLoaded());
  const int threads = std::max(1, cfg.numThreads);
  const int gravity = getGravity20(cfg.level);
  const StepDiscountTable20 disc = StepDiscountTable20::make(
      cfg.gamma, tbActive ? fastTb->discountMode() : DiscountMode::PerPiece);

  ShardedVisitCounter20 counter;
  std::atomic<int> nextGame{0};
  std::atomic<uint64_t> totalMoves{0};
  std::atomic<uint64_t> inKeyMoves{0};

  detail::runWorkerPool(threads, static_cast<size_t>(cfg.numGames), 16, [&](int /*tid*/) {
    PlacementList pl;
    uint16_t afterRows[BOARD_ROWS];
    std::vector<BoardKey> batch;
    batch.reserve(16384);
    ShardedVisitCounter20::ShardBuckets byShard;
    uint64_t locMoves = 0, locInKey = 0;

    struct MoveCand {
      double hybridScore;
      double nnScore;
      uint8_t lc;
      bool hasNnScore;
      uint16_t rows[BOARD_ROWS];
    };
    std::vector<MoveCand> cands;
    cands.reserve(64);
    std::vector<int> order;
    order.reserve(64);

    while (true) {
      const int g = nextGame.fetch_add(1, std::memory_order_relaxed);
      if (g >= cfg.numGames)
        break;

      Rng rng(cfg.baseSeed + static_cast<uint64_t>(g) * 1009ULL);
      int curP = rng.sampleOpeningPiece(cfg.rngMode);
      int nextP = rng.sampleNextPiece(cfg.rngMode, curP);
      Board20 board = Board20::empty();
      int totalLines = 0;

      for (int step = 0; step < cfg.maxPiecesPerGame; ++step) {
        if (cfg.maxLinesPerGame > 0 && totalLines >= cfg.maxLinesPerGame)
          break;

        BoardKey curKey;
        if (encodeBoardKey(board.rows(), curKey, board.topNonEmptyRow)) {
          batch.push_back(curKey);
          locInKey++;
          if (batch.size() >= 16384) {
            counter.addBatch(batch, byShard);
            batch.clear();
          }
        }
        locMoves++;

        findPlacements10Hz(board, curP, gravity, pl);
        if (pl.count == 0)
          break;

        cands.clear();
        bool anyInTb = false;
        int greedyIdx = 0;
        double bestHybrid = -std::numeric_limits<double>::infinity();

        auto computeNnScore = [&](const uint16_t rows[BOARD_ROWS], uint8_t lc,
                                  double &outCont) noexcept -> double {
          const double rew = REWARD_NORM_TETRIS[lc];
          outCont = 0.0;
          if (!nnActive)
            return rew;
          const bool topClear = ((rows[0] | rows[1] | rows[2]) & FULL_ROW_MASK) == 0U;
          if (topClear || hasAnyLegalMove10Hz(Board20::fromRows(rows), nextP, gravity)) {
            outCont = std::max(0.0, static_cast<double>(nn->evalPiece(rows, nextP)));
          }
          return rew + disc.f[lc] * outCont;
        };

        for (int m = 0; m < pl.count; ++m) {
          const CompactPlacement &p = pl.data[m];
          const uint8_t lc = applyPlacement20(board, curP, p.rot, p.x, p.y, afterRows);
          const double rew = REWARD_NORM_TETRIS[lc];
          const double dFac = disc.f[lc];
          const int startRow = (lc == 0)
                                   ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                          static_cast<int>(p.y)))
                                   : 0;
          BoardKey afterKey;
          const bool fitsKey = encodeBoardKey(afterRows, afterKey, startRow);
          float vTb = 0.0f;
          const bool inTb = tbActive && fitsKey && fastTb->lookupPiece(afterKey, nextP, vTb);

          MoveCand mc{};
          mc.lc = lc;
          std::memcpy(mc.rows, afterRows, sizeof(afterRows));
          if (inTb) {
            anyInTb = true;
            mc.hybridScore = rew + dFac * static_cast<double>(vTb);
            mc.nnScore = 0.0;
            mc.hasNnScore = false; // Lazily evaluated only on NN frontier-probe steps.
          } else if (fitsKey) {
            double nnCont = 0.0;
            mc.nnScore = computeNnScore(afterRows, lc, nnCont);
            mc.hasNnScore = true;
            mc.hybridScore = nnActive ? (rew + dFac * std::max(0.0, nnCont - cfg.outMargin)) : rew;
          } else {
            // Unrepresentable (!fitsKey, >= 10 holes) boards cannot enter the state set or TB,
            // so zero NN continuation keeps state generation from chasing OOD multi-hole sinks.
            mc.nnScore = rew;
            mc.hasNnScore = true;
            mc.hybridScore = rew;
          }
          cands.push_back(mc);
        }

        // Re-score all-OUT boards without margin clamping.
        for (int m = 0; m < static_cast<int>(cands.size()); ++m) {
          const double sc = anyInTb ? cands[static_cast<size_t>(m)].hybridScore
                                    : cands[static_cast<size_t>(m)].nnScore;
          if (m == 0 || sc > bestHybrid) {
            bestHybrid = sc;
            greedyIdx = m;
          }
        }

        int chosen = greedyIdx;
        if (cfg.epsilon > 0.0 && cands.size() > 1 && rng.nextUniform01() < cfg.epsilon) {
          // 50/50 split: half TB/hybrid runner-ups, half NN frontier probes excluding greedy move.
          const bool useNnProbe = nnActive && (rng.nextUniform01() < 0.5);
          order.resize(cands.size());
          for (size_t i = 0; i < cands.size(); ++i)
            order[i] = static_cast<int>(i);
          if (useNnProbe) {
            // Populate lazy NN scores on in-TB candidates only when actually probing via NN.
            for (auto &mc : cands) {
              if (!mc.hasNnScore) {
                double dummyCont = 0.0;
                mc.nnScore = computeNnScore(mc.rows, mc.lc, dummyCont);
                mc.hasNnScore = true;
              }
            }
            order.erase(std::remove(order.begin(), order.end(), greedyIdx), order.end());
            std::sort(order.begin(), order.end(), [&](int a, int b) {
              const double sa = cands[static_cast<size_t>(a)].nnScore;
              const double sb = cands[static_cast<size_t>(b)].nnScore;
              return (sa != sb) ? (sa > sb) : (a < b);
            });
            const int hi =
                std::min<int>(static_cast<int>(order.size()), std::max(1, cfg.exploreTopK));
            chosen = order[static_cast<size_t>(rng.nextInt(hi))];
          } else {
            std::sort(order.begin(), order.end(), [&](int a, int b) {
              const double sa = anyInTb ? cands[static_cast<size_t>(a)].hybridScore
                                        : cands[static_cast<size_t>(a)].nnScore;
              const double sb = anyInTb ? cands[static_cast<size_t>(b)].hybridScore
                                        : cands[static_cast<size_t>(b)].nnScore;
              return (sa != sb) ? (sa > sb) : (a < b);
            });
            const int hi =
                std::min<int>(static_cast<int>(order.size()), std::max(2, cfg.exploreTopK));
            chosen = order[static_cast<size_t>(1 + rng.nextInt(hi - 1))];
          }
        }

        totalLines += cands[static_cast<size_t>(chosen)].lc;
        board = Board20::fromRows(cands[static_cast<size_t>(chosen)].rows);
        curP = nextP;
        nextP = rng.sampleNextPiece(cfg.rngMode, curP);
      }
    }
    if (!batch.empty()) {
      counter.addBatch(batch, byShard);
    }
    totalMoves.fetch_add(locMoves, std::memory_order_relaxed);
    inKeyMoves.fetch_add(locInKey, std::memory_order_relaxed);
  });

  std::vector<std::pair<BoardKey, uint32_t>> rawEntries = counter.drainAndRelease();
  statsOut.totalMoves = totalMoves.load();
  statsOut.inKeyMoves = inKeyMoves.load();
  statsOut.rawUniqueBoards = rawEntries.size();

  std::vector<BoardKey> coreSiblings = detail::expandCoreSiblings20(rawEntries, cfg, fastTb, nn);
  statsOut.siblingBoardsAdded = coreSiblings.size();

  std::vector<BoardKey> result =
      detail::mergeFilterAndCap20(rawEntries, priorBoards, coreSiblings, cfg, fastTb, nn);
  statsOut.outputBoards = result.size();
  statsOut.elapsedSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

namespace detail {

// Collects unique greedy V_RT + NN sibling children on core boards (visits >= siblingMinVisits)
// across all starting DAS buckets, deduplicating lock-cell transitions per (board, piece).
inline std::vector<BoardKey>
expandCoreSiblingsRt20(const std::vector<std::pair<BoardKey, uint32_t>> &rawEntries,
                       const StateGenConfig20 &cfg, const IndexedDasRtTablebase *rtTb,
                       const DasRtMoveTables &tables, const TinyDasNet *nn) {
  if (cfg.siblingMinVisits == 0) {
    return {};
  }
  const bool tbActive = (rtTb != nullptr && rtTb->isLoaded());
  const int K = resolveRtDasBuckets(rtTb, nn);
  const bool nnActive = (nn != nullptr && nnSupportsDasBuckets(*nn, K));
  if (!tbActive && !nnActive) {
    return {};
  }

  std::vector<uint32_t> coreIdx;
  for (size_t i = 0; i < rawEntries.size(); ++i) {
    if (rawEntries[i].second >= cfg.siblingMinVisits) {
      coreIdx.push_back(static_cast<uint32_t>(i));
    }
  }
  if (coreIdx.empty()) {
    return {};
  }

  const int threads = std::max(1, cfg.numThreads);
  const int vpb = NUM_PIECES * K;
  const StepDiscountTable20 disc =
      StepDiscountTable20::make(cfg.gamma, tbActive ? rtTb->discountMode() : DiscountMode::PerLine);
  const auto transProb = buildTransitionProbMatrix(cfg.rngMode);
  int startDas[MAX_DAS_BUCKETS];
  for (int k = 0; k < K; ++k) {
    startDas[k] = dasBucketRepresentativeCharge(k, K);
  }
  constexpr size_t LOCAL_DEDUP_STEP = 1000000;

  std::vector<std::vector<BoardKey>> perThread(static_cast<size_t>(threads));
  std::atomic<size_t> nextPos{0};

  runWorkerPool(threads, coreIdx.size(), 256, [&](int tid) {
    auto &out = perThread[static_cast<size_t>(tid)];
    size_t nextDedupAt = LOCAL_DEDUP_STEP;
    auto ws = std::make_unique<RtDecisionWorkspace>();
    std::vector<BoardKey> cellKeys(RtDecisionWorkspace::MAX_CELLS, EMPTY_KEY);
    uint16_t afterRows[BOARD_ROWS];
    float childRow[MAX_RT_VALUES_PER_BOARD];

    while (true) {
      const size_t start = nextPos.fetch_add(256, std::memory_order_relaxed);
      if (start >= coreIdx.size())
        break;
      const size_t end = std::min(coreIdx.size(), start + 256);

      for (size_t idx = start; idx < end; ++idx) {
        const BoardKey parentKey = rawEntries[coreIdx[idx]].first;
        const Board20 board = Board20::fromKey(parentKey);

        for (int c = 0; c < NUM_PIECES; ++c) {
          // When no TB is loaded, only full-DAS cells are needed for the NN sibling probe.
          const int searchStarts = tbActive ? K : 1;
          const int *searchDas = tbActive ? startDas : &startDas[K - 1];
          ws->search(tables, board, c, searchDas, searchStarts);
          const int nCells = ws->numCells();
          if (nCells == 0)
            continue;

          bool anyInTb = false;
          bool haveNnBest = false;
          double bestNnQ = -std::numeric_limits<double>::infinity();
          BoardKey bestNnKey = EMPTY_KEY;

          for (int i = 0; i < nCells; ++i) {
            const uint8_t lc = ws->computeAfterRows(board, c, i, afterRows);
            const int sr = (lc == 0) ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                            static_cast<int>(ws->cell(i).y)))
                                     : 0;
            BoardKey childKey;
            const bool fits = encodeBoardKey(afterRows, childKey, sr);
            cellKeys[static_cast<size_t>(i)] = fits ? childKey : EMPTY_KEY;
            const double rew = REWARD_NORM_TETRIS[lc];
            const double dFac = disc.f[lc];
            if (tbActive) {
              double *qc = ws->q(i);
              if (fits && rtTb->lookup(childKey, childRow)) {
                anyInTb = true;
                for (int o = 0; o < vpb; ++o) {
                  qc[o] = rew + dFac * static_cast<double>(childRow[o]);
                }
              } else {
                for (int o = 0; o < vpb; ++o) {
                  qc[o] = rew;
                }
              }
            }
            // Probe the NN's preferred full-DAS sibling to pull high-value frontier boards in.
            if (nnActive && fits) {
              const double qNn =
                  rew + dFac * evalNnNextPieceExpectedRt(*nn, afterRows, cfg.rngMode, c, K - 1, K);
              if (!haveNnBest || qNn > bestNnQ) {
                haveNnBest = true;
                bestNnQ = qNn;
                bestNnKey = childKey;
              }
            }
          }

          BoardKey emitted[NUM_PIECES * MAX_DAS_BUCKETS + 1];
          int emitCnt = 0;
          if (haveNnBest && bestNnKey != EMPTY_KEY) {
            emitted[emitCnt++] = bestNnKey;
            out.push_back(bestNnKey);
          }
          if (anyInTb) {
            for (int k = 0; k < K; ++k) {
              const RtDecisionWorkspace::Choice ch =
                  ws->chooseAction(k, K, transProb[static_cast<size_t>(c)]);
              if (!ch.any || ch.value <= 0.0)
                continue;
              for (int n = 0; n < NUM_PIECES; ++n) {
                const BoardKey ck = cellKeys[static_cast<size_t>(ch.cellForNext[n])];
                if (ck != EMPTY_KEY
                    && std::find(emitted, emitted + emitCnt, ck) == emitted + emitCnt) {
                  emitted[emitCnt++] = ck;
                  out.push_back(ck);
                }
              }
            }
          }
        }

        if (out.size() >= nextDedupAt) {
          std::sort(out.begin(), out.end());
          out.erase(std::unique(out.begin(), out.end()), out.end());
          nextDedupAt = out.size() + LOCAL_DEDUP_STEP;
        }
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    out.shrink_to_fit();
  });

  while (perThread.size() > 1) {
    std::vector<std::vector<BoardKey>> nextLevel;
    nextLevel.reserve((perThread.size() + 1) / 2);
    for (size_t i = 0; i < perThread.size(); i += 2) {
      if (i + 1 == perThread.size()) {
        nextLevel.push_back(std::move(perThread[i]));
      } else {
        std::vector<BoardKey> u;
        u.reserve(std::max(perThread[i].size(), perThread[i + 1].size()));
        std::set_union(perThread[i].begin(), perThread[i].end(), perThread[i + 1].begin(),
                       perThread[i + 1].end(), std::back_inserter(u));
        std::vector<BoardKey>().swap(perThread[i]);
        std::vector<BoardKey>().swap(perThread[i + 1]);
        nextLevel.push_back(std::move(u));
      }
    }
    perThread.swap(nextLevel);
  }
  return perThread.empty() ? std::vector<BoardKey>{} : std::move(perThread[0]);
}

} // namespace detail

// On-policy live-RT + DAS-bucket state generator (SRTBV004 + multi-bucket NNUE): runs 50/50
// exploration rollouts under DasRtMoveTables with real DAS-bucket carryover, expands core V_RT
// siblings, and merges/evicts down to cfg.maxBoards deterministically.
inline std::vector<BoardKey>
generateStateSetRt20(const StateGenConfig20 &cfg, const IndexedDasRtTablebase *rtTb,
                     const DasRtMoveTables &tables, const TinyDasNet *nn,
                     const std::vector<BoardKey> &priorBoards, StateGenStats20 &statsOut) {
  const auto t0 = std::chrono::steady_clock::now();
  statsOut = {};

  const bool tbActive = (rtTb != nullptr && rtTb->isLoaded());
  const int K = detail::resolveRtDasBuckets(rtTb, nn);
  const int vpb = NUM_PIECES * K;
  const bool nnActive = (nn != nullptr && nnSupportsDasBuckets(*nn, K));
  const int threads = std::max(1, cfg.numThreads);
  const StepDiscountTable20 disc =
      StepDiscountTable20::make(cfg.gamma, tbActive ? rtTb->discountMode() : DiscountMode::PerLine);
  const auto transProb = detail::buildTransitionProbMatrix(cfg.rngMode);

  ShardedVisitCounter20 counter;
  std::atomic<int> nextGame{0};
  std::atomic<uint64_t> totalMoves{0};
  std::atomic<uint64_t> inKeyMoves{0};

  detail::runWorkerPool(threads, static_cast<size_t>(cfg.numGames), 16, [&](int /*tid*/) {
    auto ws = std::make_unique<RtDecisionWorkspace>();
    std::vector<std::array<uint16_t, BOARD_ROWS>> cellRows(DAS_MAX_LOCK_MOVES);
    std::vector<int> bestBucketForCell(DAS_MAX_LOCK_MOVES, 0);
    std::vector<uint8_t> cellIsGuidedOut(DAS_MAX_LOCK_MOVES, 0);
    std::vector<uint8_t> cellFitsKey(DAS_MAX_LOCK_MOVES, 0);
    std::vector<int> order;
    order.reserve(DAS_MAX_LOCK_MOVES);
    std::vector<BoardKey> batch;
    batch.reserve(16384);
    ShardedVisitCounter20::ShardBuckets byShard;
    float childRow[MAX_RT_VALUES_PER_BOARD];
    float nnRow[MAX_RT_VALUES_PER_BOARD];
    uint64_t locMoves = 0, locInKey = 0;

    while (true) {
      const int g = nextGame.fetch_add(1, std::memory_order_relaxed);
      if (g >= cfg.numGames)
        break;

      Rng rng(cfg.baseSeed + static_cast<uint64_t>(g) * 1009ULL);
      int curP = rng.sampleOpeningPiece(cfg.rngMode);
      int nextP = rng.sampleNextPiece(cfg.rngMode, curP);
      int bucket = K - 1;
      Board20 board = Board20::empty();
      int totalLines = 0;

      for (int step = 0; step < cfg.maxPiecesPerGame; ++step) {
        if (cfg.maxLinesPerGame > 0 && totalLines >= cfg.maxLinesPerGame)
          break;

        BoardKey curKey;
        if (encodeBoardKey(board.rows(), curKey, board.topNonEmptyRow)) {
          batch.push_back(curKey);
          locInKey++;
          if (batch.size() >= 16384) {
            counter.addBatch(batch, byShard);
            batch.clear();
          }
        }
        locMoves++;

        // Piece 0 has next-box visibility from frame 0 (zeroRt); later pieces respect RT.
        const int repDas = dasBucketRepresentativeCharge(bucket, K);
        const bool zeroRt = (step == 0);
        ws->search(tables, board, curP, &repDas, 1, /*forceZeroRt=*/zeroRt);
        const int nCells = ws->numCells();
        if (nCells == 0)
          break;

        bool bestBucketsReady = false;
        auto ensureBestBuckets = [&]() noexcept {
          if (bestBucketsReady)
            return;
          bestBucketsReady = true;
          const DasRtSearchResult &sr = ws->result(0);
          for (int i = 0; i < nCells; ++i) {
            bestBucketForCell[static_cast<size_t>(i)] = 0;
          }
          for (int e = 0; e < sr.numEarlyMoves; ++e) {
            const int cl = ws->localCellOf(0, sr.earlyMoves[e].cellIdx);
            const int kb = dasChargeToBucket(sr.earlyMoves[e].dasAfter, K);
            if (kb > bestBucketForCell[static_cast<size_t>(cl)]) {
              bestBucketForCell[static_cast<size_t>(cl)] = kb;
            }
          }
          const int totalAdjEdges = sr.adjMoveOffset[sr.numAdjStates];
          for (int e = 0; e < totalAdjEdges; ++e) {
            const int cl = ws->localCellOf(0, sr.adjEdges[e].cellIdx);
            const int kb = dasChargeToBucket(sr.adjEdges[e].dasAfter, K);
            if (kb > bestBucketForCell[static_cast<size_t>(cl)]) {
              bestBucketForCell[static_cast<size_t>(cl)] = kb;
            }
          }
        };

        // Write raw continuation directly into ws->q(i); if anyInTb is true after scanning all
        // cells, apply cfg.outMargin in-place to guided out-of-TB cells (matching Partial-OUT).
        bool anyInTb = false;
        for (int i = 0; i < nCells; ++i) {
          auto &afterRows = cellRows[static_cast<size_t>(i)];
          const uint8_t lc = ws->computeAfterRows(board, curP, i, afterRows.data());
          const double rew = REWARD_NORM_TETRIS[lc];
          const double dFac = disc.f[lc];
          const int startRow = (lc == 0)
                                   ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                          static_cast<int>(ws->cell(i).y)))
                                   : 0;
          BoardKey afterKey;
          const bool fitsKey = encodeBoardKey(afterRows.data(), afterKey, startRow);
          cellFitsKey[static_cast<size_t>(i)] = fitsKey ? 1 : 0;
          cellIsGuidedOut[static_cast<size_t>(i)] = 0;
          double *qc = ws->q(i);

          if (tbActive && fitsKey && rtTb->lookup(afterKey, childRow)) {
            anyInTb = true;
            for (int o = 0; o < vpb; ++o) {
              qc[o] = rew + dFac * static_cast<double>(childRow[o]);
            }
          } else {
            for (int o = 0; o < vpb; ++o) {
              qc[o] = rew;
            }
            // Unrepresentable (!fitsKey, >= 10 holes) boards cannot enter the state set or TB,
            // so zero NN continuation prevents chasing OOD multi-hole sinks.
            if (nnActive && fitsKey) {
              cellIsGuidedOut[static_cast<size_t>(i)] = 1;
              const bool topClear =
                  ((afterRows[0] | afterRows[1] | afterRows[2]) & FULL_ROW_MASK) == 0U;
              const Board20 bAfter =
                  topClear ? Board20::empty() : Board20::fromRows(afterRows.data());
              evalNnRtRow(*nn, afterRows.data(), K, nnRow);
              for (int n = 0; n < NUM_PIECES; ++n) {
                const bool canSpawn =
                    topClear || !collides20(bAfter, n, SPAWN_COL, PIECES_20[n].initialY, 0);
                if (!canSpawn)
                  continue;
                for (int k = 0; k < K; ++k) {
                  const int o = rtValueIndex(n, k, K);
                  const double est = std::max(0.0, static_cast<double>(nnRow[o]));
                  qc[o] = rew + dFac * est;
                }
              }
            }
          }
        }

        if (nnActive && anyInTb && cfg.outMargin > 0.0) {
          for (int i = 0; i < nCells; ++i) {
            if (cellIsGuidedOut[static_cast<size_t>(i)] == 0U)
              continue;
            const uint8_t lc = ws->cell(i).lc;
            const double rew = REWARD_NORM_TETRIS[lc];
            const double marginDelta = disc.f[lc] * cfg.outMargin;
            double *qc = ws->q(i);
            for (int o = 0; o < vpb; ++o) {
              qc[o] = std::max(rew, qc[o] - marginDelta);
            }
          }
        }

        int greedyCell = 0;
        int greedyBucket = 0;
        if (zeroRt) {
          ensureBestBuckets();
          greedyBucket = bestBucketForCell[0];
          double bestSc = ws->q(0)[rtValueIndex(nextP, greedyBucket, K)];
          for (int i = 1; i < nCells; ++i) {
            const int kb = bestBucketForCell[static_cast<size_t>(i)];
            const double sc = ws->q(i)[rtValueIndex(nextP, kb, K)];
            if (sc > bestSc) {
              bestSc = sc;
              greedyCell = i;
              greedyBucket = kb;
            }
          }
        } else {
          const RtDecisionWorkspace::Choice ch =
              ws->chooseAction(0, K, transProb[static_cast<size_t>(curP)]);
          if (!ch.any)
            break;
          greedyCell = ch.cellForNext[nextP];
          greedyBucket = ch.bucketForNext[nextP];
        }

        int chosenCell = greedyCell;
        int chosenBucket = greedyBucket;
        if (cfg.epsilon > 0.0 && nCells > 1 && rng.nextUniform01() < cfg.epsilon) {
          ensureBestBuckets();
          const bool useNnProbe = nnActive && (rng.nextUniform01() < 0.5);
          order.clear();
          for (int i = 0; i < nCells; ++i) {
            if (i != greedyCell) {
              order.push_back(i);
            }
          }
          if (useNnProbe) {
            double nnScores[DAS_MAX_LOCK_MOVES];
            for (int i : order) {
              const auto &afterRows = cellRows[static_cast<size_t>(i)];
              const uint8_t lc = ws->cell(i).lc;
              const double rew = REWARD_NORM_TETRIS[lc];
              double cont = 0.0;
              if (cellFitsKey[static_cast<size_t>(i)] != 0U) {
                const bool topClear =
                    ((afterRows[0] | afterRows[1] | afterRows[2]) & FULL_ROW_MASK) == 0U;
                const bool canSpawn = topClear
                                      || !collides20(Board20::fromRows(afterRows.data()), nextP,
                                                     SPAWN_COL, PIECES_20[nextP].initialY, 0);
                if (canSpawn) {
                  const int kb = bestBucketForCell[static_cast<size_t>(i)];
                  cont = std::max(0.0, static_cast<double>(
                                           evalNnPieceBucket(*nn, afterRows.data(), nextP, kb, K)));
                }
              }
              nnScores[i] = rew + disc.f[lc] * cont;
            }
            std::sort(order.begin(), order.end(), [&](int a, int b) {
              return (nnScores[a] != nnScores[b]) ? (nnScores[a] > nnScores[b]) : (a < b);
            });
            const int hi =
                std::min<int>(static_cast<int>(order.size()), std::max(1, cfg.exploreTopK));
            chosenCell = order[static_cast<size_t>(rng.nextInt(hi))];
            chosenBucket = bestBucketForCell[static_cast<size_t>(chosenCell)];
          } else {
            // Rank non-greedy cells by their post-RT value for nextP and sample from the top beam.
            std::sort(order.begin(), order.end(), [&](int a, int b) {
              const double sa =
                  ws->q(a)[rtValueIndex(nextP, bestBucketForCell[static_cast<size_t>(a)], K)];
              const double sb =
                  ws->q(b)[rtValueIndex(nextP, bestBucketForCell[static_cast<size_t>(b)], K)];
              return (sa != sb) ? (sa > sb) : (a < b);
            });
            const int hi =
                std::min<int>(static_cast<int>(order.size()), std::max(1, cfg.exploreTopK - 1));
            chosenCell = order[static_cast<size_t>(rng.nextInt(hi))];
            chosenBucket = bestBucketForCell[static_cast<size_t>(chosenCell)];
          }
        }

        totalLines += ws->cell(chosenCell).lc;
        board = Board20::fromRows(cellRows[static_cast<size_t>(chosenCell)].data());
        bucket = chosenBucket;
        curP = nextP;
        nextP = rng.sampleNextPiece(cfg.rngMode, curP);
      }
    }
    if (!batch.empty()) {
      counter.addBatch(batch, byShard);
    }
    totalMoves.fetch_add(locMoves, std::memory_order_relaxed);
    inKeyMoves.fetch_add(locInKey, std::memory_order_relaxed);
  });

  std::vector<std::pair<BoardKey, uint32_t>> rawEntries = counter.drainAndRelease();
  statsOut.totalMoves = totalMoves.load();
  statsOut.inKeyMoves = inKeyMoves.load();
  statsOut.rawUniqueBoards = rawEntries.size();

  std::vector<BoardKey> coreSiblings =
      detail::expandCoreSiblingsRt20(rawEntries, cfg, rtTb, tables, nn);
  statsOut.siblingBoardsAdded = coreSiblings.size();

  std::vector<BoardKey> result =
      detail::mergeFilterAndCap20(rawEntries, priorBoards, coreSiblings, cfg, rtTb, nn);
  statsOut.outputBoards = result.size();
  statsOut.elapsedSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

} // namespace das

#endif // DAS_TABLEBASE_STATE_GEN_20ROW_HPP
