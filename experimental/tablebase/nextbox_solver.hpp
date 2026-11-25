#ifndef TABLEBASE_NEXTBOX_SOLVER_HPP
#define TABLEBASE_NEXTBOX_SOLVER_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "tablebase_io.hpp"
#include "vi_solver.hpp"

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
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <unistd.h>
#else
#include <fstream>
#include <process.h>
#endif

namespace tb {

// Packed Next-Box MPI policy word (4 bytes):
//   - Bit 31 == 0 (in-TB or zero-payoff out-of-TB transition):
//       bits  0..27 : childId in [0 .. 268,435,454] (0x0FFFFFFF = out-of-set)
//       bits 28..30 : immediate linesCleared reward in [0 .. 4]
//   - Bit 31 == 1 (terminal fixed-point boundary payoff qFixed >= 0.0):
//       bits  0..30 : round(clamp(qFixed, 0.0, NB_TERMINAL_MAX_PAYOFF) * NB_TERMINAL_PAYOFF_SCALE)
//                     where scale = 2^20 (9.54e-7 line resolution, range [0, 2047.0])
inline constexpr uint32_t NB_ACTION_TERMINAL_FLAG = 0x80000000U;
inline constexpr uint32_t NB_ACTION_CHILD_MASK = 0x0FFFFFFFU;
inline constexpr uint32_t NB_ACTION_OUT_CHILD = 0x0FFFFFFFU;
inline constexpr int NB_ACTION_REWARD_SHIFT = 28;
inline constexpr uint32_t NB_ACTION_REWARD_MASK = 0x7U;
inline constexpr double NB_TERMINAL_PAYOFF_SCALE = 1048576.0; // 2^20
inline constexpr double NB_TERMINAL_MAX_PAYOFF = 2047.0;
inline constexpr size_t NB_STATES_PER_BOARD = static_cast<size_t>(NUM_PIECES);
inline constexpr size_t NB_ACTIONS_PER_BOARD = NB_STATES_PER_BOARD * NB_STATES_PER_BOARD; // 49

static_assert((NB_ACTION_CHILD_MASK & NB_ACTION_TERMINAL_FLAG) == 0U,
              "Child ID mask must not overlap terminal payoff flag");
static_assert((NB_ACTION_CHILD_MASK >> NB_ACTION_REWARD_SHIFT) == 0U,
              "Child ID mask must not overlap reward bits");
static_assert(static_cast<uint32_t>(BOARD_ROWS) <= NB_ACTION_REWARD_MASK,
              "BOARD_ROWS must fit in the packed reward field");
static_assert(((static_cast<uint32_t>(BOARD_ROWS) << NB_ACTION_REWARD_SHIFT)
               & NB_ACTION_TERMINAL_FLAG)
                  == 0U,
              "Packed reward bits must not overlap terminal payoff flag");
static_assert(static_cast<uint64_t>(NB_TERMINAL_MAX_PAYOFF *NB_TERMINAL_PAYOFF_SCALE)
                  <= 0x7FFFFFFFULL,
              "Max fixed-point terminal payoff must fit in 31 bits");

constexpr bool isNextBoxTerminalPayoff(uint32_t packed) noexcept {
  return (packed & NB_ACTION_TERMINAL_FLAG) != 0U;
}

constexpr uint32_t packNextBoxAction(uint32_t childId, uint8_t linesCleared) noexcept {
  assert(linesCleared <= BOARD_ROWS);
  assert(childId == INVALID_BOARD_ID || childId < NB_ACTION_OUT_CHILD);
  const uint32_t packedChild = (childId == INVALID_BOARD_ID) ? NB_ACTION_OUT_CHILD : childId;
  return (static_cast<uint32_t>(linesCleared) << NB_ACTION_REWARD_SHIFT) | packedChild;
}

inline uint32_t packNextBoxTerminalPayoff(double qFixed) noexcept {
  const double clamped =
      std::isfinite(qFixed) ? std::clamp(qFixed, 0.0, NB_TERMINAL_MAX_PAYOFF) : 0.0;
  const uint32_t fixed = static_cast<uint32_t>(std::llround(clamped * NB_TERMINAL_PAYOFF_SCALE));
  return NB_ACTION_TERMINAL_FLAG | (fixed & ~NB_ACTION_TERMINAL_FLAG);
}

constexpr double unpackNextBoxTerminalPayoff(uint32_t packed) noexcept {
  return static_cast<double>(packed & ~NB_ACTION_TERMINAL_FLAG) / NB_TERMINAL_PAYOFF_SCALE;
}

constexpr uint32_t unpackNextBoxChild(uint32_t packed) noexcept {
  if (isNextBoxTerminalPayoff(packed)) {
    return INVALID_BOARD_ID;
  }
  const uint32_t raw = packed & NB_ACTION_CHILD_MASK;
  return (raw == NB_ACTION_OUT_CHILD) ? INVALID_BOARD_ID : raw;
}

constexpr uint8_t unpackNextBoxReward(uint32_t packed) noexcept {
  assert(!isNextBoxTerminalPayoff(packed));
  return static_cast<uint8_t>((packed >> NB_ACTION_REWARD_SHIFT) & NB_ACTION_REWARD_MASK);
}

// Default hard-boundary evaluator (zero continuation outside the tablebase).
struct NoOpBoundaryEval {
  static constexpr bool enabled = false;
  void evalRow(uint64_t /*afterBoard*/, double /*outCont*/[NUM_PIECES]) const noexcept {}
};

enum class PolicyCacheMode {
  Auto,
  Ram,
  Disk,
};

constexpr const char *policyCacheModeName(PolicyCacheMode mode) noexcept {
  switch (mode) {
  case PolicyCacheMode::Ram:
    return "ram";
  case PolicyCacheMode::Disk:
    return "disk";
  case PolicyCacheMode::Auto:
  default:
    return "auto";
  }
}

constexpr bool parsePolicyCacheMode(std::string_view s, PolicyCacheMode &out) noexcept {
  if (s == "auto") {
    out = PolicyCacheMode::Auto;
    return true;
  }
  if (s == "ram") {
    out = PolicyCacheMode::Ram;
    return true;
  }
  if (s == "disk") {
    out = PolicyCacheMode::Disk;
    return true;
  }
  return false;
}

namespace detail {

using TransitionProbMatrix = std::array<std::array<double, NUM_PIECES>, NUM_PIECES>;

inline TransitionProbMatrix buildTransitionProbMatrix(RngMode mode) noexcept {
  TransitionProbMatrix mat{};
  for (int c = 0; c < NUM_PIECES; ++c) {
    for (int next = 0; next < NUM_PIECES; ++next) {
      mat[static_cast<size_t>(c)][static_cast<size_t>(next)] = pieceTransitionProb(mode, c, next);
    }
  }
  return mat;
}

// Sequential chunked storage for the N * 49 packed uint32_t MPI policy table.
// Supports either in-RAM storage (zero copy) or a temporary disk file (auto-unlinked on close/exit,
// ~12.8 MB RAM regardless of board count). Outer and inner MPI loops use the exact same chunk API
// so Ram and Disk modes produce bit-for-bit identical results.
class PolicyActionStore {
public:
  static constexpr size_t CHUNK_BOARDS = 65536; // 65,536 * 49 * 4 B = 12.8 MB streaming buffer

#ifndef _WIN32
  static_assert(sizeof(off_t) >= 8, "64-bit off_t required for >4 GB policy cache files");
#else
  static_assert(sizeof(std::streamoff) >= 8,
                "64-bit std::streamoff required for >4 GB policy cache files");
#endif

