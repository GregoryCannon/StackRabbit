#ifndef DAS_TABLEBASE_SOLVER_20ROW_HPP
#define DAS_TABLEBASE_SOLVER_20ROW_HPP

#include "core_20row.hpp"
#include "tablebase_io_20row.hpp"

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
#include <limits>
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

namespace das {

// Packed 32-bit MPI policy word (49 words = 196 B/board):
//   - Bit 31 == 0 (in-TB or zero-payoff out-of-TB transition):
//       bits  0..27 : childId in [0 .. 268,435,454] (0x0FFFFFFF = out-of-set / top-out)
//       bits 28..30 : linesCleared in [0 .. 4] (mapped through REWARD_NORM_TETRIS[lc])
//   - Bit 31 == 1 (terminal fixed-point soft-boundary payoff qFixed >= 0.0):
//       bits  0..30 : round(clamp(qFixed, 0.0, TERMINAL_MAX_PAYOFF) * TERMINAL_PAYOFF_SCALE)
//                     scale = 2^20 (9.54e-7 resolution, range [0, 2047.0])
inline constexpr uint32_t ACTION_TERMINAL_FLAG = 0x80000000U;
inline constexpr uint32_t ACTION_CHILD_MASK = 0x0FFFFFFFU;
inline constexpr uint32_t ACTION_OUT_CHILD = 0x0FFFFFFFU;
inline constexpr int ACTION_REWARD_SHIFT = 28;
inline constexpr uint32_t ACTION_REWARD_MASK = 0x7U;
inline constexpr double TERMINAL_PAYOFF_SCALE = 1048576.0; // 2^20
inline constexpr double TERMINAL_MAX_PAYOFF = 2047.0;
inline constexpr size_t STATES_PER_BOARD = static_cast<size_t>(NUM_PIECES);
inline constexpr size_t ACTIONS_PER_BOARD = STATES_PER_BOARD * STATES_PER_BOARD; // 49

static_assert((ACTION_CHILD_MASK & ACTION_TERMINAL_FLAG) == 0U, "Mask overlap");
static_assert((ACTION_CHILD_MASK >> ACTION_REWARD_SHIFT) == 0U, "Reward shift overlap");
static_assert(static_cast<uint64_t>(TERMINAL_MAX_PAYOFF *TERMINAL_PAYOFF_SCALE) <= 0x7FFFFFFFULL,
              "Fixed-point terminal payoff overflow");

constexpr bool isTerminalPayoff(uint32_t packed) noexcept {
  return (packed & ACTION_TERMINAL_FLAG) != 0U;
}

constexpr uint32_t packPolicyAction(uint32_t childId, uint8_t linesCleared) noexcept {
  assert(linesCleared <= 4U);
  assert(childId == INVALID_BOARD_ID || childId < ACTION_OUT_CHILD);
  const uint32_t packedChild = (childId == INVALID_BOARD_ID) ? ACTION_OUT_CHILD : childId;
  return (static_cast<uint32_t>(linesCleared) << ACTION_REWARD_SHIFT) | packedChild;
}

inline uint32_t packTerminalPayoff(double qFixed) noexcept {
  const double clamped = std::isfinite(qFixed) ? std::clamp(qFixed, 0.0, TERMINAL_MAX_PAYOFF) : 0.0;
  const uint32_t fixed = static_cast<uint32_t>(std::llround(clamped * TERMINAL_PAYOFF_SCALE));
  return ACTION_TERMINAL_FLAG | (fixed & ~ACTION_TERMINAL_FLAG);
}

constexpr double unpackTerminalPayoff(uint32_t packed) noexcept {
  return static_cast<double>(packed & ~ACTION_TERMINAL_FLAG) / TERMINAL_PAYOFF_SCALE;
}

constexpr uint32_t unpackPolicyChild(uint32_t packed) noexcept {
  if (isTerminalPayoff(packed)) {
    return INVALID_BOARD_ID;
  }
  const uint32_t raw = packed & ACTION_CHILD_MASK;
  return (raw == ACTION_OUT_CHILD) ? INVALID_BOARD_ID : raw;
}

constexpr uint8_t unpackPolicyLines(uint32_t packed) noexcept {
  assert(!isTerminalPayoff(packed));
  return static_cast<uint8_t>((packed >> ACTION_REWARD_SHIFT) & ACTION_REWARD_MASK);
}

// Default hard-boundary evaluator (zero continuation outside the tablebase).
struct NoOpBoundaryEval20 {
  static constexpr bool enabled = false;
  void evalRow(const uint16_t /*afterRows*/[BOARD_ROWS],
               double /*outCont*/[NUM_PIECES]) const noexcept {}
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
    for (int n = 0; n < NUM_PIECES; ++n) {
      mat[static_cast<size_t>(c)][static_cast<size_t>(n)] = pieceTransitionProb(mode, c, n);
    }
  }
  return mat;
}

// Per-worker direct-mapped cache for out-of-TB boundary NN evaluations keyed by BoardKey.
// Stores exact double continuations so cache hits match misses bit-for-bit (preserving 1T == MT,
// Ram == Disk, and Soft-MC parity). Freed immediately after outer iteration 1.
class SoftBoundaryCache20 {
public:
  static constexpr size_t NUM_SLOTS =
      262144; // ~18.9 MB/worker, freed immediately after outer iter 1
  static constexpr size_t MASK = NUM_SLOTS - 1;
  static_assert(NUM_SLOTS > 0 && (NUM_SLOTS & (NUM_SLOTS - 1)) == 0,
                "SoftBoundaryCache20::NUM_SLOTS must be a power of two");

  struct Entry {
    BoardKey key = EMPTY_KEY;
    double cont[NUM_PIECES] = {};
  };

  SoftBoundaryCache20() : slots_(NUM_SLOTS) {}

  void clear() noexcept {
    for (auto &e : slots_) {
      e.key = EMPTY_KEY;
    }
  }

