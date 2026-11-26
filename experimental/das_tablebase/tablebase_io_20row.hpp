#ifndef DAS_TABLEBASE_IO_20ROW_HPP
#define DAS_TABLEBASE_IO_20ROW_HPP

#include "core_20row.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace das {

static_assert(std::numeric_limits<float>::is_iec559,
              "Tablebase binary format requires IEEE-754 float");

inline constexpr char STATES_V2_MAGIC[8] = {'S', 'R', 'S', 'T', 'V', '0', '0', '2'};
inline constexpr char TABLEBASE_V3_MAGIC[8] = {'S', 'R', 'T', 'B', 'V', '0', '0', '3'};
inline constexpr uint32_t KEY_ENCODING_K16_OPT2C = 1U;
inline constexpr uint32_t REWARD_MODE_NORM_TETRIS = 0U;
inline constexpr uint32_t TB_FLAG_DISCOUNT_PER_LINE = 1U;
inline constexpr double MISSING_BOARD_VALUE = -1.0;
inline constexpr uint32_t INVALID_BOARD_ID = UINT32_MAX;

struct StatesHeaderV2 {
  char magic[8];
  uint64_t numBoards;
  uint32_t keyEncoding; // KEY_ENCODING_K16_OPT2C (1)
  uint32_t keyBytes;    // 16
  uint32_t maxRows;     // 20
  uint32_t maxHoles;    // 9
};
static_assert(sizeof(StatesHeaderV2) == 32, "StatesHeaderV2 must be 32 bytes");

struct TablebaseHeaderV3 {
  char magic[8];
  uint64_t numBoards;
  uint32_t keyEncoding;        // KEY_ENCODING_K16_OPT2C (1)
  uint32_t keyBytes;           // 16
  uint32_t maxRows;            // 20
  uint32_t maxHoles;           // 9
  uint32_t rngMode;            // 0 = Uniform, 1 = Nes
  uint32_t rewardMode;         // 0 = NormTetris
  uint32_t level;              // 18, 19, or 29
  uint32_t lineCount;          // 0 = stationary
  uint32_t reactionTimeFrames; // 0 for standard V(b, c)
  uint32_t flags;              // Bit 0: TB_FLAG_DISCOUNT_PER_LINE; bits 1..31 reserved (0)
  double gamma;                // Discount factor in (0, 1]
  double emptyBoardValue;      // E[V(0, c)] under rngMode, or MISSING_BOARD_VALUE
  double bellmanResidual;
};
static_assert(sizeof(TablebaseHeaderV3) == 80, "TablebaseHeaderV3 must be 80 bytes");

// Alignment-safe 44-byte on-disk record: 16-byte BoardKey + 7 float32 values.
struct PackedDasRecord {
  uint8_t keyBytes[16];
  uint8_t valueBytes[NUM_PIECES * sizeof(float)];

  BoardKey key() const noexcept {
    BoardKey k;
    std::memcpy(&k, keyBytes, sizeof(k));
    return k;
  }

  float value(int pieceIdx) const noexcept {
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    float v = 0.0f;
    std::memcpy(&v, valueBytes + static_cast<size_t>(pieceIdx) * sizeof(float), sizeof(v));
    return v;
  }

  void copyValues(float out[NUM_PIECES]) const noexcept {
    std::memcpy(out, valueBytes, sizeof(valueBytes));
  }

  bool hasValidValues() const noexcept {
    for (int c = 0; c < NUM_PIECES; ++c) {
      const float v = value(c);
      if (!std::isfinite(v) || v < 0.0f) {
        return false;
      }
    }
    return true;
  }

  static PackedDasRecord make(BoardKey k, const float vals[NUM_PIECES]) noexcept {
    PackedDasRecord rec{};
    std::memcpy(rec.keyBytes, &k, sizeof(k));
    std::memcpy(rec.valueBytes, vals, sizeof(rec.valueBytes));
    return rec;
  }
};
static_assert(sizeof(PackedDasRecord) == 44, "PackedDasRecord must be 44 bytes");

namespace detail {

// Reusable worker pool avoiding OS thread spawn/join churn across MPI chunks and Adam batches.
class PersistentWorkerPool {
public:
  explicit PersistentWorkerPool(int numThreads) : numThreads_(std::max(1, numThreads)) {
    if (numThreads_ > 1) {
      workers_.reserve(static_cast<size_t>(numThreads_ - 1));
      for (int tid = 1; tid < numThreads_; ++tid) {
        workers_.emplace_back([this, tid]() { workerLoop(tid); });
      }
    }
  }

  PersistentWorkerPool(const PersistentWorkerPool &) = delete;
  PersistentWorkerPool &operator=(const PersistentWorkerPool &) = delete;