  PolicyActionStore() = default;
  PolicyActionStore(const PolicyActionStore &) = delete;
  PolicyActionStore &operator=(const PolicyActionStore &) = delete;
  ~PolicyActionStore() { close(); }

  bool init(size_t numBoards, bool useDisk, std::string *errOut = nullptr) {
    close();
    numBoards_ = numBoards;
    useDisk_ = useDisk;
    if (numBoards_ == 0) {
      return true;
    }

    if (!useDisk_) {
      ramBuf_.assign(numBoards_ * NB_ACTIONS_PER_BOARD, packNextBoxAction(INVALID_BOARD_ID, 0));
      return true;
    }

    const size_t chunkElems = std::min(numBoards_, CHUNK_BOARDS) * NB_ACTIONS_PER_BOARD;
    ioBuf_.assign(chunkElems, 0U);

    std::error_code ec;
    const std::filesystem::path tmpDir = std::filesystem::temp_directory_path(ec);
    const std::filesystem::path dir = ec ? std::filesystem::path(".") : tmpDir;

#ifndef _WIN32
    std::string tmpl = (dir / "sr_nb_policy_XXXXXX").string();
    std::vector<char> pathBuf(tmpl.begin(), tmpl.end());
    pathBuf.push_back('\0');
    fd_ = ::mkstemp(pathBuf.data());
    if (fd_ < 0) {
      if (errOut)
        *errOut = "Failed to create temporary policy cache file in "
                  + dir.string()
                  + ": "
                  + std::strerror(errno);
      close();
      return false;
    }
    // Unlink immediately so the OS reclaims disk space even if the process exits abnormally.
    if (::unlink(pathBuf.data()) != 0) {
      tempPath_ = std::filesystem::path(pathBuf.data());
    }
#else
    static std::atomic<uint64_t> uniqueSeq{0};
    const uint64_t seq = uniqueSeq.fetch_add(1, std::memory_order_relaxed);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    tempPath_ = dir
                / ("sr_nb_policy_"
                   + std::to_string(::_getpid())
                   + "_"
                   + std::to_string(stamp)
                   + "_"
                   + std::to_string(seq)
                   + ".tmp");
    stream_.open(tempPath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream_) {
      if (errOut)
        *errOut = "Failed to create temporary policy cache file: " + tempPath_.string();
      close();
      return false;
    }
#endif
    return true;
  }

  void close() noexcept {
#ifndef _WIN32
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#else
    if (stream_.is_open()) {
      stream_.close();
    }
#endif
    if (!tempPath_.empty()) {
      std::error_code ec;
      std::filesystem::remove(tempPath_, ec);
      tempPath_.clear();
    }
    std::vector<uint32_t>().swap(ramBuf_);
    std::vector<uint32_t>().swap(ioBuf_);
    numBoards_ = 0;
    useDisk_ = false;
  }

  bool isDisk() const noexcept { return useDisk_; }

  uint32_t *beginWriteChunk(size_t chunkBegin, size_t chunkCount) noexcept {
    assert(chunkBegin + chunkCount <= numBoards_);
    (void)chunkCount;
    if (!useDisk_) {
      return ramBuf_.data() + chunkBegin * NB_ACTIONS_PER_BOARD;
    }
    assert(chunkCount <= CHUNK_BOARDS);
    return ioBuf_.data();
  }

  bool finishWriteChunk(size_t chunkBegin, size_t chunkCount, std::string *errOut = nullptr) {
    assert(chunkBegin + chunkCount <= numBoards_);
    if (!useDisk_ || chunkCount == 0) {
      return true;
    }
    return writeBytesAt(ioBuf_.data(), chunkBytes(chunkCount), boardByteOffset(chunkBegin), errOut);
  }

  const uint32_t *readChunk(size_t chunkBegin, size_t chunkCount, std::string *errOut = nullptr) {
    assert(chunkBegin + chunkCount <= numBoards_);
    if (!useDisk_) {
      return ramBuf_.data() + chunkBegin * NB_ACTIONS_PER_BOARD;
    }
    if (chunkCount == 0) {
      return ioBuf_.data();
    }
    assert(chunkCount <= CHUNK_BOARDS);
    if (!readBytesAt(ioBuf_.data(), chunkBytes(chunkCount), boardByteOffset(chunkBegin), errOut)) {
      return nullptr;
    }
    return ioBuf_.data();
  }

private:
  size_t numBoards_ = 0;
  bool useDisk_ = false;
  std::vector<uint32_t> ramBuf_;
  std::vector<uint32_t> ioBuf_;
  std::filesystem::path tempPath_;
#ifndef _WIN32
  int fd_ = -1;
#else
  std::fstream stream_;
#endif

  static constexpr size_t chunkBytes(size_t boards) noexcept {
    return boards * NB_ACTIONS_PER_BOARD * sizeof(uint32_t);
  }

  static constexpr uint64_t boardByteOffset(size_t chunkBegin) noexcept {
    return static_cast<uint64_t>(chunkBegin) * NB_ACTIONS_PER_BOARD * sizeof(uint32_t);
  }

  bool writeBytesAt(const void *src, size_t nBytes, uint64_t offset, std::string *errOut) {
#ifndef _WIN32
    const auto *ptr = static_cast<const uint8_t *>(src);
    size_t done = 0;
    while (done < nBytes) {
      const ssize_t w = ::pwrite(fd_, ptr + done, nBytes - done,
                                 static_cast<off_t>(offset + static_cast<uint64_t>(done)));
      if (w < 0) {
        if (errno == EINTR)
          continue;
        if (errOut)
          *errOut =
              std::string("I/O error writing temporary policy cache file: ") + std::strerror(errno);
        return false;
      }
      if (w == 0) {
        if (errOut)
          *errOut = "Short zero-byte write to temporary policy cache file";
        return false;
      }
      done += static_cast<size_t>(w);
    }
    return true;
#else
    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.write(reinterpret_cast<const char *>(src), static_cast<std::streamsize>(nBytes));
    if (!stream_.good()) {
      if (errOut)
        *errOut = "I/O error writing temporary policy cache file";
      return false;
    }
    return true;
#endif
  }

