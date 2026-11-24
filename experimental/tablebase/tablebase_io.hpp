#ifndef TABLEBASE_IO_HPP
#define TABLEBASE_IO_HPP

#include "core.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
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
inline constexpr char NEXTBOX_TABLEBASE_MAGIC[8] = {'S', 'R', 'T', 'B', 'V', '0', '0', '2'};
inline constexpr double MISSING_BOARD_VALUE = -1.0;

enum class TablebaseFormat {
  Unknown,
  NnbV1,
  NextBoxV2,
};

struct StatesFileHeader {
  char magic[8];
  uint64_t numBoards;
};
static_assert(sizeof(StatesFileHeader) == 16, "StatesFileHeader must be 16 bytes");

struct TablebaseHeader {
  char magic[8];
  uint64_t numBoards;
  double emptyBoardValue; // V(0ULL), or MISSING_BOARD_VALUE when board 0ULL is absent
  double bellmanResidual;
};
static_assert(sizeof(TablebaseHeader) == 32, "TablebaseHeader must be 32 bytes");

struct NextBoxTablebaseHeader {
  char magic[8];
  uint64_t numBoards;
  uint32_t rngMode;       // 0 = Uniform 1/7, 1 = Exact 7x7 NES
  uint32_t flags;         // Reserved (must be 0)
  double emptyBoardValue; // Expected V(0, c_0) under rngMode, or MISSING_BOARD_VALUE if 0ULL absent
  double bellmanResidual;
};
static_assert(sizeof(NextBoxTablebaseHeader) == 40, "NextBoxTablebaseHeader must be 40 bytes");

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

  bool hasValidValues() const noexcept { return std::isfinite(value()); }

  static PackedTablebaseRecord make(uint64_t b, float v) noexcept {
    PackedTablebaseRecord rec{};
    std::memcpy(rec.boardBytes, &b, sizeof(b));
    std::memcpy(rec.valueBytes, &v, sizeof(v));
    return rec;
  }
};
static_assert(sizeof(PackedTablebaseRecord) == 12, "PackedTablebaseRecord must be 12 bytes");

// Alignment-safe 36-byte on-disk record for 1-piece Next-Box tablebases (8-byte board + 7 floats).
struct PackedNextBoxRecord {
  uint8_t boardBytes[8];
  uint8_t valueBytes[NUM_PIECES * sizeof(float)];

  uint64_t board() const noexcept {
    uint64_t b = 0;
    std::memcpy(&b, boardBytes, sizeof(b));
    return b;
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

  static PackedNextBoxRecord make(uint64_t b, const float vals[NUM_PIECES]) noexcept {
    PackedNextBoxRecord rec{};
    std::memcpy(rec.boardBytes, &b, sizeof(b));
    std::memcpy(rec.valueBytes, vals, sizeof(rec.valueBytes));
    return rec;
  }
};
static_assert(sizeof(PackedNextBoxRecord) == 36, "PackedNextBoxRecord must be 36 bytes");

// Detects whether a file begins with SRTBV001 (No-Next-Box) or SRTBV002 (Next-Box) magic bytes.
inline bool detectTablebaseFormat(const std::string &path, TablebaseFormat &outFormat,
                                  std::string *errOut = nullptr) {
  outFormat = TablebaseFormat::Unknown;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (errOut)
      *errOut = "Failed to open tablebase file: " + path;
    return false;
  }
  char magic[8] = {};
  in.read(magic, sizeof(magic));
  if (!in.good()) {
    if (errOut)
      *errOut = "Tablebase file smaller than 8-byte magic header: " + path;
    return false;
  }
  if (std::memcmp(magic, TABLEBASE_MAGIC, sizeof(magic)) == 0) {
    outFormat = TablebaseFormat::NnbV1;
    return true;
  }
  if (std::memcmp(magic, NEXTBOX_TABLEBASE_MAGIC, sizeof(magic)) == 0) {
    outFormat = TablebaseFormat::NextBoxV2;
    return true;
  }
  if (errOut)
    *errOut = "Unrecognized tablebase magic header in: " + path;
  return false;
}

