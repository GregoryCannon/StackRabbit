#ifndef TABLEBASE_IO_HPP
#define TABLEBASE_IO_HPP

#include "core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tb {

static_assert(std::numeric_limits<float>::is_iec559,
              "Tablebase binary format requires IEEE-754 float");

inline constexpr char STATES_MAGIC[8] = {'S', 'R', 'S', 'T', 'V', '0', '0', '1'};
inline constexpr char TABLEBASE_MAGIC[8] = {'S', 'R', 'T', 'B', 'V', '0', '0', '1'};

struct StatesFileHeader {
  char magic[8];
  uint64_t numBoards;
};
static_assert(sizeof(StatesFileHeader) == 16, "StatesFileHeader must be 16 bytes");

struct TablebaseHeader {
  char magic[8];
  uint64_t numBoards;
  double emptyBoardValue; // V(0ULL), or -1.0 sentinel when board 0ULL is absent from the file
  double bellmanResidual;
};
static_assert(sizeof(TablebaseHeader) == 32, "TablebaseHeader must be 32 bytes");

// Alignment-safe 12-byte on-disk record (8-byte native-endian board + 4-byte IEEE-754 float).
struct PackedTablebaseRecord {
  uint8_t boardBytes[8];
  uint8_t valueBytes[4];

  uint64_t board() const noexcept {
    uint64_t b = 0;
    std::memcpy(&b, boardBytes, sizeof(b));
    return b;
  }

  float value() const noexcept {
    float v = 0.0f;
    std::memcpy(&v, valueBytes, sizeof(v));
    return v;
  }

  static PackedTablebaseRecord make(uint64_t b, float v) noexcept {
    PackedTablebaseRecord rec{};
    std::memcpy(rec.boardBytes, &b, sizeof(b));
    std::memcpy(rec.valueBytes, &v, sizeof(v));
    return rec;
  }
};
static_assert(sizeof(PackedTablebaseRecord) == 12, "PackedTablebaseRecord must be 12 bytes");

// Writes a strictly ascending sorted list of canonical 40-bit boards to disk.
inline bool writeStatesFile(const std::string &path, const std::vector<uint64_t> &sortedBoards,
                            std::string *errOut = nullptr) {
  for (size_t i = 0; i < sortedBoards.size(); ++i) {
    if ((sortedBoards[i] & ~VALID_BOARD_MASK) != 0) {
      if (errOut)
        *errOut = "Board exceeds 40-bit playfield mask";
      return false;
    }
    if (i > 0 && sortedBoards[i] <= sortedBoards[i - 1]) {
      if (errOut)
        *errOut = "Boards must be strictly sorted ascending with no duplicates";
      return false;
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errOut)
      *errOut = "Failed to open states file for writing: " + path;
    return false;
  }

  StatesFileHeader hdr{};
  std::memcpy(hdr.magic, STATES_MAGIC, sizeof(STATES_MAGIC));
  hdr.numBoards = static_cast<uint64_t>(sortedBoards.size());
  out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
  if (!sortedBoards.empty()) {
    out.write(reinterpret_cast<const char *>(sortedBoards.data()),
              static_cast<std::streamsize>(sortedBoards.size() * sizeof(uint64_t)));
  }
  if (!out.good()) {
    if (errOut)
      *errOut = "I/O error while writing states file: " + path;
    return false;
  }
  return true;
}

inline bool readStatesFile(const std::string &path, std::vector<uint64_t> &outBoards,
                           std::string *errOut = nullptr) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (errOut)
      *errOut = "Failed to open states file: " + path;
    return false;
  }
  const std::streamoff fileSize = in.tellg();
  if (fileSize < static_cast<std::streamoff>(sizeof(StatesFileHeader))) {
    if (errOut)
      *errOut = "States file smaller than header: " + path;
    return false;
  }
  in.seekg(0, std::ios::beg);

  StatesFileHeader hdr{};
  in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
  if (!in.good() || std::memcmp(hdr.magic, STATES_MAGIC, sizeof(STATES_MAGIC)) != 0) {
    if (errOut)
      *errOut = "Invalid states file magic in: " + path;
    return false;
  }

  const uint64_t payloadBytes = static_cast<uint64_t>(fileSize) - sizeof(StatesFileHeader);
  if (payloadBytes % sizeof(uint64_t) != 0
      || payloadBytes / sizeof(uint64_t) != hdr.numBoards
      || hdr.numBoards > (std::numeric_limits<size_t>::max() / sizeof(uint64_t))) {
    if (errOut)
      *errOut = "States file size mismatch vs header board count: " + path;
    return false;
  }

  outBoards.resize(static_cast<size_t>(hdr.numBoards));
  if (hdr.numBoards > 0) {
    in.read(reinterpret_cast<char *>(outBoards.data()), static_cast<std::streamsize>(payloadBytes));
    if (!in.good()) {
      if (errOut)
        *errOut = "I/O error reading states file: " + path;
      return false;
    }
  }
  for (size_t i = 0; i < outBoards.size(); ++i) {
    if ((outBoards[i] & ~VALID_BOARD_MASK) != 0) {
      if (errOut)
        *errOut = "Corrupt board (>40 bits) in states file: " + path;
      return false;
    }
    if (i > 0 && outBoards[i] <= outBoards[i - 1]) {
      if (errOut)
        *errOut = "Unsorted or duplicate boards in states file: " + path;
      return false;
    }
  }
  return true;
}

