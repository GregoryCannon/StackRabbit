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

// Packed Next-Box MPI policy word (4 bytes instead of 5):
//   bits  0..28 : childId in [0 .. 536,870,910], or NB_ACTION_OUT_CHILD (0x1FFFFFFF) for out-of-set
//   bits 29..31 : immediate linesCleared reward in [0 .. 4]
inline constexpr uint32_t NB_ACTION_CHILD_MASK = 0x1FFFFFFFU;
inline constexpr uint32_t NB_ACTION_OUT_CHILD = 0x1FFFFFFFU;
inline constexpr int NB_ACTION_REWARD_SHIFT = 29;
inline constexpr size_t NB_STATES_PER_BOARD = static_cast<size_t>(NUM_PIECES);
inline constexpr size_t NB_ACTIONS_PER_BOARD = NB_STATES_PER_BOARD * NB_STATES_PER_BOARD; // 49

static_assert((NB_ACTION_CHILD_MASK >> NB_ACTION_REWARD_SHIFT) == 0U,
              "Child ID mask must not overlap reward bits");
static_assert(static_cast<uint32_t>(BOARD_ROWS) <= (UINT32_MAX >> NB_ACTION_REWARD_SHIFT),
              "BOARD_ROWS must fit in the packed reward field");

constexpr uint32_t packNextBoxAction(uint32_t childId, uint8_t linesCleared) noexcept {
  assert(linesCleared <= BOARD_ROWS);
  assert(childId == INVALID_BOARD_ID || childId < NB_ACTION_OUT_CHILD);
  const uint32_t packedChild = (childId == INVALID_BOARD_ID) ? NB_ACTION_OUT_CHILD : childId;
  return (static_cast<uint32_t>(linesCleared) << NB_ACTION_REWARD_SHIFT) | packedChild;
}

constexpr uint32_t unpackNextBoxChild(uint32_t packed) noexcept {
  const uint32_t raw = packed & NB_ACTION_CHILD_MASK;
  return (raw == NB_ACTION_OUT_CHILD) ? INVALID_BOARD_ID : raw;
}

constexpr uint8_t unpackNextBoxReward(uint32_t packed) noexcept {
  return static_cast<uint8_t>(packed >> NB_ACTION_REWARD_SHIFT);
}

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

// Computes one exact Next-Box Bellman backup for (board, curPiece) against vCur.
// Moves are iterated outermost so each child board is hashed once and its 7-entry value row is read
// sequentially across all next pieces n = 0..6. When RecordActions is true, writes 7 packed
// policy actions into outActions[0..6].
template <bool RecordActions>
inline double computeNextBoxBellmanBackup(uint64_t board, int curPiece, const FlatBoardIndex &index,
                                          const std::vector<double> &vCur,
                                          const TransitionProbMatrix &transProb,
                                          TransitionList &scratch, uint32_t *outActions = nullptr) {
  generateTransitions(board, curPiece, scratch);
  if (scratch.empty()) {
    if constexpr (RecordActions) {
      for (int next = 0; next < NUM_PIECES; ++next) {
        outActions[next] = packNextBoxAction(INVALID_BOARD_ID, 0);
      }
    }
    return 0.0;
  }

  double bestQ[NUM_PIECES];
  uint32_t bestCid[NUM_PIECES];
  uint8_t bestRew[NUM_PIECES];

  for (int m = 0; m < scratch.size(); ++m) {
    const Transition &tr = scratch[m];
    const uint32_t cid = index.find(tr.afterBoard);
    const double rew = static_cast<double>(tr.linesCleared);
    const double *childRow =
        (cid != INVALID_BOARD_ID) ? &vCur[static_cast<size_t>(cid) * NB_STATES_PER_BOARD] : nullptr;
    for (int next = 0; next < NUM_PIECES; ++next) {
      const double q = rew + ((childRow != nullptr) ? childRow[next] : 0.0);
      if (m == 0 || q > bestQ[next]) {
        bestQ[next] = q;
        if constexpr (RecordActions) {
          bestCid[next] = cid;
          bestRew[next] = tr.linesCleared;
        }
      }
    }
  }

  double updatedV = 0.0;
  const auto &probs = transProb[static_cast<size_t>(curPiece)];
  for (int next = 0; next < NUM_PIECES; ++next) {
    if constexpr (RecordActions) {
      outActions[next] = packNextBoxAction(bestCid[next], bestRew[next]);
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

inline bool runNextBoxPolicyImprovementSweep(
    const std::vector<uint64_t> &sortedBoards, const FlatBoardIndex &index,
    const TransitionProbMatrix &transProb, int threads, size_t minParallelBoards,
    const std::vector<double> &vCur, std::vector<double> &vNext, PolicyActionStore &policyStore,
    double &outMaxDiff, std::string &errOut) {
  const size_t n = sortedBoards.size();
  double sweepMaxDiff = 0.0;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);

  for (size_t chunkBegin = 0; chunkBegin < n; chunkBegin += PolicyActionStore::CHUNK_BOARDS) {
    const size_t chunkEnd = std::min(n, chunkBegin + PolicyActionStore::CHUNK_BOARDS);
    const size_t chunkCount = chunkEnd - chunkBegin;
    uint32_t *chunkActions = policyStore.beginWriteChunk(chunkBegin, chunkCount);
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);

    runParallelSubrange(threads, chunkBegin, chunkEnd, minParallelBoards,
                        [&](int tid, size_t subBegin, size_t subEnd) {
                          TransitionList tl;
                          double localMax = 0.0;
                          for (size_t i = subBegin; i < subEnd; ++i) {
                            const uint64_t b = sortedBoards[i];
                            uint32_t *boardActions =
                                &chunkActions[(i - chunkBegin) * NB_ACTIONS_PER_BOARD];
                            const double *oldRow = &vCur[i * NB_STATES_PER_BOARD];
                            double *newRow = &vNext[i * NB_STATES_PER_BOARD];

                            for (int c = 0; c < NUM_PIECES; ++c) {
                              const double updatedV =
                                  computeNextBoxBellmanBackup</*RecordActions=*/true>(
                                      b, c, index, vCur, transProb, tl,
                                      &boardActions[static_cast<size_t>(c) * NB_STATES_PER_BOARD]);
                              newRow[c] = updatedV;
                              const double diff = std::abs(updatedV - oldRow[c]);
                              if (diff > localMax) {
                                localMax = diff;
                              }
                            }
                          }
                          threadMaxDiff[static_cast<size_t>(tid)] = localMax;
                        });

    sweepMaxDiff =
        std::max(sweepMaxDiff, *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end()));
    if (!policyStore.finishWriteChunk(chunkBegin, chunkCount, &errOut)) {
      return false;
    }
  }
  outMaxDiff = sweepMaxDiff;
  return true;
}