namespace detail {

constexpr bool isValidRngMode(uint32_t rawMode) noexcept {
  return rawMode == static_cast<uint32_t>(RngMode::Uniform)
         || rawMode == static_cast<uint32_t>(RngMode::Nes);
}

inline bool validateSortedBoards(const std::vector<uint64_t> &sortedBoards, std::string *errOut) {
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
  return true;
}

template <typename Record, typename MakeRecordFn>
inline void writeChunkedRecords(std::ofstream &out, size_t count, MakeRecordFn &&makeRecord) {
  constexpr size_t CHUNK_SIZE = 4096;
  std::vector<Record> chunk;
  chunk.reserve(std::min(count, CHUNK_SIZE));
  for (size_t i = 0; i < count; ++i) {
    chunk.push_back(makeRecord(i));
    if (chunk.size() == CHUNK_SIZE || i + 1 == count) {
      out.write(reinterpret_cast<const char *>(chunk.data()),
                static_cast<std::streamsize>(chunk.size() * sizeof(Record)));
      chunk.clear();
    }
  }
}

// Read-only memory-mapped (POSIX) or owned-buffer (Windows) sorted record table backing both
// MappedTablebase (SRTBV001) and MappedNextBoxTablebase (SRTBV002).
template <typename Header, typename Record> class MappedRecordFile {
public:
  static constexpr uint64_t NOT_FOUND = ~0ULL;

  MappedRecordFile() = default;
  MappedRecordFile(const MappedRecordFile &) = delete;
  MappedRecordFile &operator=(const MappedRecordFile &) = delete;

  MappedRecordFile(MappedRecordFile &&other) noexcept { *this = std::move(other); }

  MappedRecordFile &operator=(MappedRecordFile &&other) noexcept {
    if (this != &other) {
      close();
      header_ = other.header_;
      records_ = other.records_;
      loaded_ = other.loaded_;
      path_ = std::move(other.path_);
      other.path_.clear();
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

  ~MappedRecordFile() { close(); }

  template <typename ExtraHeaderValidator>
  bool open(const std::string &path, const char (&expectedMagic)[8], const char *label,
            ExtraHeaderValidator &&validateExtraHeader, std::string *errOut, bool verifySorted) {
    MappedRecordFile next;
    if (!next.openInternal(path, expectedMagic, label, validateExtraHeader, errOut, verifySorted)) {
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
  const Header &header() const noexcept { return header_; }
  uint64_t size() const noexcept { return loaded_ ? header_.numBoards : 0; }

  const Record &recordAt(uint64_t index) const noexcept {
    assert(loaded_ && index < header_.numBoards);
    return records_[index];
  }

  uint64_t findIndex(uint64_t board) const noexcept {
    if (!loaded_ || header_.numBoards == 0) {
      return NOT_FOUND;
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
        return mid;
      }
    }
    return NOT_FOUND;
  }

private:
  Header header_{};
  const Record *records_ = nullptr;
  bool loaded_ = false;
  std::string path_;

#ifndef _WIN32
  void *mapAddr_ = nullptr;
  size_t mapSize_ = 0;
#else
  std::vector<Record> ownedRecords_;
#endif

  template <typename ExtraHeaderValidator>
  bool validateHeaderAndPayload(const char (&expectedMagic)[8], const char *label,
                                uint64_t payloadBytes, ExtraHeaderValidator &&validateExtraHeader,
                                std::string *errOut) const {
    if (std::memcmp(header_.magic, expectedMagic, sizeof(expectedMagic)) != 0) {
      if (errOut)
        *errOut = std::string("Invalid ") + label + " magic in: " + path_;
      return false;
    }
    if (payloadBytes % sizeof(Record) != 0
        || payloadBytes / sizeof(Record) != header_.numBoards
        || header_.numBoards > (std::numeric_limits<size_t>::max() / sizeof(Record))) {
      if (errOut)
        *errOut = std::string(label) + " file size mismatch vs header board count: " + path_;
      return false;
    }
    if (!std::isfinite(header_.emptyBoardValue)
        || !std::isfinite(header_.bellmanResidual)
        || header_.bellmanResidual < 0.0) {
      if (errOut)
        *errOut = std::string("Invalid non-finite metadata in ") + label + " header: " + path_;
      return false;
    }
    return validateExtraHeader(header_, path_, errOut);
  }

  bool validateRecords(const char *label, std::string *errOut) const {
    for (uint64_t i = 0; i < header_.numBoards; ++i) {
      const uint64_t b = records_[i].board();
      if ((b & ~VALID_BOARD_MASK) != 0 || !records_[i].hasValidValues()) {
        if (errOut)
          *errOut = std::string("Corrupt record in ") + label + " file: " + path_;
        return false;
      }
      if (i > 0 && b <= records_[i - 1].board()) {
        if (errOut)
          *errOut = std::string("Unsorted records in ") + label + " file: " + path_;
        return false;
      }
    }
    return true;
  }

  template <typename ExtraHeaderValidator>
  bool openInternal(const std::string &path, const char (&expectedMagic)[8], const char *label,
                    ExtraHeaderValidator &&validateExtraHeader, std::string *errOut,
                    bool verifySorted) {
    path_ = path;
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errOut)
        *errOut = std::string("Failed to open ") + label + " file: " + path;
      return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(Header))) {
      ::close(fd);
      if (errOut)
        *errOut = std::string(label) + " file smaller than header: " + path;
      return false;
    }
    if (static_cast<uint64_t>(st.st_size) > std::numeric_limits<size_t>::max()) {
      ::close(fd);
      if (errOut)
        *errOut = std::string(label) + " file too large to mmap: " + path;
      return false;
    }
    const size_t fileBytes = static_cast<size_t>(st.st_size);
    void *addr = ::mmap(nullptr, fileBytes, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (addr == MAP_FAILED) {
      if (errOut)
        *errOut = std::string("mmap failed for ") + label + " file: " + path;
      return false;
    }
    mapAddr_ = addr;
    mapSize_ = fileBytes;

    std::memcpy(&header_, addr, sizeof(Header));
    const uint64_t payloadBytes = static_cast<uint64_t>(fileBytes) - sizeof(Header);
    if (!validateHeaderAndPayload(expectedMagic, label, payloadBytes, validateExtraHeader,
                                  errOut)) {
      close();
      return false;
    }
    records_ = reinterpret_cast<const Record *>(static_cast<const char *>(addr) + sizeof(Header));
#else
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
      if (errOut)
        *errOut = std::string("Failed to open ") + label + " file: " + path;
      return false;
    }
    const std::streamoff fileBytes = in.tellg();
    if (fileBytes < static_cast<std::streamoff>(sizeof(Header))) {
      if (errOut)
        *errOut = std::string(label) + " file smaller than header: " + path;
      return false;
    }
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char *>(&header_), sizeof(header_));
    if (!in.good()) {
      if (errOut)
        *errOut = std::string("I/O error reading ") + label + " header: " + path;
      return false;
    }
    const uint64_t payloadBytes = static_cast<uint64_t>(fileBytes) - sizeof(Header);
    if (!validateHeaderAndPayload(expectedMagic, label, payloadBytes, validateExtraHeader,
                                  errOut)) {
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
          *errOut = std::string("I/O error reading ") + label + " file: " + path;
        return false;
      }
    }
    records_ = ownedRecords_.data();
#endif

