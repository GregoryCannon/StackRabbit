#ifndef TABLEBASE_VI_SOLVER_HPP
#define TABLEBASE_VI_SOLVER_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "tablebase_io.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace tb {

inline constexpr uint32_t INVALID_BOARD_ID = UINT32_MAX;
inline constexpr int NUM_HOLE_BUCKETS = 4; // Buckets: 0, 1, 2, 3+ holes
inline constexpr int DEFAULT_MAX_PIECES_PER_GAME = 50000;
inline constexpr int DEFAULT_EVAL_MAX_STEPS = 200000;

struct HoleStratifiedCounts {
  std::array<uint64_t, NUM_HOLE_BUCKETS> total{};
  std::array<uint64_t, NUM_HOLE_BUCKETS> inSet{};

  static int bucket(int holes) noexcept {
    assert(holes >= 0);
    return (holes >= NUM_HOLE_BUCKETS - 1) ? (NUM_HOLE_BUCKETS - 1) : holes;
  }

  void accumulate(const HoleStratifiedCounts &other) noexcept {
    for (int b = 0; b < NUM_HOLE_BUCKETS; ++b) {
      total[static_cast<size_t>(b)] += other.total[static_cast<size_t>(b)];
      inSet[static_cast<size_t>(b)] += other.inSet[static_cast<size_t>(b)];
    }
  }

  uint64_t allTotal() const noexcept {
    uint64_t s = 0;
    for (uint64_t c : total)
      s += c;
    return s;
  }

  uint64_t allInSet() const noexcept {
    uint64_t s = 0;
    for (uint64_t c : inSet)
      s += c;
    return s;
  }
};

inline void printHoleStratifiedCoverage(const HoleStratifiedCounts &counts, FILE *out = stdout) {
  static_assert(NUM_HOLE_BUCKETS == 4, "Hole coverage labels assume 4 buckets (0, 1, 2, 3+)");
  constexpr const char *labels[NUM_HOLE_BUCKETS] = {"0 holes ", "1 hole  ", "2 holes ", "3+ holes"};
  for (int b = 0; b < NUM_HOLE_BUCKETS; ++b) {
    const uint64_t tot = counts.total[static_cast<size_t>(b)];
    const uint64_t hit = counts.inSet[static_cast<size_t>(b)];
    const double pct =
        (tot > 0) ? (100.0 * static_cast<double>(hit) / static_cast<double>(tot)) : 0.0;
    std::fprintf(out, "    %s: %6.2f%% (%llu / %llu moves)\n", labels[b], pct,
                 static_cast<unsigned long long>(hit), static_cast<unsigned long long>(tot));
  }
}

inline double computeClosedWorldZScore(double empiricalMean, double stdErr,
                                       double expectedValue) noexcept {
  const double absDiff = std::abs(empiricalMean - expectedValue);
  if (stdErr > 0.0) {
    return absDiff / stdErr;
  }
  return (absDiff > 1e-9) ? std::numeric_limits<double>::infinity() : 0.0;
}

namespace detail {

inline constexpr uint64_t GAME_SEED_STRIDE = 0x9E3779B97F4A7C15ULL;

constexpr uint64_t deriveGameSeed(uint64_t baseSeed, int gameIdx) noexcept {
  return baseSeed + static_cast<uint64_t>(gameIdx) * GAME_SEED_STRIDE;
}

inline std::pair<double, double> computeMeanAndStdErr(const std::vector<int> &samples) noexcept {
  if (samples.empty()) {
    return {0.0, 0.0};
  }
  double sum = 0.0;
  for (int x : samples) {
    sum += static_cast<double>(x);
  }
  const double n = static_cast<double>(samples.size());
  const double mean = sum / n;
  if (samples.size() <= 1) {
    return {mean, 0.0};
  }
  double varSum = 0.0;
  for (int x : samples) {
    const double d = static_cast<double>(x) - mean;
    varSum += d * d;
  }
  return {mean, std::sqrt(varSum / (n * (n - 1.0)))};
}

struct DistributionStats {
  double mean = 0.0;
  double stdErr = 0.0;
  double median = 0.0;
  int p90 = 0;
  int maxVal = 0;
};

inline DistributionStats computeDistributionStats(std::vector<int> samples) {
  DistributionStats d{};
  if (samples.empty()) {
    return d;
  }
  const auto meanSe = computeMeanAndStdErr(samples);
  d.mean = meanSe.first;
  d.stdErr = meanSe.second;
  std::sort(samples.begin(), samples.end());
  const size_t n = samples.size();
  d.median =
      (n % 2 == 1)
          ? static_cast<double>(samples[n / 2])
          : 0.5 * (static_cast<double>(samples[n / 2 - 1]) + static_cast<double>(samples[n / 2]));
  d.p90 = samples[(n - 1) * 9 / 10];
  d.maxVal = samples.back();
  return d;
}

template <typename WorkerFn>
inline void runWorkerPool(int numThreads, size_t workCount, size_t minParallelWork,
                          WorkerFn &&worker) {
  if (workCount == 0) {
    return;
  }
  const int threads =
      static_cast<int>(std::min<size_t>(static_cast<size_t>(std::max(1, numThreads)), workCount));
  if (threads == 1 || workCount < minParallelWork) {
    worker(0);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t]() { worker(t); });
  }
  for (auto &th : pool) {
    th.join();
  }
}

} // namespace detail