  bool readBytesAt(void *dst, size_t nBytes, uint64_t offset, std::string *errOut) {
#ifndef _WIN32
    auto *ptr = static_cast<uint8_t *>(dst);
    size_t done = 0;
    while (done < nBytes) {
      const ssize_t r = ::pread(fd_, ptr + done, nBytes - done,
                                static_cast<off_t>(offset + static_cast<uint64_t>(done)));
      if (r < 0) {
        if (errno == EINTR)
          continue;
        if (errOut)
          *errOut =
              std::string("I/O error reading temporary policy cache file: ") + std::strerror(errno);
        return false;
      }
      if (r == 0) {
        if (errOut)
          *errOut = "Unexpected EOF reading temporary policy cache file";
        return false;
      }
      done += static_cast<size_t>(r);
    }
    return true;
#else
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(nBytes));
    if (!stream_.good()) {
      if (errOut)
        *errOut = "I/O error reading temporary policy cache file";
      return false;
    }
    return true;
#endif
  }
};

// Runs a parallel chunked worker over [chunkBegin, chunkEnd), invoking fn(tid, subBegin, subEnd)
// where subBegin/subEnd are absolute board indices.
template <typename ChunkFn>
inline void runParallelSubrange(int numThreads, size_t chunkBegin, size_t chunkEnd,
                                size_t minParallelWork, ChunkFn &&fn) {
  const size_t count = chunkEnd - chunkBegin;
  if (count == 0) {
    return;
  }
  const int threads =
      static_cast<int>(std::min<size_t>(static_cast<size_t>(std::max(1, numThreads)), count));
  if (threads == 1 || count < minParallelWork) {
    fn(0, chunkBegin, chunkEnd);
    return;
  }
  const size_t slice = (count + static_cast<size_t>(threads) - 1) / static_cast<size_t>(threads);
  runWorkerPool(threads, static_cast<size_t>(threads), /*minParallelWork=*/2, [&](int tid) {
    const size_t sBegin = chunkBegin + static_cast<size_t>(tid) * slice;
    if (sBegin >= chunkEnd) {
      return;
    }
    const size_t sEnd = std::min(chunkEnd, sBegin + slice);
    fn(tid, sBegin, sEnd);
  });
}

template <typename BoundaryFn>
inline void evalSanitizedBoundaryRow(uint64_t board, const BoundaryFn &fn,
                                     double outRow[NUM_PIECES]) noexcept {
  fn.evalRow(board, outRow);
  for (int n = 0; n < NUM_PIECES; ++n) {
    if (!std::isfinite(outRow[n]) || outRow[n] < 0.0) {
      outRow[n] = 0.0;
    }
  }
}

template <bool BoundaryEnabled>
constexpr bool isBetterNextBoxMove(bool first, double q, bool inTb, double bestQ,
                                   bool bestInTb) noexcept {
  return first || (q > bestQ) || (BoundaryEnabled && q == bestQ && inTb && !bestInTb);
}

// Per-worker direct-mapped cache for deterministic out-of-TB boundary evaluations on outer
// iteration 1. Eliminates redundant neural evaluations on repeated frontier children with zero
// synchronization and bit-for-bit 1T == MT determinism.
struct alignas(128) SoftBoundaryCache {
  static constexpr int INDEX_BITS = 15;
  static constexpr size_t NUM_SLOTS = size_t{1} << INDEX_BITS; // 32,768 slots (~2 MB/worker)
  static constexpr uint64_t FIB_HASH_MULT = 11400714819323198485ULL;

  struct Entry {
    uint64_t key = ~0ULL; // Sentinel outside the 40-bit board key domain
    double vals[NUM_PIECES]{};
  };

  std::vector<Entry> slots;

  void init() {
    if (slots.size() != NUM_SLOTS) {
      slots.assign(NUM_SLOTS, Entry{});
    }
  }