    if (verifySorted && !validateRecords(label, errOut)) {
      close();
      return false;
    }
    loaded_ = true;
    return true;
  }
};

} // namespace detail

// Writes a strictly ascending sorted list of canonical 40-bit boards to disk.
inline bool writeStatesFile(const std::string &path, const std::vector<uint64_t> &sortedBoards,
                            std::string *errOut = nullptr) {
  if (!detail::validateSortedBoards(sortedBoards, errOut)) {
    return false;
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
  return detail::validateSortedBoards(outBoards, errOut);
}

// Writes sorted (board, V(board)) pairs to a compact 12-byte-per-record SRTBV001 tablebase file.
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
  if (!detail::validateSortedBoards(sortedBoards, errOut)) {
    return false;
  }

  double emptyVal = MISSING_BOARD_VALUE;
  for (size_t i = 0; i < sortedBoards.size(); ++i) {
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

  detail::writeChunkedRecords<PackedTablebaseRecord>(out, sortedBoards.size(), [&](size_t i) {
    return PackedTablebaseRecord::make(sortedBoards[i], values[i]);
  });

  if (!out.good()) {
    if (errOut)
      *errOut = "I/O error while writing tablebase file: " + path;
    return false;
  }
  return true;
}

// Read-only memory-mapped view over a 12-byte-per-record SRTBV001 tablebase file supporting
// O(log N) binary-search lookups with zero heap allocation on POSIX platforms.
class MappedTablebase {
public:
  bool open(const std::string &path, std::string *errOut = nullptr, bool verifySorted = true) {
    return file_.open(
        path, TABLEBASE_MAGIC, "tablebase",
        [](const TablebaseHeader &, const std::string &, std::string *) { return true; }, errOut,
        verifySorted);
  }

  void close() noexcept { file_.close(); }
  bool isLoaded() const noexcept { return file_.isLoaded(); }
  const std::string &path() const noexcept { return file_.path(); }
  uint64_t size() const noexcept { return file_.size(); }
  double emptyBoardValue() const noexcept {
    return isLoaded() ? file_.header().emptyBoardValue : MISSING_BOARD_VALUE;
  }
  double bellmanResidual() const noexcept {
    return isLoaded() ? file_.header().bellmanResidual : 0.0;
  }

  uint64_t boardAt(uint64_t index) const noexcept { return file_.recordAt(index).board(); }
  float valueAt(uint64_t index) const noexcept { return file_.recordAt(index).value(); }

  bool lookup(uint64_t board, float &outValue) const noexcept {
    const uint64_t idx = file_.findIndex(board);
    if (idx == File::NOT_FOUND) {
      return false;
    }
    outValue = file_.recordAt(idx).value();
    return true;
  }

private:
  using File = detail::MappedRecordFile<TablebaseHeader, PackedTablebaseRecord>;
  File file_;
};

// Writes sorted (board, V(board, 0..6)) pairs to a compact 36-byte-per-record SRTBV002 tablebase.
// values must have size sortedBoards.size() * NUM_PIECES in I, O, L, J, T, S, Z order and contain
// only finite non-negative expected line counts.
inline bool writeNextBoxTablebaseFile(const std::string &path,
                                      const std::vector<uint64_t> &sortedBoards,
                                      const std::vector<float> &values, RngMode rngMode,
                                      double bellmanResidual, std::string *errOut = nullptr) {
  if (sortedBoards.size() > std::numeric_limits<size_t>::max() / static_cast<size_t>(NUM_PIECES)
      || values.size() != sortedBoards.size() * static_cast<size_t>(NUM_PIECES)) {
    if (errOut)
      *errOut = "sortedBoards and values size mismatch (expected N * 7 floats)";
    return false;
  }
  if (!detail::isValidRngMode(static_cast<uint32_t>(rngMode))) {
    if (errOut)
      *errOut = "Unsupported RngMode when writing Next-Box tablebase";
    return false;
  }
  if (!std::isfinite(bellmanResidual) || bellmanResidual < 0.0) {
    if (errOut)
      *errOut = "bellmanResidual must be finite and non-negative";
    return false;
  }
  if (!detail::validateSortedBoards(sortedBoards, errOut)) {
    return false;
  }

  double emptyVal = MISSING_BOARD_VALUE;
  for (size_t i = 0; i < sortedBoards.size(); ++i) {
    const float *rowVals = &values[i * static_cast<size_t>(NUM_PIECES)];
    for (int c = 0; c < NUM_PIECES; ++c) {
      if (!std::isfinite(rowVals[c]) || rowVals[c] < 0.0f) {
        if (errOut)
          *errOut = "Non-finite or negative value encountered when writing Next-Box tablebase";
        return false;
      }
    }
    if (sortedBoards[i] == 0ULL) {
      double ev = 0.0;
      for (int c = 0; c < NUM_PIECES; ++c) {
        ev += openingPieceProb(rngMode, c) * static_cast<double>(rowVals[c]);
      }
      emptyVal = ev;
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errOut)
      *errOut = "Failed to open Next-Box tablebase file for writing: " + path;
    return false;
  }

  NextBoxTablebaseHeader hdr{};
  std::memcpy(hdr.magic, NEXTBOX_TABLEBASE_MAGIC, sizeof(NEXTBOX_TABLEBASE_MAGIC));
  hdr.numBoards = static_cast<uint64_t>(sortedBoards.size());
  hdr.rngMode = static_cast<uint32_t>(rngMode);
  hdr.flags = 0U;
  hdr.emptyBoardValue = emptyVal;
  hdr.bellmanResidual = bellmanResidual;
  out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));

  detail::writeChunkedRecords<PackedNextBoxRecord>(out, sortedBoards.size(), [&](size_t i) {
    return PackedNextBoxRecord::make(sortedBoards[i], &values[i * static_cast<size_t>(NUM_PIECES)]);
  });

  if (!out.good()) {
    if (errOut)
      *errOut = "I/O error while writing Next-Box tablebase file: " + path;
    return false;
  }
  return true;
}