// Read-only power-of-two open-addressing hash index mapping strictly sorted 40-bit board keys
// to dense uint32_t indices [0 .. N-1]. Uses ~0ULL as the empty-slot sentinel since valid boards
// fit within 40 bits.
class FlatBoardIndex {
public:
  FlatBoardIndex() = default;
  explicit FlatBoardIndex(const std::vector<uint64_t> &sortedBoards) { build(sortedBoards); }

  void build(const std::vector<uint64_t> &sortedBoards) {
    buildFromAccessor(sortedBoards.size(), [&](size_t i) { return sortedBoards[i]; });
  }

  template <typename KeyFn> void buildFromAccessor(size_t count, KeyFn &&keyAt) {
    if (count >= static_cast<size_t>(INVALID_BOARD_ID)) {
      std::abort();
    }
    size_ = count;
    if (size_ == 0) {
      keys_.clear();
      ids_.clear();
      mask_ = 0;
      shift_ = 64 - MIN_LOG2_CAPACITY;
      return;
    }

    // Target <= 50% load factor for ~1.4 average linear probes per lookup.
    int log2Cap = MIN_LOG2_CAPACITY;
    uint64_t cap = 1ULL << log2Cap;
    while (cap < static_cast<uint64_t>(size_) * 2ULL) {
      cap <<= 1;
      ++log2Cap;
    }
    mask_ = cap - 1ULL;
    shift_ = 64 - log2Cap;

    keys_.assign(static_cast<size_t>(cap), EMPTY_KEY);
    ids_.assign(static_cast<size_t>(cap), 0U);

#ifndef NDEBUG
    uint64_t prevKey = 0;
#endif
    for (size_t i = 0; i < size_; ++i) {
      const uint64_t b = keyAt(i);
      assert((b & ~VALID_BOARD_MASK) == 0);
#ifndef NDEBUG
      assert(i == 0 || b > prevKey);
      prevKey = b;
#endif
      uint64_t slot = hashKey(b);
      while (keys_[static_cast<size_t>(slot)] != EMPTY_KEY) {
        slot = (slot + 1ULL) & mask_;
      }
      keys_[static_cast<size_t>(slot)] = b;
      ids_[static_cast<size_t>(slot)] = static_cast<uint32_t>(i);
    }
  }

  uint32_t find(uint64_t board) const noexcept {
    if (size_ == 0 || board == EMPTY_KEY) {
      return INVALID_BOARD_ID;
    }
    uint64_t slot = hashKey(board);
    while (true) {
      const uint64_t k = keys_[static_cast<size_t>(slot)];
      if (k == EMPTY_KEY) {
        return INVALID_BOARD_ID;
      }
      if (k == board) {
        return ids_[static_cast<size_t>(slot)];
      }
      slot = (slot + 1ULL) & mask_;
    }
  }

  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

private:
  static constexpr uint64_t EMPTY_KEY = ~0ULL;
  static constexpr int MIN_LOG2_CAPACITY = 4;
  static constexpr uint64_t FIBONACCI_HASH_MULT = 0x9E3779B97F4A7C15ULL;

  std::vector<uint64_t> keys_;
  std::vector<uint32_t> ids_;
  uint64_t mask_ = 0;
  int shift_ = 64 - MIN_LOG2_CAPACITY;
  size_t size_ = 0;

  uint64_t hashKey(uint64_t b) const noexcept {
    const uint64_t x = b ^ (b >> 17);
    return (x * FIBONACCI_HASH_MULT) >> shift_;
  }
};

// In-memory O(1) hash-indexed view of a No-Next-Box tablebase (SRTBV001) for fast offline rollouts
// and Monte Carlo evaluation (1-2 memory accesses per lookup instead of O(log N) binary search).
class IndexedTablebase {
public:
  IndexedTablebase() = default;
  explicit IndexedTablebase(const MappedTablebase &mapped) { buildFromMapped(mapped); }

  void buildFromMapped(const MappedTablebase &mapped) {
    if (!mapped.isLoaded()) {
      clear();
      return;
    }
    const size_t n = static_cast<size_t>(mapped.size());
    if (n >= static_cast<size_t>(INVALID_BOARD_ID)) {
      std::abort();
    }
    index_.buildFromAccessor(n, [&](size_t i) { return mapped.boardAt(static_cast<uint64_t>(i)); });
    values_.resize(n);
    for (size_t i = 0; i < n; ++i) {
      values_[i] = mapped.valueAt(static_cast<uint64_t>(i));
    }
    loaded_ = true;
  }