  template <typename BoundaryFn>
  const double *evalCached(uint64_t board, const BoundaryFn &fn) noexcept {
    assert(slots.size() == NUM_SLOTS);
    const size_t idx = static_cast<size_t>((board * FIB_HASH_MULT) >> (64 - INDEX_BITS));
    Entry &e = slots[idx];
    if (e.key != board) {
      evalSanitizedBoundaryRow(board, fn, e.vals);
      e.key = board;
    }
    return e.vals;
  }
};

// Computes one exact Next-Box Bellman backup for (board, curPiece) against vCur, with optional
// discount factor gamma and frozen soft-boundary evaluator BoundaryFn.
// Moves are iterated outermost so each child board is hashed once and its 7-entry value row is read
// sequentially across all next pieces n = 0..6. When precomputedBestOut != nullptr (outer iters >=
// 2 and final residual), out-of-TB moves are skipped completely and initialized from the iter-1
// precomputed terminal words (exact bit-for-bit parity with zero neural re-evaluation).
template <bool RecordActions, typename BoundaryFn = NoOpBoundaryEval>
inline double
computeNextBoxBellmanBackup(uint64_t board, int curPiece, const FlatBoardIndex &index,
                            const std::vector<double> &vCur, const TransitionProbMatrix &transProb,
                            double gamma, const BoundaryFn &boundaryFn,
                            SoftBoundaryCache *boundaryCache, const uint32_t *precomputedBestOut,
                            TransitionList &scratch, uint32_t *outActions = nullptr,
                            uint32_t *outBestOut = nullptr, bool *outTerminalOverflow = nullptr) {
  generateTransitions(board, curPiece, scratch);
  if (scratch.empty()) {
    if constexpr (RecordActions) {
      for (int next = 0; next < NUM_PIECES; ++next) {
        outActions[next] = packNextBoxAction(INVALID_BOARD_ID, 0);
      }
    }
    if (outBestOut != nullptr) {
      for (int next = 0; next < NUM_PIECES; ++next) {
        outBestOut[next] = 0U;
      }
    }
    return 0.0;
  }

  bool hasBest[NUM_PIECES]{};
  double bestQ[NUM_PIECES]{};
  uint32_t bestWord[NUM_PIECES]{};
  bool bestInTb[NUM_PIECES]{};

  if constexpr (BoundaryFn::enabled) {
    if (precomputedBestOut != nullptr) {
      for (int next = 0; next < NUM_PIECES; ++next) {
        const uint32_t ow = precomputedBestOut[next];
        if (ow != 0U) {
          hasBest[next] = true;
          bestQ[next] = unpackNextBoxTerminalPayoff(ow);
          bestWord[next] = ow;
          bestInTb[next] = false;
        }
      }
    } else if (outBestOut != nullptr) {
      for (int next = 0; next < NUM_PIECES; ++next) {
        outBestOut[next] = 0U;
      }
    }
  }

  for (int m = 0; m < scratch.size(); ++m) {
    const Transition &tr = scratch[m];
    const uint32_t cid = index.find(tr.afterBoard);
    const bool inTb = (cid != INVALID_BOARD_ID);
    if constexpr (BoundaryFn::enabled) {
      if (!inTb && precomputedBestOut != nullptr) {
        continue;
      }
    }
    const double rew = static_cast<double>(tr.linesCleared);
    const double *childRow = inTb ? &vCur[static_cast<size_t>(cid) * NB_STATES_PER_BOARD] : nullptr;

    double stackBoundary[NUM_PIECES];
    const double *boundRow = nullptr;
    if constexpr (BoundaryFn::enabled) {
      if (!inTb) {
        if (boundaryCache != nullptr) {
          boundRow = boundaryCache->evalCached(tr.afterBoard, boundaryFn);
        } else {
          evalSanitizedBoundaryRow(tr.afterBoard, boundaryFn, stackBoundary);
          boundRow = stackBoundary;
        }
      }
    }

    for (int next = 0; next < NUM_PIECES; ++next) {
      double q = rew;
      uint32_t word = 0U;
      if (inTb) {
        q = rew + gamma * childRow[next];
        if constexpr (RecordActions) {
          word = packNextBoxAction(cid, tr.linesCleared);
        }
      } else if constexpr (BoundaryFn::enabled) {
        // Quantize soft-boundary terminal payoffs to exact packed fixed-point resolution so outer
        // sweeps, inner policy-eval sweeps, and final Bellman residuals agree to machine epsilon.
        const double qRaw = rew + gamma * boundRow[next];
        if (outTerminalOverflow != nullptr && qRaw > NB_TERMINAL_MAX_PAYOFF) {
          *outTerminalOverflow = true;
        }
        const uint32_t termWord = packNextBoxTerminalPayoff(qRaw);
        q = unpackNextBoxTerminalPayoff(termWord);
        if (outBestOut != nullptr && (outBestOut[next] == 0U || termWord > outBestOut[next])) {
          outBestOut[next] = termWord;
        }
        if constexpr (RecordActions) {
          word = termWord;
        }
      } else {
        if constexpr (RecordActions) {
          word = packNextBoxAction(INVALID_BOARD_ID, tr.linesCleared);
        }
      }

      if (isBetterNextBoxMove<BoundaryFn::enabled>(!hasBest[next], q, inTb, bestQ[next],
                                                   bestInTb[next])) {
        hasBest[next] = true;
        bestQ[next] = q;
        bestInTb[next] = inTb;
        if constexpr (RecordActions) {
          bestWord[next] = word;
        }
      }
    }
  }

  double updatedV = 0.0;
  const auto &probs = transProb[static_cast<size_t>(curPiece)];
  for (int next = 0; next < NUM_PIECES; ++next) {
    if constexpr (RecordActions) {
      outActions[next] = bestWord[next];
    }
    updatedV += probs[static_cast<size_t>(next)] * bestQ[next];
  }
  return updatedV;
}

// Two-pointer sorted merge for O(|S_prev| + |S_new|) warm-starting with strict validation of
// sortedness and finite non-negative values in initTb.
inline bool applyNextBoxWarmStart(const std::vector<uint64_t> &sortedBoards,
                                  const MappedNextBoxTablebase &initTb, std::vector<double> &vCur,
                                  size_t &outMatched, std::string &errOut) {
  outMatched = 0;
  const uint64_t initCount = initTb.size();
  if (initCount == 0) {
    return true;
  }
  uint64_t j = 0;
  uint64_t prevInitBoard = 0;
  bool havePrev = false;
  float row[NUM_PIECES];

  const auto advanceInit = [&](uint64_t &idx) -> bool {
    const uint64_t b = initTb.boardAt(idx);
    if (havePrev && b <= prevInitBoard) {
      errOut = "Warm-start tablebase boards are not strictly sorted ascending";
      return false;
    }
    prevInitBoard = b;
    havePrev = true;
    ++idx;
    return true;
  };

  for (size_t i = 0; i < sortedBoards.size() && j < initCount; ++i) {
    const uint64_t target = sortedBoards[i];
    while (j < initCount && initTb.boardAt(j) < target) {
      if (!advanceInit(j)) {
        return false;
      }
    }
    if (j < initCount && initTb.boardAt(j) == target) {
      initTb.copyValuesAt(j, row);
      double *dst = &vCur[i * NB_STATES_PER_BOARD];
      for (int c = 0; c < NUM_PIECES; ++c) {
        if (!std::isfinite(row[c]) || row[c] < 0.0f) {
          errOut = "Warm-start tablebase contains non-finite or negative value";
          return false;
        }
        dst[c] = static_cast<double>(row[c]);
      }
      ++outMatched;
      if (!advanceInit(j)) {
        return false;
      }
    }
  }
  return true;
}

template <typename BoundaryFn>
inline bool runNextBoxPolicyImprovementSweep(
    const std::vector<uint64_t> &sortedBoards, const FlatBoardIndex &index,
    const TransitionProbMatrix &transProb, double gamma, const BoundaryFn &boundaryFn,
    std::vector<SoftBoundaryCache> *threadCaches, PolicyActionStore *bestOutStore,
    bool writeBestOut, int threads, size_t minParallelBoards, const std::vector<double> &vCur,
    std::vector<double> &vNext, PolicyActionStore &policyStore, double &outMaxDiff,
    double &outMaxVal, std::string &errOut) {
  const size_t n = sortedBoards.size();
  const size_t chunkStride =
      policyStore.isDisk() ? PolicyActionStore::CHUNK_BOARDS : std::max<size_t>(1, n);
  double sweepMaxDiff = 0.0;
  double sweepMaxVal = 0.0;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);
  std::vector<double> threadMaxVal(static_cast<size_t>(threads), 0.0);
  std::atomic<bool> sawNonFinite{false};
  std::atomic<bool> sawTerminalOverflow{false};

