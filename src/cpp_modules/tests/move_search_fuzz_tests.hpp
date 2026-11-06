// Move search fuzz harness (see TESTING.md, "TS oracle fuzz fixtures").
//
// Consumes the generated golden fixtures (move_search_fixtures.hpp, produced by
// `npm run fuzz_generate` from the TS implementation) and checks the C++ move
// search against them.
//
//   - Tap-mode fixtures (dasCharge == -1): hard-asserted. This is both the
//     TS<->C++ equivalence milestone and the non-regression guard for the
//     future DAS port.
//   - DAS-mode fixtures (dasCharge >= 0): reported, not asserted, until the
//     C++ DAS search exists. Flip SR_FUZZ_ASSERT_DAS to 1 when it lands.
//
// Comparison surface: the multiset of
//   (rotationIndex, x, y, dasChargeAfter, inputCostX10)
// where inputCostX10 comes from tuckInput (E/F/I/G -> -3, A/B -> -2, L/R -> -1,
// '.' -> 0). Cell mismatches are hard failures; tuck-input-choice mismatches
// (same cell, different cost) are expected to be possible and are reported.

#ifndef SR_MOVE_SEARCH_FUZZ_TESTS
#define SR_MOVE_SEARCH_FUZZ_TESTS

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "move_search_fixtures.hpp"

// Flip to 1 once the C++ DAS move search lands.
#ifndef SR_FUZZ_ASSERT_DAS
#define SR_FUZZ_ASSERT_DAS 1
#endif

struct FuzzEntry {
  int rot;
  int x;
  int y;
  int dasChargeAfter;
  int inputCostX10;

  bool operator<(const FuzzEntry &o) const {
    if (rot != o.rot)
      return rot < o.rot;
    if (x != o.x)
      return x < o.x;
    if (y != o.y)
      return y < o.y;
    if (dasChargeAfter != o.dasChargeAfter)
      return dasChargeAfter < o.dasChargeAfter;
    return inputCostX10 < o.inputCostX10;
  }
};

struct FuzzCell {
  int rot;
  int x;
  int y;

  bool operator<(const FuzzCell &o) const {
    if (rot != o.rot)
      return rot < o.rot;
    if (x != o.x)
      return x < o.x;
    return y < o.y;
  }
  bool operator==(const FuzzCell &o) const { return rot == o.rot && x == o.x && y == o.y; }
};

static int tuckCostX10(char tuckInput) {
  switch (tuckInput) {
  case 'E':
  case 'F':
  case 'I':
  case 'G':
    return -3;
  case 'A':
  case 'B':
    return -2;
  case 'L':
  case 'R':
    return -1;
  default:
    return 0; // NO_TUCK_NOTATION -> basic placement
  }
}

// Unpacks one expected entry from FIXTURE_ENTRY_POOL (bit layout documented in
// the generator's packEntry; 18 bits: rot | (x+8)<<2 | (y+2)<<6 |
// (dasChargeAfter+1)<<11 | (inputCostX10+3)<<16).
static FuzzEntry unpackEntry(int p) {
  FuzzEntry e;
  e.rot = p & 3;
  e.x = ((p >> 2) & 15) - 8;
  e.y = ((p >> 6) & 31) - 2;
  e.dasChargeAfter = ((p >> 11) & 31) - 1;
  e.inputCostX10 = ((p >> 16) & 3) - 3;
  return e;
}

static std::vector<FuzzEntry> expectedEntries(const MoveSearchFixture &f) {
  std::vector<FuzzEntry> out;
  out.reserve(f.entryCount);
  for (int i = 0; i < f.entryCount; i++) {
    out.push_back(unpackEntry(FIXTURE_ENTRY_POOL[f.entryOffset + i]));
  }
  return out; // generated pre-sorted
}