  ~PersistentWorkerPool() {
    if (!workers_.empty()) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        ++epoch_;
      }
      cvWork_.notify_all();
      for (auto &w : workers_) {
        w.join();
      }
    }
  }

  int numThreads() const noexcept { return numThreads_; }

  template <typename Fn> void run(Fn &&fn) {
    if (numThreads_ <= 1) {
      fn(0);
      return;
    }
    std::function<void(int)> task(std::forward<Fn>(fn));
    {
      std::lock_guard<std::mutex> lk(mu_);
      currentTask_ = &task;
      remaining_ = numThreads_ - 1;
      ++epoch_;
    }
    cvWork_.notify_all();
    task(0);
    {
      std::unique_lock<std::mutex> lk(mu_);
      cvDone_.wait(lk, [&]() { return remaining_ == 0; });
      currentTask_ = nullptr;
    }
  }

  template <typename Fn>
  void runSubrange(size_t beginIdx, size_t endIdx, size_t minParallelWork, Fn &&fn) {
    if (beginIdx >= endIdx) {
      return;
    }
    const size_t total = endIdx - beginIdx;
    if (total < minParallelWork || numThreads_ <= 1) {
      fn(0, beginIdx, endIdx);
      return;
    }
    const int nActive = static_cast<int>(std::min<size_t>(static_cast<size_t>(numThreads_), total));
    run([&](int tid) {
      if (tid >= nActive)
        return;
      const size_t subBegin =
          beginIdx + (total * static_cast<size_t>(tid)) / static_cast<size_t>(nActive);
      const size_t subEnd =
          beginIdx + (total * static_cast<size_t>(tid + 1)) / static_cast<size_t>(nActive);
      if (subBegin < subEnd) {
        fn(tid, subBegin, subEnd);
      }
    });
  }

private:
  int numThreads_ = 1;
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable cvWork_;
  std::condition_variable cvDone_;
  uint64_t epoch_ = 0;
  int remaining_ = 0;
  bool stop_ = false;
  std::function<void(int)> *currentTask_ = nullptr;

  void workerLoop(int tid) {
    uint64_t seenEpoch = 0;
    while (true) {
      std::function<void(int)> *task = nullptr;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cvWork_.wait(lk, [&]() { return epoch_ != seenEpoch; });
        seenEpoch = epoch_;
        if (stop_) {
          return;
        }
        task = currentTask_;
      }
      if (task != nullptr) {
        (*task)(tid);
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (--remaining_ == 0) {
          cvDone_.notify_one();
        }
      }
    }
  }
};

inline bool isSameFilePath(const std::string &a, const std::string &b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  if (a == b) {
    return true;
  }
  std::error_code ec;
  if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
    return std::filesystem::equivalent(a, b, ec) && !ec;
  }
  const auto ca = std::filesystem::weakly_canonical(a, ec);
  if (ec) {
    return false;
  }
  const auto cb = std::filesystem::weakly_canonical(b, ec);
  if (ec) {
    return false;
  }
  return ca == cb;
}

template <typename Fn>
inline void runWorkerPool(int threads, size_t totalWork, size_t minParallelWork, Fn &&fn) {
  const int nThreads = (totalWork < minParallelWork || threads <= 1) ? 1 : threads;
  if (nThreads == 1) {
    fn(0);
    return;
  }
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(nThreads));
  for (int tid = 0; tid < nThreads; ++tid) {
    workers.emplace_back(fn, tid);
  }
  for (auto &w : workers) {
    w.join();
  }
}

inline bool validateSortedKeys(const std::vector<BoardKey> &sortedKeys, std::string *errOut) {
  for (size_t i = 0; i < sortedKeys.size(); ++i) {
    if (!isValidBoardKey(sortedKeys[i])) {
      if (errOut) {
        *errOut = "Invalid BoardKey at index " + std::to_string(i);
      }
      return false;
    }
    if (i > 0 && sortedKeys[i] <= sortedKeys[i - 1]) {
      if (errOut) {
        *errOut = "BoardKeys must be strictly sorted ascending with no duplicates";
      }
      return false;
    }
  }
  return true;
}

inline bool validateTbHeaderV3(const TablebaseHeaderV3 &hdr, uint64_t payloadBytes,
                               const std::string &path, std::string *errOut) {
  if (std::memcmp(hdr.magic, TABLEBASE_V3_MAGIC, sizeof(TABLEBASE_V3_MAGIC)) != 0) {
    if (errOut)
      *errOut = "Invalid SRTBV003 magic in: " + path;
    return false;
  }
  if (hdr.keyEncoding != KEY_ENCODING_K16_OPT2C
      || hdr.keyBytes != sizeof(BoardKey)
      || hdr.maxRows != BOARD_ROWS
      || hdr.maxHoles != KEY_MAX_HOLES) {
    if (errOut)
      *errOut = "Unsupported key geometry/encoding in SRTBV003: " + path;
    return false;
  }
  if (hdr.rngMode > 1U
      || hdr.rewardMode != REWARD_MODE_NORM_TETRIS
      || (hdr.flags & ~TB_FLAG_DISCOUNT_PER_LINE) != 0U) {
    if (errOut)
      *errOut = "Unsupported rngMode/rewardMode/flags in SRTBV003: " + path;
    return false;
  }
  if (hdr.level != 18U && hdr.level != 19U && hdr.level != 29U) {
    if (errOut)
      *errOut = "Unsupported level in SRTBV003: " + path;
    return false;
  }
  if (!std::isfinite(hdr.gamma) || hdr.gamma <= 0.0 || hdr.gamma > 1.0) {
    if (errOut)
      *errOut = "Invalid gamma in SRTBV003: " + path;
    return false;
  }
  if (!std::isfinite(hdr.emptyBoardValue)
      || !std::isfinite(hdr.bellmanResidual)
      || hdr.bellmanResidual < 0.0) {
    if (errOut)
      *errOut = "Invalid header metrics in SRTBV003: " + path;
    return false;
  }
  if (payloadBytes % sizeof(PackedDasRecord) != 0
      || payloadBytes / sizeof(PackedDasRecord) != hdr.numBoards) {
    if (errOut)
      *errOut = "File size mismatch for SRTBV003 payload in: " + path;
    return false;
  }
  return true;
}