  void clear() {
    index_ = FlatBoardIndex{};
    std::vector<float>().swap(values_);
    loaded_ = false;
  }

  bool isLoaded() const noexcept { return loaded_; }
  uint64_t size() const noexcept { return loaded_ ? static_cast<uint64_t>(index_.size()) : 0; }

  bool lookup(uint64_t board, float &outValue) const noexcept {
    if (!loaded_) {
      return false;
    }
    const uint32_t id = index_.find(board);
    if (id == INVALID_BOARD_ID) {
      return false;
    }
    outValue = values_[id];
    return true;
  }

private:
  FlatBoardIndex index_;
  std::vector<float> values_;
  bool loaded_ = false;
};

// In-memory O(1) hash-indexed view of a 1-piece Next-Box tablebase (SRTBV002).
class IndexedNextBoxTablebase {
public:
  IndexedNextBoxTablebase() = default;
  explicit IndexedNextBoxTablebase(const MappedNextBoxTablebase &mapped) {
    buildFromMapped(mapped);
  }

  void buildFromMapped(const MappedNextBoxTablebase &mapped) {
    if (!mapped.isLoaded()) {
      clear();
      return;
    }
    const size_t n = static_cast<size_t>(mapped.size());
    if (n >= static_cast<size_t>(INVALID_BOARD_ID)) {
      std::abort();
    }
    index_.buildFromAccessor(n, [&](size_t i) { return mapped.boardAt(static_cast<uint64_t>(i)); });
    values_.resize(n * static_cast<size_t>(NUM_PIECES));
    for (size_t i = 0; i < n; ++i) {
      mapped.copyValuesAt(static_cast<uint64_t>(i), &values_[i * static_cast<size_t>(NUM_PIECES)]);
    }
    rngMode_ = mapped.rngMode();
    loaded_ = true;
  }

  void clear() {
    index_ = FlatBoardIndex{};
    std::vector<float>().swap(values_);
    rngMode_ = RngMode::Uniform;
    loaded_ = false;
  }

  bool isLoaded() const noexcept { return loaded_; }
  uint64_t size() const noexcept { return loaded_ ? static_cast<uint64_t>(index_.size()) : 0; }
  RngMode rngMode() const noexcept { return rngMode_; }

  bool lookup(uint64_t board, float outValues[NUM_PIECES]) const noexcept {
    const float *row = findRow(board);
    if (row == nullptr) {
      return false;
    }
    std::memcpy(outValues, row, static_cast<size_t>(NUM_PIECES) * sizeof(float));
    return true;
  }

  bool lookupPiece(uint64_t board, int pieceIdx, float &outValue) const noexcept {
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    const float *row = findRow(board);
    if (row == nullptr) {
      return false;
    }
    outValue = row[pieceIdx];
    return true;
  }

private:
  FlatBoardIndex index_;
  std::vector<float> values_;
  RngMode rngMode_ = RngMode::Uniform;
  bool loaded_ = false;

  const float *findRow(uint64_t board) const noexcept {
    if (!loaded_) {
      return nullptr;
    }
    const uint32_t id = index_.find(board);
    if (id == INVALID_BOARD_ID) {
      return nullptr;
    }
    return &values_[static_cast<size_t>(id) * static_cast<size_t>(NUM_PIECES)];
  }
};