  bool lookup(BoardKey key, double outCont[NUM_PIECES]) const noexcept {
    const Entry &e = slots_[static_cast<size_t>(hashBoardKeySlot(key)) & MASK];
    if (e.key != key) {
      return false;
    }
    for (int n = 0; n < NUM_PIECES; ++n) {
      outCont[n] = e.cont[n];
    }
    return true;
  }

  void store(BoardKey key, const double cont[NUM_PIECES]) noexcept {
    Entry &e = slots_[static_cast<size_t>(hashBoardKeySlot(key)) & MASK];
    e.key = key;
    for (int n = 0; n < NUM_PIECES; ++n) {
      e.cont[n] = cont[n];
    }
  }

private:
  std::vector<Entry> slots_;
};

// Sequential chunked storage for N * wordsPerBoard packed uint32_t words (49 words/board for
// V(b, c) MPI policy & bestOutStore; 7 * K * 7 words/board for V_RT(b, c, d) MPI policy).
// Supports RAM or auto-unlinked 64-bit SSD temp files.
class PolicyActionStore20 {
public:
  static constexpr size_t CHUNK_BOARDS = 65536; // 65,536 boards per sequential I/O chunk

#ifndef _WIN32
  static_assert(sizeof(off_t) >= 8, "64-bit off_t required for >4 GB policy cache files");
#else
  static_assert(sizeof(std::streamoff) >= 8, "64-bit streamoff required");
#endif

  PolicyActionStore20() = default;
  PolicyActionStore20(const PolicyActionStore20 &) = delete;
  PolicyActionStore20 &operator=(const PolicyActionStore20 &) = delete;
  ~PolicyActionStore20() { close(); }

  bool init(size_t numBoards, bool useDisk, std::string *errOut = nullptr,
            size_t wordsPerBoard = ACTIONS_PER_BOARD) {
    close();
    numBoards_ = numBoards;
    useDisk_ = useDisk;
    wordsPerBoard_ = wordsPerBoard;
    if (numBoards_ == 0) {
      return true;
    }
    if (!useDisk_) {
      if (numBoards_ > std::numeric_limits<size_t>::max() / wordsPerBoard_) {
        if (errOut)
          *errOut = "PolicyActionStore20 RAM allocation overflow";
        return false;
      }
      ramBuf_.assign(numBoards_ * wordsPerBoard_, packPolicyAction(INVALID_BOARD_ID, 0));
      return true;
    }

    const size_t chunkElems = std::min(numBoards_, CHUNK_BOARDS) * wordsPerBoard_;
    ioBuf_.assign(chunkElems, 0U);

    std::error_code ec;
    const std::filesystem::path tmpDir = std::filesystem::temp_directory_path(ec);
    const std::filesystem::path dir = ec ? std::filesystem::path(".") : tmpDir;

#ifndef _WIN32
    std::string tmpl = (dir / "sr_das_policy_XXXXXX").string();
    std::vector<char> pathBuf(tmpl.begin(), tmpl.end());
    pathBuf.push_back('\0');
    fd_ = ::mkstemp(pathBuf.data());
    if (fd_ < 0) {
      if (errOut) {
        *errOut = "Failed to create temporary policy cache file in "
                  + dir.string()
                  + ": "
                  + std::strerror(errno);
      }
      close();
      return false;
    }
    if (::unlink(pathBuf.data()) != 0) {
      tempPath_ = std::filesystem::path(pathBuf.data());
    }
#else
    static std::atomic<uint64_t> uniqueSeq{0};
    const uint64_t seq = uniqueSeq.fetch_add(1, std::memory_order_relaxed);
    tempPath_ =
        dir / ("sr_das_policy_" + std::to_string(::_getpid()) + "_" + std::to_string(seq) + ".tmp");
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
      return ramBuf_.data() + chunkBegin * wordsPerBoard_;
    }
    assert(chunkCount <= CHUNK_BOARDS);
    return ioBuf_.data();
  }

  // Loads an existing chunk for in-place read-modify-write; pair with finishWriteChunk().
  uint32_t *beginModifyChunk(size_t chunkBegin, size_t chunkCount, std::string *errOut = nullptr) {
    assert(chunkBegin + chunkCount <= numBoards_);
    if (!useDisk_) {
      return ramBuf_.data() + chunkBegin * wordsPerBoard_;
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
      return ramBuf_.data() + chunkBegin * wordsPerBoard_;
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
  size_t wordsPerBoard_ = ACTIONS_PER_BOARD;
  bool useDisk_ = false;
  std::vector<uint32_t> ramBuf_;
  std::vector<uint32_t> ioBuf_;
  std::filesystem::path tempPath_;
#ifndef _WIN32
  int fd_ = -1;
#else
  std::fstream stream_;
#endif

  size_t chunkBytes(size_t boards) const noexcept {
    return boards * wordsPerBoard_ * sizeof(uint32_t);
  }
  uint64_t boardByteOffset(size_t chunkBegin) const noexcept {
    return static_cast<uint64_t>(chunkBegin) * wordsPerBoard_ * sizeof(uint32_t);
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
          *errOut = std::string("pwrite failed: ") + std::strerror(errno);
        return false;
      }
      done += static_cast<size_t>(w);
    }
    return true;
#else
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.write(static_cast<const char *>(src), static_cast<std::streamsize>(nBytes));
    if (!stream_.good()) {
      if (errOut)
        *errOut = "write failed on temporary policy cache";
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
          *errOut = std::string("pread failed: ") + std::strerror(errno);
        return false;
      }
      if (r == 0) {
        if (errOut)
          *errOut = "Unexpected EOF reading temporary policy cache";
        return false;
      }
      done += static_cast<size_t>(r);
    }
    return true;
#else
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(static_cast<char *>(dst), static_cast<std::streamsize>(nBytes));
    if (!stream_.good()) {
      if (errOut)
        *errOut = "read failed on temporary policy cache";
      return false;
    }
    return true;