inline size_t fastRange64(uint64_t hash, size_t range) noexcept {
#if defined(__SIZEOF_INT128__)
  return static_cast<size_t>(
      (static_cast<unsigned __int128>(hash) * static_cast<unsigned __int128>(range)) >> 64);
#else
  return static_cast<size_t>(hash % static_cast<uint64_t>(range));
#endif
}

} // namespace detail

// Writes strictly sorted BoardKeys to an SRSTV002 file.
inline bool writeStatesFileV2(const std::string &path, const std::vector<BoardKey> &sortedKeys,
                              std::string *errOut = nullptr) {
  if (!detail::validateSortedKeys(sortedKeys, errOut)) {
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errOut)
      *errOut = "Failed to open states file for writing: " + path;
    return false;
  }
  StatesHeaderV2 hdr{};
  std::memcpy(hdr.magic, STATES_V2_MAGIC, sizeof(STATES_V2_MAGIC));
  hdr.numBoards = static_cast<uint64_t>(sortedKeys.size());
  hdr.keyEncoding = KEY_ENCODING_K16_OPT2C;
  hdr.keyBytes = sizeof(BoardKey);
  hdr.maxRows = BOARD_ROWS;
  hdr.maxHoles = KEY_MAX_HOLES;
  out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
  if (!sortedKeys.empty()) {
    constexpr size_t CHUNK = 65536;
    for (size_t off = 0; off < sortedKeys.size(); off += CHUNK) {
      const size_t cnt = std::min(CHUNK, sortedKeys.size() - off);
      out.write(reinterpret_cast<const char *>(sortedKeys.data() + off),
                static_cast<std::streamsize>(cnt * sizeof(BoardKey)));
    }
  }
  if (!out.good()) {
    if (errOut)
      *errOut = "I/O error writing states file: " + path;
    return false;
  }
  return true;
}

// Reads SRSTV002 states file with strict overflow and sortedness validation.
inline bool readStatesFileV2(const std::string &path, std::vector<BoardKey> &outKeys,
                             std::string *errOut = nullptr) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (errOut)
      *errOut = "Failed to open states file: " + path;
    return false;
  }
  const std::streamoff endPos = in.tellg();
  if (endPos < static_cast<std::streamoff>(sizeof(StatesHeaderV2))) {
    if (errOut)
      *errOut = "States file smaller than header: " + path;
    return false;
  }
  in.seekg(0, std::ios::beg);
  StatesHeaderV2 hdr{};
  in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
  if (!in.good() || std::memcmp(hdr.magic, STATES_V2_MAGIC, sizeof(STATES_V2_MAGIC)) != 0) {
    if (errOut)
      *errOut = "Invalid SRSTV002 magic in: " + path;
    return false;
  }
  if (hdr.keyEncoding != KEY_ENCODING_K16_OPT2C
      || hdr.keyBytes != sizeof(BoardKey)
      || hdr.maxRows != BOARD_ROWS
      || hdr.maxHoles != KEY_MAX_HOLES) {
    if (errOut)
      *errOut = "Unsupported key geometry/encoding in SRSTV002: " + path;
    return false;
  }
  const uint64_t payloadBytes = static_cast<uint64_t>(endPos) - sizeof(StatesHeaderV2);
  if (payloadBytes % sizeof(BoardKey) != 0 || payloadBytes / sizeof(BoardKey) != hdr.numBoards) {
    if (errOut)
      *errOut = "States file size mismatch: " + path;
    return false;
  }
  if (hdr.numBoards
      > static_cast<uint64_t>(std::numeric_limits<size_t>::max() / sizeof(BoardKey))) {
    if (errOut)
      *errOut = "States file board count overflows size_t";
    return false;
  }
  std::vector<BoardKey> loaded(static_cast<size_t>(hdr.numBoards));
  constexpr size_t CHUNK = 65536;
  for (size_t off = 0; off < loaded.size(); off += CHUNK) {
    const size_t cnt = std::min(CHUNK, loaded.size() - off);
    in.read(reinterpret_cast<char *>(loaded.data() + off),
            static_cast<std::streamsize>(cnt * sizeof(BoardKey)));
    if (!in.good()) {
      if (errOut)
        *errOut = "I/O error reading states file: " + path;
      return false;
    }
  }
  if (!detail::validateSortedKeys(loaded, errOut)) {
    return false;
  }
  outKeys = std::move(loaded);
  return true;
}