// Writes sorted (board, V(board)) pairs to a compact 12-byte-per-record binary tablebase file.
inline bool writeTablebaseFile(const std::string &path, const std::vector<uint64_t> &sortedBoards,
                               const std::vector<float> &values, double bellmanResidual,
                               std::string *errOut = nullptr) {
  if (sortedBoards.size() != values.size()) {
    if (errOut)
      *errOut = "sortedBoards and values size mismatch";
    return false;
  }
  if (!std::isfinite(bellmanResidual) || bellmanResidual < 0.0) {
    if (errOut)
      *errOut = "bellmanResidual must be finite and non-negative";
    return false;
  }

  double emptyVal = -1.0;
  for (size_t i = 0; i < sortedBoards.size(); ++i) {
    if ((sortedBoards[i] & ~VALID_BOARD_MASK) != 0) {
      if (errOut)
        *errOut = "Board exceeds 40-bit playfield mask";
      return false;
    }
    if (i > 0 && sortedBoards[i] <= sortedBoards[i - 1]) {
      if (errOut)
        *errOut = "Tablebase boards must be strictly sorted ascending";
      return false;
    }
    if (!std::isfinite(values[i])) {
      if (errOut)
        *errOut = "Non-finite value encountered when writing tablebase";
      return false;
    }
    if (sortedBoards[i] == 0ULL) {
      emptyVal = static_cast<double>(values[i]);
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errOut)
      *errOut = "Failed to open tablebase file for writing: " + path;
    return false;
  }

  TablebaseHeader hdr{};
  std::memcpy(hdr.magic, TABLEBASE_MAGIC, sizeof(TABLEBASE_MAGIC));
  hdr.numBoards = static_cast<uint64_t>(sortedBoards.size());
  hdr.emptyBoardValue = emptyVal;
  hdr.bellmanResidual = bellmanResidual;
  out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));

  constexpr size_t CHUNK_SIZE = 4096;
  std::vector<PackedTablebaseRecord> chunk;
  chunk.reserve(std::min(sortedBoards.size(), CHUNK_SIZE));
  for (size_t i = 0; i < sortedBoards.size(); ++i) {
    chunk.push_back(PackedTablebaseRecord::make(sortedBoards[i], values[i]));
    if (chunk.size() == CHUNK_SIZE || i + 1 == sortedBoards.size()) {
      out.write(reinterpret_cast<const char *>(chunk.data()),
                static_cast<std::streamsize>(chunk.size() * sizeof(PackedTablebaseRecord)));
      chunk.clear();
    }
  }

  if (!out.good()) {
    if (errOut)
      *errOut = "I/O error while writing tablebase file: " + path;
    return false;
  }
  return true;
}

// Read-only memory-mapped view over a *.tb.bin file supporting O(log N) binary-search lookups
// with zero heap allocation on POSIX platforms.
class MappedTablebase {
public:
  MappedTablebase() = default;
  MappedTablebase(const MappedTablebase &) = delete;
  MappedTablebase &operator=(const MappedTablebase &) = delete;

  MappedTablebase(MappedTablebase &&other) noexcept { *this = std::move(other); }