// Enumerates all canonical 4x10 boards with sealedHoles == 0 and overhangs <= maxOverhangs
// (where maxOverhangs must be 0 or 1), restricted to column heights <= maxColHeight across
// columns [0 .. activeCols - 1] (with remaining columns empty), sorted strictly ascending.
inline std::vector<uint64_t> enumerateZeroSealedHoleBoards(int maxOverhangs, int numThreads = 1,
                                                           int maxColHeight = BOARD_ROWS,
                                                           int activeCols = BOARD_WIDTH) {
  if (maxOverhangs < 0
      || maxOverhangs > 1
      || maxColHeight < 1
      || maxColHeight > BOARD_ROWS
      || activeCols < 2
      || activeCols > BOARD_WIDTH) {
    std::abort();
  }
  const int threads = std::max(1, numThreads);
  const int heightRadix = maxColHeight + 1;
  const int numPrefixes = heightRadix * heightRadix;

  int suffixCompletions = 1;
  for (int c = 2; c < activeCols; ++c) {
    suffixCompletions *= heightRadix;
  }

  constexpr size_t FULL_4ROW_RESERVE_0_OH = 200000;
  constexpr size_t FULL_4ROW_RESERVE_1_OH = 1300000;
  const bool isFull4x10 = (maxColHeight == BOARD_ROWS && activeCols == BOARD_WIDTH);

  std::vector<std::vector<uint64_t>> buckets(static_cast<size_t>(numPrefixes));
  std::atomic<int> nextPrefix{0};

  auto worker = [&]() {
    while (true) {
      const int pIdx = nextPrefix.fetch_add(1, std::memory_order_relaxed);
      if (pIdx >= numPrefixes)
        break;

      auto &out = buckets[static_cast<size_t>(pIdx)];
      if (isFull4x10) {
        out.reserve(maxOverhangs == 1 ? FULL_4ROW_RESERVE_1_OH : FULL_4ROW_RESERVE_0_OH);
      }

      int h[BOARD_WIDTH] = {};
      h[0] = pIdx / heightRadix;
      h[1] = pIdx % heightRadix;

      for (int code = 0; code < suffixCompletions; ++code) {
        int tmp = code;
        int sumH = h[0] + h[1];
        for (int c = 2; c < activeCols; ++c) {
          h[c] = tmp % heightRadix;
          tmp /= heightRadix;
          sumH += h[c];
        }

        uint64_t solidBoard = 0ULL;
        for (int c = 0; c < activeCols; ++c) {
          const int hc = h[c];
          for (int r = 0; r < hc; ++r) {
            solidBoard |= cellBit(r, c);
          }
        }

        if ((sumH & 1) == 0) {
          if (isCanonicalBoard(solidBoard)) {
            out.push_back(solidBoard);
          }
        } else if (maxOverhangs == 1) {
          for (int c = 0; c < activeCols; ++c) {
            const int hc = h[c];
            for (int rHole = 0; rHole <= hc - 2; ++rHole) {
              // On a 1-hole board, an adjacent column is open down through rHole iff h[adj] <=
              // rHole.
              const bool leftOpen = (c > 0) && (h[c - 1] <= rHole);
              const bool rightOpen = (c + 1 < BOARD_WIDTH) && (h[c + 1] <= rHole);
              if (!leftOpen && !rightOpen) {
                continue;
              }
              const uint64_t cand = solidBoard & ~cellBit(rHole, c);
              if (isCanonicalBoard(cand)) {
                out.push_back(cand);
              }
            }
          }
        }
      }
    }
  };

  detail::runWorkerPool(threads, static_cast<size_t>(numPrefixes), /*minParallelWork=*/2,
                        [&](int /*tid*/) { worker(); });

  size_t total = 0;
  for (const auto &b : buckets) {
    total += b.size();
  }
  std::vector<uint64_t> all;
  all.reserve(total);
  for (auto &b : buckets) {
    all.insert(all.end(), b.begin(), b.end());
    std::vector<uint64_t>().swap(b);
  }
  std::sort(all.begin(), all.end());
  return all;
}

struct ViSolveConfig {
  double tolerance = 1e-7;
  int maxOuterIters = 250;
  int innerEvalSweeps = 25;
  double innerStopRatio = 0.05; // Stop inner policy-eval early once step diff < 0.05 * tolerance
  int numThreads = 1;
  bool verbose = false;
};

struct ViSolveResult {
  std::vector<float> values;
  double emptyBoardValue = MISSING_BOARD_VALUE;
  double finalBellmanResidual = 0.0;
  int outerIterations = 0;
  int totalInnerSweeps = 0;
  double elapsedSec = 0.0;
};

namespace detail {

struct BellmanAction {
  uint32_t childId = INVALID_BOARD_ID;
  uint8_t reward = 0;
  double bestQ = 0.0;
};

inline BellmanAction bestBellmanAction(uint64_t board, int pieceIdx, const FlatBoardIndex &index,
                                       const std::vector<double> &vCur, TransitionList &scratch) {
  generateTransitions(board, pieceIdx, scratch);
  if (scratch.empty()) {
    return {};
  }
  BellmanAction best{};
  for (int m = 0; m < scratch.size(); ++m) {
    const Transition &tr = scratch[m];
    const uint32_t cid = index.find(tr.afterBoard);
    const double cont = (cid != INVALID_BOARD_ID) ? vCur[cid] : 0.0;
    const double q = static_cast<double>(tr.linesCleared) + cont;
    if (m == 0 || q > best.bestQ) {
      best = {cid, tr.linesCleared, q};
    }
  }
  return best;
}

} // namespace detail