#endif
  }
};

// Evaluates Bellman backup for one (board, curPiece) across all 7 next pieces simultaneously.
// Generates legal placements once and reuses child board lookups across n = 0..6.
// When `bestOutRead` is non-null (outer iterations >= 2 and residual check), skips all out-of-TB
// NN calls completely and initializes bestQ[n] from the precomputed iter-1 packed terminal payoff!
template <bool RecordActions, typename BoundaryFn>
inline double
computeBellmanBackup20(const Board20 &board, const TuckSetupInfo &tucks, int curPiece, int gravity,
                       const FlatBoardIndex &index, const std::vector<double> &vCur,
                       const TransitionProbMatrix &transProb, const StepDiscountTable20 &disc,
                       const BoundaryFn &boundaryFn, SoftBoundaryCache20 *cache,
                       const uint32_t *bestOutRead, PlacementList &pl,
                       uint32_t *outActions = nullptr, uint32_t *bestOutWrite = nullptr,
                       bool *sawOverflow = nullptr) noexcept {
  findPlacements10Hz(board, tucks, curPiece, gravity, pl);
  if (pl.count == 0) {
    if constexpr (RecordActions) {
      const uint32_t emptyAct = packPolicyAction(INVALID_BOARD_ID, 0);
      for (int n = 0; n < NUM_PIECES; ++n) {
        outActions[n] = emptyAct;
        if constexpr (BoundaryFn::enabled) {
          if (bestOutWrite != nullptr) {
            bestOutWrite[n] = emptyAct;
          }
        }
      }
    }
    return 0.0;
  }

  double bestQ[NUM_PIECES];
  uint32_t bestAct[NUM_PIECES];
  bool haveBest[NUM_PIECES];
  double bestOutQ[NUM_PIECES];
  uint32_t bestOutAct[NUM_PIECES];
  bool haveBestOut[NUM_PIECES];

  for (int n = 0; n < NUM_PIECES; ++n) {
    bestQ[n] = -1.0;
    bestAct[n] = packPolicyAction(INVALID_BOARD_ID, 0);
    haveBest[n] = false;
    bestOutQ[n] = -1.0;
    bestOutAct[n] = packPolicyAction(INVALID_BOARD_ID, 0);
    haveBestOut[n] = false;
  }

  // On outer iter >= 2 / residual check, seed bestQ from the iter-1 precomputed best-OUT word.
  if constexpr (BoundaryFn::enabled) {
    if (bestOutRead != nullptr) {
      for (int n = 0; n < NUM_PIECES; ++n) {
        const uint32_t outWord = bestOutRead[n];
        if (isTerminalPayoff(outWord)) {
          bestQ[n] = unpackTerminalPayoff(outWord);
          bestAct[n] = outWord;
          haveBest[n] = true;
        }
      }
    }
  }

  uint16_t afterRows[BOARD_ROWS];
  double boundCont[NUM_PIECES];

  for (int m = 0; m < pl.count; ++m) {
    const CompactPlacement &p = pl.data[m];
    const uint8_t lc = applyPlacement20(board, curPiece, p.rot, p.x, p.y, afterRows);
    const double rew = REWARD_NORM_TETRIS[lc];
    const int startRow =
        (lc == 0)
            ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow), static_cast<int>(p.y)))
            : 0;
    BoardKey afterKey;
    const bool fitsKey = encodeBoardKey(afterRows, afterKey, startRow);
    const uint32_t cid = fitsKey ? index.find(afterKey) : INVALID_BOARD_ID;

    if (cid != INVALID_BOARD_ID) {
      const double *childRow = &vCur[static_cast<size_t>(cid) * STATES_PER_BOARD];
      const uint32_t packed = packPolicyAction(cid, lc);
      const double dFac = disc.f[lc];
      for (int n = 0; n < NUM_PIECES; ++n) {
        const double q = rew + dFac * childRow[n];
        // Strict '>' ensures precomputed best-OUT ties stay consistent across iter 1 and iter 2+.
        if (!haveBest[n] || q > bestQ[n]) {
          bestQ[n] = q;
          bestAct[n] = packed;
          haveBest[n] = true;
        }
      }
    } else {
      if constexpr (BoundaryFn::enabled) {
        if (bestOutRead != nullptr) {
          // Out-of-TB moves were already maximized into bestOutRead on iter 1.
          continue;
        }
        if (fitsKey) {
          bool cacheHit = false;
          if (cache != nullptr) {
            cacheHit = cache->lookup(afterKey, boundCont);
          }
          if (!cacheHit) {
            boundaryFn.evalRow(afterRows, boundCont);
            if (cache != nullptr) {
              cache->store(afterKey, boundCont);
            }
          }
          const double dFac = disc.f[lc];
          for (int n = 0; n < NUM_PIECES; ++n) {
            const double cont =
                (std::isfinite(boundCont[n]) && boundCont[n] > 0.0) ? boundCont[n] : 0.0;
            const double qRaw = rew + dFac * cont;
            if (qRaw > TERMINAL_MAX_PAYOFF && sawOverflow != nullptr) {
              *sawOverflow = true;
            }
            const uint32_t termWord = packTerminalPayoff(qRaw);
            const double qQuant = unpackTerminalPayoff(termWord);
            if (!haveBestOut[n] || qQuant > bestOutQ[n]) {
              bestOutQ[n] = qQuant;
              bestOutAct[n] = termWord;
              haveBestOut[n] = true;
            }
            if (!haveBest[n] || qQuant > bestQ[n]) {
              bestQ[n] = qQuant;
              bestAct[n] = termWord;
              haveBest[n] = true;
            }
          }
        } else {
          // Unrepresentable boards (!fitsKey, e.g. >= 10 holes) can never enter the tablebase,
          // so gating Soft-Boundary NN continuation to 0.0 prevents uncorrectable OOD reward
          // hacking. Pack as terminal payoff so iter-1 bestOutStore matches iter-2+ bit-for-bit.
          const uint32_t termWord = packTerminalPayoff(rew);
          const double qQuant = unpackTerminalPayoff(termWord);
          for (int n = 0; n < NUM_PIECES; ++n) {
            if (!haveBestOut[n] || qQuant > bestOutQ[n]) {
              bestOutQ[n] = qQuant;
              bestOutAct[n] = termWord;
              haveBestOut[n] = true;
            }
            if (!haveBest[n] || qQuant > bestQ[n]) {
              bestQ[n] = qQuant;
              bestAct[n] = termWord;
              haveBest[n] = true;
            }
          }
        }
      } else {
        const uint32_t packed = packPolicyAction(INVALID_BOARD_ID, lc);
        for (int n = 0; n < NUM_PIECES; ++n) {
          if (!haveBest[n] || rew > bestQ[n]) {
            bestQ[n] = rew;
            bestAct[n] = packed;
            haveBest[n] = true;
          }
        }
      }
    }
  }

  // Ensure iter-1 tie-breaking between IN and OUT matches iter-2+ (where OUT is seeded first, so
  // IN only replaces OUT on strict `qIn > qOut`). Since we evaluated IN and OUT in move order on
  // iter 1, reconcile exact `bestQ == bestOutQ` ties in favor of bestOutAct when bestOutStore is
  // active so iter-1 and iter-2+ are bit-for-bit identical!
  if constexpr (BoundaryFn::enabled) {
    if (bestOutWrite != nullptr) {
      for (int n = 0; n < NUM_PIECES; ++n) {
        bestOutWrite[n] = haveBestOut[n] ? bestOutAct[n] : packPolicyAction(INVALID_BOARD_ID, 0);
        if (haveBestOut[n] && bestOutQ[n] == bestQ[n]) {
          bestAct[n] = bestOutAct[n];
        }
      }
    }
  }

  double expectedV = 0.0;
  const auto &probs = transProb[static_cast<size_t>(curPiece)];
  for (int n = 0; n < NUM_PIECES; ++n) {
    if constexpr (RecordActions) {
      outActions[n] = bestAct[n];
    }
    expectedV += probs[static_cast<size_t>(n)] * (haveBest[n] ? bestQ[n] : 0.0);
  }
  return expectedV;
}