  for (size_t chunkBegin = 0; chunkBegin < n; chunkBegin += chunkStride) {
    const size_t chunkEnd = std::min(n, chunkBegin + chunkStride);
    const size_t chunkCount = chunkEnd - chunkBegin;
    uint32_t *chunkActions = policyStore.beginWriteChunk(chunkBegin, chunkCount);
    uint32_t *chunkBestOutWrite = nullptr;
    const uint32_t *chunkBestOutRead = nullptr;
    if constexpr (BoundaryFn::enabled) {
      if (bestOutStore != nullptr) {
        if (writeBestOut) {
          chunkBestOutWrite = bestOutStore->beginWriteChunk(chunkBegin, chunkCount);
        } else {
          chunkBestOutRead = bestOutStore->readChunk(chunkBegin, chunkCount, &errOut);
          if (chunkBestOutRead == nullptr) {
            return false;
          }
        }
      }
    }
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
    std::fill(threadMaxVal.begin(), threadMaxVal.end(), 0.0);

    runParallelSubrange(
        threads, chunkBegin, chunkEnd, minParallelBoards,
        [&](int tid, size_t subBegin, size_t subEnd) {
          TransitionList tl;
          SoftBoundaryCache *cache =
              (threadCaches != nullptr) ? &(*threadCaches)[static_cast<size_t>(tid)] : nullptr;
          double localMaxDiff = 0.0;
          double localMaxVal = 0.0;
          bool localOverflow = false;
          for (size_t i = subBegin; i < subEnd; ++i) {
            const uint64_t b = sortedBoards[i];
            const size_t localIdx = i - chunkBegin;
            uint32_t *boardActions = &chunkActions[localIdx * NB_ACTIONS_PER_BOARD];
            uint32_t *boardBestOutWrite = (chunkBestOutWrite != nullptr)
                                              ? &chunkBestOutWrite[localIdx * NB_ACTIONS_PER_BOARD]
                                              : nullptr;
            const uint32_t *boardBestOutRead =
                (chunkBestOutRead != nullptr) ? &chunkBestOutRead[localIdx * NB_ACTIONS_PER_BOARD]
                                              : nullptr;
            const double *oldRow = &vCur[i * NB_STATES_PER_BOARD];
            double *newRow = &vNext[i * NB_STATES_PER_BOARD];

            for (int c = 0; c < NUM_PIECES; ++c) {
              const size_t cOff = static_cast<size_t>(c) * NB_STATES_PER_BOARD;
              const double updatedV = computeNextBoxBellmanBackup</*RecordActions=*/true>(
                  b, c, index, vCur, transProb, gamma, boundaryFn, cache,
                  (boardBestOutRead != nullptr) ? &boardBestOutRead[cOff] : nullptr, tl,
                  &boardActions[cOff],
                  (boardBestOutWrite != nullptr) ? &boardBestOutWrite[cOff] : nullptr,
                  &localOverflow);
              newRow[c] = updatedV;
              if (!std::isfinite(updatedV)) {
                sawNonFinite.store(true, std::memory_order_relaxed);
              } else {
                if (updatedV > localMaxVal) {
                  localMaxVal = updatedV;
                }
                const double diff = std::abs(updatedV - oldRow[c]);
                if (diff > localMaxDiff) {
                  localMaxDiff = diff;
                }
              }
            }
          }
          if (localOverflow) {
            sawTerminalOverflow.store(true, std::memory_order_relaxed);
          }
          threadMaxDiff[static_cast<size_t>(tid)] = localMaxDiff;
          threadMaxVal[static_cast<size_t>(tid)] = localMaxVal;
        });

    if (sawNonFinite.load(std::memory_order_relaxed)) {
      errOut = "Non-finite value encountered during policy-improvement sweep";
      return false;
    }
    if (sawTerminalOverflow.load(std::memory_order_relaxed)) {
      errOut = "Soft-boundary terminal payoff exceeded NB_TERMINAL_MAX_PAYOFF ("
               + std::to_string(NB_TERMINAL_MAX_PAYOFF)
               + ")";
      return false;
    }
    sweepMaxDiff =
        std::max(sweepMaxDiff, *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end()));
    sweepMaxVal =
        std::max(sweepMaxVal, *std::max_element(threadMaxVal.begin(), threadMaxVal.end()));
    if (!policyStore.finishWriteChunk(chunkBegin, chunkCount, &errOut)) {
      return false;
    }
    if constexpr (BoundaryFn::enabled) {
      if (bestOutStore != nullptr && writeBestOut) {
        if (!bestOutStore->finishWriteChunk(chunkBegin, chunkCount, &errOut)) {
          return false;
        }
      }
    }
  }
  outMaxDiff = sweepMaxDiff;
  outMaxVal = sweepMaxVal;
  return true;
}

inline bool runNextBoxPolicyEvalSweep(size_t numBoards, const TransitionProbMatrix &transProb,
                                      double gamma, int threads, size_t minParallelBoards,
                                      const std::vector<double> &vCur, std::vector<double> &vNext,
                                      PolicyActionStore &policyStore, double &outMaxDiff,
                                      double &outMaxVal, std::string &errOut) {
  const size_t chunkStride =
      policyStore.isDisk() ? PolicyActionStore::CHUNK_BOARDS : std::max<size_t>(1, numBoards);
  double sweepMaxDiff = 0.0;
  double sweepMaxVal = 0.0;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);
  std::vector<double> threadMaxVal(static_cast<size_t>(threads), 0.0);
  std::atomic<bool> sawNonFinite{false};

  for (size_t chunkBegin = 0; chunkBegin < numBoards; chunkBegin += chunkStride) {
    const size_t chunkEnd = std::min(numBoards, chunkBegin + chunkStride);
    const size_t chunkCount = chunkEnd - chunkBegin;
    const uint32_t *chunkActions = policyStore.readChunk(chunkBegin, chunkCount, &errOut);
    if (chunkActions == nullptr) {
      return false;
    }
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
    std::fill(threadMaxVal.begin(), threadMaxVal.end(), 0.0);

    runParallelSubrange(threads, chunkBegin, chunkEnd, minParallelBoards,
                        [&](int tid, size_t subBegin, size_t subEnd) {
                          double localMaxDiff = 0.0;
                          double localMaxVal = 0.0;
                          for (size_t i = subBegin; i < subEnd; ++i) {
                            const uint32_t *boardActions =
                                &chunkActions[(i - chunkBegin) * NB_ACTIONS_PER_BOARD];
                            const double *oldRow = &vCur[i * NB_STATES_PER_BOARD];
                            double *newRow = &vNext[i * NB_STATES_PER_BOARD];

                            for (int c = 0; c < NUM_PIECES; ++c) {
                              const uint32_t *cActions =
                                  &boardActions[static_cast<size_t>(c) * NB_STATES_PER_BOARD];
                              const auto &probs = transProb[static_cast<size_t>(c)];
                              double updatedV = 0.0;
                              for (int next = 0; next < NUM_PIECES; ++next) {
                                const uint32_t packed = cActions[next];
                                double q = 0.0;
                                if (isNextBoxTerminalPayoff(packed)) {
                                  q = unpackNextBoxTerminalPayoff(packed);
                                } else {
                                  q = static_cast<double>(unpackNextBoxReward(packed));
                                  const uint32_t cid = unpackNextBoxChild(packed);
                                  if (cid != INVALID_BOARD_ID) {
                                    q += gamma
                                         * vCur[static_cast<size_t>(cid) * NB_STATES_PER_BOARD
                                                + static_cast<size_t>(next)];
                                  }
                                }
                                updatedV += probs[static_cast<size_t>(next)] * q;
                              }
                              newRow[c] = updatedV;
                              if (!std::isfinite(updatedV)) {
                                sawNonFinite.store(true, std::memory_order_relaxed);
                              } else {
                                if (updatedV > localMaxVal) {
                                  localMaxVal = updatedV;
                                }
                                const double diff = std::abs(updatedV - oldRow[c]);
                                if (diff > localMaxDiff) {
                                  localMaxDiff = diff;
                                }
                              }
                            }
                          }
                          threadMaxDiff[static_cast<size_t>(tid)] = localMaxDiff;
                          threadMaxVal[static_cast<size_t>(tid)] = localMaxVal;
                        });

    if (sawNonFinite.load(std::memory_order_relaxed)) {
      errOut = "Non-finite value encountered during policy-evaluation sweep";
      return false;
    }
    sweepMaxDiff =
        std::max(sweepMaxDiff, *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end()));
    sweepMaxVal =
        std::max(sweepMaxVal, *std::max_element(threadMaxVal.begin(), threadMaxVal.end()));
  }
  outMaxDiff = sweepMaxDiff;
  outMaxVal = sweepMaxVal;
  return true;
}

