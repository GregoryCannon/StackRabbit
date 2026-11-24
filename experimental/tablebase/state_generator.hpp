#ifndef TABLEBASE_STATE_GENERATOR_HPP
#define TABLEBASE_STATE_GENERATOR_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "tablebase_io.hpp"
#include "vi_solver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace tb {

namespace detail {

template <typename NnbTablebase>
inline ChosenTransition chooseGreedyPolicyMove(uint64_t board, int pieceIdx,
                                               const NnbTablebase *tb, int heurPly,
                                               const EvalWeights &w) {
  assert(heurPly == 1 || heurPly == 2);
  if (tb != nullptr && tb->isLoaded()) {
    const TablebaseMoveChoice ch =
        chooseTablebaseMove(board, pieceIdx, *tb, /*closedWorld=*/false, heurPly, w);
    return {ch.transition, ch.tablebaseQ, ch.found};
  }
  return (heurPly == 2) ? pickBestTransition2Ply(board, pieceIdx, w)
                        : pickBestTransition1Ply(board, pieceIdx, w);
}

inline ChosenTransition chooseGreedyPolicyMove(uint64_t board, int pieceIdx, std::nullptr_t,
                                               int heurPly, const EvalWeights &w) {
  return chooseGreedyPolicyMove<MappedTablebase>(board, pieceIdx, nullptr, heurPly, w);
}

} // namespace detail

// Power-of-two open-addressing map counting saturating uint32_t visits per 40-bit board.
// Uses ~0ULL as the empty-slot sentinel since all valid boards fit in bits 0..39.
class FlatVisitMap {
public:
  explicit FlatVisitMap(size_t initialCap = 1024) { rehash(nextPow2(initialCap)); }

  void add(uint64_t board, uint32_t delta = 1) {
    assert((board & ~VALID_BOARD_MASK) == 0);
    assert(delta > 0);
    // Maintain <= 60% load factor so linear probing stays short and at least one EMPTY_KEY exists.
    if ((size_ + 1) * 10 >= keys_.size() * 6) {
      rehash(keys_.size() * 2);
    }
    uint64_t slot = hashKey(board);
    while (true) {
      const uint64_t k = keys_[static_cast<size_t>(slot)];
      if (k == EMPTY_KEY) {
        keys_[static_cast<size_t>(slot)] = board;
        counts_[static_cast<size_t>(slot)] = delta;
        ++size_;
        return;
      }
      if (k == board) {
        uint32_t &c = counts_[static_cast<size_t>(slot)];
        c = (UINT32_MAX - c < delta) ? UINT32_MAX : (c + delta);
        return;
      }
      slot = (slot + 1ULL) & mask_;
    }
  }

  uint32_t get(uint64_t board) const noexcept {
    if (size_ == 0 || board == EMPTY_KEY) {
      return 0;
    }
    uint64_t slot = hashKey(board);
    while (true) {
      const uint64_t k = keys_[static_cast<size_t>(slot)];
      if (k == EMPTY_KEY) {
        return 0;
      }
      if (k == board) {
        return counts_[static_cast<size_t>(slot)];
      }
      slot = (slot + 1ULL) & mask_;
    }
  }

  template <typename Fn> void forEach(Fn &&fn) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] != EMPTY_KEY) {
        fn(keys_[i], counts_[i]);
      }
    }
  }

  void clear() {
    std::fill(keys_.begin(), keys_.end(), EMPTY_KEY);
    size_ = 0;
  }

  size_t size() const noexcept { return size_; }