// Solves the undiscounted afterstate Bellman equation on sortedBoards via parallel Modified Policy
// Iteration (MPI) in IEEE-754 double precision:
//   V*(b) = (1/7) * sum_{c=0..6} max_{b' in Moves(b, c)} [r(b, b') + V*(b')],
// with V(b') = 0.0 for b' outside sortedBoards and 0.0 when Moves(b, c) is empty.
// Double-buffered Jacobi updates make every outer and inner step 100% deterministic and race-free
// regardless of thread count.
inline ViSolveResult solveValueIterationMPI(const std::vector<uint64_t> &sortedBoards,
                                            const FlatBoardIndex &index,
                                            const ViSolveConfig &cfg = ViSolveConfig{}) {
  assert(index.size() == sortedBoards.size());
  const auto t0 = std::chrono::steady_clock::now();
  const size_t n = sortedBoards.size();
  ViSolveResult result{};
  if (n == 0) {
    return result;
  }

  constexpr size_t MIN_PARALLEL_BOARDS = 4096;
  const int threads = std::max(1, cfg.numThreads);

  std::vector<double> vCur(n, 0.0);
  std::vector<double> vNext(n, 0.0);
  std::vector<uint32_t> bestChild(n * static_cast<size_t>(NUM_PIECES), INVALID_BOARD_ID);
  std::vector<uint8_t> bestReward(n * static_cast<size_t>(NUM_PIECES), 0);

  auto runParallelChunks = [&](auto &&chunkFn) {
    if (threads == 1 || n < MIN_PARALLEL_BOARDS) {
      chunkFn(0, size_t{0}, n);
      return;
    }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    const size_t chunkSize = (n + static_cast<size_t>(threads) - 1) / static_cast<size_t>(threads);
    for (int t = 0; t < threads; ++t) {
      const size_t begin = std::min(n, static_cast<size_t>(t) * chunkSize);
      const size_t end = std::min(n, begin + chunkSize);
      pool.emplace_back([&, t, begin, end]() {
        if (begin < end) {
          chunkFn(t, begin, end);
        }
      });
    }
    for (auto &th : pool) {
      th.join();
    }
  };

  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);

  for (int outer = 1; outer <= cfg.maxOuterIters; ++outer) {
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);

    // 1. Exact Bellman Policy Improvement sweep across all N boards.
    runParallelChunks([&](int tid, size_t begin, size_t end) {
      TransitionList tl;
      double localMax = 0.0;
      for (size_t i = begin; i < end; ++i) {
        const uint64_t b = sortedBoards[i];
        uint32_t *outChildren = &bestChild[i * static_cast<size_t>(NUM_PIECES)];
        uint8_t *outRewards = &bestReward[i * static_cast<size_t>(NUM_PIECES)];

        double sumBestQ = 0.0;
        for (int c = 0; c < NUM_PIECES; ++c) {
          const detail::BellmanAction act = detail::bestBellmanAction(b, c, index, vCur, tl);
          outChildren[c] = act.childId;
          outRewards[c] = act.reward;
          sumBestQ += act.bestQ;
        }

        const double updatedV = sumBestQ * (1.0 / static_cast<double>(NUM_PIECES));
        const double diff = std::abs(updatedV - vCur[i]);
        if (diff > localMax) {
          localMax = diff;
        }
        vNext[i] = updatedV;
      }
      threadMaxDiff[static_cast<size_t>(tid)] = localMax;
    });

    vCur.swap(vNext);
    result.outerIterations = outer;
    result.finalBellmanResidual = *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end());

    if (cfg.verbose) {
      const uint32_t emptyId = index.find(0ULL);
      const double v0 = (emptyId != INVALID_BOARD_ID) ? vCur[emptyId] : MISSING_BOARD_VALUE;
      std::printf(
          "  [MPI outer %3d] bellman_residual = %.3e | V(empty) = %.6f | inner_sweeps = %d\n",
          outer, result.finalBellmanResidual, v0, result.totalInnerSweeps);
      std::fflush(stdout);
    }

    if (result.finalBellmanResidual <= cfg.tolerance) {
      break;
    }

    // 2. Fast fixed-policy evaluation sweeps (no move generation or hash lookups).
    const double innerThreshold = cfg.tolerance * cfg.innerStopRatio;
    for (int s = 0; s < cfg.innerEvalSweeps; ++s) {
      std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
      runParallelChunks([&](int tid, size_t begin, size_t end) {
        double localMax = 0.0;
        for (size_t i = begin; i < end; ++i) {
          const uint32_t *children = &bestChild[i * static_cast<size_t>(NUM_PIECES)];
          const uint8_t *rewards = &bestReward[i * static_cast<size_t>(NUM_PIECES)];
          double sumQ = 0.0;
          for (int c = 0; c < NUM_PIECES; ++c) {
            double q = static_cast<double>(rewards[c]);
            const uint32_t cid = children[c];
            if (cid != INVALID_BOARD_ID) {
              q += vCur[cid];
            }
            sumQ += q;
          }
          const double updatedV = sumQ * (1.0 / static_cast<double>(NUM_PIECES));
          const double diff = std::abs(updatedV - vCur[i]);
          if (diff > localMax) {
            localMax = diff;
          }
          vNext[i] = updatedV;
        }
        threadMaxDiff[static_cast<size_t>(tid)] = localMax;
      });

      vCur.swap(vNext);
      ++result.totalInnerSweeps;
      const double innerDiff = *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end());
      if (innerDiff <= innerThreshold) {
        break;
      }
    }
  }

  // Release temporary MPI buffers (~43 bytes/board) before final residual check & output
  // allocation.
  std::vector<uint32_t>().swap(bestChild);
  std::vector<uint8_t>().swap(bestReward);
  std::vector<double>().swap(vNext);

  // Measure exact Bellman residual of the final returned value vector vCur.
  std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
  runParallelChunks([&](int tid, size_t begin, size_t end) {
    TransitionList tl;
    double localMax = 0.0;
    for (size_t i = begin; i < end; ++i) {
      const uint64_t b = sortedBoards[i];
      double sumBestQ = 0.0;
      for (int c = 0; c < NUM_PIECES; ++c) {
        sumBestQ += detail::bestBellmanAction(b, c, index, vCur, tl).bestQ;
      }
      const double backup = sumBestQ * (1.0 / static_cast<double>(NUM_PIECES));
      const double diff = std::abs(backup - vCur[i]);
      if (diff > localMax) {
        localMax = diff;
      }
    }
    threadMaxDiff[static_cast<size_t>(tid)] = localMax;
  });
  result.finalBellmanResidual = *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end());

  const uint32_t emptyId = index.find(0ULL);
  result.emptyBoardValue = (emptyId != INVALID_BOARD_ID) ? vCur[emptyId] : MISSING_BOARD_VALUE;

  result.values.resize(n);
  for (size_t i = 0; i < n; ++i) {
    result.values[i] = static_cast<float>(vCur[i]);
  }
  result.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