// Writes an SRTBV003 tablebase file, validating all keys and values before truncating the output.
inline bool writeTablebaseV3(const std::string &path, const std::vector<BoardKey> &sortedKeys,
                             const std::vector<double> &valuesFlat, const TablebaseHeaderV3 &meta,
                             std::string *errOut = nullptr) {
  if (sortedKeys.size() > std::numeric_limits<size_t>::max() / NUM_PIECES
      || valuesFlat.size() != sortedKeys.size() * NUM_PIECES) {
    if (errOut)
      *errOut = "Value array size does not match numBoards * 7";
    return false;
  }
  if (!detail::validateSortedKeys(sortedKeys, errOut)) {
    return false;
  }
  for (double dv : valuesFlat) {
    const float fv = static_cast<float>(dv);
    if (!std::isfinite(dv) || dv < 0.0 || !std::isfinite(fv) || fv < 0.0f) {
      if (errOut)
        *errOut = "Refusing to write non-finite, overflowing, or negative TB value";
      return false;
    }
  }

  TablebaseHeaderV3 hdr = meta;
  std::memcpy(hdr.magic, TABLEBASE_V3_MAGIC, sizeof(TABLEBASE_V3_MAGIC));
  hdr.numBoards = static_cast<uint64_t>(sortedKeys.size());
  hdr.keyEncoding = KEY_ENCODING_K16_OPT2C;
  hdr.keyBytes = sizeof(BoardKey);
  hdr.maxRows = BOARD_ROWS;
  hdr.maxHoles = KEY_MAX_HOLES;

  const uint64_t payloadBytes =
      static_cast<uint64_t>(sortedKeys.size()) * static_cast<uint64_t>(sizeof(PackedDasRecord));
  if (!detail::validateTbHeaderV3(hdr, payloadBytes, path, errOut)) {
    return false;
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errOut)
      *errOut = "Failed to open tablebase for writing: " + path;
    return false;
  }
  out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));

  constexpr size_t CHUNK = 16384;
  std::vector<PackedDasRecord> buf;
  buf.reserve(std::min(sortedKeys.size(), CHUNK));
  for (size_t i = 0; i < sortedKeys.size(); ++i) {
    float row[NUM_PIECES];
    for (int c = 0; c < NUM_PIECES; ++c) {
      row[c] = static_cast<float>(valuesFlat[i * NUM_PIECES + static_cast<size_t>(c)]);
    }
    buf.push_back(PackedDasRecord::make(sortedKeys[i], row));
    if (buf.size() == CHUNK || i + 1 == sortedKeys.size()) {
      out.write(reinterpret_cast<const char *>(buf.data()),
                static_cast<std::streamsize>(buf.size() * sizeof(PackedDasRecord)));
      buf.clear();
    }
  }
  if (!out.good()) {
    if (errOut)
      *errOut = "I/O error writing SRTBV003 file: " + path;
    return false;
  }
  return true;
}

// Read-only memory-mapped SRTBV003 tablebase (O(log N) binary search, transactional open).
class MappedDasTablebase {
public:
  static constexpr uint64_t NOT_FOUND = ~0ULL;

  MappedDasTablebase() = default;
  MappedDasTablebase(const MappedDasTablebase &) = delete;
  MappedDasTablebase &operator=(const MappedDasTablebase &) = delete;
  MappedDasTablebase(MappedDasTablebase &&other) noexcept { *this = std::move(other); }

  MappedDasTablebase &operator=(MappedDasTablebase &&other) noexcept {
    if (this != &other) {
      close();
      header_ = other.header_;
      records_ = other.records_;
      loaded_ = other.loaded_;
      path_ = std::move(other.path_);
#ifndef _WIN32
      mapAddr_ = other.mapAddr_;
      mapSize_ = other.mapSize_;
      other.mapAddr_ = nullptr;
      other.mapSize_ = 0;
#else
      ownedRecords_ = std::move(other.ownedRecords_);
      records_ = ownedRecords_.data();
#endif
      other.records_ = nullptr;
      other.loaded_ = false;
      other.header_ = {};
      other.path_.clear();
    }
    return *this;
  }

  ~MappedDasTablebase() { close(); }

  bool open(const std::string &path, std::string *errOut = nullptr, bool verifyRecords = false) {
    MappedDasTablebase staged;
    if (!staged.openInternal(path, errOut, verifyRecords)) {
      return false;
    }
    *this = std::move(staged);
    return true;
  }

  // Advises OS to reclaim clean file-cache pages once a sequential scan completes (e.g., index
  // build, key extraction, or warm-start load).
  void adviseDontNeed() const noexcept {
#ifndef _WIN32
    if (mapAddr_ != nullptr && mapSize_ > 0) {
      ::madvise(mapAddr_, mapSize_, MADV_DONTNEED);
    }
#endif
  }