private:
  static constexpr uint64_t EMPTY_KEY = ~0ULL;
  static constexpr size_t MIN_CAPACITY = 16;
  static constexpr uint64_t HASH_MULT = 0x9E3779B97F4A7C15ULL;

  std::vector<uint64_t> keys_;
  std::vector<uint32_t> counts_;
  uint64_t mask_ = 0;
  int shift_ = 60;
  size_t size_ = 0;

  static size_t nextPow2(size_t v) {
    size_t p = MIN_CAPACITY;
    while (p < v && p <= (std::numeric_limits<size_t>::max() >> 1)) {
      p <<= 1;
    }
    return p;
  }

  uint64_t hashKey(uint64_t b) const noexcept {
    const uint64_t x = b ^ (b >> 17);
    return (x * HASH_MULT) >> shift_;
  }

  void rehash(size_t newCap) {
    std::vector<uint64_t> oldKeys = std::move(keys_);
    std::vector<uint32_t> oldCounts = std::move(counts_);

    int log2Cap = 0;
    for (size_t c = newCap; c > 1; c >>= 1)
      ++log2Cap;

    keys_.assign(newCap, EMPTY_KEY);
    counts_.assign(newCap, 0U);
    mask_ = static_cast<uint64_t>(newCap) - 1ULL;
    shift_ = 64 - log2Cap;
    size_ = 0;

    for (size_t i = 0; i < oldKeys.size(); ++i) {
      if (oldKeys[i] != EMPTY_KEY) {
        uint64_t slot = hashKey(oldKeys[i]);
        while (keys_[static_cast<size_t>(slot)] != EMPTY_KEY) {
          slot = (slot + 1ULL) & mask_;
        }
        keys_[static_cast<size_t>(slot)] = oldKeys[i];
        counts_[static_cast<size_t>(slot)] = oldCounts[i];
        ++size_;
      }
    }
  }
};

// Thread-safe 64-shard visit counter. Workers accumulate in local FlatVisitMaps and flush
// periodically, giving zero lock contention during rollouts and identical merged totals regardless
// of thread count (saturating addition is associative and commutative).
class ShardedVisitCounter {
public:
  static constexpr size_t NUM_SHARDS = 64;

  ShardedVisitCounter() = default;

  void flushLocal(const FlatVisitMap &local) {
    std::array<std::vector<std::pair<uint64_t, uint32_t>>, NUM_SHARDS> partitioned;
    local.forEach([&](uint64_t board, uint32_t count) {
      partitioned[shardIndex(board)].emplace_back(board, count);
    });
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
      if (partitioned[s].empty())
        continue;
      std::lock_guard<std::mutex> lock(shards_[s].mu);
      const size_t before = shards_[s].map.size();
      for (const auto &kv : partitioned[s]) {
        shards_[s].map.add(kv.first, kv.second);
      }
      const size_t added = shards_[s].map.size() - before;
      if (added > 0) {
        approxUnique_.fetch_add(added, std::memory_order_relaxed);
      }
    }
  }

  size_t approxUniqueCount() const noexcept {
    return approxUnique_.load(std::memory_order_relaxed);
  }

  std::vector<std::pair<uint64_t, uint32_t>> extractAll() const {
    std::vector<std::pair<uint64_t, uint32_t>> out;
    out.reserve(approxUniqueCount());
    for (const auto &sh : shards_) {
      std::lock_guard<std::mutex> lock(sh.mu);
      sh.map.forEach([&](uint64_t b, uint32_t c) { out.emplace_back(b, c); });
    }
    return out;
  }

private:
  struct Shard {
    mutable std::mutex mu;
    FlatVisitMap map{1024};
  };

  std::array<Shard, NUM_SHARDS> shards_;
  std::atomic<size_t> approxUnique_{0};

  // Uses an independent SplitMix64 mixing step so shard bits are completely decorrelated from
  // FlatVisitMap::hashKey's upper-bit slot index (preventing prefix clustering inside shards).
  static size_t shardIndex(uint64_t board) noexcept {
    uint64_t x = (board ^ (board >> 23)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return static_cast<size_t>((x ^ (x >> 31)) >> 58); // Top 6 bits -> [0..63]
  }
};

struct StateGenConfig {
  int numGames = 100000;
  int maxPiecesPerGame = DEFAULT_MAX_PIECES_PER_GAME;
  uint64_t baseSeed = 1000000ULL;
  int heurPly = 2;             // 1 or 2 for heuristic rollouts / TB out-of-set fallback
  double epsilon = 0.08;       // Probability of picking a non-greedy top-K exploration move
  int exploreTopK = 4;         // Exploration chooses uniformly among ranks 2..K (if available)
  uint32_t minVisits = 2;      // Prune rare one-off tail states below this visit count
  size_t maxBoards = 15000000; // Maximum output state-set size (0 = unlimited)
  uint32_t siblingMinVisits =
      5;              // Expand greedy replies for all 7 pieces on >= M-visit boards (0=off)
  int siblingPly = 1; // Heuristic ply used during sibling expansion when TB absent/OUT
  int numThreads = 1;
  int progressInterval = 25000;
  bool verbose = false;
  EvalWeights weights{};
};