static std::vector<FuzzEntry> runCppMoveSearch(const MoveSearchFixture &f) {
  // Mirror main.cpp's GameState preparation exactly.
  GameState gameState = {};
  gameState.level = f.level;
  gameState.lines = 0;
  encodeBoard(FIXTURE_BOARD_POOL[f.boardIdx], gameState.board);
  getSurfaceArray(gameState.board, gameState.surfaceArray);
  updateSurfaceAndHoles(gameState.surfaceArray, gameState.board, /*excludeHolesColumn=*/9,
                        /*isDigMode=*/false);

  const Piece *piece = &PIECE_LIST[f.pieceIndex];
  std::vector<FuzzEntry> out;
  if (f.dasCharge == -1) {
    // Tap mode: the legacy walk-based search (untouched by the DAS port)
    std::vector<LockPlacement> lockPlacements;
    moveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], lockPlacements);
    out.reserve(lockPlacements.size());
    for (const LockPlacement &lp : lockPlacements) {
      out.push_back(
          {lp.rotationIndex, lp.x, lp.y, /*dasChargeAfter=*/-1, tuckCostX10(lp.tuckInput)});
    }
  } else {
    // DAS mode: the per-goal pipeline (returns DasPossibility with charge/cost directly)
    std::vector<DasPossibility> results;
    dasMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                  /*trackInputSequences=*/false, results);
    out.reserve(results.size());
    for (const DasPossibility &p : results) {
      out.push_back({p.rotationIndex, p.x, p.y, p.dasChargeAfter, p.inputCostX10});
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

static std::string fuzzEntryStr(const FuzzEntry &e) {
  std::ostringstream ss;
  ss
      << "rot="
      << e.rot
      << " x="
      << e.x
      << " y="
      << e.y
      << " charge="
      << e.dasChargeAfter
      << " cost="
      << e.inputCostX10;
  return ss.str();
}

// Two-pointer diff over sorted entry lists; returns a human-readable report
// (empty string if identical). Details are capped at `limit` per category.
static std::string diffEntries(const std::vector<FuzzEntry> &expected,
                               const std::vector<FuzzEntry> &actual, int limit) {
  const size_t cap = (size_t)limit;
  std::vector<std::string> missing, extra;
  size_t i = 0, j = 0;
  while (i < expected.size() || j < actual.size()) {
    if (j >= actual.size() || (i < expected.size() && expected[i] < actual[j])) {
      if (missing.size() < cap)
        missing.push_back("  expected-only: " + fuzzEntryStr(expected[i]));
      i++;
    } else if (i >= expected.size() || actual[j] < expected[i]) {
      if (extra.size() < cap)
        extra.push_back("  cpp-only:       " + fuzzEntryStr(actual[j]));
      j++;
    } else {
      i++;
      j++;
    }
  }

  if (missing.empty() && extra.empty())
    return "";

  std::ostringstream ss;
  ss
      << "    "
      << missing.size()
      << (missing.size() == cap ? "+" : "")
      << " expected-only, "
      << extra.size()
      << (extra.size() == cap ? "+" : "")
      << " cpp-only";
  for (const std::string &m : missing)
    ss << "\n" << m;
  for (const std::string &e : extra)
    ss << "\n" << e;
  return ss.str();
}

// Strips charge/cost, keeping only lock cells (multiset semantics preserved:
// a cell may legitimately appear twice -- basic + tuck).
static std::vector<FuzzCell> toCells(const std::vector<FuzzEntry> &entries) {
  std::vector<FuzzCell> out;
  out.reserve(entries.size());
  for (const FuzzEntry &e : entries)
    out.push_back({e.rot, e.x, e.y});
  std::sort(out.begin(), out.end());
  return out;
}

static std::string diffCells(const std::vector<FuzzCell> &expected,
                             const std::vector<FuzzCell> &actual, int limit) {
  const size_t cap = (size_t)limit;
  std::vector<std::string> missing, extra;
  size_t i = 0, j = 0;
  while (i < expected.size() || j < actual.size()) {
    auto cellStr = [](const FuzzCell &c) {
      std::ostringstream ss;
      ss << "rot=" << c.rot << " x=" << c.x << " y=" << c.y;
      return ss.str();
    };
    if (j >= actual.size() || (i < expected.size() && expected[i] < actual[j])) {
      if (missing.size() < cap)
        missing.push_back("  expected-only: " + cellStr(expected[i]));
      i++;
    } else if (i >= expected.size() || actual[j] < expected[i]) {
      if (extra.size() < cap)
        extra.push_back("  cpp-only:       " + cellStr(actual[j]));
      j++;
    } else {
      i++;
      j++;
    }
  }

  if (missing.empty() && extra.empty())
    return "";

  std::ostringstream ss;
  ss
      << "    "
      << missing.size()
      << (missing.size() == cap ? "+" : "")
      << " expected-only, "
      << extra.size()
      << (extra.size() == cap ? "+" : "")
      << " cpp-only";
  for (const std::string &m : missing)
    ss << "\n" << m;
  for (const std::string &e : extra)
    ss << "\n" << e;
  return ss.str();
}

static std::string describeFixture(int index, const MoveSearchFixture &f) {
  std::ostringstream ss;
  ss
      << "fixture #"
      << index
      << " piece="
      << PIECE_LIST[f.pieceIndex].id
      << " level="
      << f.level
      << " timeline="
      << FIXTURE_TIMELINE_POOL[f.timelineIdx]
      << " dasCharge="
      << f.dasCharge;
  return ss.str();
}

TEST_CASE("Move search fuzz: tap-mode fixtures match the TS golden") {
  int checked = 0;
  int failures = 0;
  std::ostringstream report;
  for (int i = 0; i < MOVE_SEARCH_FIXTURE_COUNT; i++) {
    const MoveSearchFixture &f = MOVE_SEARCH_FIXTURES[i];
    if (f.dasCharge != -1)
      continue;
    checked++;

    std::vector<FuzzEntry> actual = runCppMoveSearch(f);
    std::vector<FuzzEntry> expected = expectedEntries(f);
    std::string diff = diffEntries(expected, actual, /*limit=*/4);
    if (!diff.empty()) {
      failures++;
      if (failures <= 200) {
        report
            << "  MISMATCH "
            << describeFixture(i, f)
            << " (expected "
            << expected.size()
            << " entries, got "
            << actual.size()
            << ")\n"
            << diff
            << "\n";
      }
    }
  }

  MESSAGE("Tap-mode fuzz: " << checked << " fixtures checked, " << failures << " mismatched");
  REQUIRE_MESSAGE(failures == 0, "Tap-mode fuzz: "
                                     << failures
                                     << "/"
                                     << checked
                                     << " fixtures mismatched\n"
                                     << report.str());
}

TEST_CASE("Move search fuzz: DAS fixtures (pending C++ DAS support)") {
  int dasCount = 0;
  int cellMatches = 0;
  std::ostringstream report;
#if SR_FUZZ_ASSERT_DAS
  int failures = 0;
#endif
  for (int i = 0; i < MOVE_SEARCH_FIXTURE_COUNT; i++) {
    const MoveSearchFixture &f = MOVE_SEARCH_FIXTURES[i];
    if (f.dasCharge == -1)
      continue;
    dasCount++;

    std::vector<FuzzEntry> actual = runCppMoveSearch(f);
    std::vector<FuzzEntry> expected = expectedEntries(f);
#if SR_FUZZ_ASSERT_DAS
    std::string diff = diffEntries(expected, actual, /*limit=*/4);
    if (!diff.empty()) {
      failures++;
      if (failures <= 200) {
        report
            << "  MISMATCH "
            << describeFixture(i, f)
            << " (expected "
            << expected.size()
            << " entries, got "
            << actual.size()
            << ")\n"
            << diff
            << "\n";
      }
    }
#else
    // Informational: how close is the current tap-only C++ search to the TS
    // DAS golden when charge is ignored entirely? Quantifies the gap that the
    // DAS port needs to close (and what getDasEquivalentInputTimeline papers over).
    if (toCells(expected) == toCells(actual)) {
      cellMatches++;
    } else if (report.str().empty()) {
      report
          << "  first diff at "
          << describeFixture(i, f)
          << "\n"
          << diffCells(toCells(expected), toCells(actual), 3)
          << "\n";
    }
#endif
  }

  MESSAGE("DAS-mode fuzz: "
          << dasCount
          << " fixtures"
#if SR_FUZZ_ASSERT_DAS
          << " asserted (full rot/x/y/charge/cost match)"
#else
          << ", "
          << cellMatches
          << " match on cells alone (charge ignored)"
#endif
  );
#if SR_FUZZ_ASSERT_DAS
  REQUIRE_MESSAGE(failures == 0, "DAS-mode fuzz: "
                                     << failures
                                     << "/"
                                     << dasCount
                                     << " fixtures mismatched\n"
                                     << report.str());
#else
  REQUIRE(dasCount > 0);
#endif
}

#endif // SR_MOVE_SEARCH_FUZZ_TESTS
