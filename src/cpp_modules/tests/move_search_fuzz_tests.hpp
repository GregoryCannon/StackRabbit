// Move search fuzz harness (see TESTING.md, "TS oracle fuzz fixtures").
//
// Consumes the generated golden fixtures (move_search_fixtures.hpp, produced
// by `npm run fuzz_generate`) and checks the C++ move search against them.
//
//   - Tap-mode fixtures (dasCharge == -1): hard-asserted twice -- through
//     fastMoveSearch (the hot-path legacy search; fixtures containing spin
//     entries are skipped there, see the test case) and through fullMoveSearch's
//     ARR-gated tap path.
//   - DAS-mode fixtures (dasCharge >= 0): asserted through fullMoveSearch
//     (flip SR_FUZZ_ASSERT_DAS to 0 to downgrade to informational).
//   - Adjustment fixtures (ADJUSTMENT_FIXTURES): mid-air entry states run
//     through fullMoveSearch with a FullMoveSearchContext.
//   - Reaction-time fixtures (REACTION_TIME_FIXTURES): spawn searches run
//     with a reaction time; entries AND their adjTimeSimState snapshot slots
//     are compared order-exactly (result order, no sorting).
//
// Comparison surface: every fullMoveSearch-based section (tap, DAS,
// adjustment) is compared ORDER-EXACTLY against the golden: element i of the
// golden list must equal element i of the search output. The golden lists
// record the reference search's result order (basic entries then tuck/spin
// entries, post-dedupe) -- the order is load-bearing downstream
// (possibleMoves[0] is the default placement; the phantom sort is stable over
// this order), so an order divergence is a real bug, not noise. Entry fields:
// (rotationIndex, x, y, dasChargeAfter, inputCostX10), where inputCostX10
// comes from tuckInput (E/F/I/G -> -3, A/B -> -2, L/R -> -1, '.' -> 0). The
// fastMoveSearch guard alone stays multiset: it exercises a different search
// with different tuck semantics, so only the placement set is meaningful
// there. Input sequences are verified character-exactly by `npm run
// fuzz_verify_sequences`.

#ifndef SR_MOVE_SEARCH_FUZZ_TESTS
#define SR_MOVE_SEARCH_FUZZ_TESTS

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "move_search_fixtures.hpp"
#include "../src/full_move_search.hpp"

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

  bool operator==(const FuzzEntry &o) const {
    return rot == o.rot && x == o.x && y == o.y && dasChargeAfter == o.dasChargeAfter &&
           inputCostX10 == o.inputCostX10;
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
  return out; // generated in result order
}

// Adjustment fixtures pack their entries with the same bit layout, but in
// ADJUSTMENT_ENTRY_POOL.
static std::vector<FuzzEntry> expectedAdjustmentEntries(const AdjustmentSearchFixture &f) {
  std::vector<FuzzEntry> out;
  out.reserve(f.entryCount);
  for (int i = 0; i < f.entryCount; i++) {
    out.push_back(unpackEntry(ADJUSTMENT_ENTRY_POOL[f.entryOffset + i]));
  }
  return out; // generated in result order
}

static std::vector<FuzzEntry> runCppMoveSearch(const MoveSearchFixture &f,
                                               bool forceFullSearch = false) {
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
  if (f.dasCharge == -1 && !forceFullSearch) {
    // Tap mode: the legacy walk-based search (untouched by the DAS port)
    std::vector<LockPlacement> lockPlacements;
    fastMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], lockPlacements);
    out.reserve(lockPlacements.size());
    for (const LockPlacement &lp : lockPlacements) {
      out.push_back(
          {lp.rotationIndex, lp.x, lp.y, /*dasChargeAfter=*/-1, tuckCostX10(lp.tuckInput)});
    }
  } else {
    // fullMoveSearch: DAS mode, or tap mode via its ARR-gated tap path
    // (dasCharge == -1; results carry dasChargeAfter = -1).
    std::vector<FullMovePossibility> results;
    fullMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                   /*trackInputSequences=*/false, results);
    out.reserve(results.size());
    for (const FullMovePossibility &p : results) {
      out.push_back({p.rotationIndex, p.x, p.y, p.dasChargeAfter, p.inputCostX10});
    }
  }
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