struct CoverageReport {
  int games = 0;
  double meanLines = 0.0;
  double stdErrLines = 0.0;
  double overallHitPct = 0.0;
  double fullyCoveredGamePct = 0.0;
  HoleStratifiedCounts byHoles{};
};

struct StateGenResult {
  std::vector<uint64_t> sortedBoards;
  uint64_t rawUniqueDiscovered = 0;
  uint64_t siblingBoardsAdded = 0; // Post-cap count of boards retained solely via sibling expansion
  uint64_t totalMovesSimulated = 0;
  uint64_t priorBoardsMerged = 0; // Post-cap count of prior boards retained in sortedBoards
  double meanTrainLines = 0.0;
  double elapsedSec = 0.0;
  std::array<uint64_t, NUM_HOLE_BUCKETS> boardHoleDistribution{};
};

// Picks a rollout move under the mixed (1 - epsilon) greedy + epsilon top-K exploration policy.
// When epsilon triggers and >= 2 legal moves exist, selects uniformly among ranks [1 .. min(K,
// N)-1] (strictly non-greedy runner-up moves) so epsilon equals the exact deviation rate.
template <typename NnbTablebase>
inline ChosenTransition selectRolloutTransition(uint64_t board, int pieceIdx, Rng &rng,
                                                const NnbTablebase *tb, int heurPly, double epsilon,
                                                int exploreTopK, const EvalWeights &w) {
  assert(heurPly == 1 || heurPly == 2);
  const bool tbActive = (tb != nullptr && tb->isLoaded());
  const bool doExplore = (epsilon > 0.0 && exploreTopK >= 2 && rng.nextDouble() < epsilon);

  if (!doExplore) {
    return detail::chooseGreedyPolicyMove(board, pieceIdx, tb, heurPly, w);
  }

  TransitionList tl;
  generateTransitions(board, pieceIdx, tl);
  if (tl.empty()) {
    return {};
  }
  if (tl.size() == 1) {
    return {tl[0], 0.0, true};
  }

  struct ScoredMove {
    Transition t;
    double primary;
    double secondary;
    int origIdx;
  };
  ScoredMove scored[MAX_TRANSITIONS];
  const int n = tl.size();

  const auto evalHeur = [&](const Transition &t) {
    return (heurPly == 2) ? scoreTransition2Ply(t.afterBoard, t.linesCleared, w)
                          : scoreTransition1Ply(t.afterBoard, t.linesCleared, w);
  };

  constexpr uint64_t NO_BOARD = ~0ULL;
  bool anyInTb = false;
  double tbQ[MAX_TRANSITIONS];
  if (tbActive) {
    for (int i = 0; i < n; ++i) {
      float childVal = 0.0f;
      const bool inTb = tb->lookup(tl[i].afterBoard, childVal);
      anyInTb = anyInTb || inTb;
      tbQ[i] =
          static_cast<double>(tl[i].linesCleared) + (inTb ? static_cast<double>(childVal) : 0.0);
    }
  }

  // When a tablebase is active with in-set moves, split exploration 50/50 between:
  //   - TB runner-up moves (ranks 2..K by TB Q): explores alternative in-set trajectories;
  //   - Heuristic frontier probes (top non-greedy moves by heuristic): discovers strong moves
  //     outside the current tablebase that TB currently values at 0.0.
  const bool useHeuristicProbe = !tbActive || !anyInTb || ((rng.nextU64() & 1ULL) != 0ULL);
  uint64_t excludeBoard = NO_BOARD;

  if (useHeuristicProbe) {
    int bestTbIdx = 0;
    double bestTbQ = -std::numeric_limits<double>::infinity();
    double bestTbHeur = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) {
      const double h = evalHeur(tl[i]);
      scored[i] = {tl[i], h, 0.0, i};
      if (tbActive && anyInTb) {
        if (tbQ[i] > bestTbQ || (tbQ[i] == bestTbQ && h > bestTbHeur)) {
          bestTbQ = tbQ[i];
          bestTbHeur = h;
          bestTbIdx = i;
        }
      }
    }
    if (tbActive && anyInTb) {
      excludeBoard = tl[bestTbIdx].afterBoard;
    }
  } else {
    for (int i = 0; i < n; ++i) {
      scored[i] = {tl[i], tbQ[i], 0.0, i};
    }
    // Lazily break equal-Q ties (including tied q == 0 out-of-TB exits) with the same heurPly
    // tie-breaker used by chooseTablebaseMove so rank 0 always matches the greedy TB move.
    for (int i = 0; i < n; ++i) {
      bool hasTie = false;
      for (int j = 0; j < n; ++j) {
        if (i != j && scored[i].primary == scored[j].primary) {
          hasTie = true;
          break;
        }
      }
      if (hasTie) {
        scored[i].secondary = evalHeur(scored[i].t);
      }
    }
  }

  // Pin excludeBoard (if set) to rank 0 so pickRank in [1 .. kEff - 1] never repeats the greedy TB
  // move (in heuristic-probe mode, rank 1 can be the #1 heuristic move whenever it disagrees with
  // TB).
  const int kEff = std::min(exploreTopK, n);
  std::partial_sort(scored, scored + kEff, scored + n,
                    [excludeBoard](const ScoredMove &a, const ScoredMove &b) {
                      const bool aExcl = (a.t.afterBoard == excludeBoard);
                      const bool bExcl = (b.t.afterBoard == excludeBoard);
                      if (aExcl != bExcl)
                        return aExcl;
                      if (a.primary != b.primary)
                        return a.primary > b.primary;
                      if (a.secondary != b.secondary)
                        return a.secondary > b.secondary;
                      return a.origIdx < b.origIdx;
                    });

  const int pickRank = 1 + static_cast<int>(rng.nextU64() % static_cast<uint64_t>(kEff - 1));
  return {scored[pickRank].t, scored[pickRank].primary, true};
}