inline ViSolveResult solveValueIterationMPI(const std::vector<uint64_t> &sortedBoards,
                                            const ViSolveConfig &cfg = ViSolveConfig{}) {
  const FlatBoardIndex index(sortedBoards);
  return solveValueIterationMPI(sortedBoards, index, cfg);
}

struct TablebaseMoveChoice {
  Transition transition{0ULL, 0};
  double tablebaseQ = 0.0;
  bool afterInTablebase = false;
  bool usedFallback = false;
  bool found = false;
};

// Selects a move using a loaded No-Next-Box tablebase (MappedTablebase or IndexedTablebase):
//   - If closedWorld == true: strictly maximizes r + V_tb(b') with V_tb(b') = 0 for b' outside TB,
//     breaking ties in move-generation order (exact optimal policy of the closed-world MDP).
//   - If closedWorld == false: maximizes r + V_tb(b') when at least one child lands in TB, breaking
//     exact tablebaseQ ties lazily via the fallbackPly heuristic (matching /state-info and
//     index.html), and falls back to the heuristic when all legal moves exit the tablebase.
template <typename NnbTablebase>
inline TablebaseMoveChoice chooseTablebaseMove(uint64_t board, int pieceIdx, const NnbTablebase &tb,
                                               bool closedWorld, int fallbackPly = 2,
                                               const EvalWeights &w = EvalWeights{}) {
  assert(fallbackPly == 1 || fallbackPly == 2);
  TransitionList tl;
  generateTransitions(board, pieceIdx, tl);
  if (tl.empty()) {
    return {};
  }

  const auto evalHeur = [&](const Transition &t) {
    return (fallbackPly == 2) ? scoreTransition2Ply(t.afterBoard, t.linesCleared, w)
                              : scoreTransition1Ply(t.afterBoard, t.linesCleared, w);
  };

  bool anyInTb = false;
  int bestIdx = 0;
  double bestQ = 0.0;
  bool bestInTb = false;
  bool hasBestHeur = false;
  double bestTieHeur = 0.0;

  for (int i = 0; i < tl.size(); ++i) {
    float childVal = 0.0f;
    const bool inTb = tb.lookup(tl[i].afterBoard, childVal);
    anyInTb = anyInTb || inTb;
    const double q =
        static_cast<double>(tl[i].linesCleared) + (inTb ? static_cast<double>(childVal) : 0.0);
    if (i == 0 || q > bestQ) {
      bestQ = q;
      bestIdx = i;
      bestInTb = inTb;
      hasBestHeur = false;
    } else if (!closedWorld && q == bestQ) {
      if (!hasBestHeur) {
        bestTieHeur = evalHeur(tl[bestIdx]);
        hasBestHeur = true;
      }
      const double candHeur = evalHeur(tl[i]);
      if (candHeur > bestTieHeur) {
        bestIdx = i;
        bestInTb = inTb;
        bestTieHeur = candHeur;
      }
    }
  }

  if (closedWorld || anyInTb) {
    return {tl[bestIdx], bestQ, bestInTb, false, true};
  }

  const ChosenTransition fb = (fallbackPly == 2) ? pickBestTransition2Ply(board, pieceIdx, w)
                                                 : pickBestTransition1Ply(board, pieceIdx, w);
  if (!fb.found) {
    return {};
  }
  return {fb.transition, static_cast<double>(fb.transition.linesCleared), false, true, true};
}