template <typename BoundaryFn>
inline bool computeNextBoxBellmanResidual(const std::vector<uint64_t> &sortedBoards,
                                          const FlatBoardIndex &index,
                                          const TransitionProbMatrix &transProb, double gamma,
                                          const BoundaryFn &boundaryFn,
                                          PolicyActionStore *bestOutStore, int threads,
                                          size_t minParallelBoards, const std::vector<double> &vCur,
                                          double &outResidual, std::string &errOut) {
  const size_t n = sortedBoards.size();
  const size_t chunkStride = (bestOutStore != nullptr && bestOutStore->isDisk())
                                 ? PolicyActionStore::CHUNK_BOARDS
                                 : std::max<size_t>(1, n);
  double maxResidual = 0.0;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);

  for (size_t chunkBegin = 0; chunkBegin < n; chunkBegin += chunkStride) {
    const size_t chunkEnd = std::min(n, chunkBegin + chunkStride);
    const size_t chunkCount = chunkEnd - chunkBegin;
    const uint32_t *chunkBestOutRead = nullptr;
    if constexpr (BoundaryFn::enabled) {
      if (bestOutStore != nullptr) {
        chunkBestOutRead = bestOutStore->readChunk(chunkBegin, chunkCount, &errOut);
        if (chunkBestOutRead == nullptr) {
          return false;
        }
      }
    }
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);

    runParallelSubrange(
        threads, chunkBegin, chunkEnd, minParallelBoards,
        [&](int tid, size_t subBegin, size_t subEnd) {
          TransitionList tl;
          double localMax = 0.0;
          for (size_t i = subBegin; i < subEnd; ++i) {
            const uint64_t b = sortedBoards[i];
            const double *curRow = &vCur[i * NB_STATES_PER_BOARD];
            const uint32_t *boardBestOutRead =
                (chunkBestOutRead != nullptr)
                    ? &chunkBestOutRead[(i - chunkBegin) * NB_ACTIONS_PER_BOARD]
                    : nullptr;
            for (int c = 0; c < NUM_PIECES; ++c) {
              const uint32_t *cBestOut =
                  (boardBestOutRead != nullptr)
                      ? &boardBestOutRead[static_cast<size_t>(c) * NB_STATES_PER_BOARD]
                      : nullptr;
              const double backup = computeNextBoxBellmanBackup</*RecordActions=*/false>(
                  b, c, index, vCur, transProb, gamma, boundaryFn, nullptr, cBestOut, tl);
              const double diff = std::abs(backup - curRow[c]);
              if (diff > localMax) {
                localMax = diff;
              }
            }
          }
          threadMaxDiff[static_cast<size_t>(tid)] = localMax;
        });

    maxResidual =
        std::max(maxResidual, *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end()));
  }
  outResidual = maxResidual;
  return true;
}

} // namespace detail

struct NextBoxSolveConfig {
  RngMode rngMode = RngMode::Nes;
  PolicyCacheMode policyCacheMode = PolicyCacheMode::Auto;
  size_t autoDiskThresholdBoards = 6000000; // > 6M boards (~1.18 GB policy) streams through SSD
  double tolerance = 1e-7;
  int maxOuterIters = 250;
  int innerEvalSweeps = 50;
  double innerStopRatio = 0.05;
  double gamma = 1.0;                      // Discount factor in (0, 1]
  double maxValueDivergenceGuard = 1000.0; // Abort if any V exceeds this cap
  int numThreads = 1;
  bool verbose = false;
};

struct NextBoxSolveResult {
  std::vector<float> values; // Size N * 7 in I, O, L, J, T, S, Z order
  std::array<double, NUM_PIECES> emptyBoardValues{};
  double emptyBoardExpectedValue = MISSING_BOARD_VALUE;
  double maxBoardValue = 0.0;
  double finalBellmanResidual = 0.0;
  int outerIterations = 0;
  int totalInnerSweeps = 0;
  size_t warmStartBoardsMatched = 0;
  bool usedDiskPolicyCache = false;
  double elapsedSec = 0.0;
  bool ok = true;
  std::string errorMessage;
};