inline bool runNextBoxPolicyEvalSweep(size_t numBoards, const TransitionProbMatrix &transProb,
                                      int threads, size_t minParallelBoards,
                                      const std::vector<double> &vCur, std::vector<double> &vNext,
                                      PolicyActionStore &policyStore, double &outMaxDiff,
                                      std::string &errOut) {
  double sweepMaxDiff = 0.0;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);

  for (size_t chunkBegin = 0; chunkBegin < numBoards;
       chunkBegin += PolicyActionStore::CHUNK_BOARDS) {
    const size_t chunkEnd = std::min(numBoards, chunkBegin + PolicyActionStore::CHUNK_BOARDS);
    const size_t chunkCount = chunkEnd - chunkBegin;
    const uint32_t *chunkActions = policyStore.readChunk(chunkBegin, chunkCount, &errOut);
    if (chunkActions == nullptr) {
      return false;
    }
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);

    runParallelSubrange(threads, chunkBegin, chunkEnd, minParallelBoards,
                        [&](int tid, size_t subBegin, size_t subEnd) {
                          double localMax = 0.0;
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
                                double q = static_cast<double>(unpackNextBoxReward(packed));
                                const uint32_t cid = unpackNextBoxChild(packed);
                                if (cid != INVALID_BOARD_ID) {
                                  q += vCur[static_cast<size_t>(cid) * NB_STATES_PER_BOARD
                                            + static_cast<size_t>(next)];
                                }
                                updatedV += probs[static_cast<size_t>(next)] * q;
                              }
                              newRow[c] = updatedV;
                              const double diff = std::abs(updatedV - oldRow[c]);
                              if (diff > localMax) {
                                localMax = diff;
                              }
                            }
                          }
                          threadMaxDiff[static_cast<size_t>(tid)] = localMax;
                        });

    sweepMaxDiff =
        std::max(sweepMaxDiff, *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end()));
  }
  outMaxDiff = sweepMaxDiff;
  return true;
}

inline double computeNextBoxBellmanResidual(const std::vector<uint64_t> &sortedBoards,
                                            const FlatBoardIndex &index,
                                            const TransitionProbMatrix &transProb, int threads,
                                            size_t minParallelBoards,
                                            const std::vector<double> &vCur) {
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);
  runParallelSubrange(threads, 0, sortedBoards.size(), minParallelBoards,
                      [&](int tid, size_t subBegin, size_t subEnd) {
                        TransitionList tl;
                        double localMax = 0.0;
                        for (size_t i = subBegin; i < subEnd; ++i) {
                          const uint64_t b = sortedBoards[i];
                          const double *curRow = &vCur[i * NB_STATES_PER_BOARD];
                          for (int c = 0; c < NUM_PIECES; ++c) {
                            const double backup =
                                computeNextBoxBellmanBackup</*RecordActions=*/false>(
                                    b, c, index, vCur, transProb, tl);
                            const double diff = std::abs(backup - curRow[c]);
                            if (diff > localMax) {
                              localMax = diff;
                            }
                          }
                        }
                        threadMaxDiff[static_cast<size_t>(tid)] = localMax;
                      });
  return *std::max_element(threadMaxDiff.begin(), threadMaxDiff.end());
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
  int numThreads = 1;
  bool verbose = false;
};