inline ChosenTransition selectRolloutTransition(uint64_t board, int pieceIdx, Rng &rng,
                                                std::nullptr_t, int heurPly, double epsilon,
                                                int exploreTopK, const EvalWeights &w) {
  return selectRolloutTransition<MappedTablebase>(board, pieceIdx, rng, nullptr, heurPly, epsilon,
                                                  exploreTopK, w);
}

namespace detail {

template <typename NnbTablebase>
inline std::vector<uint64_t>
expandCoreSiblings(const std::vector<std::pair<uint64_t, uint32_t>> &rawEntries,
                   const StateGenConfig &cfg, const NnbTablebase *tb) {
  std::vector<uint64_t> siblingCandidates;
  if (cfg.siblingMinVisits == 0 || rawEntries.empty()) {
    return siblingCandidates;
  }

  std::vector<uint64_t> coreBoards;
  for (const auto &kv : rawEntries) {
    if (kv.second >= cfg.siblingMinVisits) {
      coreBoards.push_back(kv.first);
    }
  }
  if (coreBoards.empty()) {
    return siblingCandidates;
  }

  constexpr size_t SIBLING_CHUNK_SIZE = 256;
  const int threads = std::max(1, cfg.numThreads);
  const bool tbActive = (tb != nullptr && tb->isLoaded());
  std::vector<std::vector<uint64_t>> threadSiblings(static_cast<size_t>(threads));
  std::atomic<size_t> nextCore{0};

  runWorkerPool(threads, coreBoards.size(), /*minParallelWork=*/512, [&](int tid) {
    auto &out = threadSiblings[static_cast<size_t>(tid)];
    while (true) {
      const size_t start = nextCore.fetch_add(SIBLING_CHUNK_SIZE, std::memory_order_relaxed);
      if (start >= coreBoards.size())
        break;
      const size_t end = std::min(coreBoards.size(), start + SIBLING_CHUNK_SIZE);
      for (size_t i = start; i < end; ++i) {
        const uint64_t b = coreBoards[i];
        for (int c = 0; c < NUM_PIECES; ++c) {
          // Always expand the heuristic reply so frontier moves preferred by the heuristic enter
          // the next state set even when TB currently values them at 0.0; also expand TB's reply
          // when different.
          const ChosenTransition heurCh =
              chooseGreedyPolicyMove(b, c, nullptr, cfg.siblingPly, cfg.weights);
          if (!heurCh.found) {
            continue;
          }
          out.push_back(heurCh.transition.afterBoard);
          if (tbActive) {
            const ChosenTransition tbCh =
                chooseGreedyPolicyMove(b, c, tb, cfg.siblingPly, cfg.weights);
            if (tbCh.found && tbCh.transition.afterBoard != heurCh.transition.afterBoard) {
              out.push_back(tbCh.transition.afterBoard);
            }
          }
        }
      }
    }
  });

  size_t totalSibs = 0;
  for (const auto &v : threadSiblings)
    totalSibs += v.size();
  siblingCandidates.reserve(totalSibs);
  for (auto &v : threadSiblings) {
    siblingCandidates.insert(siblingCandidates.end(), v.begin(), v.end());
    std::vector<uint64_t>().swap(v);
  }
  std::sort(siblingCandidates.begin(), siblingCandidates.end());
  siblingCandidates.erase(std::unique(siblingCandidates.begin(), siblingCandidates.end()),
                          siblingCandidates.end());
  return siblingCandidates;
}

inline void mergeFilterAndCapBoards(std::vector<std::pair<uint64_t, uint32_t>> &rawEntries,
                                    const std::vector<uint64_t> &siblingCandidates,
                                    const std::vector<uint64_t> *priorBoards,
                                    const StateGenConfig &cfg, StateGenResult &result) {
  std::sort(rawEntries.begin(), rawEntries.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  std::vector<uint64_t> priorSorted;
  if (priorBoards != nullptr && !priorBoards->empty()) {
    priorSorted = *priorBoards;
#ifndef NDEBUG
    for (uint64_t b : priorSorted) {
      assert((b & ~VALID_BOARD_MASK) == 0);
    }
#endif
    std::sort(priorSorted.begin(), priorSorted.end());
    priorSorted.erase(std::unique(priorSorted.begin(), priorSorted.end()), priorSorted.end());
  }

  struct RankedBoard {
    uint64_t board;
    uint32_t visits;
    bool protectedPrior;
    bool siblingOnly;
  };

  std::vector<RankedBoard> merged;
  merged.reserve(rawEntries.size() + priorSorted.size() + siblingCandidates.size() / 4);

  size_t iRaw = 0;
  size_t iPrior = 0;
  size_t iSib = 0;
  while (
      iRaw < rawEntries.size() || iPrior < priorSorted.size() || iSib < siblingCandidates.size()) {
    uint64_t nextKey = ~0ULL;
    if (iRaw < rawEntries.size())
      nextKey = std::min(nextKey, rawEntries[iRaw].first);
    if (iPrior < priorSorted.size())
      nextKey = std::min(nextKey, priorSorted[iPrior]);
    if (iSib < siblingCandidates.size())
      nextKey = std::min(nextKey, siblingCandidates[iSib]);

    uint32_t visits = 0;
    bool isPrior = false;
    bool isSib = false;
    if (iRaw < rawEntries.size() && rawEntries[iRaw].first == nextKey) {
      visits = rawEntries[iRaw].second;
      ++iRaw;
    }
    if (iPrior < priorSorted.size() && priorSorted[iPrior] == nextKey) {
      isPrior = true;
      ++iPrior;
    }
    if (iSib < siblingCandidates.size() && siblingCandidates[iSib] == nextKey) {
      isSib = true;
      ++iSib;
    }

    const bool keep = isPrior || isSib || (nextKey == 0ULL) || (visits >= cfg.minVisits);
    if (keep) {
      const bool rescuedBySibling =
          isSib && !isPrior && (nextKey != 0ULL) && (visits < cfg.minVisits);
      const uint32_t effectiveVisits = (isSib && visits < cfg.minVisits) ? cfg.minVisits : visits;
      merged.push_back({nextKey, effectiveVisits, isPrior, rescuedBySibling});
    }
  }

  // Enforce maxBoards cap deterministically. Priority order:
  //   1. Board 0ULL (empty start state is never evicted),
  //   2. Protected priorBoards (if priorSorted.size() > maxBoards, ranked by visits then board
  //   key),
  //   3. Newly sampled/sibling boards ranked by visits descending, then board key ascending.
  bool needsResort = false;
  if (cfg.maxBoards > 0 && merged.size() > cfg.maxBoards) {
    std::nth_element(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(cfg.maxBoards),
                     merged.end(), [](const RankedBoard &a, const RankedBoard &b) {
                       const bool aEmpty = (a.board == 0ULL);
                       const bool bEmpty = (b.board == 0ULL);
                       if (aEmpty != bEmpty)
                         return aEmpty;
                       if (a.protectedPrior != b.protectedPrior)
                         return a.protectedPrior;
                       if (a.visits != b.visits)
                         return a.visits > b.visits;
                       return a.board < b.board;
                     });
    merged.resize(cfg.maxBoards);
    needsResort = true;
  }

  result.sortedBoards.reserve(merged.size());
  for (const auto &rb : merged) {
    result.sortedBoards.push_back(rb.board);
    if (rb.protectedPrior)
      ++result.priorBoardsMerged;
    if (rb.siblingOnly)
      ++result.siblingBoardsAdded;
  }
  if (needsResort) {
    std::sort(result.sortedBoards.begin(), result.sortedBoards.end());
  }

  for (uint64_t b : result.sortedBoards) {
    const int hBucket = HoleStratifiedCounts::bucket(countHoles(b));
    ++result.boardHoleDistribution[static_cast<size_t>(hBucket)];
  }
}

} // namespace detail

// Runs parallel trajectory sampling, merges optional priorBoards, applies optional 1-ply sibling
// expansion on frequent boards, filters by minVisits and maxBoards, and returns sorted boards.
inline StateGenResult generateStateSpace(const StateGenConfig &cfg,
                                         const MappedTablebase *tb = nullptr,
                                         const std::vector<uint64_t> *priorBoards = nullptr) {
  assert(cfg.heurPly == 1 || cfg.heurPly == 2);
  assert(cfg.siblingPly == 1 || cfg.siblingPly == 2);
  assert(cfg.numGames >= 0);
  const auto t0 = std::chrono::steady_clock::now();

  const int threads = std::max(1, cfg.numThreads);
  ShardedVisitCounter counter;

  std::atomic<int> nextGame{0};
  std::atomic<int> completedGames{0};
  std::atomic<uint64_t> totalMoves{0};
  std::atomic<uint64_t> totalLines{0};

  constexpr size_t LOCAL_FLUSH_THRESHOLD = 100000;

  // Build an in-memory O(1) hash index once before launching workers so high-volume rollouts avoid
  // repeated O(log N) binary-search cache misses over mmap.
  IndexedTablebase indexedTb;
  const IndexedTablebase *fastTb = nullptr;
  if (tb != nullptr && tb->isLoaded()) {
    indexedTb.buildFromMapped(*tb);
    fastTb = &indexedTb;
  }

  detail::runWorkerPool(
      threads, static_cast<size_t>(cfg.numGames), /*minParallelWork=*/64, [&](int /*tid*/) {
        FlatVisitMap localMap(65536);
        uint64_t localMoves = 0;
        uint64_t localLines = 0;

        while (true) {
          const int g = nextGame.fetch_add(1, std::memory_order_relaxed);
          if (g >= cfg.numGames)
            break;

          Rng rng(detail::deriveGameSeed(cfg.baseSeed, g));
          uint64_t board = 0ULL;
          localMap.add(board, 1);

          for (int step = 0; step < cfg.maxPiecesPerGame; ++step) {
            const int piece = rng.nextPiece();
            const ChosenTransition ch = selectRolloutTransition(
                board, piece, rng, fastTb, cfg.heurPly, cfg.epsilon, cfg.exploreTopK, cfg.weights);
            if (!ch.found) {
              break;
            }
            ++localMoves;
            localLines += ch.transition.linesCleared;
            board = ch.transition.afterBoard;
            localMap.add(board, 1);
          }

          if (localMap.size() >= LOCAL_FLUSH_THRESHOLD) {
            counter.flushLocal(localMap);
            localMap.clear();
          }

          const int done = completedGames.fetch_add(1, std::memory_order_relaxed) + 1;
          if (cfg.verbose && cfg.progressInterval > 0 && (done % cfg.progressInterval == 0)) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  [gen %7d / %d games] ~%zu unique flushed | %.1fs\n", done, cfg.numGames,
                        counter.approxUniqueCount(), elapsed);
            std::fflush(stdout);
          }
        }

        if (localMap.size() > 0) {
          counter.flushLocal(localMap);
          localMap.clear();
        }
        totalMoves.fetch_add(localMoves, std::memory_order_relaxed);
        totalLines.fetch_add(localLines, std::memory_order_relaxed);
      });

  StateGenResult result{};
  result.totalMovesSimulated = totalMoves.load(std::memory_order_relaxed);
  result.meanTrainLines = (cfg.numGames > 0)
                              ? static_cast<double>(totalLines.load(std::memory_order_relaxed))
                                    / static_cast<double>(cfg.numGames)
                              : 0.0;

  std::vector<std::pair<uint64_t, uint32_t>> rawEntries = counter.extractAll();
  result.rawUniqueDiscovered = rawEntries.size();

  const std::vector<uint64_t> siblingCandidates =
      detail::expandCoreSiblings(rawEntries, cfg, fastTb);
  detail::mergeFilterAndCapBoards(rawEntries, siblingCandidates, priorBoards, cfg, result);

  result.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