// Solves the 1-piece Next-Box Bellman equation on sortedBoards via parallel two-buffer Modified
// Policy Iteration (MPI) in IEEE-754 double precision:
//   V*(b, c) = sum_{n=0..6} P(n | c) * max_{b' in Moves(b, c)} [r(b, b') + gamma * C(b', n)],
// where C(b', n) = V*(b', n) for b' inside sortedBoards, and C(b', n) = B(b', n) from the frozen
// boundary evaluator BoundaryFn (0.0 for NoOpBoundaryEval) for b' outside sortedBoards.
template <typename BoundaryFn = NoOpBoundaryEval>
inline NextBoxSolveResult solveNextBoxMPI(const std::vector<uint64_t> &sortedBoards,
                                          const FlatBoardIndex &index,
                                          const NextBoxSolveConfig &cfg = NextBoxSolveConfig{},
                                          const MappedNextBoxTablebase *initTb = nullptr,
                                          const BoundaryFn &boundaryFn = BoundaryFn{}) {
  const auto t0 = std::chrono::steady_clock::now();
  const size_t n = sortedBoards.size();
  NextBoxSolveResult result{};
  result.emptyBoardValues.fill(MISSING_BOARD_VALUE);

  if (index.size() != n) {
    result.ok = false;
    result.errorMessage = "FlatBoardIndex size does not match sortedBoards size";
    return result;
  }
  if (!std::isfinite(cfg.gamma) || cfg.gamma <= 0.0 || cfg.gamma > 1.0) {
    result.ok = false;
    result.errorMessage = "Solver discount factor gamma must be finite and in (0, 1]";
    return result;
  }
  if (!std::isfinite(cfg.maxValueDivergenceGuard) || cfg.maxValueDivergenceGuard <= 0.0) {
    result.ok = false;
    result.errorMessage = "Solver maxValueDivergenceGuard must be finite and > 0";
    return result;
  }
  if (cfg.maxOuterIters <= 0) {
    result.ok = false;
    result.errorMessage = "Solver maxOuterIters must be >= 1";
    return result;
  }
  if (!std::isfinite(cfg.tolerance) || cfg.tolerance < 0.0) {
    result.ok = false;
    result.errorMessage = "Solver tolerance must be finite and >= 0";
    return result;
  }
  if (n == 0) {
    return result;
  }
  if (n >= static_cast<size_t>(NB_ACTION_OUT_CHILD)) {
    result.ok = false;
    result.errorMessage = "Board count exceeds 28-bit packed action capacity";
    return result;
  }
  if (initTb != nullptr && initTb->isLoaded() && initTb->rngMode() != cfg.rngMode) {
    result.ok = false;
    result.errorMessage = "Warm-start tablebase RngMode does not match solver RngMode";
    return result;
  }

  const detail::TransitionProbMatrix transProb = detail::buildTransitionProbMatrix(cfg.rngMode);

  const bool useDisk =
      (cfg.policyCacheMode == PolicyCacheMode::Disk)
      || (cfg.policyCacheMode == PolicyCacheMode::Auto && n > cfg.autoDiskThresholdBoards);
  result.usedDiskPolicyCache = useDisk;

  detail::PolicyActionStore policyStore;
  if (!policyStore.init(n, useDisk, &result.errorMessage)) {
    result.ok = false;
    return result;
  }

  detail::PolicyActionStore bestOutStore;
  detail::PolicyActionStore *bestOutPtr = nullptr;
  if constexpr (BoundaryFn::enabled) {
    if (!bestOutStore.init(n, useDisk, &result.errorMessage)) {
      result.ok = false;
      return result;
    }
    bestOutPtr = &bestOutStore;
  }

  constexpr size_t MIN_PARALLEL_BOARDS = 2048;
  const int threads = std::max(1, cfg.numThreads);
  const size_t totalStates = n * NB_STATES_PER_BOARD;

  std::vector<double> vCur(totalStates, 0.0);
  std::vector<double> vNext(totalStates, 0.0);
  std::vector<detail::SoftBoundaryCache> threadCaches;
  std::vector<detail::SoftBoundaryCache> *cachePtr = nullptr;
  if constexpr (BoundaryFn::enabled) {
    threadCaches.resize(static_cast<size_t>(threads));
    for (auto &c : threadCaches) {
      c.init();
    }
    cachePtr = &threadCaches;
  }

  if (initTb != nullptr && initTb->isLoaded()) {
    if (!detail::applyNextBoxWarmStart(sortedBoards, *initTb, vCur, result.warmStartBoardsMatched,
                                       result.errorMessage)) {
      result.ok = false;
      return result;
    }
  }

  const uint32_t emptyId = index.find(0ULL);
  const auto computeExpectedEmpty = [&](const std::vector<double> &v) -> double {
    if (emptyId == INVALID_BOARD_ID) {
      return MISSING_BOARD_VALUE;
    }
    const double *row = &v[static_cast<size_t>(emptyId) * NB_STATES_PER_BOARD];
    double ev = 0.0;
    for (int c = 0; c < NUM_PIECES; ++c) {
      ev += openingPieceProb(cfg.rngMode, c) * row[c];
    }
    return ev;
  };
  const auto checkDivergenceGuard = [&](double maxVal, const char *stage) -> bool {
    if (maxVal > cfg.maxValueDivergenceGuard) {
      result.ok = false;
      result.errorMessage = std::string(stage)
                            + " diverged above maxValueDivergenceGuard ("
                            + std::to_string(maxVal)
                            + " > "
                            + std::to_string(cfg.maxValueDivergenceGuard)
                            + ")";
      return false;
    }
    return true;
  };

  for (int outer = 1; outer <= cfg.maxOuterIters; ++outer) {
    double outerDiff = 0.0;
    double outerMaxVal = 0.0;
    const bool writeBestOut = (outer == 1);
    if (!detail::runNextBoxPolicyImprovementSweep(sortedBoards, index, transProb, cfg.gamma,
                                                  boundaryFn, writeBestOut ? cachePtr : nullptr,
                                                  bestOutPtr, writeBestOut, threads,
                                                  MIN_PARALLEL_BOARDS, vCur, vNext, policyStore,
                                                  outerDiff, outerMaxVal, result.errorMessage)) {
      result.ok = false;
      return result;
    }
    if constexpr (BoundaryFn::enabled) {
      if (outer == 1) {
        std::vector<detail::SoftBoundaryCache>().swap(threadCaches);
        cachePtr = nullptr;
      }
    }

    vCur.swap(vNext);
    result.outerIterations = outer;
    result.finalBellmanResidual = outerDiff;
    result.maxBoardValue = outerMaxVal;

    if (!checkDivergenceGuard(outerMaxVal, "MPI value")) {
      return result;
    }

    if (cfg.verbose) {
      std::printf("  [NB-MPI outer %3d] bellman_residual = %.3e | E[V(0)] = %.6f | maxV = %.2f | "
                  "inner_sweeps = %d\n",
                  outer, result.finalBellmanResidual, computeExpectedEmpty(vCur), outerMaxVal,
                  result.totalInnerSweeps);
      std::fflush(stdout);
    }

    if (result.finalBellmanResidual <= cfg.tolerance) {
      break;
    }

    const double innerThreshold = cfg.tolerance * cfg.innerStopRatio;
    for (int s = 0; s < cfg.innerEvalSweeps; ++s) {
      double innerDiff = 0.0;
      double innerMaxVal = 0.0;
      if (!detail::runNextBoxPolicyEvalSweep(n, transProb, cfg.gamma, threads, MIN_PARALLEL_BOARDS,
                                             vCur, vNext, policyStore, innerDiff, innerMaxVal,
                                             result.errorMessage)) {
        result.ok = false;
        return result;
      }
      vCur.swap(vNext);
      ++result.totalInnerSweeps;
      result.maxBoardValue = innerMaxVal;
      if (!checkDivergenceGuard(innerMaxVal, "MPI inner eval")) {
        return result;
      }
      if (innerDiff <= innerThreshold) {
        break;
      }
    }
  }

  // Release policy storage and vNext before final residual verification and output copy.
  policyStore.close();
  std::vector<double>().swap(vNext);

  if (!detail::computeNextBoxBellmanResidual(sortedBoards, index, transProb, cfg.gamma, boundaryFn,
                                             bestOutPtr, threads, MIN_PARALLEL_BOARDS, vCur,
                                             result.finalBellmanResidual, result.errorMessage)) {
    result.ok = false;
    return result;
  }
  bestOutStore.close();

  if (emptyId != INVALID_BOARD_ID) {
    const double *row = &vCur[static_cast<size_t>(emptyId) * NB_STATES_PER_BOARD];
    for (int c = 0; c < NUM_PIECES; ++c) {
      result.emptyBoardValues[static_cast<size_t>(c)] = row[c];
    }
    result.emptyBoardExpectedValue = computeExpectedEmpty(vCur);
  }

  result.values.resize(totalStates);
  for (size_t k = 0; k < totalStates; ++k) {
    result.values[k] = static_cast<float>(vCur[k]);
  }
  result.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

template <typename BoundaryFn = NoOpBoundaryEval>
inline NextBoxSolveResult solveNextBoxMPI(const std::vector<uint64_t> &sortedBoards,
                                          const NextBoxSolveConfig &cfg = NextBoxSolveConfig{},
                                          const MappedNextBoxTablebase *initTb = nullptr,
                                          const BoundaryFn &boundaryFn = BoundaryFn{}) {
  const FlatBoardIndex index(sortedBoards);
  return solveNextBoxMPI(sortedBoards, index, cfg, initTb, boundaryFn);
}

// Selects a move using a loaded 1-piece Next-Box tablebase (MappedNextBoxTablebase or
// IndexedNextBoxTablebase):
//   - closedWorld == true: strictly maximizes r + V_tb(b', nextPiece) with V = 0 outside TB,
//     breaking ties in move-generation order (exact closed-world Bellman optimal policy).
//   - closedWorld == false: maximizes r + V_tb(b', nextPiece) when at least one child lands in TB,
//     breaking exact Q ties lazily via scoreNextBoxHeuristicTransition, and falling back to
//     pickBestNextBoxHeuristic when all legal moves exit the tablebase.
template <typename NextBoxTablebase>
inline TablebaseMoveChoice chooseNextBoxTablebaseMove(uint64_t board, int curPiece, int nextPiece,
                                                      const NextBoxTablebase &tb, bool closedWorld,
                                                      const EvalWeights &w = EvalWeights{}) {
  assert(nextPiece >= 0 && nextPiece < NUM_PIECES);
  TransitionList tl;
  generateTransitions(board, curPiece, tl);
  if (tl.empty()) {
    return {};
  }

  bool anyInTb = false;
  int bestIdx = 0;
  double bestQ = 0.0;
  bool bestInTb = false;
  bool hasBestHeur = false;
  double bestTieHeur = 0.0;

  for (int i = 0; i < tl.size(); ++i) {
    float childVal = 0.0f;
    const bool inTb = tb.lookupPiece(tl[i].afterBoard, nextPiece, childVal);
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
        bestTieHeur = scoreNextBoxHeuristicTransition(tl[bestIdx].afterBoard,
                                                      tl[bestIdx].linesCleared, nextPiece, w);
        hasBestHeur = true;
      }
      const double candHeur =
          scoreNextBoxHeuristicTransition(tl[i].afterBoard, tl[i].linesCleared, nextPiece, w);
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

  const ChosenTransition fb = pickBestNextBoxHeuristic(board, curPiece, nextPiece, w);
  if (!fb.found) {
    return {};
  }
  return {fb.transition, static_cast<double>(fb.transition.linesCleared), false, true, true};
}

// Multi-threaded deterministic Monte Carlo evaluator for any 1-piece Next-Box tablebase
// (MappedNextBoxTablebase or IndexedNextBoxTablebase). Uses rngMode for opening and transition
// draws so closedWorld == true verifies directly against expected V(0, c_0).
template <typename NextBoxTablebase>
inline TablebaseEvalStats
evaluateNextBoxPolicy(const NextBoxTablebase &tb, RngMode rngMode, bool closedWorld, int numGames,
                      uint64_t baseSeed, int numThreads, const EvalWeights &w = EvalWeights{},
                      int maxSteps = DEFAULT_EVAL_MAX_STEPS) {
  return detail::runTablebaseEval(
      closedWorld, numGames, baseSeed, numThreads, maxSteps, [&](Rng &rng) {
        int curPiece = rng.nextOpeningPiece(rngMode);
        int nextPiece = rng.nextPiece(rngMode, curPiece);
        return [&rng, &tb, rngMode, closedWorld, &w, curPiece, nextPiece](uint64_t board) mutable {
          const TablebaseMoveChoice mv =
              chooseNextBoxTablebaseMove(board, curPiece, nextPiece, tb, closedWorld, w);
          if (mv.found) {
            curPiece = nextPiece;
            nextPiece = rng.nextPiece(rngMode, curPiece);
          }
          return mv;
        };
      });
}

// Exact Monte Carlo verification for soft-boundary and/or discounted Next-Box MPI solves:
// executes the exact Bellman-optimal policy (maximizing r + gamma * V_tb in-TB vs quantized
// r + gamma * B(b', n) out-of-TB, preferring in-TB on ties), terminates immediately on TB exit
// with the exact terminal boundary payoff, and verifies the Monte Carlo return matches E[V(0)].
struct SoftBoundaryMcStats {
  int games = 0;
  double meanReturn = 0.0;
  double stdErr = 0.0;
  double inTbMovePct = 0.0;
  int stepCapHits = 0;
};

template <typename NextBoxTablebase, typename BoundaryFn>
inline SoftBoundaryMcStats
evaluateNextBoxSoftBoundaryMC(const NextBoxTablebase &tb, RngMode rngMode, double gamma,
                              const BoundaryFn &boundaryFn, int numGames, uint64_t baseSeed,
                              int numThreads, int maxSteps = DEFAULT_EVAL_MAX_STEPS) {
  SoftBoundaryMcStats stats{};
  if (numGames <= 0
      || !tb.isLoaded()
      || !std::isfinite(gamma)
      || gamma <= 0.0
      || gamma > 1.0
      || maxSteps <= 0) {
    return stats;
  }
  stats.games = numGames;

  struct PerGame {
    double ret = 0.0;
    uint64_t moves = 0;
    uint64_t inTbMoves = 0;
    bool hitCap = false;
  };
  std::vector<PerGame> perGame(static_cast<size_t>(numGames));
  std::atomic<int> nextIdx{0};

  detail::runWorkerPool(numThreads, static_cast<size_t>(numGames), 16, [&](int /*tid*/) {
    TransitionList tl;
    detail::SoftBoundaryCache cache;
    if constexpr (BoundaryFn::enabled) {
      cache.init();
    }
    while (true) {
      const int g = nextIdx.fetch_add(1, std::memory_order_relaxed);
      if (g >= numGames) {
        break;
      }
      Rng rng(detail::deriveGameSeed(baseSeed, g));
      uint64_t board = 0ULL;
      int curPiece = rng.nextOpeningPiece(rngMode);
      int nextPiece = rng.nextPiece(rngMode, curPiece);
      double disc = 1.0;
      PerGame pg{};

      for (int step = 0; step < maxSteps; ++step) {
        generateTransitions(board, curPiece, tl);
        if (tl.empty()) {
          break;
        }
        int bestIdx = 0;
        double bestQ = 0.0;
        bool bestInTb = false;
        for (int i = 0; i < tl.size(); ++i) {
          float v = 0.0f;
          const bool inTb = tb.lookupPiece(tl[i].afterBoard, nextPiece, v);
          const double rew = static_cast<double>(tl[i].linesCleared);
          double q = rew;
          if (inTb) {
            q = rew + gamma * static_cast<double>(v);
          } else if constexpr (BoundaryFn::enabled) {
            const double *boundRow = cache.evalCached(tl[i].afterBoard, boundaryFn);
            const uint32_t tw = packNextBoxTerminalPayoff(rew + gamma * boundRow[nextPiece]);
            q = unpackNextBoxTerminalPayoff(tw);
          }
          if (detail::isBetterNextBoxMove<BoundaryFn::enabled>(i == 0, q, inTb, bestQ, bestInTb)) {
            bestIdx = i;
            bestQ = q;
            bestInTb = inTb;
          }
        }

        ++pg.moves;
        if (!bestInTb) {
          pg.ret += disc * bestQ;
          break;
        }
        ++pg.inTbMoves;
        pg.ret += disc * static_cast<double>(tl[bestIdx].linesCleared);
        disc *= gamma;
        board = tl[bestIdx].afterBoard;
        curPiece = nextPiece;
        nextPiece = rng.nextPiece(rngMode, curPiece);
        if (step + 1 == maxSteps) {
          pg.hitCap = true;
        }
      }
      perGame[static_cast<size_t>(g)] = pg;
    }
  });

  double sum = 0.0;
  uint64_t totalMoves = 0;
  uint64_t totalInTb = 0;
  for (const auto &pg : perGame) {
    sum += pg.ret;
    totalMoves += pg.moves;
    totalInTb += pg.inTbMoves;
    if (pg.hitCap) {
      ++stats.stepCapHits;
    }
  }
  stats.meanReturn = sum / static_cast<double>(numGames);
  if (numGames > 1) {
    double sqSum = 0.0;
    for (const auto &pg : perGame) {
      const double d = pg.ret - stats.meanReturn;
      sqSum += d * d;
    }
    const double var = sqSum / static_cast<double>(numGames - 1);
    stats.stdErr = std::sqrt(var / static_cast<double>(numGames));
  }
  if (totalMoves > 0) {
    stats.inTbMovePct = 100.0 * static_cast<double>(totalInTb) / static_cast<double>(totalMoves);
  }
  return stats;
}

} // namespace tb

#endif // TABLEBASE_NEXTBOX_SOLVER_HPP