struct NextBoxSolveResult {
  std::vector<float> values; // Size N * 7 in I, O, L, J, T, S, Z order
  std::array<double, NUM_PIECES> emptyBoardValues{};
  double emptyBoardExpectedValue = MISSING_BOARD_VALUE;
  double finalBellmanResidual = 0.0;
  int outerIterations = 0;
  int totalInnerSweeps = 0;
  size_t warmStartBoardsMatched = 0;
  bool usedDiskPolicyCache = false;
  double elapsedSec = 0.0;
  bool ok = true;
  std::string errorMessage;
};

// Solves the undiscounted 1-piece Next-Box Bellman equation on sortedBoards via parallel two-buffer
// Modified Policy Iteration (MPI) in IEEE-754 double precision:
//   V*(b, c) = sum_{n=0..6} P(n | c) * max_{b' in Moves(b, c)} [r(b, b') + V*(b', n)],
// with V(b', n) = 0.0 for b' outside sortedBoards and 0.0 when Moves(b, c) is empty.
// Supports warm-starting from an existing SRTBV002 tablebase with matching RngMode, and streams
// the 49-action/board policy cache to a temporary disk file when large to bound RAM.
inline NextBoxSolveResult solveNextBoxMPI(const std::vector<uint64_t> &sortedBoards,
                                          const FlatBoardIndex &index,
                                          const NextBoxSolveConfig &cfg = NextBoxSolveConfig{},
                                          const MappedNextBoxTablebase *initTb = nullptr) {
  assert(index.size() == sortedBoards.size());
  const auto t0 = std::chrono::steady_clock::now();
  const size_t n = sortedBoards.size();
  NextBoxSolveResult result{};
  result.emptyBoardValues.fill(MISSING_BOARD_VALUE);

  if (n == 0) {
    return result;
  }
  if (n >= static_cast<size_t>(NB_ACTION_OUT_CHILD)) {
    result.ok = false;
    result.errorMessage = "Board count exceeds 29-bit packed action capacity";
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

  constexpr size_t MIN_PARALLEL_BOARDS = 2048;
  const int threads = std::max(1, cfg.numThreads);
  const size_t totalStates = n * NB_STATES_PER_BOARD;

  std::vector<double> vCur(totalStates, 0.0);
  std::vector<double> vNext(totalStates, 0.0);

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

  for (int outer = 1; outer <= cfg.maxOuterIters; ++outer) {
    double outerDiff = 0.0;
    if (!detail::runNextBoxPolicyImprovementSweep(sortedBoards, index, transProb, threads,
                                                  MIN_PARALLEL_BOARDS, vCur, vNext, policyStore,
                                                  outerDiff, result.errorMessage)) {
      result.ok = false;
      return result;
    }

    vCur.swap(vNext);
    result.outerIterations = outer;
    result.finalBellmanResidual = outerDiff;

    if (cfg.verbose) {
      std::printf(
          "  [NB-MPI outer %3d] bellman_residual = %.3e | E[V(0)] = %.6f | inner_sweeps = %d\n",
          outer, result.finalBellmanResidual, computeExpectedEmpty(vCur), result.totalInnerSweeps);
      std::fflush(stdout);
    }

    if (result.finalBellmanResidual <= cfg.tolerance) {
      break;
    }

    const double innerThreshold = cfg.tolerance * cfg.innerStopRatio;
    for (int s = 0; s < cfg.innerEvalSweeps; ++s) {
      double innerDiff = 0.0;
      if (!detail::runNextBoxPolicyEvalSweep(n, transProb, threads, MIN_PARALLEL_BOARDS, vCur,
                                             vNext, policyStore, innerDiff, result.errorMessage)) {
        result.ok = false;
        return result;
      }
      vCur.swap(vNext);
      ++result.totalInnerSweeps;
      if (innerDiff <= innerThreshold) {
        break;
      }
    }
  }

  // Release temporary policy storage and vNext before final residual verification and output copy.
  policyStore.close();
  std::vector<double>().swap(vNext);

  result.finalBellmanResidual = detail::computeNextBoxBellmanResidual(
      sortedBoards, index, transProb, threads, MIN_PARALLEL_BOARDS, vCur);

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

inline NextBoxSolveResult solveNextBoxMPI(const std::vector<uint64_t> &sortedBoards,
                                          const NextBoxSolveConfig &cfg = NextBoxSolveConfig{},
                                          const MappedNextBoxTablebase *initTb = nullptr) {
  const FlatBoardIndex index(sortedBoards);
  return solveNextBoxMPI(sortedBoards, index, cfg, initTb);
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

} // namespace tb

#endif // TABLEBASE_NEXTBOX_SOLVER_HPP