inline bool initializeValuesFromWarmStart(const std::vector<BoardKey> &sortedKeys,
                                          const MappedDasTablebase &initTb,
                                          std::vector<double> &vCur, size_t &outMatched,
                                          std::string &errOut) {
  outMatched = 0;
  const uint64_t initCount = initTb.size();
  if (initCount == 0 || sortedKeys.empty()) {
    return true;
  }
  uint64_t j = 0;
  BoardKey prevKey = EMPTY_BOARD_KEY;
  bool havePrev = false;
  float row[NUM_PIECES];

  auto advanceInit = [&](uint64_t &idx) -> bool {
    const BoardKey k = initTb.keyAt(idx);
    if (havePrev && k <= prevKey) {
      errOut = "Warm-start tablebase keys are not strictly sorted ascending";
      return false;
    }
    prevKey = k;
    havePrev = true;
    ++idx;
    return true;
  };

  for (size_t i = 0; i < sortedKeys.size() && j < initCount; ++i) {
    const BoardKey target = sortedKeys[i];
    while (j < initCount && initTb.keyAt(j) < target) {
      if (!advanceInit(j))
        return false;
    }
    if (j < initCount && initTb.keyAt(j) == target) {
      initTb.copyValuesAt(j, row);
      double *dst = &vCur[i * STATES_PER_BOARD];
      for (int c = 0; c < NUM_PIECES; ++c) {
        if (!std::isfinite(row[c]) || row[c] < 0.0f) {
          errOut = "Warm-start tablebase contains non-finite or negative value";
          return false;
        }
        dst[c] = static_cast<double>(row[c]);
      }
      ++outMatched;
      if (!advanceInit(j))
        return false;
    }
  }
  return true;
}