  // Advises OS for sequential scans (e.g., single-pass index build, key extraction, or warm-start).
  void adviseSequential() const noexcept {
#ifndef _WIN32
    if (mapAddr_ != nullptr && mapSize_ > 0) {
      ::madvise(mapAddr_, mapSize_, MADV_SEQUENTIAL);
    }
#endif
  }

  // Advises OS against read-ahead thrashing during random binary-search lookups (e.g., Soft-MC).
  void adviseRandom() const noexcept {
#ifndef _WIN32
    if (mapAddr_ != nullptr && mapSize_ > 0) {
      ::madvise(mapAddr_, mapSize_, MADV_RANDOM);
    }
#endif
  }

  void close() noexcept {
#ifndef _WIN32
    if (mapAddr_ != nullptr && mapSize_ > 0) {
      ::munmap(mapAddr_, mapSize_);
    }
    mapAddr_ = nullptr;
    mapSize_ = 0;
#else
    ownedRecords_.clear();
    ownedRecords_.shrink_to_fit();
#endif
    records_ = nullptr;
    header_ = {};
    loaded_ = false;
    path_.clear();
  }

  bool isLoaded() const noexcept { return loaded_; }
  const std::string &path() const noexcept { return path_; }
  const TablebaseHeaderV3 &header() const noexcept { return header_; }
  uint64_t size() const noexcept { return loaded_ ? header_.numBoards : 0; }
  RngMode rngMode() const noexcept {
    return (header_.rngMode == 1U) ? RngMode::Nes : RngMode::Uniform;
  }
  int level() const noexcept { return static_cast<int>(header_.level); }
  int gravity() const noexcept { return getGravity20(level()); }
  double gamma() const noexcept { return header_.gamma; }
  DiscountMode discountMode() const noexcept {
    return ((header_.flags & TB_FLAG_DISCOUNT_PER_LINE) != 0U) ? DiscountMode::PerLine
                                                               : DiscountMode::PerPiece;
  }
  double emptyBoardValue() const noexcept {
    return loaded_ ? header_.emptyBoardValue : MISSING_BOARD_VALUE;
  }
  double bellmanResidual() const noexcept { return loaded_ ? header_.bellmanResidual : 0.0; }

  BoardKey keyAt(uint64_t idx) const noexcept {
    assert(loaded_ && idx < header_.numBoards);
    return records_[idx].key();
  }

  bool recordValuesValidAt(uint64_t idx) const noexcept {
    assert(loaded_ && idx < header_.numBoards);
    return records_[idx].hasValidValues();
  }

  void copyValuesAt(uint64_t idx, float out[NUM_PIECES]) const noexcept {
    assert(loaded_ && idx < header_.numBoards);
    records_[idx].copyValues(out);
  }

  uint64_t findIndex(BoardKey target) const noexcept {
    if (!loaded_ || header_.numBoards == 0) {
      return NOT_FOUND;
    }
    uint64_t lo = 0;
    uint64_t hi = header_.numBoards;
    while (lo < hi) {
      const uint64_t mid = lo + ((hi - lo) >> 1);
      const BoardKey midKey = records_[mid].key();
      if (midKey < target) {
        lo = mid + 1;
      } else if (midKey > target) {
        hi = mid;
      } else {
        return mid;
      }
    }
    return NOT_FOUND;
  }

  bool lookup(BoardKey target, float outRow[NUM_PIECES]) const noexcept {
    const uint64_t idx = findIndex(target);
    if (idx == NOT_FOUND) {
      return false;
    }
    records_[idx].copyValues(outRow);
    return true;
  }

  bool lookupPiece(BoardKey target, int pieceIdx, float &outVal) const noexcept {
    const uint64_t idx = findIndex(target);
    if (idx == NOT_FOUND) {
      return false;
    }
    outVal = records_[idx].value(pieceIdx);
    return true;
  }

private:
  TablebaseHeaderV3 header_{};
  const PackedDasRecord *records_ = nullptr;
  bool loaded_ = false;
  std::string path_;
#ifndef _WIN32
  void *mapAddr_ = nullptr;
  size_t mapSize_ = 0;
#else
  std::vector<PackedDasRecord> ownedRecords_;
#endif