  MappedTablebase &operator=(MappedTablebase &&other) noexcept {
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
    }
    return *this;
  }

  ~MappedTablebase() { close(); }

  bool open(const std::string &path, std::string *errOut = nullptr, bool verifySorted = true) {
    MappedTablebase next;
    if (!next.openInternal(path, errOut, verifySorted)) {
      return false;
    }
    *this = std::move(next);
    return true;
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
  uint64_t size() const noexcept { return loaded_ ? header_.numBoards : 0; }
  double emptyBoardValue() const noexcept { return loaded_ ? header_.emptyBoardValue : -1.0; }
  double bellmanResidual() const noexcept { return loaded_ ? header_.bellmanResidual : 0.0; }

  bool lookup(uint64_t board, float &outValue) const noexcept {
    if (!loaded_ || header_.numBoards == 0) {
      return false;
    }
    uint64_t lo = 0;
    uint64_t hi = header_.numBoards;
    while (lo < hi) {
      const uint64_t mid = lo + ((hi - lo) >> 1);
      const uint64_t midBoard = records_[mid].board();
      if (midBoard < board) {
        lo = mid + 1;
      } else if (midBoard > board) {
        hi = mid;
      } else {
        outValue = records_[mid].value();
        return true;
      }
    }
    return false;
  }

private:
  TablebaseHeader header_{};
  const PackedTablebaseRecord *records_ = nullptr;
  bool loaded_ = false;
  std::string path_;

#ifndef _WIN32
  void *mapAddr_ = nullptr;
  size_t mapSize_ = 0;
#else
  std::vector<PackedTablebaseRecord> ownedRecords_;
#endif

  bool validateHeaderAndPayloadSize(uint64_t payloadBytes, std::string *errOut) const {
    if (std::memcmp(header_.magic, TABLEBASE_MAGIC, sizeof(TABLEBASE_MAGIC)) != 0) {
      if (errOut)
        *errOut = "Invalid tablebase magic in: " + path_;
      return false;
    }
    if (payloadBytes % sizeof(PackedTablebaseRecord) != 0
        || payloadBytes / sizeof(PackedTablebaseRecord) != header_.numBoards
        || header_.numBoards
               > (std::numeric_limits<size_t>::max() / sizeof(PackedTablebaseRecord))) {
      if (errOut)
        *errOut = "Tablebase file size mismatch vs header board count: " + path_;
      return false;
    }
    if (!std::isfinite(header_.emptyBoardValue)
        || !std::isfinite(header_.bellmanResidual)
        || header_.bellmanResidual < 0.0) {
      if (errOut)
        *errOut = "Invalid non-finite metadata in tablebase header: " + path_;
      return false;
    }
    return true;
  }

  bool validateRecords(std::string *errOut) const {
    for (uint64_t i = 0; i < header_.numBoards; ++i) {
      const uint64_t b = records_[i].board();
      const float v = records_[i].value();
      if ((b & ~VALID_BOARD_MASK) != 0 || !std::isfinite(v)) {
        if (errOut)
          *errOut = "Corrupt record in tablebase file: " + path_;
        return false;
      }
      if (i > 0 && b <= records_[i - 1].board()) {
        if (errOut)
          *errOut = "Unsorted records in tablebase file: " + path_;
        return false;
      }
    }
    return true;
  }

  bool openInternal(const std::string &path, std::string *errOut, bool verifySorted) {
    path_ = path;
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errOut)
        *errOut = "Failed to open tablebase file: " + path;
      return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(TablebaseHeader))) {
      ::close(fd);
      if (errOut)
        *errOut = "Tablebase file smaller than header: " + path;
      return false;
    }
    const size_t fileBytes = static_cast<size_t>(st.st_size);
    void *addr = ::mmap(nullptr, fileBytes, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (addr == MAP_FAILED) {
      if (errOut)
        *errOut = "mmap failed for tablebase file: " + path;
      return false;
    }
    mapAddr_ = addr;
    mapSize_ = fileBytes;

    std::memcpy(&header_, addr, sizeof(TablebaseHeader));
    const uint64_t payloadBytes = static_cast<uint64_t>(fileBytes) - sizeof(TablebaseHeader);
    if (!validateHeaderAndPayloadSize(payloadBytes, errOut)) {
      close();
      return false;
    }
    records_ = reinterpret_cast<const PackedTablebaseRecord *>(static_cast<const char *>(addr)
                                                               + sizeof(TablebaseHeader));
#else
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
      if (errOut)
        *errOut = "Failed to open tablebase file: " + path;
      return false;
    }
    const std::streamoff fileBytes = in.tellg();
    if (fileBytes < static_cast<std::streamoff>(sizeof(TablebaseHeader))) {
      if (errOut)
        *errOut = "Tablebase file smaller than header: " + path;
      return false;
    }
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char *>(&header_), sizeof(header_));
    if (!in.good()) {
      if (errOut)
        *errOut = "I/O error reading tablebase header: " + path;
      return false;
    }
    const uint64_t payloadBytes = static_cast<uint64_t>(fileBytes) - sizeof(TablebaseHeader);
    if (!validateHeaderAndPayloadSize(payloadBytes, errOut)) {
      close();
      return false;
    }
    ownedRecords_.resize(static_cast<size_t>(header_.numBoards));
    if (header_.numBoards > 0) {
      in.read(reinterpret_cast<char *>(ownedRecords_.data()),
              static_cast<std::streamsize>(payloadBytes));
      if (!in.good()) {
        close();
        if (errOut)
          *errOut = "I/O error reading tablebase file: " + path;
        return false;
      }
    }
    records_ = ownedRecords_.data();
#endif

    if (verifySorted && !validateRecords(errOut)) {
      close();
      return false;
    }
    loaded_ = true;
    return true;
  }
};

} // namespace tb

#endif // TABLEBASE_IO_HPP