template <typename BoundaryFn>
inline bool runPolicyImprovementSweep20(
    const std::vector<BoardKey> &sortedKeys, int gravity, const FlatBoardIndex &index,
    const TransitionProbMatrix &transProb, const StepDiscountTable20 &disc,
    const BoundaryFn &boundaryFn, std::vector<SoftBoundaryCache20> *threadCaches,
    PolicyActionStore20 *bestOutStore, bool writeBestOut, PersistentWorkerPool &pool,
    size_t minParallelBoards, const std::vector<double> &vCur, std::vector<double> &vNext,
    PolicyActionStore20 &policyStore, double &outMaxDiff, double &outMaxVal, std::string &errOut) {
  const size_t n = sortedKeys.size();
  const int threads = pool.numThreads();
  const size_t chunkStride =
      policyStore.isDisk() ? PolicyActionStore20::CHUNK_BOARDS : std::max<size_t>(1, n);
  double sweepMaxDiff = 0.0;
  double sweepMaxVal = 0.0;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);
  std::vector<double> threadMaxVal(static_cast<size_t>(threads), 0.0);
  std::atomic<bool> sawNonFinite{false};
  std::atomic<bool> sawOverflow{false};

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
          if (chunkBestOutRead == nullptr)
            return false;
        }
      }
    }
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
    std::fill(threadMaxVal.begin(), threadMaxVal.end(), 0.0);

    pool.runSubrange(
        chunkBegin, chunkEnd, minParallelBoards, [&](int tid, size_t subBegin, size_t subEnd) {
          PlacementList pl;
          SoftBoundaryCache20 *cache =
              (threadCaches != nullptr) ? &(*threadCaches)[static_cast<size_t>(tid)] : nullptr;
          double localMaxDiff = 0.0;
          double localMaxVal = 0.0;
          bool localOverflow = false;

          for (size_t i = subBegin; i < subEnd; ++i) {
            const Board20 board = Board20::fromKey(sortedKeys[i]);
            const TuckSetupInfo tucks = computeTuckSetupInfo(board);
            const size_t localIdx = i - chunkBegin;
            uint32_t *boardActions = &chunkActions[localIdx * ACTIONS_PER_BOARD];
            uint32_t *boardBestOutWrite = (chunkBestOutWrite != nullptr)
                                              ? &chunkBestOutWrite[localIdx * ACTIONS_PER_BOARD]
                                              : nullptr;
            const uint32_t *boardBestOutRead = (chunkBestOutRead != nullptr)
                                                   ? &chunkBestOutRead[localIdx * ACTIONS_PER_BOARD]
                                                   : nullptr;
            const double *oldRow = &vCur[i * STATES_PER_BOARD];
            double *newRow = &vNext[i * STATES_PER_BOARD];

            for (int c = 0; c < NUM_PIECES; ++c) {
              const size_t cOff = static_cast<size_t>(c) * STATES_PER_BOARD;
              const double updatedV = computeBellmanBackup20</*RecordActions=*/true>(
                  board, tucks, c, gravity, index, vCur, transProb, disc, boundaryFn, cache,
                  (boardBestOutRead != nullptr) ? &boardBestOutRead[cOff] : nullptr, pl,
                  &boardActions[cOff],
                  (boardBestOutWrite != nullptr) ? &boardBestOutWrite[cOff] : nullptr,
                  &localOverflow);
              newRow[c] = updatedV;
              if (!std::isfinite(updatedV)) {
                sawNonFinite.store(true, std::memory_order_relaxed);
              } else {
                if (updatedV > localMaxVal)
                  localMaxVal = updatedV;
                const double diff = std::abs(updatedV - oldRow[c]);
                if (diff > localMaxDiff)
                  localMaxDiff = diff;
              }
            }
          }
          if (localOverflow)
            sawOverflow.store(true, std::memory_order_relaxed);
          threadMaxDiff[static_cast<size_t>(tid)] = localMaxDiff;
          threadMaxVal[static_cast<size_t>(tid)] = localMaxVal;
        });

    if (sawNonFinite.load(std::memory_order_relaxed)) {
      errOut = "Non-finite value encountered during policy-improvement sweep";
      return false;
    }
    if (sawOverflow.load(std::memory_order_relaxed)) {
      errOut = "Soft-boundary terminal payoff exceeded TERMINAL_MAX_PAYOFF ("
               + std::to_string(TERMINAL_MAX_PAYOFF)
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

inline bool runPolicyEvalSweep20(size_t numBoards, const TransitionProbMatrix &transProb,
                                 const StepDiscountTable20 &disc, PersistentWorkerPool &pool,
                                 size_t minParallelBoards, const std::vector<double> &vCur,
                                 std::vector<double> &vNext, PolicyActionStore20 &policyStore,
                                 double &outMaxDiff, double &outMaxVal, std::string &errOut) {
  const int threads = pool.numThreads();
  const size_t chunkStride =
      policyStore.isDisk() ? PolicyActionStore20::CHUNK_BOARDS : std::max<size_t>(1, numBoards);
  double sweepMaxDiff = 0.0;
  double sweepMaxVal = 0.0;
  std::vector<double> threadMaxDiff(static_cast<size_t>(threads), 0.0);
  std::vector<double> threadMaxVal(static_cast<size_t>(threads), 0.0);
  std::atomic<bool> sawNonFinite{false};

  for (size_t chunkBegin = 0; chunkBegin < numBoards; chunkBegin += chunkStride) {
    const size_t chunkEnd = std::min(numBoards, chunkBegin + chunkStride);
    const size_t chunkCount = chunkEnd - chunkBegin;
    const uint32_t *chunkActions = policyStore.readChunk(chunkBegin, chunkCount, &errOut);
    if (chunkActions == nullptr)
      return false;

    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);
    std::fill(threadMaxVal.begin(), threadMaxVal.end(), 0.0);

    pool.runSubrange(
        chunkBegin, chunkEnd, minParallelBoards, [&](int tid, size_t subBegin, size_t subEnd) {
          double localMaxDiff = 0.0;
          double localMaxVal = 0.0;
          const StepDiscountTable20 localDisc = disc;
          const double *__restrict__ vCurPtr = vCur.data();
          const size_t zeroRowBase = numBoards * STATES_PER_BOARD;
          for (size_t i = subBegin; i < subEnd; ++i) {
            const uint32_t *boardActions = &chunkActions[(i - chunkBegin) * ACTIONS_PER_BOARD];
            const double *oldRow = &vCurPtr[i * STATES_PER_BOARD];
            double *__restrict__ newRow = &vNext[i * STATES_PER_BOARD];

            for (int c = 0; c < NUM_PIECES; ++c) {
              const uint32_t *cActions = &boardActions[static_cast<size_t>(c) * STATES_PER_BOARD];
              const auto &probs = transProb[static_cast<size_t>(c)];
              double updatedV = 0.0;
              for (int n = 0; n < NUM_PIECES; ++n) {
                const uint32_t packed = cActions[n];
                double q = 0.0;
                if (isTerminalPayoff(packed)) {
                  q = unpackTerminalPayoff(packed);
                } else {
                  const uint8_t lc = unpackPolicyLines(packed);
                  q = REWARD_NORM_TETRIS[lc];
                  const uint32_t rawChild = packed & ACTION_CHILD_MASK;
                  const size_t rowBase = (rawChild == ACTION_OUT_CHILD)
                                             ? zeroRowBase
                                             : (static_cast<size_t>(rawChild) * STATES_PER_BOARD);
                  q += localDisc.f[lc] * vCurPtr[rowBase + static_cast<size_t>(n)];
                }
                updatedV += probs[static_cast<size_t>(n)] * q;
              }
              newRow[c] = updatedV;
              if (!std::isfinite(updatedV)) {
                sawNonFinite.store(true, std::memory_order_relaxed);
              } else {
                if (updatedV > localMaxVal)
                  localMaxVal = updatedV;
                const double diff = std::abs(updatedV - oldRow[c]);
                if (diff > localMaxDiff)
                  localMaxDiff = diff;
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
inline bool computeBellmanResidual20(const std::vector<BoardKey> &sortedKeys, int gravity,
                                     const FlatBoardIndex &index,
                                     const TransitionProbMatrix &transProb,
                                     const StepDiscountTable20 &disc, const BoundaryFn &boundaryFn,
                                     PolicyActionStore20 *bestOutStore, PersistentWorkerPool &pool,
                                     size_t minParallelBoards, const std::vector<double> &vCur,
                                     double &outResidual, std::string &errOut) {
  const size_t n = sortedKeys.size();
  const int threads = pool.numThreads();
  const size_t chunkStride = (bestOutStore != nullptr && bestOutStore->isDisk())
                                 ? PolicyActionStore20::CHUNK_BOARDS
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
        if (chunkBestOutRead == nullptr)
          return false;
      }
    }
    std::fill(threadMaxDiff.begin(), threadMaxDiff.end(), 0.0);

    pool.runSubrange(chunkBegin, chunkEnd, minParallelBoards,
                     [&](int tid, size_t subBegin, size_t subEnd) {
                       PlacementList pl;
                       double localMax = 0.0;
                       for (size_t i = subBegin; i < subEnd; ++i) {
                         const Board20 board = Board20::fromKey(sortedKeys[i]);
                         const TuckSetupInfo tucks = computeTuckSetupInfo(board);
                         const double *curRow = &vCur[i * STATES_PER_BOARD];
                         const uint32_t *boardBestOutRead =
                             (chunkBestOutRead != nullptr)
                                 ? &chunkBestOutRead[(i - chunkBegin) * ACTIONS_PER_BOARD]
                                 : nullptr;
                         for (int c = 0; c < NUM_PIECES; ++c) {
                           const uint32_t *cBestOut =
                               (boardBestOutRead != nullptr)
                                   ? &boardBestOutRead[static_cast<size_t>(c) * STATES_PER_BOARD]
                                   : nullptr;
                           const double backup = computeBellmanBackup20</*RecordActions=*/false>(
                               board, tucks, c, gravity, index, vCur, transProb, disc, boundaryFn,
                               nullptr, cBestOut, pl);
                           const double diff = std::abs(backup - curRow[c]);
                           if (diff > localMax)
                             localMax = diff;
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

struct SolveConfig20 {
  RngMode rngMode = RngMode::Nes;
  int level = 19;
  double gamma = 0.995;
  DiscountMode discountMode = DiscountMode::PerPiece;
  PolicyCacheMode policyCacheMode = PolicyCacheMode::Auto;
  size_t autoDiskThresholdBoards = 6000000; // >6M boards (~1.18 GB policy) streams via SSD
  double tolerance = 1e-5;
  double innerStopRatio = 0.05;
  int maxOuterIterations = 80;
  int innerEvalSweeps = 200;
  double maxValueDivergenceGuard = 500.0;
  int numThreads = 1;
  size_t minParallelBoards = 4096;
  bool verbose = false;
};

struct SolveResult20 {
  bool ok = false;
  std::string error;
  std::vector<double> values; // Flat numBoards * 7 (double)
  int outerIterations = 0;
  int totalInnerSweeps = 0;
  double finalOuterDiff = 0.0;
  double bellmanResidual = 0.0;
  double emptyBoardValue = MISSING_BOARD_VALUE;
  double maxValue = 0.0;
  double elapsedSeconds = 0.0;
  size_t warmStartMatched = 0;
  bool converged = false;
  bool usedDiskPolicyCache = false;
};

// Solves stationary 20-row Next-Box MDP V(b, c) using Modified Policy Iteration with optional
// warm-start (`initTb`) and optional frozen soft-boundary evaluator (`boundaryFn`).
template <typename BoundaryFn = NoOpBoundaryEval20>
inline SolveResult20 solve20MPI(const std::vector<BoardKey> &sortedKeys, const SolveConfig20 &cfg,
                                const MappedDasTablebase *initTb = nullptr,
                                const BoundaryFn &boundaryFn = BoundaryFn{}) {
  SolveResult20 res{};
  const auto t0 = std::chrono::steady_clock::now();

  if (!std::isfinite(cfg.gamma) || cfg.gamma <= 0.0 || cfg.gamma > 1.0) {
    res.error = "Invalid gamma (must be in (0, 1])";
    return res;
  }
  if (!std::isfinite(cfg.tolerance) || cfg.tolerance <= 0.0) {
    res.error = "Invalid tolerance (must be > 0)";
    return res;
  }
  if (!std::isfinite(cfg.maxValueDivergenceGuard) || cfg.maxValueDivergenceGuard <= 0.0) {
    res.error = "Invalid maxValueDivergenceGuard";
    return res;
  }
  if (cfg.level != 18 && cfg.level != 19 && cfg.level != 29) {
    res.error = "Unsupported level (expected 18, 19, or 29)";
    return res;
  }
  if (cfg.maxOuterIterations <= 0 || cfg.innerEvalSweeps < 0) {
    res.error = "Invalid iteration limits";
    return res;
  }
  const size_t n = sortedKeys.size();
  if (n >= static_cast<size_t>(ACTION_OUT_CHILD)) {
    res.error = "Board count exceeds 28-bit packed childId capacity (268,435,454)";
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

  // Extra trailing zero row at index n acts as a branchless sentinel for ACTION_OUT_CHILD.
  std::vector<double> vCur((n + 1) * STATES_PER_BOARD, 0.0);
  std::vector<double> vNext((n + 1) * STATES_PER_BOARD, 0.0);
  if (initTb != nullptr && initTb->isLoaded()) {
    initTb->adviseSequential();
    if (!detail::initializeValuesFromWarmStart(sortedKeys, *initTb, vCur, res.warmStartMatched,
                                               res.error)) {
      initTb->adviseDontNeed();
      return res;
    }
    initTb->adviseDontNeed();
  }

  const size_t storeCount = BoundaryFn::enabled ? 2 : 1;
  const bool useDisk = (cfg.policyCacheMode == PolicyCacheMode::Disk)
                       || (cfg.policyCacheMode == PolicyCacheMode::Auto
                           && n > cfg.autoDiskThresholdBoards / storeCount);
  res.usedDiskPolicyCache = useDisk;

  detail::PolicyActionStore20 policyStore;
  if (!policyStore.init(n, useDisk, &res.error)) {
    return res;
  }

  detail::PolicyActionStore20 bestOutStore;
  if constexpr (BoundaryFn::enabled) {
    if (!bestOutStore.init(n, useDisk, &res.error)) {
      return res;
    }
  }

  const int threads = std::max(1, cfg.numThreads);
  detail::PersistentWorkerPool pool(threads);
  const int gravity = getGravity20(cfg.level);
  const auto transProb = detail::buildTransitionProbMatrix(cfg.rngMode);
  const StepDiscountTable20 disc = StepDiscountTable20::make(cfg.gamma, cfg.discountMode);
  const uint32_t emptyBoardId = index.find(EMPTY_BOARD_KEY);

  std::vector<detail::SoftBoundaryCache20> threadCaches;
  if constexpr (BoundaryFn::enabled) {
    threadCaches.resize(static_cast<size_t>(threads));
  }

  for (int outer = 1; outer <= cfg.maxOuterIterations; ++outer) {
    const auto iterStart = std::chrono::steady_clock::now();
    const bool writeBestOut = (outer == 1);
    double outerDiff = 0.0;
    double outerMaxV = 0.0;

    if (!detail::runPolicyImprovementSweep20(
            sortedKeys, gravity, index, transProb, disc, boundaryFn,
            BoundaryFn::enabled && writeBestOut ? &threadCaches : nullptr,
            BoundaryFn::enabled ? &bestOutStore : nullptr, writeBestOut, pool,
            cfg.minParallelBoards, vCur, vNext, policyStore, outerDiff, outerMaxV, res.error)) {
      return res;
    }
    if (writeBestOut && !threadCaches.empty()) {
      // Free per-worker boundary caches once bestOutStore is populated.
      std::vector<detail::SoftBoundaryCache20>().swap(threadCaches);
    }

    vCur.swap(vNext);
    res.outerIterations = outer;
    res.finalOuterDiff = outerDiff;
    res.maxValue = outerMaxV;

    if (outerMaxV > cfg.maxValueDivergenceGuard) {
      res.error = "Solver diverged: maxV="
                  + std::to_string(outerMaxV)
                  + " > guard="
                  + std::to_string(cfg.maxValueDivergenceGuard);
      return res;
    }

    // Adaptive inner early-stop: stop early on throwaway early policies, tighten near convergence.
    const double innerTol =
        std::max(cfg.tolerance * cfg.innerStopRatio, outerDiff * cfg.innerStopRatio);
    int innerDone = 0;
    double innerDiff = 0.0;
    double innerMaxV = outerMaxV;
    if (outerDiff > cfg.tolerance && cfg.innerEvalSweeps > 0) {
      for (int k = 0; k < cfg.innerEvalSweeps; ++k) {
        if (!detail::runPolicyEvalSweep20(n, transProb, disc, pool, cfg.minParallelBoards, vCur,
                                          vNext, policyStore, innerDiff, innerMaxV, res.error)) {
          return res;
        }
        vCur.swap(vNext);
        innerDone++;
        res.totalInnerSweeps++;
        res.maxValue = innerMaxV;
        if (innerMaxV > cfg.maxValueDivergenceGuard) {
          res.error = "Solver diverged in inner sweep: maxV=" + std::to_string(innerMaxV);
          return res;
        }
        if (innerDiff <= innerTol) {
          break;
        }
      }
    }

    if (cfg.verbose) {
      const double iterSec =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - iterStart).count();
      const double v0 =
          (emptyBoardId != INVALID_BOARD_ID)
              ? expectedValueFromRow(&vCur[static_cast<size_t>(emptyBoardId) * STATES_PER_BOARD],
                                     cfg.rngMode)
              : MISSING_BOARD_VALUE;
      printf("  [MPI outer=%2d] outerDiff=%.3e inner=%d(%.2e) E[V(0)]=%.5f maxV=%.3f (%.2fs)\n",
             outer, outerDiff, innerDone, innerDiff, v0, res.maxValue, iterSec);
      fflush(stdout);
    }

    if (outerDiff <= cfg.tolerance) {
      res.converged = true;
      break;
    }
  }

  // Release vNext and policyStore before exact residual check to lower peak RAM at 20M-50M scale.
  std::vector<double>().swap(vNext);
  policyStore.close();

  // Exact Bellman residual check on converged vCur.
  if (!detail::computeBellmanResidual20(sortedKeys, gravity, index, transProb, disc, boundaryFn,
                                        BoundaryFn::enabled ? &bestOutStore : nullptr, pool,
                                        cfg.minParallelBoards, vCur, res.bellmanResidual,
                                        res.error)) {
    return res;
  }

  if (emptyBoardId != INVALID_BOARD_ID) {
    res.emptyBoardValue = expectedValueFromRow(
        &vCur[static_cast<size_t>(emptyBoardId) * STATES_PER_BOARD], cfg.rngMode);
  }
  vCur.resize(n * STATES_PER_BOARD);
  res.values = std::move(vCur);
  res.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  res.ok = true;
  return res;
}

struct SoftMcResult20 {
  int numGames = 0;
  double meanReturn = 0.0;
  double stdError = 0.0;
  double meanSteps = 0.0;
  double exitRate = 0.0;
  double zScore = 0.0; // |meanReturn - expectedV0| / stdError
};

// Exact Soft-Boundary Monte Carlo verification matching the Bellman equation solved by solve20MPI.
// Uses the exact same quantized terminal payoffs when exiting the TB and step discount schedule.
template <typename TbType, typename BoundaryFn = NoOpBoundaryEval20>
inline SoftMcResult20
evaluateSoftBoundaryMC20(const TbType &tb, int numGames, uint64_t seed, int numThreads,
                         const BoundaryFn &boundaryFn = BoundaryFn{}, int maxSteps = 25000) {
  SoftMcResult20 out{};
  if (!tb.isLoaded() || numGames <= 0) {
    return out;
  }
  const int threads = std::max(1, std::min(numThreads, numGames));
  const int gravity = tb.gravity();
  const StepDiscountTable20 disc = StepDiscountTable20::make(tb.gamma(), tb.discountMode());
  const RngMode rngMode = tb.rngMode();

  std::vector<double> gameReturns(static_cast<size_t>(numGames), 0.0);
  std::vector<uint64_t> gameSteps(static_cast<size_t>(numGames), 0);
  std::vector<uint8_t> gameExited(static_cast<size_t>(numGames), 0);
  std::atomic<int> nextG{0};

  detail::runWorkerPool(threads, static_cast<size_t>(numGames), 32, [&](int /*tid*/) {
    PlacementList pl;
    uint16_t afterRows[BOARD_ROWS];
    double boundCont[NUM_PIECES];
    while (true) {
      const int g = nextG.fetch_add(1, std::memory_order_relaxed);
      if (g >= numGames)
        break;

      Rng rng(seed + static_cast<uint64_t>(g) * 1009ULL);
      int curP = rng.sampleOpeningPiece(rngMode);
      int nextP = rng.sampleNextPiece(rngMode, curP);
      BoardKey curKey = EMPTY_BOARD_KEY;
      double total = 0.0;
      double discount = 1.0;
      uint64_t steps = 0;
      bool exited = false;

      for (int step = 0; step < maxSteps; ++step) {
        if (discount < 1e-12) {
          break;
        }
        const Board20 board = Board20::fromKey(curKey);
        const TuckSetupInfo tucks = computeTuckSetupInfo(board);
        findPlacements10Hz(board, tucks, curP, gravity, pl);
        if (pl.count == 0) {
          break;
        }

        double bestQ = -1.0;
        bool bestIsInTb = false;
        BoardKey bestNextKey = EMPTY_KEY;
        uint8_t bestLc = 0;
        bool haveBest = false;

        for (int m = 0; m < pl.count; ++m) {
          const CompactPlacement &p = pl.data[m];
          const uint8_t lc = applyPlacement20(board, curP, p.rot, p.x, p.y, afterRows);
          const double rew = REWARD_NORM_TETRIS[lc];
          const int startRow = (lc == 0)
                                   ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                          static_cast<int>(p.y)))
                                   : 0;
          BoardKey afterKey;
          const bool fitsKey = encodeBoardKey(afterRows, afterKey, startRow);
          float vNextPiece = 0.0f;
          const bool inTb = fitsKey && tb.lookupPiece(afterKey, nextP, vNextPiece);

          if (inTb) {
            const double q = rew + disc.f[lc] * static_cast<double>(vNextPiece);
            if (!haveBest || q > bestQ) {
              bestQ = q;
              bestIsInTb = true;
              bestNextKey = afterKey;
              bestLc = lc;
              haveBest = true;
            }
          } else if constexpr (BoundaryFn::enabled) {
            double qQuant = 0.0;
            if (fitsKey) {
              boundaryFn.evalRow(afterRows, boundCont);
              const double cont = (std::isfinite(boundCont[nextP]) && boundCont[nextP] > 0.0)
                                      ? boundCont[nextP]
                                      : 0.0;
              qQuant = unpackTerminalPayoff(packTerminalPayoff(rew + disc.f[lc] * cont));
            } else {
              qQuant = unpackTerminalPayoff(packTerminalPayoff(rew));
            }
            if (!haveBest || qQuant > bestQ) {
              bestQ = qQuant;
              bestIsInTb = false;
              bestLc = lc;
              haveBest = true;
            }
          } else {
            if (!haveBest || rew > bestQ) {
              bestQ = rew;
              bestIsInTb = false;
              bestLc = lc;
              haveBest = true;
            }
          }
        }

        if (!haveBest) {
          break;
        }
        steps++;
        if (bestIsInTb) {
          total += discount * REWARD_NORM_TETRIS[bestLc];
          discount *= disc.f[bestLc];
          curKey = bestNextKey;
          curP = nextP;
          nextP = rng.sampleNextPiece(rngMode, curP);
        } else {
          // Terminal boundary exit: add exact quantized boundary payoff (or immediate reward).
          total += discount * bestQ;
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
  const double n = static_cast<double>(numGames);
  const double mean = sum / n;
  double varAcc = 0.0;
  for (double r : gameReturns) {
    const double d = r - mean;
    varAcc += d * d;
  }
  const double se = (numGames > 1) ? std::sqrt((varAcc / (n - 1.0)) / n) : 0.0;

  out.numGames = numGames;
  out.meanReturn = mean;
  out.stdError = se;
  out.meanSteps = static_cast<double>(sumSteps) / n;
  out.exitRate = 100.0 * static_cast<double>(exits) / n;
  out.zScore = (se > 1e-12 && tb.emptyBoardValue() >= 0.0)
                   ? std::abs(mean - tb.emptyBoardValue()) / se
                   : 0.0;
  return out;
}

} // namespace das

#endif // DAS_TABLEBASE_SOLVER_20ROW_HPP