  bool openInternal(const std::string &path, std::string *errOut, bool verifyRecords) {
    path_ = path;
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errOut)
        *errOut = "Failed to open SRTBV003 file: " + path;
      return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(TablebaseHeaderV3))) {
      ::close(fd);
      if (errOut)
        *errOut = "SRTBV003 file too small or unreadable: " + path;
      return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(st.st_size);
    if (fileSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      ::close(fd);
      if (errOut)
        *errOut = "SRTBV003 file exceeds addressable size_t";
      return false;
    }
    void *mapped = ::mmap(nullptr, static_cast<size_t>(fileSize), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
      if (errOut)
        *errOut = "mmap failed for SRTBV003 file: " + path;
      return false;
    }
    mapAddr_ = mapped;
    mapSize_ = static_cast<size_t>(fileSize);
    std::memcpy(&header_, mapped, sizeof(header_));
    const uint64_t payloadBytes = fileSize - sizeof(TablebaseHeaderV3);
    if (!detail::validateTbHeaderV3(header_, payloadBytes, path, errOut)) {
      close();
      return false;
    }
    records_ = reinterpret_cast<const PackedDasRecord *>(static_cast<const uint8_t *>(mapped)
                                                         + sizeof(TablebaseHeaderV3));
#else
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
      if (errOut)
        *errOut = "Failed to open SRTBV003 file: " + path;
      return false;
    }
    const std::streamoff endPos = in.tellg();
    if (endPos < static_cast<std::streamoff>(sizeof(TablebaseHeaderV3))) {
      if (errOut)
        *errOut = "SRTBV003 file smaller than header: " + path;
      return false;
    }
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char *>(&header_), sizeof(header_));
    const uint64_t payloadBytes = static_cast<uint64_t>(endPos) - sizeof(TablebaseHeaderV3);
    if (!detail::validateTbHeaderV3(header_, payloadBytes, path, errOut)) {
      close();
      return false;
    }
    ownedRecords_.resize(static_cast<size_t>(header_.numBoards));
    if (!ownedRecords_.empty()) {
      in.read(reinterpret_cast<char *>(ownedRecords_.data()),
              static_cast<std::streamsize>(payloadBytes));
      if (!in.good()) {
        if (errOut)
          *errOut = "Failed reading SRTBV003 payload: " + path;
        close();
        return false;
      }
    }
    records_ = ownedRecords_.data();
#endif

    if (verifyRecords) {
      for (uint64_t i = 0; i < header_.numBoards; ++i) {
        const BoardKey k = records_[i].key();
        if (!isValidBoardKey(k) || !records_[i].hasValidValues()) {
          if (errOut)
            *errOut = "Corrupt key or value in SRTBV003 at index " + std::to_string(i);
          close();
          return false;
        }
        if (i > 0 && k <= records_[i - 1].key()) {
          if (errOut)
            *errOut = "Unsorted or duplicate key in SRTBV003 at index " + std::to_string(i);
          close();
          return false;
        }
      }
    }
    loaded_ = true;
    return true;
  }
};

// Loads sorted BoardKeys from either an SRSTV002 states file or an SRTBV003 tablebase.
inline bool loadInputBoardKeys(const std::string &path, std::vector<BoardKey> &outKeys,
                               std::string *errOut = nullptr) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (errOut)
      *errOut = "Failed to open input board file: " + path;
    return false;
  }
  char magic[8] = {};
  in.read(magic, sizeof(magic));
  if (!in.good()) {
    if (errOut)
      *errOut = "Input file smaller than 8-byte magic: " + path;
    return false;
  }
  in.close();

  if (std::memcmp(magic, STATES_V2_MAGIC, sizeof(STATES_V2_MAGIC)) == 0) {
    return readStatesFileV2(path, outKeys, errOut);
  }
  if (std::memcmp(magic, TABLEBASE_V3_MAGIC, sizeof(TABLEBASE_V3_MAGIC)) == 0) {
    MappedDasTablebase tb;
    if (!tb.open(path, errOut, /*verifyRecords=*/false)) {
      return false;
    }
    if (tb.size() > static_cast<uint64_t>(std::numeric_limits<size_t>::max() / sizeof(BoardKey))) {
      if (errOut)
        *errOut = "Tablebase board count overflows size_t";
      return false;
    }
    tb.adviseSequential();
    std::vector<BoardKey> keys(static_cast<size_t>(tb.size()));
    for (uint64_t i = 0; i < tb.size(); ++i) {
      const BoardKey k = tb.keyAt(i);
      if (!isValidBoardKey(k) || !tb.recordValuesValidAt(i)) {
        tb.adviseDontNeed();
        if (errOut)
          *errOut = "Corrupt key or value in SRTBV003 at index " + std::to_string(i);
        return false;
      }
      if (i > 0 && k <= keys[static_cast<size_t>(i - 1)]) {
        tb.adviseDontNeed();
        if (errOut)
          *errOut = "Unsorted or duplicate key in SRTBV003 at index " + std::to_string(i);
        return false;
      }
      keys[static_cast<size_t>(i)] = k;
    }
    tb.adviseDontNeed();
    outKeys = std::move(keys);
    return true;
  }
  if (errOut)
    *errOut = "Unrecognized input file magic (expected SRSTV002 or SRTBV003): " + path;
  return false;
}

// Cache-line-aligned 6-slot AoS bucket hash table: BoardKey -> uint32_t boardId.
// 128 B/bucket (6 slots = 21.33 B/slot) at ~65% load using Lemire fastrange:
//   - ~32.8 B/board (164 MB at 5M, 656 MB at 20M, 1.64 GB at 50M)
//   - Single 128 B cache-line fetch serves both key probe and boardId on hits.
class FlatBoardIndex {
public:
  static constexpr size_t SLOTS_PER_BUCKET = 6;