// Hybrid Next-Box move selector using a No-Next-Box tablebase (SRTBV001): searches all legal
// (curPiece, nextPiece) pairs and maximizes r1 + r2 + V_nnb(b2), where out-of-TB b2 leaves get
// V = 0.0 and nextPiece top-outs contribute r1 + 0.0.
// Note: TablebaseMoveChoice::afterInTablebase reports whether the winning 2-ply leaf b2 landed in
// nnbTb (not the 1-ply transition.afterBoard). If no 2-ply leaf lands in the tablebase (or nnbTb
// is unloaded), falls back to pickBestNextBoxHeuristic with tablebaseQ = r1 + max_r2. Exact Q ties
// break lazily via the Next-Box heuristic.
template <typename NnbTablebase>
inline TablebaseMoveChoice chooseNextBoxHybridMove(uint64_t board, int curPiece, int nextPiece,
                                                   const NnbTablebase &nnbTb,
                                                   const EvalWeights &w = EvalWeights{}) {
  TransitionList tl1;
  generateTransitions(board, curPiece, tl1);
  if (tl1.empty()) {
    return {};
  }

  TransitionList tl2;
  const auto makeFallbackChoice = [&]() -> TablebaseMoveChoice {
    const ChosenTransition fb = pickBestNextBoxHeuristic(board, curPiece, nextPiece, w);
    if (!fb.found) {
      return {};
    }
    generateTransitions(fb.transition.afterBoard, nextPiece, tl2);
    int maxR2 = 0;
    for (int j = 0; j < tl2.size(); ++j) {
      if (tl2[j].linesCleared > maxR2) {
        maxR2 = tl2[j].linesCleared;
      }
    }
    return {fb.transition, static_cast<double>(fb.transition.linesCleared + maxR2), false, true,
            true};
  };

  if (!nnbTb.isLoaded() || nnbTb.size() == 0) {
    return makeFallbackChoice();
  }

  bool anyLeafInTb = false;
  int bestIdx = 0;
  double bestQ = 0.0;
  bool bestLeafInTb = false;
  bool hasBestHeur = false;
  double bestTieHeur = 0.0;

  for (int i = 0; i < tl1.size(); ++i) {
    const Transition &t1 = tl1[i];
    const double r1 = static_cast<double>(t1.linesCleared);
    generateTransitions(t1.afterBoard, nextPiece, tl2);

    double branchQ = r1;
    bool branchBestInTb = false;
    if (!tl2.empty()) {
      for (int j = 0; j < tl2.size(); ++j) {
        float leafVal = 0.0f;
        const bool inTb = nnbTb.lookup(tl2[j].afterBoard, leafVal);
        anyLeafInTb = anyLeafInTb || inTb;
        const double q = r1
                         + static_cast<double>(tl2[j].linesCleared)
                         + (inTb ? static_cast<double>(leafVal) : 0.0);
        if (j == 0 || q > branchQ || (q == branchQ && inTb && !branchBestInTb)) {
          branchQ = q;
          branchBestInTb = inTb;
        }
      }
    }

    if (i == 0 || branchQ > bestQ) {
      bestQ = branchQ;
      bestIdx = i;
      bestLeafInTb = branchBestInTb;
      hasBestHeur = false;
    } else if (branchQ == bestQ) {
      if (!hasBestHeur) {
        bestTieHeur = scoreNextBoxHeuristicTransition(tl1[bestIdx].afterBoard,
                                                      tl1[bestIdx].linesCleared, nextPiece, w);
        hasBestHeur = true;
      }
      const double candHeur =
          scoreNextBoxHeuristicTransition(t1.afterBoard, t1.linesCleared, nextPiece, w);
      if (candHeur > bestTieHeur) {
        bestIdx = i;
        bestLeafInTb = branchBestInTb;
        bestTieHeur = candHeur;
      }
    }
  }

  if (anyLeafInTb) {
    return {tl1[bestIdx], bestQ, bestLeafInTb, false, true};
  }
  return makeFallbackChoice();
}

struct TablebaseEvalStats {
  int games = 0;
  double meanLines = 0.0;
  double stdErrLines = 0.0;
  double medianLines = 0.0;
  int p90Lines = 0;
  int maxLines = 0;
  double inTbMovePct = 0.0;
  double fallbackMovePct = 0.0;
  double fullyCoveredGamePct = 0.0;
  int stepCapHits = 0;
  HoleStratifiedCounts byHoles{};
};