// Measures held-out greedy rollout coverage against sortedBoards (overall and stratified by
// afterBoard hole count 0, 1, 2, 3+).
inline CoverageReport measureCoverage(const std::vector<uint64_t> &sortedBoards, int numGames,
                                      uint64_t baseSeed, const MappedTablebase *tb, int heurPly,
                                      int numThreads, const EvalWeights &w = EvalWeights{},
                                      int maxPiecesPerGame = DEFAULT_MAX_PIECES_PER_GAME) {
  assert(heurPly == 1 || heurPly == 2);
  assert(numGames >= 0);
  CoverageReport report{};
  if (numGames <= 0) {
    return report;
  }
  report.games = numGames;

  const FlatBoardIndex index(sortedBoards);
  IndexedTablebase indexedTb;
  const IndexedTablebase *fastTb = nullptr;
  if (tb != nullptr && tb->isLoaded()) {
    indexedTb.buildFromMapped(*tb);
    fastTb = &indexedTb;
  }
  const int threads = std::max(1, numThreads);

  std::vector<int> gameLines(static_cast<size_t>(numGames), 0);
  std::vector<HoleStratifiedCounts> threadCounts(static_cast<size_t>(threads));
  std::atomic<int> fullyCoveredGames{0};
  std::atomic<int> nextGame{0};

  detail::runWorkerPool(threads, static_cast<size_t>(numGames), /*minParallelWork=*/32,
                        [&](int tid) {
                          HoleStratifiedCounts local{};
                          int localFull = 0;
                          while (true) {
                            const int g = nextGame.fetch_add(1, std::memory_order_relaxed);
                            if (g >= numGames)
                              break;

                            Rng rng(detail::deriveGameSeed(baseSeed, g));
                            uint64_t board = 0ULL;
                            int lines = 0;
                            bool allIn = true;

                            for (int step = 0; step < maxPiecesPerGame; ++step) {
                              const int piece = rng.nextPiece();
                              const ChosenTransition ch =
                                  detail::chooseGreedyPolicyMove(board, piece, fastTb, heurPly, w);
                              if (!ch.found) {
                                break;
                              }
                              lines += ch.transition.linesCleared;
                              board = ch.transition.afterBoard;

                              const int hb = HoleStratifiedCounts::bucket(countHoles(board));
                              ++local.total[static_cast<size_t>(hb)];
                              if (index.find(board) != INVALID_BOARD_ID) {
                                ++local.inSet[static_cast<size_t>(hb)];
                              } else {
                                allIn = false;
                              }
                            }
                            if (allIn)
                              ++localFull;
                            gameLines[static_cast<size_t>(g)] = lines;
                          }
                          threadCounts[static_cast<size_t>(tid)] = local;
                          fullyCoveredGames.fetch_add(localFull, std::memory_order_relaxed);
                        });

  for (const auto &tc : threadCounts) {
    report.byHoles.accumulate(tc);
  }

  const auto meanSe = detail::computeMeanAndStdErr(gameLines);
  report.meanLines = meanSe.first;
  report.stdErrLines = meanSe.second;

  const uint64_t totMoves = report.byHoles.allTotal();
  report.overallHitPct =
      (totMoves > 0)
          ? (100.0 * static_cast<double>(report.byHoles.allInSet()) / static_cast<double>(totMoves))
          : 0.0;
  report.fullyCoveredGamePct =
      100.0
      * static_cast<double>(fullyCoveredGames.load(std::memory_order_relaxed))
      / static_cast<double>(numGames);
  return report;
}

} // namespace tb

#endif // TABLEBASE_STATE_GENERATOR_HPP