  struct alignas(128) Bucket {
    BoardKey keys[SLOTS_PER_BUCKET];
    uint32_t ids[SLOTS_PER_BUCKET];
    uint8_t pad[8];
  };
  static_assert(sizeof(Bucket) == 128, "FlatBoardIndex::Bucket must fit one 128-byte cache line");

  void clear() noexcept {
    std::vector<Bucket>().swap(buckets_);
    numBuckets_ = 0;
    size_ = 0;
    buildReservedCount_ = 0;
  }

  template <typename KeyAtFn>
  bool buildFromAccessor(size_t count, KeyAtFn &&keyAt, std::string *errOut = nullptr) {
    if (!beginBuild(count, errOut)) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      if (!insertBuiltKey(keyAt(i), static_cast<uint32_t>(i), errOut)) {
        return false;
      }
    }
    return true;
  }

  bool build(const std::vector<BoardKey> &sortedKeys, std::string *errOut = nullptr) {
    return buildFromAccessor(
        sortedKeys.size(), [&](size_t i) noexcept { return sortedKeys[i]; }, errOut);
  }

  void prefetch(BoardKey key) const noexcept {
    if (numBuckets_ == 0)
      return;
    const size_t b = detail::fastRange64(hashBoardKeySlot(key), numBuckets_);
    __builtin_prefetch(&buckets_[b], 0, 1);
  }

  uint32_t find(BoardKey key) const noexcept {
    if (numBuckets_ == 0) {
      return INVALID_BOARD_ID;
    }
    size_t b = detail::fastRange64(hashBoardKeySlot(key), numBuckets_);
    while (true) {
      const Bucket &bkt = buckets_[b];
      for (size_t j = 0; j < SLOTS_PER_BUCKET; ++j) {
        const BoardKey cur = bkt.keys[j];
        if (cur == EMPTY_KEY) {
          return INVALID_BOARD_ID;
        }
        if (cur == key) {
          return bkt.ids[j];
        }
      }
      b = (b + 1 == numBuckets_) ? 0 : (b + 1);
    }
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return numBuckets_ * SLOTS_PER_BUCKET; }

private:
  friend class IndexedDasTablebase;

  bool beginBuild(size_t count, std::string *errOut = nullptr) {
    clear();
    if (count >= static_cast<size_t>(INVALID_BOARD_ID)) {
      if (errOut)
        *errOut = "Board count exceeds 32-bit boardId capacity";
      return false;
    }
    if (count == 0) {
      return true;
    }

    // Target <= 65% load factor (6 * 0.65 = 3.9 entries/bucket; integer ratio 10 / 39).
    if (count > (std::numeric_limits<size_t>::max() - 38) / 10) {
      if (errOut)
        *errOut = "FlatBoardIndex bucket count overflow";
      return false;
    }
    const size_t nBuckets = std::max<size_t>(2, (count * 10 + 38) / 39);
    Bucket emptyBucket{};
    for (size_t j = 0; j < SLOTS_PER_BUCKET; ++j) {
      emptyBucket.keys[j] = EMPTY_KEY;
      emptyBucket.ids[j] = INVALID_BOARD_ID;
    }
    buckets_.assign(nBuckets, emptyBucket);
    numBuckets_ = nBuckets;
    size_ = 0;
    buildReservedCount_ = count;
    return true;
  }

  bool insertBuiltKey(BoardKey k, uint32_t id, std::string *errOut = nullptr) {
    if (k == EMPTY_KEY) {
      clear();
      if (errOut)
        *errOut = "FlatBoardIndex cannot store EMPTY_KEY sentinel";
      return false;
    }
    if (id == INVALID_BOARD_ID) {
      clear();
      if (errOut)
        *errOut = "FlatBoardIndex cannot store INVALID_BOARD_ID sentinel";
      return false;
    }
    if (size_ >= buildReservedCount_ || numBuckets_ == 0) {
      clear();
      if (errOut)
        *errOut = "FlatBoardIndex::insertBuiltKey exceeded reserved build capacity";
      return false;
    }
    size_t b = detail::fastRange64(hashBoardKeySlot(k), numBuckets_);
    while (true) {
      Bucket &bkt = buckets_[b];
      for (size_t j = 0; j < SLOTS_PER_BUCKET; ++j) {
        if (bkt.keys[j] == EMPTY_KEY) {
          bkt.keys[j] = k;
          bkt.ids[j] = id;
          ++size_;
          return true;
        }
        if (bkt.keys[j] == k) {
          clear();
          if (errOut)
            *errOut = "Duplicate BoardKey encountered while building FlatBoardIndex";
          return false;
        }
      }
      b = (b + 1 == numBuckets_) ? 0 : (b + 1);
    }
  }

  std::vector<Bucket> buckets_;
  size_t numBuckets_ = 0;
  size_t size_ = 0;
  size_t buildReservedCount_ = 0;
};