// Element-wise order-exact diff: compares entry i to entry i. Returns a
// human-readable report (empty string if identical), showing the first
// mismatching index with its neighborhood.
static std::string diffEntriesInOrder(const std::vector<FuzzEntry> &expected,
                                      const std::vector<FuzzEntry> &actual,
                                      int context) {
  if (expected == actual) {
    return "";
  }
  std::ostringstream ss;
  size_t n = std::max(expected.size(), actual.size());
  for (size_t i = 0; i < n; i++) {
    bool eOk = i < expected.size();
    bool aOk = i < actual.size();
    if (eOk && aOk && expected[i] == actual[i]) {
      continue;
    }
    ss << "    first order mismatch at index " << i << " (expected "
       << expected.size() << " entries, actual " << actual.size() << "):\n";
    for (long j = (long)i - context; j <= (long)i + context; j++) {
      if (j < 0 || j >= (long)n) {
        continue;
      }
      ss << "      [" << j << "] expected: "
         << (j < (long)expected.size() ? fuzzEntryStr(expected[j]) : "<none>")
         << " | actual: "
         << (j < (long)actual.size() ? fuzzEntryStr(actual[j]) : "<none>")
         << "\n";
    }
    break;
  }
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

TEST_CASE("Move search fuzz: tap-mode fixtures match the golden") {
  int checked = 0;
  int failures = 0;
  int skippedSpinCoverage = 0;
  std::ostringstream report;
  for (int i = 0; i < MOVE_SEARCH_FIXTURE_COUNT; i++) {
    const MoveSearchFixture &f = MOVE_SEARCH_FIXTURES[i];
    if (f.dasCharge != -1)
      continue;

    std::vector<FuzzEntry> expected = expectedEntries(f);

    // The two searches define "tuck" differently, by design: fastMoveSearch
    // finds gameplay tucks (destination cells flagged as overhang setups by the
    // board analysis), while fullMoveSearch mirrors TS's dfs, which appends any
    // input producing a novel lock during the fall. On the jagged-overhang
    // board the dfs-based definition yields 46 spins + 14 spin-tucks that
    // fastMoveSearch intentionally does not model, so those fixtures are
    // skipped here (asserted via the fullMoveSearch tap-path test below and
    // `npm run fuzz_verify_sequences`).
    const bool hasSpinEntries =
        std::any_of(expected.begin(), expected.end(),
                    [](const FuzzEntry &e) { return e.inputCostX10 != 0; });
    if (hasSpinEntries) {
      skippedSpinCoverage++;
      continue;
    }
    checked++;

    // Multiset comparison: this guard asserts the placement SET only (the
    // two searches differ in tuck semantics and the legacy search's output
    // order is its own). Sort copies of both sides before the two-pointer
    // diff -- the golden pools are in result order, not sorted order.
    std::vector<FuzzEntry> actual = runCppMoveSearch(f);
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
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

  MESSAGE("Tap-mode fuzz (fastMoveSearch): "
          << checked
          << " fixtures checked, "
          << skippedSpinCoverage
          << " skipped (spin coverage, out of fastMoveSearch's reach), "
          << failures
          << " mismatched");
  REQUIRE_MESSAGE(failures == 0, "Tap-mode fuzz (fastMoveSearch): "
                                     << failures
                                     << "/"
                                     << checked
                                     << " fixtures mismatched\n"
                                     << report.str());
}

TEST_CASE("Move search fuzz: tap fixtures match the golden via fullMoveSearch (tap path)") {
  // fullMoveSearch covers tap mode (dasCharge == -1), so the same tuple
  // multiset the fastMoveSearch guard checks is asserted through the ARR-gated
  // tap path as well. Sequences are verified character-exactly by
  // `npm run fuzz_verify_sequences`.
  int checked = 0;
  int failures = 0;
  std::ostringstream report;
  for (int i = 0; i < MOVE_SEARCH_FIXTURE_COUNT; i++) {
    const MoveSearchFixture &f = MOVE_SEARCH_FIXTURES[i];
    if (f.dasCharge != -1)
      continue;
    checked++;

    std::vector<FuzzEntry> actual = runCppMoveSearch(f, /*forceFullSearch=*/true);
    std::vector<FuzzEntry> expected = expectedEntries(f);
    std::string diff = diffEntriesInOrder(expected, actual, /*context=*/2);
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

  MESSAGE("Tap-mode fuzz (fullMoveSearch tap path): "
          << checked
          << " fixtures checked, "
          << failures
          << " mismatched");
  REQUIRE_MESSAGE(failures == 0, "Tap-mode fuzz (fullMoveSearch tap path): "
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
    std::string diff = diffEntriesInOrder(expected, actual, /*context=*/2);
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

// Mid-air adjustment entry states. Each fixture starts from the adjTimeSimState
// snapshot of a spawn search run with a reaction time (the phantom-placement
// pipeline); fullMoveSearch +
// FullMoveSearchContext must reproduce the adjustment move search (which runs
// with reaction time disabled). Sequences are verified character-exactly by
// `npm run fuzz_verify_sequences`.
TEST_CASE("Move search fuzz: adjustment fixtures match the golden (mid-air entry)") {
  int checked = 0;
  int failures = 0;
  std::ostringstream report;
  for (int i = 0; i < ADJUSTMENT_FIXTURE_COUNT; i++) {
    const AdjustmentSearchFixture &f = ADJUSTMENT_FIXTURES[i];
    checked++;

    // Mirror main.cpp's GameState preparation exactly.
    GameState gameState = {};
    gameState.level = f.level;
    gameState.lines = 0;
    encodeBoard(FIXTURE_BOARD_POOL[f.boardIdx], gameState.board);
    getSurfaceArray(gameState.board, gameState.surfaceArray);
    updateSurfaceAndHoles(gameState.surfaceArray, gameState.board, /*excludeHolesColumn=*/9,
                          /*isDigMode=*/false);

    const Piece *piece = &PIECE_LIST[f.pieceIndex];
    FullMoveSearchContext ctx;
    ctx.existingXOffset = f.existingXOffset;
    ctx.existingYOffset = f.existingYOffset;
    ctx.framesAlreadyElapsed = f.framesAlreadyElapsed;
    ctx.existingRotation = f.existingRotation;
    ctx.isAdjustment = true;
    ctx.isArrContinued = f.isArrContinued != 0;
    ctx.dasButtonHeld = f.dasButtonHeld;
    ctx.preAdjInputSequence = FIXTURE_PREFIX_POOL[f.preAdjSeqIdx];

    std::vector<FullMovePossibility> results;
    fullMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                   /*trackInputSequences=*/false, results, ctx);
    std::vector<FuzzEntry> actual;
    actual.reserve(results.size());
    for (const FullMovePossibility &p : results) {
      actual.push_back({p.rotationIndex, p.x, p.y, p.dasChargeAfter, p.inputCostX10});
    }
    std::vector<FuzzEntry> expected = expectedAdjustmentEntries(f);
    std::string diff = diffEntriesInOrder(expected, actual, /*context=*/2);
    if (!diff.empty()) {
      failures++;
      if (failures <= 200) {
        std::ostringstream adjDesc;
        adjDesc << "fixture #" << i << " piece=" << piece->id << " level=" << f.level
                << " timeline=" << FIXTURE_TIMELINE_POOL[f.timelineIdx] << " dasCharge=" << f.dasCharge
                << " frames=" << f.framesAlreadyElapsed << " xOffset=" << f.existingXOffset
                << " yOffset=" << f.existingYOffset << " rot=" << f.existingRotation
                << " arrContinued=" << f.isArrContinued << " held=" << f.dasButtonHeld
                << " preAdj=" << FIXTURE_PREFIX_POOL[f.preAdjSeqIdx];
        report << "  MISMATCH " << adjDesc.str() << " (expected " << expected.size()
               << " entries, got " << actual.size() << ")\n"
               << diff << "\n";
      }
    }
  }

  MESSAGE("Adjustment-mode fuzz (fullMoveSearch mid-air entry): "
          << checked
          << " fixtures checked, "
          << failures
          << " mismatched");
  REQUIRE(ADJUSTMENT_FIXTURE_COUNT > 0);
  REQUIRE_MESSAGE(failures == 0, "Adjustment-mode fuzz: "
                                     << failures
                                     << "/"
                                     << checked
                                     << " fixtures mismatched\n"
                                     << report.str());
}

// Reaction-time snapshots: spawn searches run WITH a reaction time and every
// result must map to its walk's adjTimeSimState snapshot (or none). Compared
// ORDER-EXACTLY against the generated pools (the result-to-snapshot mapping
// is part of the golden; see TESTING.md). Sequences are verified
// character-exactly by `npm run fuzz_verify_sequences`.
TEST_CASE("Move search fuzz: reaction-time fixtures match the golden (adjTimeSimState snapshot)") {
  int checked = 0;
  int failures = 0;
  std::ostringstream report;
  for (int i = 0; i < REACTION_TIME_FIXTURE_COUNT; i++) {
    const ReactionTimeSearchFixture &f = REACTION_TIME_FIXTURES[i];
    checked++;

    // Mirror main.cpp's GameState preparation exactly.
    GameState gameState = {};
    gameState.level = f.level;
    gameState.lines = 0;
    encodeBoard(FIXTURE_BOARD_POOL[f.boardIdx], gameState.board);
    getSurfaceArray(gameState.board, gameState.surfaceArray);
    updateSurfaceAndHoles(gameState.surfaceArray, gameState.board, /*excludeHolesColumn=*/9,
                          /*isDigMode=*/false);

    const Piece *piece = &PIECE_LIST[f.pieceIndex];
    std::vector<FullMovePossibility> results;
    std::vector<AdjTimeSimStateSlot> captures;
    fullMoveSearch(gameState, piece, FIXTURE_TIMELINE_POOL[f.timelineIdx], f.dasCharge,
                   /*trackInputSequences=*/false, results, FullMoveSearchContext(), f.reactionTime,
                   &captures);

    const int entryCount = f.entryCount;
    std::ostringstream desc;
    desc << "fixture #" << i << " piece=" << piece->id << " level=" << f.level
         << " timeline=" << FIXTURE_TIMELINE_POOL[f.timelineIdx] << " dasCharge=" << f.dasCharge
         << " reactionTime=" << f.reactionTime;

    if ((int)results.size() != entryCount || (int)captures.size() != entryCount) {
      failures++;
      if (failures <= 200) {
        report << "  MISMATCH " << desc.str() << " (expected " << entryCount
               << " entries, got results=" << results.size()
               << " captures=" << captures.size() << ")\n";
      }
      continue;
    }

    for (int j = 0; j < entryCount; j++) {
      const FuzzEntry expected = unpackEntry(REACTION_TIME_ENTRY_POOL[f.entryOffset + j]);
      const FuzzEntry actual = {results[j].rotationIndex, results[j].x, results[j].y,
                                results[j].dasChargeAfter, results[j].inputCostX10};
      const FixtureAdjTimeCapture &ec = REACTION_TIME_CAPTURE_POOL[f.captureOffset + j];
      const AdjTimeSimStateSlot &ac = captures[j];
      const bool captureMatch =
          (ec.hasCapture != 0) == ac.hasCapture &&
          (!ac.hasCapture ||
           (ac.capture.frameIndex == ec.frameIndex && ac.capture.x == ec.x &&
            ac.capture.y == ec.y && ac.capture.rotationIndex == ec.rotationIndex &&
            ac.capture.dasCharge == ec.dasCharge && ac.capture.isArrContinued == (ec.isArrContinued != 0) &&
            ac.capture.dasButtonHeld == ec.dasButtonHeld &&
            ac.capture.preAdjInputSequence == FIXTURE_RT_PREFIX_POOL[ec.preAdjSeqIdx]));
      if ((expected.rot != actual.rot || expected.x != actual.x || expected.y != actual.y ||
           expected.dasChargeAfter != actual.dasChargeAfter ||
           expected.inputCostX10 != actual.inputCostX10) ||
          !captureMatch) {
        failures++;
        if (failures <= 200) {
          report << "  MISMATCH " << desc.str() << " entry " << j << "\n"
                 << "    expected: rot=" << expected.rot << " x=" << expected.x
                 << " y=" << expected.y << " charge=" << expected.dasChargeAfter
                 << " cost=" << expected.inputCostX10 << " hasCapture=" << (ec.hasCapture != 0)
                 << (ec.hasCapture ? " f=" + std::to_string(ec.frameIndex)
                                         + " x=" + std::to_string(ec.x)
                                         + " y=" + std::to_string(ec.y)
                                         + " rot=" + std::to_string(ec.rotationIndex)
                                         + " c=" + std::to_string(ec.dasCharge)
                                         + " arr=" + std::to_string(ec.isArrContinued)
                                         + " held=" + std::to_string(ec.dasButtonHeld)
                                         + " pre=" + FIXTURE_RT_PREFIX_POOL[ec.preAdjSeqIdx]
                                       : "")
                 << "\n"
                 << "    actual  : rot=" << actual.rot << " x=" << actual.x << " y=" << actual.y
                 << " charge=" << actual.dasChargeAfter << " cost=" << actual.inputCostX10
                 << " hasCapture=" << ac.hasCapture
                 << (ac.hasCapture ? " f=" + std::to_string(ac.capture.frameIndex)
                                         + " x=" + std::to_string(ac.capture.x)
                                         + " y=" + std::to_string(ac.capture.y)
                                         + " rot=" + std::to_string(ac.capture.rotationIndex)
                                         + " c=" + std::to_string(ac.capture.dasCharge)
                                         + " arr=" + std::to_string(ac.capture.isArrContinued)
                                         + " held=" + std::to_string(ac.capture.dasButtonHeld)
                                         + " pre=" + ac.capture.preAdjInputSequence
                                       : "")
                 << "\n";
        }
        break; // one report line per fixture is enough
      }
    }
  }

  MESSAGE("Reaction-time fuzz (fullMoveSearch adj-time capture): "
          << checked
          << " fixtures checked, "
          << failures
          << " mismatched");
  REQUIRE(REACTION_TIME_FIXTURE_COUNT > 0);
  REQUIRE_MESSAGE(failures == 0, "Reaction-time fuzz: "
                                     << failures
                                     << "/"
                                     << checked
                                     << " fixtures mismatched\n"
                                     << report.str());
}

#endif // SR_MOVE_SEARCH_FUZZ_TESTS