// Read-only memory-mapped view over a 36-byte-per-record SRTBV002 Next-Box tablebase file
// supporting O(log N) binary-search lookups with zero heap allocation on POSIX platforms.
class MappedNextBoxTablebase {
public:
  bool open(const std::string &path, std::string *errOut = nullptr, bool verifySorted = true) {
    return file_.open(
        path, NEXTBOX_TABLEBASE_MAGIC, "Next-Box tablebase",
        [](const NextBoxTablebaseHeader &hdr, const std::string &p, std::string *err) {
          if (!detail::isValidRngMode(hdr.rngMode)) {
            if (err)
              *err = "Unsupported rngMode in Next-Box tablebase header: " + p;
            return false;
          }
          if (hdr.flags != 0U) {
            if (err)
              *err = "Unsupported non-zero flags in Next-Box tablebase header: " + p;
            return false;
          }
          if (hdr.emptyBoardValue < 0.0 && hdr.emptyBoardValue != MISSING_BOARD_VALUE) {
            if (err)
              *err = "Invalid negative emptyBoardValue in Next-Box tablebase header: " + p;
            return false;
          }
          return true;
        },
        errOut, verifySorted);
  }

  void close() noexcept { file_.close(); }
  bool isLoaded() const noexcept { return file_.isLoaded(); }
  const std::string &path() const noexcept { return file_.path(); }
  uint64_t size() const noexcept { return file_.size(); }
  RngMode rngMode() const noexcept {
    return isLoaded() ? static_cast<RngMode>(file_.header().rngMode) : RngMode::Uniform;
  }
  double emptyBoardValue() const noexcept {
    return isLoaded() ? file_.header().emptyBoardValue : MISSING_BOARD_VALUE;
  }
  double bellmanResidual() const noexcept {
    return isLoaded() ? file_.header().bellmanResidual : 0.0;
  }

  uint64_t boardAt(uint64_t index) const noexcept { return file_.recordAt(index).board(); }
  float valueAt(uint64_t index, int pieceIdx) const noexcept {
    return file_.recordAt(index).value(pieceIdx);
  }
  void copyValuesAt(uint64_t index, float out[NUM_PIECES]) const noexcept {
    file_.recordAt(index).copyValues(out);
  }

  bool lookup(uint64_t board, float outValues[NUM_PIECES]) const noexcept {
    const uint64_t idx = file_.findIndex(board);
    if (idx == File::NOT_FOUND) {
      return false;
    }
    file_.recordAt(idx).copyValues(outValues);
    return true;
  }

  bool lookupPiece(uint64_t board, int pieceIdx, float &outValue) const noexcept {
    assert(pieceIdx >= 0 && pieceIdx < NUM_PIECES);
    const uint64_t idx = file_.findIndex(board);
    if (idx == File::NOT_FOUND) {
      return false;
    }
    outValue = file_.recordAt(idx).value(pieceIdx);
    return true;
  }

private:
  using File = detail::MappedRecordFile<NextBoxTablebaseHeader, PackedNextBoxRecord>;
  File file_;
};

} // namespace tb

#endif // TABLEBASE_IO_HPP