// In-RAM O(1) indexed SRTBV003 tablebase (for fast rollouts, distillation, and benchmarking).
// Memory: ~60.8 B/board (~32.8 B bucket index + 28 B float[7] values -> 1.22 GB at 20M, 3.04 GB at
// 50M).
class IndexedDasTablebase {
public:
  // Single-pass transactional build directly from MappedDasTablebase (no temporary keys vector,
  // validates sorted keys & finite non-negative values while populating FlatBoardIndex inline).
  bool buildFromMapped(const MappedDasTablebase &mapped, std::string *errOut = nullptr) {
    if (!mapped.isLoaded()) {
      if (errOut)
        *errOut = "Cannot index unloaded MappedDasTablebase";
      return false;
    }
    const uint64_t n64 = mapped.size();
    if (n64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max() / NUM_PIECES)) {
      if (errOut)
        *errOut = "IndexedDasTablebase board count overflows size_t";
      return false;
    }
    const size_t n = static_cast<size_t>(n64);

    FlatBoardIndex stagedIndex;
    if (!stagedIndex.beginBuild(n, errOut)) {
      return false;
    }
    std::vector<float> stagedVals(n * NUM_PIECES);

    BoardKey prevKey = EMPTY_BOARD_KEY;
    for (size_t i = 0; i < n; ++i) {
      const BoardKey k = mapped.keyAt(static_cast<uint64_t>(i));
      if (!isValidBoardKey(k) || !mapped.recordValuesValidAt(static_cast<uint64_t>(i))) {
        if (errOut)
          *errOut = "Corrupt key or value in SRTBV003 at index " + std::to_string(i);
        return false;
      }
      if (i > 0 && k <= prevKey) {
        if (errOut)
          *errOut = "Unsorted or duplicate key in SRTBV003 at index " + std::to_string(i);
        return false;
      }
      prevKey = k;
      mapped.copyValuesAt(static_cast<uint64_t>(i), &stagedVals[i * NUM_PIECES]);
      if (!stagedIndex.insertBuiltKey(k, static_cast<uint32_t>(i), errOut)) {
        return false;
      }
    }

    index_ = std::move(stagedIndex);
    values_ = std::move(stagedVals);
    header_ = mapped.header();
    loaded_ = true;
    return true;
  }

  bool open(const std::string &path, std::string *errOut = nullptr, bool verifyRecords = false) {
    (void)verifyRecords; // buildFromMapped validates all keys and values inline in a single pass.
    MappedDasTablebase mapped;
    if (!mapped.open(path, errOut, /*verifyRecords=*/false)) {
      return false;
    }
    mapped.adviseSequential();
    const bool ok = buildFromMapped(mapped, errOut);
    mapped.adviseDontNeed();
    return ok;
  }

  void clear() noexcept {
    index_.clear();
    std::vector<float>().swap(values_);
    header_ = {};
    loaded_ = false;
  }

  bool isLoaded() const noexcept { return loaded_; }
  uint64_t size() const noexcept { return loaded_ ? header_.numBoards : 0; }
  const TablebaseHeaderV3 &header() const noexcept { return header_; }
  RngMode rngMode() const noexcept {
    return (header_.rngMode == 1U) ? RngMode::Nes : RngMode::Uniform;
  }
  int level() const noexcept { return static_cast<int>(header_.level); }
  int gravity() const noexcept { return getGravity20(level()); }
  double gamma() const noexcept { return header_.gamma; }
  DiscountMode discountMode() const noexcept {
    return ((header_.flags & TB_FLAG_DISCOUNT_PER_LINE) != 0U) ? DiscountMode::PerLine
                                                               : DiscountMode::PerPiece;
  }
  double emptyBoardValue() const noexcept {
    return loaded_ ? header_.emptyBoardValue : MISSING_BOARD_VALUE;
  }
  double bellmanResidual() const noexcept { return loaded_ ? header_.bellmanResidual : 0.0; }

  bool lookup(BoardKey key, float outRow[NUM_PIECES]) const noexcept {
    if (!loaded_) {
      return false;
    }
    const uint32_t id = index_.find(key);
    if (id == INVALID_BOARD_ID) {
      return false;
    }
    std::memcpy(outRow, &values_[static_cast<size_t>(id) * NUM_PIECES], NUM_PIECES * sizeof(float));
    return true;
  }

  bool lookupPiece(BoardKey key, int pieceIdx, float &outVal) const noexcept {
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    if (!loaded_) {
      return false;
    }
    const uint32_t id = index_.find(key);
    if (id == INVALID_BOARD_ID) {
      return false;
    }
    outVal = values_[static_cast<size_t>(id) * NUM_PIECES + static_cast<size_t>(pieceIdx)];
    return true;
  }

private:
  FlatBoardIndex index_;
  std::vector<float> values_;
  TablebaseHeaderV3 header_{};
  bool loaded_ = false;
};

} // namespace das

#endif // DAS_TABLEBASE_IO_20ROW_HPP