// Runs multi-threaded deterministic Monte Carlo evaluation of a loaded MappedTablebase.
// In closedWorld mode, exiting the tablebase ends the episode with 0 continuation reward (verifying
// empirical mean lines directly against V(0ULL)). In live mode (closedWorld == false), all-OUT
// states fall back to fallbackPly heuristic play.
inline TablebaseEvalStats evaluateTablebasePolicy(const MappedTablebase &mappedTb, bool closedWorld,
                                                  int numGames, uint64_t baseSeed, int numThreads,
                                                  int fallbackPly = 2,
                                                  const EvalWeights &w = EvalWeights{},
                                                  int maxSteps = DEFAULT_EVAL_MAX_STEPS) {
  assert(fallbackPly == 1 || fallbackPly == 2);
  assert(numGames >= 0);
  assert(maxSteps > 0);
  TablebaseEvalStats stats{};
  if (numGames <= 0) {
    return stats;
  }
  stats.games = numGames;

  const int threads = std::max(1, numThreads);
  std::vector<int> gameLines(static_cast<size_t>(numGames), 0);
  std::vector<HoleStratifiedCounts> threadHoles(static_cast<size_t>(threads));
  std::vector<uint64_t> threadFallbacks(static_cast<size_t>(threads), 0);
  std::atomic<int> fullyCoveredGames{0};
  std::atomic<int> stepCapHits{0};
  std::atomic<int> nextGame{0};

  detail::runWorkerPool(
      threads, static_cast<size_t>(numGames), /*minParallelWork=*/32, [&](int tid) {
        HoleStratifiedCounts localHoles{};
        uint64_t localFallback = 0;
        int localFull = 0;
        int localCapHits = 0;

        while (true) {
          const int g = nextGame.fetch_add(1, std::memory_order_relaxed);
          if (g >= numGames) {
            break;
          }

          Rng rng(detail::deriveGameSeed(baseSeed, g));
          uint64_t board = 0ULL;
          int lines = 0;
          bool terminated = false;
          bool allIn = true;

          for (int step = 0; step < maxSteps; ++step) {
            const int piece = rng.nextPiece();
            const TablebaseMoveChoice mv =
                chooseTablebaseMove(board, piece, mappedTb, closedWorld, fallbackPly, w);
            if (!mv.found) {
              terminated = true;
              break;
            }
            lines += mv.transition.linesCleared;
            const uint64_t nextBoard = mv.transition.afterBoard;
            const int hb = HoleStratifiedCounts::bucket(countHoles(nextBoard));
            ++localHoles.total[static_cast<size_t>(hb)];
            if (mv.usedFallback) {
              ++localFallback;
            }
            if (mv.afterInTablebase) {
              ++localHoles.inSet[static_cast<size_t>(hb)];
              board = nextBoard;
            } else {
              allIn = false;
              if (closedWorld) {
                // Exiting S in the closed-world MDP yields 0.0 continuation.
                terminated = true;
                break;
              }
              board = nextBoard;
            }
          }
          if (!terminated) {
            ++localCapHits;
          }
          if (allIn) {
            ++localFull;
          }
          gameLines[static_cast<size_t>(g)] = lines;
        }

        threadHoles[static_cast<size_t>(tid)] = localHoles;
        threadFallbacks[static_cast<size_t>(tid)] = localFallback;
        fullyCoveredGames.fetch_add(localFull, std::memory_order_relaxed);
        stepCapHits.fetch_add(localCapHits, std::memory_order_relaxed);
      });

  uint64_t totalFallbacks = 0;
  for (int t = 0; t < threads; ++t) {
    totalFallbacks += threadFallbacks[static_cast<size_t>(t)];
    stats.byHoles.accumulate(threadHoles[static_cast<size_t>(t)]);
  }

  const detail::DistributionStats dist = detail::computeDistributionStats(std::move(gameLines));
  stats.meanLines = dist.mean;
  stats.stdErrLines = dist.stdErr;
  stats.medianLines = dist.median;
  stats.p90Lines = dist.p90;
  stats.maxLines = dist.maxVal;

  const uint64_t totMoves = stats.byHoles.allTotal();
  if (totMoves > 0) {
    stats.inTbMovePct =
        100.0 * static_cast<double>(stats.byHoles.allInSet()) / static_cast<double>(totMoves);
    stats.fallbackMovePct =
        100.0 * static_cast<double>(totalFallbacks) / static_cast<double>(totMoves);
  }
  stats.fullyCoveredGamePct =
      100.0
      * static_cast<double>(fullyCoveredGames.load(std::memory_order_relaxed))
      / static_cast<double>(numGames);
  stats.stepCapHits = stepCapHits.load(std::memory_order_relaxed);
  return stats;
}

} // namespace tb

#endif // TABLEBASE_VI_SOLVER_HPP
