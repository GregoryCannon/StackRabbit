#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../src/tests/vendor/doctest.h"

#include "core.hpp"
#include "heuristic.hpp"
#include "server_routes.hpp"
#include "state_generator.hpp"
#include "tablebase_io.hpp"
#include "vi_solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace {

uint64_t mustParse(std::string_view ascii) {
  uint64_t b = 0;
  const bool ok = tb::parseBoardAscii(ascii, b);
  REQUIRE(ok);
  return b;
}

struct RawCheck {
  bool collides = true;
  bool fitsCeiling = false;
  uint64_t mask = 0;
};

// Evaluates raw piece cell coordinates directly from tb::PIECES without touching tb::GEOMETRY,
// so the fuzz oracle independently verifies buildGeometryTable() as well as forEachLegalMove().
RawCheck evaluateRawPieceAt(uint64_t board, int pieceIdx, int rot, int x, int y) {
  RawCheck out{false, true, 0ULL};
  const tb::PieceDef &piece = tb::PIECES[pieceIdx];
  for (int r = 0; r < 4; ++r) {
    const uint16_t rowBits = piece.rows[rot][r];
    if (rowBits == 0) {
      continue;
    }
    for (int c = 0; c < 4; ++c) {
      if ((rowBits & (1U << (9 - c))) == 0) {
        continue;
      }
      const int col = x + c;
      const int gridY = y + r;
      if (col < 0 || col >= tb::BOARD_WIDTH || gridY < 0 || gridY >= tb::SEARCH_GRID_ROWS) {
        return {true, false, 0ULL};
      }
      if (gridY < tb::ENTRY_ROWS) {
        out.fitsCeiling = false;
      } else {
        const int rowFromBottom = (tb::SEARCH_GRID_ROWS - 1) - gridY;
        const uint64_t bit = tb::cellBit(rowFromBottom, col);
        if ((board & bit) != 0) {
          return {true, false, 0ULL};
        }
        out.mask |= bit;
      }
    }
  }
  return out;
}

std::set<uint64_t> referenceLegalLockedMasks(uint64_t board, int pieceIdx) {
  std::set<uint64_t> masks;
  const int numRots = tb::PIECES[pieceIdx].numRots;

  for (int rot = 0; rot < numRots; ++rot) {
    for (int x = -5; x <= 10; ++x) {
      if (evaluateRawPieceAt(board, pieceIdx, rot, x, 0).collides) {
        continue;
      }
      int dropY = 0;
      while (!evaluateRawPieceAt(board, pieceIdx, rot, x, dropY + 1).collides) {
        ++dropY;
      }
      const RawCheck dropRes = evaluateRawPieceAt(board, pieceIdx, rot, x, dropY);
      if (!dropRes.collides && dropRes.fitsCeiling) {
        masks.insert(dropRes.mask);
      }

      for (int y = 0; y <= dropY; ++y) {
        for (const tb::TuckInput &tuck : tb::TUCK_INPUTS) {
          const int postX = x + tuck.dx;
          const int postRot = (rot + tuck.dRot + numRots) % numRots;
          if (evaluateRawPieceAt(board, pieceIdx, rot, postX, y).collides) {
            continue;
          }
          if (evaluateRawPieceAt(board, pieceIdx, postRot, postX, y).collides) {
            continue;
          }
          int lockY = y;
          while (!evaluateRawPieceAt(board, pieceIdx, postRot, postX, lockY + 1).collides) {
            ++lockY;
          }
          const RawCheck lockRes = evaluateRawPieceAt(board, pieceIdx, postRot, postX, lockY);
          if (!lockRes.collides && lockRes.fitsCeiling) {
            masks.insert(lockRes.mask);
          }
        }
      }
    }
  }
  return masks;
}

} // namespace

TEST_CASE("Bitboard cell addressing, formatting, and strict boundary parsing") {
  uint64_t b = 0;
  for (int r = 0; r < tb::BOARD_ROWS; ++r) {
    for (int c = 0; c < tb::BOARD_WIDTH; ++c) {
      CHECK_FALSE(tb::getCell(b, r, c));
      const uint64_t single = tb::setCell(0ULL, r, c);
      CHECK(tb::popcount64(single) == 1);
      CHECK(tb::getCell(single, r, c));
      b = tb::setCell(b, r, c);
    }
  }
  CHECK(b == tb::VALID_BOARD_MASK);

  const uint64_t sample = mustParse("..........\n"
                                    "..XX......\n"
                                    ".XXXX.....\n"
                                    "XXXXXX..XX");
  CHECK(tb::boardToAscii(sample)
        == "..........\n"
           "..XX......\n"
           ".XXXX.....\n"
           "XXXXXX..XX");

  uint64_t fromHex = 0;
  const std::string hex = tb::formatBoardHex(sample);
  CHECK(hex.size() == 10);
  CHECK(tb::parseBoardHex(hex, fromHex));
  CHECK(fromHex == sample);

  // Uppercase and short hex inputs parse cleanly.
  CHECK(tb::parseBoardHex("00ABCDEF12", fromHex));
  CHECK(fromHex == 0x00ABCDEF12ULL);
  CHECK(tb::parseBoardHex("0", fromHex));
  CHECK(fromHex == 0ULL);

  // Piece character round-trip and rejection of invalid piece chars.
  for (int p = 0; p < tb::NUM_PIECES; ++p) {
    CHECK(tb::parsePieceChar(tb::PIECE_CHARS[p]) == p);
  }
  CHECK(tb::parsePieceChar('X') == -1);
  CHECK(tb::parsePieceChar('i') == -1);
  CHECK(tb::parsePieceChar('\0') == -1);

  // Out-of-range pieceIdx returns empty lists safely.
  tb::PlacementList pl;
  tb::TransitionList tl;
  CHECK(tb::generatePlacements(0ULL, -1, pl) == 0);
  CHECK(tb::generatePlacements(0ULL, 7, pl) == 0);
  CHECK(tb::generateTransitions(0ULL, -1, tl) == 0);
  CHECK(tb::generateTransitions(0ULL, 7, tl) == 0);

  // Reject malformed ASCII and hex inputs.
  uint64_t dummy = 0;
  CHECK_FALSE(tb::parseBoardAscii("..........", dummy));
  CHECK_FALSE(tb::parseBoardAscii(std::string(41, '.'), dummy));
  CHECK_FALSE(tb::parseBoardAscii(std::string(39, '.') + "?", dummy));
  CHECK_FALSE(tb::parseBoardHex("", dummy));
  CHECK_FALSE(tb::parseBoardHex("0x00000000", dummy));
  CHECK_FALSE(tb::parseBoardHex("10000000000", dummy));
  CHECK_FALSE(tb::parseBoardHex("000000000g", dummy));
}

TEST_CASE("Line clearing and canonical board invariants") {
  // Simultaneous non-adjacent clear of rows 0 and 2; rows 1 and 3 compact down into rows 0 and 1.
  const uint64_t placed = mustParse("X........X\n"
                                    "XXXXXXXXXX\n"
                                    ".XX....XX.\n"
                                    "XXXXXXXXXX");
  const tb::ClearResult res = tb::clearFullRows(placed);
  CHECK(res.linesCleared == 2);
  CHECK(res.board
        == mustParse("..........\n"
                     "..........\n"
                     "X........X\n"
                     ".XX....XX."));
  CHECK(tb::isCanonicalBoard(res.board));

  // 4-line clear returns empty board 0ULL.
  const tb::ClearResult quad = tb::clearFullRows(tb::VALID_BOARD_MASK);
  CHECK(quad.linesCleared == 4);
  CHECK(quad.board == 0ULL);
  CHECK(tb::isCanonicalBoard(0ULL));

  // Non-canonical boards rejected.
  CHECK_FALSE(tb::isCanonicalBoard(1ULL));
  CHECK_FALSE(tb::isCanonicalBoard(1ULL << 40));
  CHECK_FALSE(tb::isCanonicalBoard(tb::FULL_ROW_MASK));
  const uint64_t floatingRow = mustParse("..........\n"
                                         "..........\n"
                                         "XX........\n"
                                         "..........");
  CHECK_FALSE(tb::isCanonicalBoard(floatingRow));
}

TEST_CASE("Column heights, hole masks, and side-accessible overhang masks") {
  // 4 holes total:
  //   - (r=0, col=1) adjacent to open col 0
  //   - (r=0, col=3) & (r=1, col=3) adjacent to open col 4
  //   - (r=0, col=8) sealed between filled cols 7 and 9
  const uint64_t board = mustParse("..........\n"
                                   "..XX......\n"
                                   ".XX....XXX\n"
                                   "..X....X.X");
  int heights[10] = {};
  tb::getColumnHeights(board, heights);
  CHECK(heights[0] == 0);
  CHECK(heights[1] == 2);
  CHECK(heights[2] == 3);
  CHECK(heights[3] == 3);
  CHECK(heights[4] == 0);
  CHECK(heights[7] == 2);
  CHECK(heights[8] == 2);
  CHECK(heights[9] == 2);
  CHECK(tb::getMaxHeight(board) == 3);

  CHECK(tb::countHoles(board) == 4);
  const uint64_t overhangs = tb::computeOverhangMask(board);
  CHECK(tb::popcount64(overhangs) == 3);
  CHECK((overhangs & tb::cellBit(0, 1)) != 0);
  CHECK((overhangs & tb::cellBit(0, 3)) != 0);
  CHECK((overhangs & tb::cellBit(1, 3)) != 0);
  CHECK((overhangs & tb::cellBit(0, 8)) == 0);

  // Raised overhang at (r=1, col=0) next to col 1 where r=0 is filled and r=1..3 are open,
  // plus wall overhang at (r=0, col=9) next to open col 8 (verifying no row wrap-around).
  const uint64_t wallAndRaisedBoard = mustParse("..........\n"
                                                "XX........\n"
                                                ".X.......X\n"
                                                "XX.....X..");
  const uint64_t wallOh = tb::computeOverhangMask(wallAndRaisedBoard);
  CHECK((wallOh & tb::cellBit(1, 0)) == 0); // col 1 has r=1 filled, so col 0 is sealed at r=1
  const uint64_t raisedOpenBoard = mustParse("..........\n"
                                             "X.........\n"
                                             ".........X\n"
                                             "XX.....X..");
  const uint64_t raisedOh = tb::computeOverhangMask(raisedOpenBoard);
  CHECK((raisedOh & tb::cellBit(1, 0)) != 0); // col 1 is open at r=1..3 despite r=0 filled
  CHECK((raisedOh & tb::cellBit(0, 9)) != 0); // col 8 is open down to r=0
}

TEST_CASE("Empty board placement counts match exact 4-row geometry") {
  constexpr int expectedCounts[7] = {17, 9, 34, 34, 34, 17, 17};
  tb::PlacementList pl;
  tb::TransitionList tl;
  int total = 0;
  for (int p = 0; p < tb::NUM_PIECES; ++p) {
    CHECK(tb::generatePlacements(0ULL, p, pl) == expectedCounts[p]);
    CHECK(tb::generateTransitions(0ULL, p, tl) == expectedCounts[p]);
    std::sort(pl.begin(), pl.end(), [](const tb::Placement &a, const tb::Placement &b) {
      return a.afterBoard < b.afterBoard;
    });
    for (const auto &m : pl) {
      CHECK(m.tuckInput == tb::NO_TUCK);
      CHECK(m.linesCleared == 0);
      CHECK(tb::isCanonicalBoard(m.afterBoard));
    }
    total += pl.size();
  }
  CHECK(total == 162);
}

TEST_CASE("Pre-clear 4-row ceiling rule enforces strict pre-clear bounds and top-outs") {
  // Row 0 has cols 0..8 filled (col 9 open), plus (row 1, col 0) for even parity.
  const uint64_t row0AlmostFull = mustParse("..........\n"
                                            "..........\n"
                                            "X.........\n"
                                            "XXXXXXXXX.");

  tb::PlacementList pl;
  tb::TransitionList tl;
  // Vertical I in col 9 drops to the floor (rows 0..3) and clears row 0;
  // vertical I in cols 0..8 would extend above the 4-row cap and is rejected.
  tb::generatePlacements(row0AlmostFull, tb::parsePieceChar('I'), pl);
  int vertCount = 0;
  int lineClearMoves = 0;
  for (const auto &m : pl) {
    if (m.rot == 1) {
      ++vertCount;
      CHECK(m.x == 7);
      CHECK(m.linesCleared == 1);
    }
    if (m.linesCleared > 0) {
      ++lineClearMoves;
    }
  }
  CHECK(vertCount == 1);
  CHECK(lineClearMoves >= 1);

  // Pre-clear vs post-clear distinction: col 9 has row 0 filled while row 1 has cols 0..8 filled.
  // Placing vertical I in col 9 rests on row 0 (spanning rows 1..4) — even though clearing row 1
  // would bring the board back within 4 rows post-clear, pre-clear enforcement rejects it.
  const uint64_t postClearTrap = mustParse("..........\n"
                                           "..........\n"
                                           "XXXXXXXXX.\n"
                                           "X........X");
  tb::generatePlacements(postClearTrap, tb::parsePieceChar('I'), pl);
  for (const auto &m : pl) {
    CHECK(m.rot == 0);
  }

  // O-piece rejected on any column pair with height >= 3, and 0-move top-out when all pairs are h
  // >= 3.
  const uint64_t height3Even = mustParse("..........\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         ".XXXXXXXX.");
  REQUIRE(tb::isCanonicalBoard(height3Even));
  CHECK(tb::generatePlacements(height3Even, tb::parsePieceChar('O'), pl) == 0);
  CHECK(tb::generateTransitions(height3Even, tb::parsePieceChar('O'), tl) == 0);

  // 4-line clear in col 9 from a 4-high 9-0 stack (and 0-move top-out for O/L/J/T/S/Z on it).
  const uint64_t tetrisReady = mustParse("XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.");
  tb::generatePlacements(tetrisReady, tb::parsePieceChar('I'), pl);
  REQUIRE(pl.size() == 1);
  CHECK(pl[0].rot == 1);
  CHECK(pl[0].linesCleared == 4);
  CHECK(pl[0].afterBoard == 0ULL);
  for (char c : {'O', 'L', 'J', 'T', 'S', 'Z'}) {
    CHECK(tb::generatePlacements(tetrisReady, tb::parsePieceChar(c), pl) == 0);
    CHECK(tb::generateTransitions(tetrisReady, tb::parsePieceChar(c), tl) == 0);
  }
}

TEST_CASE("1-input horizontal tucks, post-tuck gravity drop, spins, and NES Shift-before-Rotate") {
  // 1. High tuck with post-tuck multi-row gravity drop (lockY > y):
  // Overhang at (row 3, col 1) supported by col 0 (rows 0..3). O drops in cols 2..3 to row 1..2
  // (y = 4), shifts 'L' under the row-3 overhang into cols 1..2 at y = 4, and then falls 1 more
  // row to the floor (lockY = 5, rows 0..1).
  const uint64_t highTuckDropBoard = mustParse("XX........\n"
                                               "X.........\n"
                                               "X.........\n"
                                               "X........X");
  REQUIRE(tb::isCanonicalBoard(highTuckDropBoard));
  tb::PlacementList pl;
  tb::generatePlacements(highTuckDropBoard, tb::parsePieceChar('O'), pl);
  bool foundTuckedO = false;
  for (const auto &m : pl) {
    if (m.x == 0 && m.lockY == 5) {
      foundTuckedO = true;
      CHECK(m.tuckInput == 'L');
    }
  }
  CHECK(foundTuckedO);

  // 2. Pure 1-input T-spin ('A'): T drops in rot 3 (cols 1..2) down a 2-wide throat under a
  // col-0 overhang and spins CW ('A') into rot 0 (XXX on row 1, .X. on row 0) for a T-spin double.
  const uint64_t tSpinBoard = mustParse("X.........\n"
                                        "...XXXXXXX\n"
                                        "...XXXXXXX\n"
                                        "X.XXXXXXXX");
  tb::generatePlacements(tSpinBoard, tb::parsePieceChar('T'), pl);
  const uint64_t tSlotMask =
      tb::cellBit(1, 0) | tb::cellBit(1, 1) | tb::cellBit(1, 2) | tb::cellBit(0, 1);
  bool foundTSpin = false;
  for (const auto &m : pl) {
    if (m.lockedMask == tSlotMask) {
      foundTSpin = true;
      CHECK(m.linesCleared == 2);
      CHECK(m.tuckInput == 'A');
    }
  }
  CHECK(foundTSpin);

  // 3. NES Shift-before-Rotate order test on simultaneous shift+rotate ('I' = Right + Rotate CW):
  // Target: J piece in rot 3 at x = 1, y = 5 (occupying row 2 cols 3..4, row 1 col 3, row 0 col 3).
  // Reachable ONLY from preRot = 2, preX = 0, y = 5 via 'I':
  //   - Straight drops at x = 1 are blocked by the overhang at (row 3, col 4).
  //   - Pure 'R' in rot 3 from x = 0 is blocked at (row 0, col 2).
  //   - 'G' from preRot = 0, preX = 0 hits (row 0, col 4) during its intermediate right shift.
  //   - Intermediate right-shift of preRot = 2 sweeps through (row 1, col 4), which neither the
  //     pre-tuck nor post-rotate footprint occupies — so a blocker at (row 1, col 4) blocks ONLY
  //     the intermediate shift.
  const uint64_t jTargetMask =
      tb::cellBit(2, 3) | tb::cellBit(2, 4) | tb::cellBit(1, 3) | tb::cellBit(0, 3);

  const uint64_t shiftBlockedBoard = mustParse("....XX....\n"
                                               ".....X....\n"
                                               "....XX....\n"
                                               ".XX.XX...X");
  REQUIRE(tb::isCanonicalBoard(shiftBlockedBoard));
  tb::generatePlacements(shiftBlockedBoard, tb::parsePieceChar('J'), pl);
  for (const auto &m : pl) {
    CHECK(m.lockedMask != jTargetMask);
  }

  // Removing ONLY the intermediate-shift blocker at (row 1, col 4) makes jTargetMask reachable via
  // 'I'.
  const uint64_t shiftClearBoard = (shiftBlockedBoard & ~tb::cellBit(1, 4)) | tb::cellBit(0, 8);
  REQUIRE(tb::isCanonicalBoard(shiftClearBoard));
  tb::generatePlacements(shiftClearBoard, tb::parsePieceChar('J'), pl);
  bool foundComboSpin = false;
  for (const auto &m : pl) {
    if (m.lockedMask == jTargetMask) {
      foundComboSpin = true;
      CHECK(m.tuckInput == 'I');
    }
  }
  CHECK(foundComboSpin);
}

TEST_CASE("Distinct placements collapsing to identical afterBoard are preserved in Placements and "
          "merged in Transitions") {
  // Rows 0 and 1 both have cols 0..6 filled and cols 7..9 empty (14 cells, even).
  // T rot 0 (XXX on row 1, .X. on row 0) clears row 1 -> row 0 becomes 1111111010.
  // T rot 2 (.X. on row 1, XXX on row 0) clears row 0 -> row 1 drops to row 0 as 1111111010.
  const uint64_t twinClearBoard = mustParse("..........\n"
                                            "..........\n"
                                            "XXXXXXX...\n"
                                            "XXXXXXX...");
  const uint64_t collapsedAfter = mustParse("..........\n"
                                            "..........\n"
                                            "..........\n"
                                            "XXXXXXX.X.");

  tb::PlacementList pl;
  tb::TransitionList tl;
  tb::generatePlacements(twinClearBoard, tb::parsePieceChar('T'), pl);
  tb::generateTransitions(twinClearBoard, tb::parsePieceChar('T'), tl);

  int placementMatches = 0;
  for (const auto &m : pl) {
    if (m.afterBoard == collapsedAfter) {
      ++placementMatches;
      CHECK(m.linesCleared == 1);
    }
  }
  int transitionMatches = 0;
  for (const auto &t : tl) {
    if (t.afterBoard == collapsedAfter) {
      ++transitionMatches;
      CHECK(t.linesCleared == 1);
    }
  }
  CHECK(placementMatches == 2);
  CHECK(transitionMatches == 1);
}

TEST_CASE("Fuzz: overhang-window move generator matches independent raw-bit oracle on 3,500 "
          "board-piece pairs") {
  tb::Rng rng(123456789ULL);
  uint64_t board = 0ULL;
  tb::PlacementList pl;
  tb::TransitionList tl;

  for (int step = 0; step < 500; ++step) {
    REQUIRE(tb::isCanonicalBoard(board));
    for (int p = 0; p < tb::NUM_PIECES; ++p) {
      const std::set<uint64_t> expectedMasks = referenceLegalLockedMasks(board, p);
      tb::generatePlacements(board, p, pl);
      tb::generateTransitions(board, p, tl);

      std::set<uint64_t> actualMasks;
      std::set<uint64_t> expectedAfterBoards;
      for (const auto &m : pl) {
        actualMasks.insert(m.lockedMask);
        expectedAfterBoards.insert(m.afterBoard);
        CHECK(tb::isCanonicalBoard(m.afterBoard));
      }
      CHECK(actualMasks == expectedMasks);

      std::set<uint64_t> actualAfterBoards;
      for (const auto &t : tl) {
        actualAfterBoards.insert(t.afterBoard);
      }
      CHECK(actualAfterBoards == expectedAfterBoards);
      CHECK(static_cast<size_t>(tl.size()) == expectedAfterBoards.size());
    }

    const int nextP = rng.nextPiece();
    tb::generateTransitions(board, nextP, tl);
    if (tl.empty()) {
      board = 0ULL;
    } else {
      const int idx = static_cast<int>(rng.nextU64() % static_cast<uint64_t>(tl.size()));
      board = tl[idx].afterBoard;
    }
  }
}

TEST_CASE("Fuzz: 100,000 random rollout steps preserve canonical invariants and PRNG uniformity") {
  tb::Rng rng(42ULL);
  uint64_t board = 0ULL;
  tb::TransitionList tl;
  int pieceCounts[tb::NUM_PIECES] = {};

  for (int step = 0; step < 100000; ++step) {
    const int p = rng.nextPiece();
    REQUIRE(p >= 0);
    REQUIRE(p < tb::NUM_PIECES);
    ++pieceCounts[p];

    tb::generateTransitions(board, p, tl);
    if (tl.empty()) {
      board = 0ULL;
    } else {
      const auto &chosen = tl[static_cast<int>(rng.nextU64() % static_cast<uint64_t>(tl.size()))];
      board = chosen.afterBoard;
      REQUIRE(tb::isCanonicalBoard(board));
    }
  }

  // Expected count per piece = 100000 / 7 = 14285.7, sigma ~ 110.7; check within +-6 sigma.
  for (int p = 0; p < tb::NUM_PIECES; ++p) {
    CHECK(std::abs(pieceCounts[p] - 14285.7) < 700.0);
  }

  tb::Rng a(999ULL);
  tb::Rng b(999ULL);
  for (int i = 0; i < 100; ++i) {
    CHECK(a.nextU64() == b.nextU64());
    const double u = a.nextDouble();
    CHECK(u == b.nextDouble());
    CHECK(u >= 0.0);
    CHECK(u < 1.0);
  }
}

TEST_CASE("Tablebase and state-set binary I/O round-trip, mmap lookup, and corruption checks") {
  const std::string statesPath = "build/test_sample.states.bin";
  const std::string tbPath = "build/test_sample.tb.bin";
  const std::string corruptPath = "build/test_corrupt.bin";

  // 0-board empty files round-trip cleanly.
  std::string err;
  REQUIRE(tb::writeStatesFile(statesPath, {}, &err));
  std::vector<uint64_t> loadedBoards;
  REQUIRE(tb::readStatesFile(statesPath, loadedBoards, &err));
  CHECK(loadedBoards.empty());

  REQUIRE(tb::writeTablebaseFile(tbPath, {}, {}, 0.0, &err));
  tb::MappedTablebase emptyMapped;
  REQUIRE(emptyMapped.open(tbPath, &err));
  CHECK(emptyMapped.isLoaded());
  CHECK(emptyMapped.size() == 0);
  CHECK(emptyMapped.emptyBoardValue() < 0.0);
  float v = 0.0f;
  CHECK_FALSE(emptyMapped.lookup(0ULL, v));
  emptyMapped.close();

  const std::vector<uint64_t> boards = {0ULL, 0x0000000003ULL, 0x00000003FCULL, 0x00000FF0FFULL};
  const std::vector<float> vals = {42.5f, 39.25f, 31.0f, 12.75f};

  REQUIRE(tb::writeStatesFile(statesPath, boards, &err));
  REQUIRE(tb::readStatesFile(statesPath, loadedBoards, &err));
  CHECK(loadedBoards == boards);

  // Reject unsorted or duplicate boards on write.
  CHECK_FALSE(tb::writeStatesFile(statesPath, {10ULL, 5ULL}, &err));
  CHECK_FALSE(tb::writeStatesFile(statesPath, {5ULL, 5ULL}, &err));

  REQUIRE(tb::writeTablebaseFile(tbPath, boards, vals, 1.25e-6, &err));

  tb::MappedTablebase mapped;
  REQUIRE(mapped.open(tbPath, &err));
  CHECK(mapped.isLoaded());
  CHECK(mapped.size() == 4);
  CHECK(mapped.emptyBoardValue() == doctest::Approx(42.5));
  CHECK(mapped.bellmanResidual() == doctest::Approx(1.25e-6));

  for (size_t i = 0; i < boards.size(); ++i) {
    REQUIRE(mapped.lookup(boards[i], v));
    CHECK(v == doctest::Approx(vals[i]));
  }
  CHECK_FALSE(mapped.lookup(0x000000000FULL, v));
  CHECK_FALSE(mapped.lookup(0xFFFFFFFFFCULL, v));

  // Move semantics preserve mapped view and clear source.
  tb::MappedTablebase moved = std::move(mapped);
  CHECK_FALSE(mapped.isLoaded());
  CHECK(moved.isLoaded());
  REQUIRE(moved.lookup(0x00000003FCULL, v));
  CHECK(v == doctest::Approx(31.0f));
  moved.close();

  // On-disk corruption & integer-overflow checks:
  // 1. Bad magic rejected with magic error (even if passed states file to MappedTablebase).
  CHECK_FALSE(mapped.open(statesPath, &err));
  CHECK(err.find("magic") != std::string::npos);

  // 2. Overflowing numBoards (0x4000000000000001ULL * 12 == 12 mod 2^64) rejected safely!
  {
    std::ofstream out(corruptPath, std::ios::binary | std::ios::trunc);
    tb::TablebaseHeader hdr{};
    std::memcpy(hdr.magic, tb::TABLEBASE_MAGIC, sizeof(tb::TABLEBASE_MAGIC));
    hdr.numBoards = 0x4000000000000001ULL;
    hdr.emptyBoardValue = -1.0;
    hdr.bellmanResidual = 0.0;
    const auto rec = tb::PackedTablebaseRecord::make(0ULL, 1.0f);
    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char *>(&rec), sizeof(rec));
  }
  CHECK_FALSE(mapped.open(corruptPath, &err));

  // 3. Unsorted records on disk rejected.
  {
    std::ofstream out(corruptPath, std::ios::binary | std::ios::trunc);
    tb::TablebaseHeader hdr{};
    std::memcpy(hdr.magic, tb::TABLEBASE_MAGIC, sizeof(tb::TABLEBASE_MAGIC));
    hdr.numBoards = 2;
    hdr.emptyBoardValue = -1.0;
    hdr.bellmanResidual = 0.0;
    const auto r1 = tb::PackedTablebaseRecord::make(0x000000000FULL, 1.0f);
    const auto r2 = tb::PackedTablebaseRecord::make(0x0000000003ULL, 2.0f);
    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char *>(&r1), sizeof(r1));
    out.write(reinterpret_cast<const char *>(&r2), sizeof(r2));
  }
  CHECK_FALSE(mapped.open(corruptPath, &err));

  std::remove(statesPath.c_str());
  std::remove(tbPath.c_str());
  std::remove(corruptPath.c_str());
}

TEST_CASE(
    "Server HTTP routes validate parameters strictly and serve moves and tablebase Q-values") {
  const std::string tbPath = "build/test_routes.tb.bin";
  tb::PlacementList emptyIMoves;
  tb::generatePlacements(0ULL, tb::parsePieceChar('I'), emptyIMoves);
  REQUIRE(emptyIMoves.size() == 17);

  // Assign a high tablebase value specifically to the last I-piece placement (a vertical placement
  // that heuristic dislikes compared to flat horizontal placement), omitting 0ULL to also test
  // emptyBoardValue:null when 0ULL is absent.
  const uint64_t boostedBoard = emptyIMoves[16].afterBoard;
  REQUIRE(tb::writeTablebaseFile(tbPath, {boostedBoard}, {99.0f}, 1.5e-7));

  tb::ServerContext ctx;
  const auto pingUnloaded = tb::handleApiRequest("/ping", "", ctx);
  CHECK(pingUnloaded.status == 200);
  CHECK(pingUnloaded.body.find("\"tablebaseLoaded\":false") != std::string::npos);
  CHECK(tb::handleApiRequest("/reload", "", ctx).status == 400);

  ctx.configuredTbPath = tbPath;
  const auto reloadRes = tb::handleApiRequest("/reload", "", ctx);
  CHECK(reloadRes.status == 200);
  CHECK(reloadRes.body.find("\"tablebaseLoaded\":true") != std::string::npos);
  CHECK(reloadRes.body.find("\"boardCount\":1") != std::string::npos);
  CHECK(reloadRes.body.find("\"emptyBoardValue\":null") != std::string::npos);

  const auto movesHex = tb::handleApiRequest("/moves", "board=0&piece=I", ctx);
  const auto moves40 =
      tb::handleApiRequest("/moves", "board=" + std::string(40, '.') + "&piece=0", ctx);
  CHECK(movesHex.status == 200);
  CHECK(moves40.status == 200);
  CHECK(movesHex.body == moves40.body);
  CHECK(movesHex.body.find("\"tablebaseQ\":99") != std::string::npos);

  // /state-info reports disagree:true on I (boosted in TB) and disagree:false on O (all children
  // OUT
  // -> TB policy falls back to heuristic).
  const auto infoRes = tb::handleApiRequest("/state-info", "board=0000000000", ctx);
  CHECK(infoRes.status == 200);
  CHECK(infoRes.body.find("\"pieceChar\":\"I\",\"numTransitions\":17") != std::string::npos);
  CHECK(infoRes.body.find("\"disagree\":true") != std::string::npos);
  CHECK(infoRes.body.find("\"pieceChar\":\"O\",\"numTransitions\":9") != std::string::npos);
  CHECK(infoRes.body.find("\"anyInTablebase\":false,\"allInTablebase\":false,\"disagree\":false")
        != std::string::npos);

  // Strict boundary validation rejects malformed queries, unknown params, duplicates, and bad
  // values.
  CHECK(tb::handleApiRequest("/ping", "extra=1", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=0", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=I&piece=O", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=I&typo=1", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=7", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=i", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=123456789ab&piece=I", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=%ZZ&piece=I", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=%00&piece=I", ctx).status == 400);
  CHECK(tb::handleApiRequest("/unknown", "", ctx).status == 404);

  ctx.tablebase.close();
  std::remove(tbPath.c_str());
}

TEST_CASE("Heuristic breakdown features, lossless 2-ply branch-and-bound pruning, and survival") {
  // 1. Complete 11-feature breakdown check on a 0-hole board:
  // Col heights: [3, 2, 0, 0, 2, 2, 0, 4, 3, 4]
  //   - r=3: cols 7, 9 -> 2 cells
  //   - r=2: cols 0, 7, 8, 9 -> 4 cells
  //   - r=1: cols 0, 1, 4, 5, 7, 8, 9 -> 7 cells
  //   - r=0: cols 0, 1, 4, 5, 7, 8, 9 -> 7 cells (20 filled cells total, zero holes)
  const uint64_t featBoard = mustParse(".......X.X\n"
                                       "X......XXX\n"
                                       "XX..XX.XXX\n"
                                       "XX..XX.XXX");
  REQUIRE(tb::isCanonicalBoard(featBoard));
  const tb::HeuristicBreakdown hb = tb::evaluateBoardBreakdown(featBoard);
  CHECK(hb.holes == 0);
  CHECK(hb.overhangs == 0);
  CHECK(hb.sealedHoles == 0);
  CHECK(hb.sumHeightSq == 62);
  CHECK(hb.height3Cols == 2);
  CHECK(hb.ceilingCols == 2);
  CHECK(hb.bumpiness == 13);
  CHECK(hb.wellDepthSq == 4);
  CHECK(hb.parityImbalance == 4);
  CHECK(hb.safeFlatPairs == 2);
  CHECK(hb.rowFillSq == (2 * 2 + 4 * 4 + 7 * 7 + 7 * 7));

  // Also check non-zero sealedHoles and overhangs in evaluateBoardBreakdown:
  const uint64_t holeyBoard = mustParse("..........\n"
                                        "..XX......\n"
                                        ".XX....XXX\n"
                                        "..X....X.X");
  const tb::HeuristicBreakdown hbHoley = tb::evaluateBoardBreakdown(holeyBoard);
  CHECK(hbHoley.holes == 4);
  CHECK(hbHoley.overhangs == 3);
  CHECK(hbHoley.sealedHoles == 1);

  // 2. Verify bit-exact parity (== with zero epsilon!) between pruned pickBestTransition2Ply,
  // unpruned pickBestTransition2Ply, and scoreTransition2Ply across 250 random boards x 7 pieces
  // under default weights and an adversarial positive-weight stress config exercising every upper
  // bound term (including dominant deathPenalty).
  tb::EvalWeights stressPosW{};
  stressPosW.lineClear = 3.5;
  stressPosW.sealedHole = 4.0;
  stressPosW.overhang = 2.5;
  stressPosW.heightSq = 0.3;
  stressPosW.height3Col = 1.8;
  stressPosW.ceilingCol = 2.9;
  stressPosW.bumpiness = 1.2;
  stressPosW.wellDepthSq = 0.7;
  stressPosW.parityImbalance = 0.9;
  stressPosW.safeFlatPairBonus = 3.0;
  stressPosW.rowFillSq = 0.25;
  stressPosW.deathPenalty = 500.0;

  tb::EvalWeights negRowFillW = tb::EvalWeights{};
  negRowFillW.rowFillSq = -0.35;
  negRowFillW.safeFlatPairBonus = -2.0;

  tb::Rng rng(77777ULL);
  uint64_t b = 0ULL;
  tb::TransitionList tl;
  for (int step = 0; step < 250; ++step) {
    for (int p = 0; p < tb::NUM_PIECES; ++p) {
      for (const tb::EvalWeights &w : {tb::EvalWeights{}, stressPosW, negRowFillW}) {
        const auto pruned = tb::pickBestTransition2Ply(b, p, w, true);
        const auto unpruned = tb::pickBestTransition2Ply(b, p, w, false);
        CHECK(pruned.found == unpruned.found);
        if (pruned.found) {
          CHECK(pruned.transition.afterBoard == unpruned.transition.afterBoard);
          CHECK(pruned.transition.linesCleared == unpruned.transition.linesCleared);
          CHECK(pruned.score == unpruned.score);
          CHECK(pruned.score
                == tb::scoreTransition2Ply(pruned.transition.afterBoard,
                                           pruned.transition.linesCleared, w));
        }
      }
    }

    const int nextP = rng.nextPiece();
    tb::generateTransitions(b, nextP, tl);
    b = tl.empty()
            ? 0ULL
            : tl[static_cast<int>(rng.nextU64() % static_cast<uint64_t>(tl.size()))].afterBoard;
  }

  // 3. Verify HTTP routes accept ply=1 and ply=2 (returning distinct scores) and reject bad ply.
  tb::ServerContext ctx;
  const auto mPly1 = tb::handleApiRequest("/moves", "board=0&piece=T&ply=1", ctx);
  const auto mPly2 = tb::handleApiRequest("/moves", "board=0&piece=T&ply=2", ctx);
  CHECK(mPly1.status == 200);
  CHECK(mPly2.status == 200);
  CHECK(mPly1.body != mPly2.body);
  CHECK(mPly2.body.find("\"heuristicPly\":2") != std::string::npos);

  const auto sPly2 = tb::handleApiRequest("/state-info", "board=0&ply=2", ctx);
  CHECK(sPly2.status == 200);
  CHECK(sPly2.body.find("\"heuristicPly\":2") != std::string::npos);
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=T&ply=3", ctx).status == 400);
  CHECK(tb::handleApiRequest("/state-info", "board=0&ply=0", ctx).status == 400);

  // 4. Verify 2-ply expectimax beats 1-ply NNB over 120 deterministic fixed-seed games.
  int totalLines1 = 0;
  int totalLines2 = 0;
  for (uint64_t g = 0; g < 120; ++g) {
    const uint64_t seed = 50000ULL + g * 1337ULL;
    {
      tb::Rng grng(seed);
      uint64_t gb = 0ULL;
      for (int step = 0; step < 2000; ++step) {
        const auto ch = tb::pickBestTransition1Ply(gb, grng.nextPiece());
        if (!ch.found)
          break;
        gb = ch.transition.afterBoard;
        totalLines1 += ch.transition.linesCleared;
      }
    }
    {
      tb::Rng grng(seed);
      uint64_t gb = 0ULL;
      for (int step = 0; step < 2000; ++step) {
        const auto ch = tb::pickBestTransition2Ply(gb, grng.nextPiece());
        if (!ch.found)
          break;
        gb = ch.transition.afterBoard;
        totalLines2 += ch.transition.linesCleared;
      }
    }
  }
  CHECK(totalLines2 > totalLines1);
}

TEST_CASE(
    "FlatBoardIndex, bounded-overhang enumeration, MPI Bellman solver, and chooseTablebaseMove") {
  // 1a. Exhaustive 4-row x 4-active-column check (2^16 = 65,536 bitmasks): exercises all 4 rows,
  // raised overhangs (rHole = 1, 2 with adjacent h = 1, 2), and floating-row rejection.
  std::vector<uint64_t> brute4x4_0;
  std::vector<uint64_t> brute4x4_1;
  for (uint32_t mask16 = 0; mask16 < (1U << 16); ++mask16) {
    uint64_t b = 0ULL;
    for (int r = 0; r < 4; ++r) {
      const uint64_t row4 = (mask16 >> (r * 4)) & 0xFULL;
      b |= (row4 << (r * 10 + 6)); // cols 0..3 occupy bits 9..6 of each 10-bit row
    }
    if (!tb::isCanonicalBoard(b)) {
      continue;
    }
    const uint64_t holes = tb::computeHoleMask(b);
    const int nHoles = tb::popcount64(holes);
    if (nHoles == 0) {
      brute4x4_0.push_back(b);
      brute4x4_1.push_back(b);
    } else if (nHoles == 1 && tb::computeOverhangMask(b) == holes) {
      brute4x4_1.push_back(b);
    }
  }
  CHECK(tb::enumerateZeroSealedHoleBoards(0, 1, 4, 4) == brute4x4_0);
  CHECK(tb::enumerateZeroSealedHoleBoards(1, 4, 4, 4) == brute4x4_1);

  // 1b. Exhaustive 2-row x 10-column check (2^20 = 1,048,576 bitmasks): exercises all 10 columns
  // and both walls, plus exact 4x10 pure-surface count (4,358,525).
  std::vector<uint64_t> brute2x10_0;
  std::vector<uint64_t> brute2x10_1;
  constexpr uint64_t TWO_ROW_LIMIT = 1ULL << 20;
  for (uint64_t b = 0; b < TWO_ROW_LIMIT; ++b) {
    if (!tb::isCanonicalBoard(b)) {
      continue;
    }
    const uint64_t holes = tb::computeHoleMask(b);
    const int nHoles = tb::popcount64(holes);
    if (nHoles == 0) {
      brute2x10_0.push_back(b);
      brute2x10_1.push_back(b);
    } else if (nHoles == 1 && tb::computeOverhangMask(b) == holes) {
      brute2x10_1.push_back(b);
    }
  }
  CHECK(tb::enumerateZeroSealedHoleBoards(0, 4, 2, 10) == brute2x10_0);
  CHECK(tb::enumerateZeroSealedHoleBoards(1, 4, 2, 10) == brute2x10_1);
  CHECK(tb::enumerateZeroSealedHoleBoards(0, 4).size() == 4358525ULL);

  // 2. FlatBoardIndex lookup accuracy (empty index, ~0ULL sentinel, 0ULL, and missing keys).
  tb::FlatBoardIndex emptyIdx;
  CHECK(emptyIdx.empty());
  CHECK(emptyIdx.find(0ULL) == tb::INVALID_BOARD_ID);
  CHECK(emptyIdx.find(~0ULL) == tb::INVALID_BOARD_ID);

  const tb::FlatBoardIndex idx(brute4x4_1);
  CHECK(idx.size() == brute4x4_1.size());
  CHECK(idx.find(~0ULL) == tb::INVALID_BOARD_ID);
  CHECK(idx.find(0x123456789ULL) == tb::INVALID_BOARD_ID);
  for (size_t i = 0; i < brute4x4_1.size(); ++i) {
    CHECK(idx.find(brute4x4_1[i]) == static_cast<uint32_t>(i));
  }

  // 3. Solve MPI on brute2x10_1 on 1 thread vs 4 threads: verify bit-for-bit determinism and
  // exact Bellman residual <= 1e-10.
  tb::ViSolveConfig cfg1{};
  cfg1.tolerance = 1e-10;
  cfg1.maxOuterIters = 100;
  cfg1.innerEvalSweeps = 20;
  cfg1.numThreads = 1;
  const tb::ViSolveResult res1 = tb::solveValueIterationMPI(brute2x10_1, cfg1);

  tb::ViSolveConfig cfg4 = cfg1;
  cfg4.numThreads = 4;
  const tb::ViSolveResult res4 = tb::solveValueIterationMPI(brute2x10_1, cfg4);

  CHECK(res1.finalBellmanResidual <= 1e-10);
  CHECK(res1.emptyBoardValue > 0.5);
  CHECK(res1.emptyBoardValue == res4.emptyBoardValue);
  CHECK(res1.values == res4.values);

  // 4. Test chooseTablebaseMove: in-TB choice, lazy heuristic tie-breaking on equal Q, all-OUT
  // fallback, and 0-move top-out.
  const std::string tmpTbPath = "build/test_choose_move.tb.bin";
  tb::TransitionList oMoves;
  tb::generateTransitions(0ULL, tb::parsePieceChar('O'), oMoves);
  REQUIRE(oMoves.size() == 9);
  // Put two O placements in TB with equal value 10.0f so closedWorld=true picks index 0 while
  // closedWorld=false breaks the tie via heuristic score.
  std::vector<uint64_t> tbKeys = {oMoves[0].afterBoard, oMoves[4].afterBoard};
  std::sort(tbKeys.begin(), tbKeys.end());
  REQUIRE(tb::writeTablebaseFile(tmpTbPath, tbKeys, {10.0f, 10.0f}, 0.0));

  tb::MappedTablebase mapped;
  REQUIRE(mapped.open(tmpTbPath));

  const auto cwMove = tb::chooseTablebaseMove(0ULL, tb::parsePieceChar('O'), mapped, true, 1);
  REQUIRE(cwMove.found);
  CHECK(cwMove.afterInTablebase);
  CHECK_FALSE(cwMove.usedFallback);
  CHECK(cwMove.tablebaseQ == doctest::Approx(10.0));

  const auto liveMove = tb::chooseTablebaseMove(0ULL, tb::parsePieceChar('O'), mapped, false, 1);
  REQUIRE(liveMove.found);
  CHECK(liveMove.afterInTablebase);
  CHECK_FALSE(liveMove.usedFallback);
  const double s0 = tb::scoreTransition1Ply(oMoves[0].afterBoard, 0);
  const double s4 = tb::scoreTransition1Ply(oMoves[4].afterBoard, 0);
  if (s0 != s4) {
    const uint64_t expectedWinner = (s4 > s0) ? oMoves[4].afterBoard : oMoves[0].afterBoard;
    CHECK(liveMove.transition.afterBoard == expectedWinner);
  }

  // Piece 'I' has zero children in this 2-entry O-only TB -> closedWorld=false uses heuristic
  // fallback!
  const auto fbMove = tb::chooseTablebaseMove(0ULL, tb::parsePieceChar('I'), mapped, false, 2);
  REQUIRE(fbMove.found);
  CHECK(fbMove.usedFallback);
  CHECK_FALSE(fbMove.afterInTablebase);
  CHECK(fbMove.transition.afterBoard
        == tb::pickBestTransition2Ply(0ULL, tb::parsePieceChar('I')).transition.afterBoard);

  // Top-out board returns found == false.
  const uint64_t tetrisReady = mustParse("XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.");
  CHECK_FALSE(
      tb::chooseTablebaseMove(tetrisReady, tb::parsePieceChar('O'), mapped, false, 2).found);

  mapped.close();
  std::remove(tmpTbPath.c_str());
}

TEST_CASE("State-space generator: FlatVisitMap, ShardedVisitCounter, thread determinism, sibling "
          "expansion, TB-guided sampling, merge, and coverage") {
  // 1. FlatVisitMap insertion, clear, accumulation, saturation, rehash, and sentinel safety.
  tb::FlatVisitMap vm(16);
  CHECK(vm.get(0ULL) == 0);
  CHECK(vm.get(~0ULL) == 0);
  for (uint64_t i = 0; i < 200; ++i) {
    vm.add(i * 3ULL, 2);
  }
  CHECK(vm.size() == 200);
  vm.add(3ULL, 5);
  CHECK(vm.get(3ULL) == 7);
  vm.add(0ULL, UINT32_MAX - 1);
  CHECK(vm.get(0ULL) == UINT32_MAX);
  vm.clear();
  CHECK(vm.size() == 0);
  CHECK(vm.get(3ULL) == 0);

  // 2. ShardedVisitCounter multi-threaded flush with 20,000 keys (verifies decorrelated sharding
  // stays fast and cross-flush saturating accumulation is exact).
  {
    tb::ShardedVisitCounter svc;
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
      workers.emplace_back([&, t]() {
        tb::FlatVisitMap local(4096);
        for (uint64_t k = 0; k < 5000; ++k) {
          local.add(k * 4ULL, 1);
          local.add((static_cast<uint64_t>(t) * 5000ULL + k + 5000ULL) * 4ULL, 2);
        }
        svc.flushLocal(local);
      });
    }
    for (auto &th : workers)
      th.join();
    auto extracted = svc.extractAll();
    CHECK(extracted.size() == 25000);
    std::sort(extracted.begin(), extracted.end());
    CHECK(extracted[0].first == 0ULL);
    CHECK(extracted[0].second == 4U); // 4 threads x 1 visit
    CHECK(extracted[5000].second == 2U);
  }

  // 3. selectRolloutTransition edge cases: top-out, single-move, epsilon=0 vs epsilon=1, TB-guided.
  {
    const uint64_t tetrisReady = mustParse("XXXXXXXXX.\n"
                                           "XXXXXXXXX.\n"
                                           "XXXXXXXXX.\n"
                                           "XXXXXXXXX.");
    tb::Rng rng(123ULL);
    // Top-out for O on tetrisReady:
    CHECK_FALSE(tb::selectRolloutTransition(tetrisReady, tb::parsePieceChar('O'), rng, nullptr, 1,
                                            1.0, 4, tb::EvalWeights{})
                    .found);
    // Single legal move (vertical I in col 9) on tetrisReady even at epsilon = 1.0:
    const auto singleMove = tb::selectRolloutTransition(tetrisReady, tb::parsePieceChar('I'), rng,
                                                        nullptr, 1, 1.0, 4, tb::EvalWeights{});
    REQUIRE(singleMove.found);
    CHECK(singleMove.transition.afterBoard == 0ULL);
    CHECK(singleMove.transition.linesCleared == 4);

    // At epsilon = 1.0 on empty board, runner-up choice must never equal the greedy move.
    const auto greedyO = tb::pickBestTransition1Ply(0ULL, tb::parsePieceChar('O'));
    for (int i = 0; i < 20; ++i) {
      const auto expO = tb::selectRolloutTransition(0ULL, tb::parsePieceChar('O'), rng, nullptr, 1,
                                                    1.0, 4, tb::EvalWeights{});
      REQUIRE(expO.found);
      CHECK(expO.transition.afterBoard != greedyO.transition.afterBoard);
    }
  }

  // 4. Multi-threaded generateStateSpace determinism (1 thread vs 4 threads) + sibling expansion
  // actually rescues boards compared to siblingMinVisits = 0.
  tb::StateGenConfig cfg1{};
  cfg1.numGames = 150;
  cfg1.baseSeed = 424242ULL;
  cfg1.heurPly = 1;
  cfg1.epsilon = 0.10;
  cfg1.exploreTopK = 4;
  cfg1.minVisits = 2;
  cfg1.maxBoards = 0;
  cfg1.siblingMinVisits = 3;
  cfg1.siblingPly = 1;
  cfg1.numThreads = 1;

  tb::StateGenConfig cfg4 = cfg1;
  cfg4.numThreads = 4;

  const tb::StateGenResult r1 = tb::generateStateSpace(cfg1);
  const tb::StateGenResult r4 = tb::generateStateSpace(cfg4);
  CHECK(r1.sortedBoards == r4.sortedBoards);
  CHECK(r1.rawUniqueDiscovered == r4.rawUniqueDiscovered);
  CHECK(r1.siblingBoardsAdded == r4.siblingBoardsAdded);
  CHECK(r1.siblingBoardsAdded > 0);
  CHECK(r1.totalMovesSimulated == r4.totalMovesSimulated);
  REQUIRE_FALSE(r1.sortedBoards.empty());
  CHECK(r1.sortedBoards.front() == 0ULL);
  for (size_t i = 0; i < r1.sortedBoards.size(); ++i) {
    CHECK(tb::isCanonicalBoard(r1.sortedBoards[i]));
    if (i > 0) {
      CHECK(r1.sortedBoards[i] > r1.sortedBoards[i - 1]);
    }
  }

  tb::StateGenConfig noSibCfg = cfg1;
  noSibCfg.siblingMinVisits = 0;
  const tb::StateGenResult rNoSib = tb::generateStateSpace(noSibCfg);
  CHECK(r1.sortedBoards.size() > rNoSib.sortedBoards.size());

  // 5. Verify maxBoards cap preserves 0ULL even when priorBoards (with duplicates) exceeds cap,
  // and post-cap counters stay consistent.
  const std::vector<uint64_t> prior = {0x000000000FULL, 0x0000000003ULL, 0x0000000003ULL, 0ULL};
  tb::StateGenConfig cappedCfg = cfg1;
  cappedCfg.maxBoards = 2;
  const tb::StateGenResult capped = tb::generateStateSpace(cappedCfg, nullptr, &prior);
  CHECK(capped.sortedBoards.size() == 2);
  CHECK(capped.sortedBoards[0] == 0ULL);
  CHECK(capped.priorBoardsMerged == 2);
  CHECK(capped.siblingBoardsAdded == 0);

  // 6. Verify TB-guided generateStateSpace (with heurPly = 2 fallback) and measureCoverage.
  const std::string tmpTbPath = "build/test_gen_tb.tb.bin";
  REQUIRE(tb::writeTablebaseFile(tmpTbPath, r1.sortedBoards,
                                 std::vector<float>(r1.sortedBoards.size(), 5.0f), 0.0));
  tb::MappedTablebase mappedTb;
  REQUIRE(mappedTb.open(tmpTbPath));

  tb::StateGenConfig tbGenCfg{};
  tbGenCfg.numGames = 30;
  tbGenCfg.baseSeed = 55555ULL;
  tbGenCfg.heurPly = 2;
  tbGenCfg.epsilon = 0.10;
  tbGenCfg.exploreTopK = 3;
  tbGenCfg.minVisits = 1;
  tbGenCfg.maxBoards = 0;
  tbGenCfg.siblingMinVisits = 2;
  tbGenCfg.siblingPly = 1;
  tbGenCfg.numThreads = 2;
  const tb::StateGenResult tbGen = tb::generateStateSpace(tbGenCfg, &mappedTb, nullptr);
  CHECK_FALSE(tbGen.sortedBoards.empty());
  CHECK(tbGen.sortedBoards.front() == 0ULL);

  // 7. Verify measureCoverage: exact 100% hit-rate, partial coverage, numGames = 0/1, empty set.
  tb::StateGenConfig exactCfg{};
  exactCfg.numGames = 40;
  exactCfg.baseSeed = 88888ULL;
  exactCfg.heurPly = 1;
  exactCfg.epsilon = 0.0;
  exactCfg.minVisits = 1;
  exactCfg.maxBoards = 0;
  exactCfg.siblingMinVisits = 0;
  exactCfg.numThreads = 2;
  const tb::StateGenResult exactGen = tb::generateStateSpace(exactCfg);
  const tb::CoverageReport exactCov =
      tb::measureCoverage(exactGen.sortedBoards, 40, 88888ULL, nullptr, 1, 2);
  CHECK(exactCov.overallHitPct == doctest::Approx(100.0));
  CHECK(exactCov.fullyCoveredGamePct == doctest::Approx(100.0));
  CHECK(exactCov.byHoles.allTotal() == exactGen.totalMovesSimulated);

  const tb::CoverageReport partialCov =
      tb::measureCoverage(exactGen.sortedBoards, 30, 999999ULL, &mappedTb, 1, 2);
  CHECK(partialCov.overallHitPct < 100.0);
  CHECK(partialCov.byHoles.allTotal() > 0);

  const tb::CoverageReport zeroGamesCov =
      tb::measureCoverage(exactGen.sortedBoards, 0, 88888ULL, nullptr, 1, 1);
  CHECK(zeroGamesCov.games == 0);
  CHECK(zeroGamesCov.overallHitPct == 0.0);

  const tb::CoverageReport singleGameEmptySet = tb::measureCoverage({}, 1, 88888ULL, nullptr, 1, 1);
  CHECK(singleGameEmptySet.stdErrLines == 0.0);
  CHECK(singleGameEmptySet.overallHitPct == 0.0);

  mappedTb.close();
  std::remove(tmpTbPath.c_str());
}

TEST_CASE("Solver evaluation, TB frontier exploration, and iterative expansion monotonicity") {
  // 1. Test evaluateTablebasePolicy: closed-world MC agreement, live fallback, determinism (1T vs
  // 4T), and numGames=0/1 edge cases on a small exact 2-row board set.
  const std::vector<uint64_t> boards2x10 = tb::enumerateZeroSealedHoleBoards(1, 4, 2, 10);
  tb::ViSolveConfig viCfg{};
  viCfg.tolerance = 1e-9;
  viCfg.maxOuterIters = 100;
  viCfg.innerEvalSweeps = 20;
  viCfg.numThreads = 4;
  const tb::ViSolveResult solved2x10 = tb::solveValueIterationMPI(boards2x10, viCfg);
  REQUIRE(solved2x10.finalBellmanResidual <= 1e-9);

  const std::string tmpTb2x10 = "build/test_eval_2x10.tb.bin";
  REQUIRE(tb::writeTablebaseFile(tmpTb2x10, boards2x10, solved2x10.values,
                                 solved2x10.finalBellmanResidual));
  tb::MappedTablebase mapped2x10;
  REQUIRE(mapped2x10.open(tmpTb2x10));

  const tb::TablebaseEvalStats cw1 =
      tb::evaluateTablebasePolicy(mapped2x10, /*closedWorld=*/true, 2000, 12345ULL, 1, 1);
  const tb::TablebaseEvalStats cw4 =
      tb::evaluateTablebasePolicy(mapped2x10, /*closedWorld=*/true, 2000, 12345ULL, 4, 1);
  CHECK(cw1.meanLines == cw4.meanLines);
  CHECK(cw1.stdErrLines == cw4.stdErrLines);
  CHECK(cw1.medianLines == cw4.medianLines);
  CHECK(cw1.p90Lines == cw4.p90Lines);
  CHECK(cw1.maxLines == cw4.maxLines);
  CHECK(cw1.inTbMovePct == cw4.inTbMovePct);
  CHECK(cw1.fullyCoveredGamePct == cw4.fullyCoveredGamePct);
  CHECK(cw1.stepCapHits == 0);
  CHECK(std::abs(cw1.meanLines - solved2x10.emptyBoardValue) <= 4.0 * cw1.stdErrLines);

  const tb::TablebaseEvalStats live =
      tb::evaluateTablebasePolicy(mapped2x10, /*closedWorld=*/false, 400, 12345ULL, 4, 1);
  CHECK(live.games == 400);
  CHECK(live.stepCapHits == 0);
  CHECK(live.byHoles.allTotal() > 0);
  CHECK(live.inTbMovePct > 0.0);
  CHECK(live.meanLines > cw1.meanLines);

  // Verify stepCapHits triggers when maxSteps == 1 and fallbackMovePct == 100% on an empty TB.
  const tb::TablebaseEvalStats capEval = tb::evaluateTablebasePolicy(
      mapped2x10, /*closedWorld=*/false, 20, 12345ULL, 2, 1, tb::EvalWeights{}, /*maxSteps=*/1);
  CHECK(capEval.stepCapHits == 20);

  const tb::MappedTablebase unloadedTb;
  const tb::TablebaseEvalStats emptyTbEval =
      tb::evaluateTablebasePolicy(unloadedTb, /*closedWorld=*/false, 20, 12345ULL, 2, 1);
  CHECK(emptyTbEval.inTbMovePct == 0.0);
  CHECK(emptyTbEval.fallbackMovePct == doctest::Approx(100.0));

  const tb::TablebaseEvalStats zeroEval =
      tb::evaluateTablebasePolicy(mapped2x10, /*closedWorld=*/false, 0, 1ULL, 1, 1);
  CHECK(zeroEval.games == 0);
  CHECK(zeroEval.meanLines == 0.0);
  const tb::TablebaseEvalStats oneEval =
      tb::evaluateTablebasePolicy(mapped2x10, /*closedWorld=*/false, 1, 1ULL, 1, 1);
  CHECK(oneEval.games == 1);
  CHECK(oneEval.stdErrLines == 0.0);

  mapped2x10.close();
  std::remove(tmpTb2x10.c_str());

  // 2. Test TB frontier exploration: put 4 distinct suboptimal O moves in TB (excluding
  // heurBestO) with values 5.0, 4.0, 3.0, 2.0 so TB runner-up exploration can ONLY pick in-TB
  // moves while heuristic frontier probing discovers out-of-TB heurBestO.
  tb::TransitionList oMoves;
  tb::generateTransitions(0ULL, tb::parsePieceChar('O'), oMoves);
  REQUIRE(oMoves.size() == 9);
  const auto heurBestO = tb::pickBestTransition1Ply(0ULL, tb::parsePieceChar('O'));
  REQUIRE(heurBestO.found);
  std::vector<uint64_t> suboptOs;
  for (int i = 0; i < oMoves.size() && suboptOs.size() < 4; ++i) {
    if (oMoves[i].afterBoard != heurBestO.transition.afterBoard) {
      suboptOs.push_back(oMoves[i].afterBoard);
    }
  }
  REQUIRE(suboptOs.size() == 4);
  const uint64_t tbGreedySubopt = suboptOs[0];
  std::vector<std::pair<uint64_t, float>> tinyEntries = {
      {0ULL, 1.0f},        {suboptOs[0], 5.0f}, {suboptOs[1], 4.0f},
      {suboptOs[2], 3.0f}, {suboptOs[3], 2.0f},
  };
  std::sort(tinyEntries.begin(), tinyEntries.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<uint64_t> tinyKeys;
  std::vector<float> tinyVals;
  for (const auto &kv : tinyEntries) {
    tinyKeys.push_back(kv.first);
    tinyVals.push_back(kv.second);
  }
  const std::string tmpTinyTb = "build/test_frontier_tiny.tb.bin";
  REQUIRE(tb::writeTablebaseFile(tmpTinyTb, tinyKeys, tinyVals, 0.0));
  tb::MappedTablebase tinyTb;
  REQUIRE(tinyTb.open(tmpTinyTb));

  tb::Rng probeRng(777ULL);
  bool sawHeurBest = false;
  bool sawInTbRunnerUp = false;
  for (int i = 0; i < 80; ++i) {
    const auto mv = tb::selectRolloutTransition(0ULL, tb::parsePieceChar('O'), probeRng, &tinyTb,
                                                /*heurPly=*/1, /*epsilon=*/1.0, /*exploreTopK=*/4,
                                                tb::EvalWeights{});
    REQUIRE(mv.found);
    CHECK(mv.transition.afterBoard != tbGreedySubopt);
    if (mv.transition.afterBoard == heurBestO.transition.afterBoard) {
      sawHeurBest = true;
    }
    if (mv.transition.afterBoard == suboptOs[1]
        || mv.transition.afterBoard == suboptOs[2]
        || mv.transition.afterBoard == suboptOs[3]) {
      sawInTbRunnerUp = true;
    }
  }
  CHECK(sawHeurBest);
  CHECK(sawInTbRunnerUp);

  // Also pin excludeBoard when tbGreedyBoard == heurBestO: epsilon=1.0 must never repeat heurBestO.
  const std::string tmpAgreeTb = "build/test_frontier_agree.tb.bin";
  REQUIRE(heurBestO.transition.afterBoard > 0ULL);
  REQUIRE(tb::writeTablebaseFile(tmpAgreeTb, {0ULL, heurBestO.transition.afterBoard}, {1.0f, 9.0f},
                                 0.0));
  tb::MappedTablebase agreeTb;
  REQUIRE(agreeTb.open(tmpAgreeTb));
  for (int i = 0; i < 60; ++i) {
    const auto mv = tb::selectRolloutTransition(0ULL, tb::parsePieceChar('O'), probeRng, &agreeTb,
                                                /*heurPly=*/1, /*epsilon=*/1.0, /*exploreTopK=*/4,
                                                tb::EvalWeights{});
    REQUIRE(mv.found);
    CHECK(mv.transition.afterBoard != heurBestO.transition.afterBoard);
  }
  agreeTb.close();
  std::remove(tmpAgreeTb.c_str());

  // Verify expandCoreSiblings with a loaded TB includes the out-of-TB heuristic reply on 0ULL.
  tb::StateGenConfig sibCfg{};
  sibCfg.numGames = 10;
  sibCfg.baseSeed = 99ULL;
  sibCfg.heurPly = 1;
  sibCfg.epsilon = 0.0;
  sibCfg.minVisits = 100; // Only 0ULL and sibling replies survive
  sibCfg.maxBoards = 0;
  sibCfg.siblingMinVisits = 1;
  sibCfg.siblingPly = 1;
  sibCfg.numThreads = 2;
  const tb::StateGenResult sibRes = tb::generateStateSpace(sibCfg, &tinyTb, nullptr);
  CHECK(std::binary_search(sibRes.sortedBoards.begin(), sibRes.sortedBoards.end(),
                           heurBestO.transition.afterBoard));
  CHECK(std::binary_search(sibRes.sortedBoards.begin(), sibRes.sortedBoards.end(), tbGreedySubopt));

  tinyTb.close();
  std::remove(tmpTinyTb.c_str());

  // 3. Mini iterative expansion monotonicity: Round 0 (pure heuristic) -> Solve TB_0 -> Round 1
  // (TB_0 rollouts + merge S_0) -> Solve TB_1 must satisfy |S_1| >= |S_0| and V_1(0) >= V_0(0).
  tb::StateGenConfig r0Cfg{};
  r0Cfg.numGames = 200;
  r0Cfg.baseSeed = 202600ULL;
  r0Cfg.heurPly = 1;
  r0Cfg.epsilon = 0.08;
  r0Cfg.exploreTopK = 4;
  r0Cfg.minVisits = 2;
  r0Cfg.maxBoards = 0;
  r0Cfg.siblingMinVisits = 4;
  r0Cfg.siblingPly = 1;
  r0Cfg.numThreads = 4;
  const tb::StateGenResult r0 = tb::generateStateSpace(r0Cfg);
  REQUIRE_FALSE(r0.sortedBoards.empty());

  tb::ViSolveConfig miniVi{};
  miniVi.tolerance = 1e-8;
  miniVi.maxOuterIters = 150;
  miniVi.innerEvalSweeps = 25;
  miniVi.numThreads = 4;
  const tb::ViSolveResult v0 = tb::solveValueIterationMPI(r0.sortedBoards, miniVi);
  REQUIRE(v0.finalBellmanResidual <= 1e-8);
  CHECK(v0.emptyBoardValue > 0.0);

  const std::string tmpR0Tb = "build/test_iter_r0.tb.bin";
  REQUIRE(tb::writeTablebaseFile(tmpR0Tb, r0.sortedBoards, v0.values, v0.finalBellmanResidual));
  tb::MappedTablebase mappedR0;
  REQUIRE(mappedR0.open(tmpR0Tb));

  tb::StateGenConfig r1Cfg = r0Cfg;
  r1Cfg.baseSeed = 202601ULL;
  r1Cfg.numGames = 400;
  const tb::StateGenResult r1 = tb::generateStateSpace(r1Cfg, &mappedR0, &r0.sortedBoards);
  CHECK(r1.sortedBoards.size() >= r0.sortedBoards.size());
  CHECK(r1.priorBoardsMerged == r0.sortedBoards.size());

  const tb::ViSolveResult v1 = tb::solveValueIterationMPI(r1.sortedBoards, miniVi);
  REQUIRE(v1.finalBellmanResidual <= 1e-8);
  CHECK(v1.emptyBoardValue >= v0.emptyBoardValue - 1e-6);

  mappedR0.close();
  std::remove(tmpR0Tb.c_str());
}

TEST_CASE("NES RNG transition matrix, stationary distribution, and Next-Box heuristic/hybrid") {
  // 1. Verify exact NES transition table row sums, exact stationary equation pi * P = pi, and
  // exact match against the canonical T, J, Z, O, S, L, I NES ROM transition probabilities.
  uint64_t statSum = 0;
  for (int i = 0; i < tb::NUM_PIECES; ++i) {
    int rowSum = 0;
    for (int j = 0; j < tb::NUM_PIECES; ++j) {
      rowSum += tb::NES_TRANSITION_NUM[i][j];
    }
    CHECK(rowSum == tb::NES_TRANSITION_DENOM);
    statSum += tb::NES_STATIONARY_NUM[i];
  }
  CHECK(statSum == tb::NES_STATIONARY_DENOM);

  for (int next = 0; next < tb::NUM_PIECES; ++next) {
    uint64_t weighted = 0;
    for (int prev = 0; prev < tb::NUM_PIECES; ++prev) {
      weighted +=
          tb::NES_STATIONARY_NUM[prev] * static_cast<uint64_t>(tb::NES_TRANSITION_NUM[prev][next]);
    }
    CHECK(weighted
          == tb::NES_STATIONARY_NUM[next] * static_cast<uint64_t>(tb::NES_TRANSITION_DENOM));
  }

  // Spot-check key NES asymmetric pairs: I->S=12, I->I=2, O->O=4, S->S=4, L->T=12, J->T=12,
  // T->Z=12, Z->J=12.
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('I')][tb::parsePieceChar('S')] == 12);
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('I')][tb::parsePieceChar('I')] == 2);
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('O')][tb::parsePieceChar('O')] == 4);
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('S')][tb::parsePieceChar('S')] == 4);
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('L')][tb::parsePieceChar('T')] == 12);
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('J')][tb::parsePieceChar('T')] == 12);
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('T')][tb::parsePieceChar('Z')] == 12);
  CHECK(tb::NES_TRANSITION_NUM[tb::parsePieceChar('Z')][tb::parsePieceChar('J')] == 12);

  tb::RngMode parsedMode = tb::RngMode::Uniform;
  CHECK(tb::parseRngMode("nes", parsedMode));
  CHECK(parsedMode == tb::RngMode::Nes);
  CHECK(std::string(tb::rngModeName(parsedMode)) == "nes");
  CHECK(tb::parseRngMode("uniform", parsedMode));
  CHECK(parsedMode == tb::RngMode::Uniform);
  CHECK(std::string(tb::rngModeName(parsedMode)) == "uniform");
  CHECK_FALSE(tb::parseRngMode("NES", parsedMode));
  CHECK_FALSE(tb::parseRngMode("", parsedMode));
  CHECK(tb::openingPieceProb(tb::RngMode::Uniform, 0) == doctest::Approx(1.0 / 7.0));
  CHECK(tb::pieceTransitionProb(tb::RngMode::Uniform, 0, 0) == doctest::Approx(1.0 / 7.0));

  // Verify Uniform mode delegates to nextPiece() identically across opening & transition draws.
  {
    tb::Rng rA(999ULL);
    tb::Rng rB(999ULL);
    int pA = rA.nextOpeningPiece(tb::RngMode::Uniform);
    int pB = rB.nextPiece();
    CHECK(pA == pB);
    for (int k = 0; k < 50; ++k) {
      pA = rA.nextPiece(tb::RngMode::Uniform, pA);
      pB = rB.nextPiece();
      CHECK(pA == pB);
    }
  }

  // 2. Verify empirical NES RNG stream matches stationary frequencies and low repeat rate (2/64
  // for I/L/J/T/Z, 4/64 for O/S) over 200,000 draws.
  tb::Rng nesRng(123456ULL);
  int piece = nesRng.nextOpeningPiece(tb::RngMode::Nes);
  uint64_t counts[tb::NUM_PIECES] = {};
  uint64_t iRepeats = 0;
  uint64_t iTotal = 0;
  constexpr uint64_t N_DRAWS = 200000;
  for (uint64_t k = 0; k < N_DRAWS; ++k) {
    ++counts[piece];
    const int next = nesRng.nextPiece(tb::RngMode::Nes, piece);
    if (piece == tb::parsePieceChar('I')) {
      ++iTotal;
      if (next == piece) {
        ++iRepeats;
      }
    }
    piece = next;
  }
  for (int p = 0; p < tb::NUM_PIECES; ++p) {
    const double emp = static_cast<double>(counts[p]) / static_cast<double>(N_DRAWS);
    CHECK(std::abs(emp - tb::openingPieceProb(tb::RngMode::Nes, p)) < 0.004);
  }
  const double empIRepeat = static_cast<double>(iRepeats) / static_cast<double>(iTotal);
  const double expectedIRepeat =
      tb::pieceTransitionProb(tb::RngMode::Nes, tb::parsePieceChar('I'), tb::parsePieceChar('I'));
  CHECK(std::abs(empIRepeat - expectedIRepeat) < 0.005);

  // 3. Test pickBestNextBoxHeuristic & chooseNextBoxHybridMove:
  //    (a) Top-out returns found == false.
  const uint64_t tetrisReady = mustParse("XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.");
  CHECK_FALSE(
      tb::pickBestNextBoxHeuristic(tetrisReady, tb::parsePieceChar('O'), tb::parsePieceChar('I'))
          .found);

  //    (b) Ply-2 top-out avoidance: on a 3-wide open well, only vertical I in col 7 leaves a
  //        2-wide column pair (cols 8..9) where nextPiece='O' survives.
  const uint64_t narrowO = mustParse("XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...");
  const auto avoidDeath =
      tb::pickBestNextBoxHeuristic(narrowO, tb::parsePieceChar('I'), tb::parsePieceChar('O'));
  REQUIRE(avoidDeath.found);
  tb::TransitionList oAfterI;
  tb::generateTransitions(avoidDeath.transition.afterBoard, tb::parsePieceChar('O'), oAfterI);
  CHECK_FALSE(oAfterI.empty());

  //    (c) On a board where O clears 2 full rows, knowing nextPiece='I' matches
  //        scoreNextBoxHeuristicTransition exactly.
  const uint64_t lowBoard = mustParse("..........\n"
                                      "..........\n"
                                      "XXXX..XXXX\n"
                                      "XXXX..XXXX");
  const auto nbHeur =
      tb::pickBestNextBoxHeuristic(lowBoard, tb::parsePieceChar('O'), tb::parsePieceChar('I'));
  REQUIRE(nbHeur.found);
  CHECK(nbHeur.score
        == tb::scoreNextBoxHeuristicTransition(
            nbHeur.transition.afterBoard, nbHeur.transition.linesCleared, tb::parsePieceChar('I')));
  CHECK(nbHeur.transition.afterBoard == 0ULL);
  CHECK(nbHeur.transition.linesCleared == 2);

  //    (d) Hybrid move selector: place a single high-value target leaf b2 in a tiny NNB TB and
  //        verify chooseNextBoxHybridMove steers curPiece='O' to chosenMid so nextPiece='I' reaches
  //        b2, handles ply-1 top-out, inner-loop in-TB vs out-of-TB q ties, outer-loop equal-Q
  //        heuristic tie-breaking, and unloaded-TB fallback cleanly.
  tb::TransitionList oMoves;
  tb::generateTransitions(0ULL, tb::parsePieceChar('O'), oMoves);
  REQUIRE(oMoves.size() == 9);
  const uint64_t chosenMid = oMoves[7].afterBoard;
  tb::TransitionList iMovesFromMid;
  tb::generateTransitions(chosenMid, tb::parsePieceChar('I'), iMovesFromMid);
  REQUIRE_FALSE(iMovesFromMid.empty());
  const uint64_t targetLeaf = iMovesFromMid[0].afterBoard;

  const std::string tmpHybridTb = "build/test_nb_hybrid.tb.bin";
  REQUIRE(tb::writeTablebaseFile(tmpHybridTb, {targetLeaf}, {50.0f}, 0.0));
  tb::MappedTablebase hybridTb;
  REQUIRE(hybridTb.open(tmpHybridTb));

  CHECK_FALSE(tb::chooseNextBoxHybridMove(tetrisReady, tb::parsePieceChar('O'),
                                          tb::parsePieceChar('I'), hybridTb)
                  .found);

  const auto hyb =
      tb::chooseNextBoxHybridMove(0ULL, tb::parsePieceChar('O'), tb::parsePieceChar('I'), hybridTb);
  REQUIRE(hyb.found);
  CHECK(hyb.transition.afterBoard == chosenMid);
  CHECK(hyb.afterInTablebase);
  CHECK_FALSE(hyb.usedFallback);
  CHECK(hyb.tablebaseQ
        == doctest::Approx(50.0 + static_cast<double>(iMovesFromMid[0].linesCleared)));

  // Inner-loop tie check: targetLeaf has value 0.0f (same q=0.0 as out-of-TB leaves) at j > 0;
  // branchBestInTb must still report true for the winning branch.
  const uint64_t lateLeaf = iMovesFromMid[iMovesFromMid.size() - 1].afterBoard;
  const std::string tmpTieTb = "build/test_nb_hybrid_tie.tb.bin";
  REQUIRE(tb::writeTablebaseFile(tmpTieTb, {lateLeaf}, {0.0f}, 0.0));
  tb::MappedTablebase tieTb;
  REQUIRE(tieTb.open(tmpTieTb));
  // With all branches tied at q=0.0, outer-loop heuristic tie-breaking picks the exact same move as
  // pickBestNextBoxHeuristic; if we instead give two distinct O branches' leaves equal value 25.0f,
  // outer-loop tie-break selects whichever of those two has higher Next-Box heuristic score.
  const auto zeroTie =
      tb::chooseNextBoxHybridMove(0ULL, tb::parsePieceChar('O'), tb::parsePieceChar('I'), tieTb);
  REQUIRE(zeroTie.found);
  CHECK_FALSE(zeroTie.usedFallback);
  CHECK(zeroTie.transition.afterBoard
        == tb::pickBestNextBoxHeuristic(0ULL, tb::parsePieceChar('O'), tb::parsePieceChar('I'))
               .transition.afterBoard);
  tieTb.close();

  // Direct inner-loop in-TB tie check on a branch where j == 0 is out-of-TB (q=0) and last j is
  // in-TB with value 0.0f on a board with only 1 ply-1 move (vertical I on tetrisReady cleared ->
  // 0ULL, nextPiece='O' has 9 moves, last O move in TB with 0.0f).
  tb::TransitionList oOnEmpty;
  tb::generateTransitions(0ULL, tb::parsePieceChar('O'), oOnEmpty);
  REQUIRE(oOnEmpty.size() == 9);
  REQUIRE(
      tb::writeTablebaseFile(tmpTieTb, {oOnEmpty[oOnEmpty.size() - 1].afterBoard}, {0.0f}, 0.0));
  REQUIRE(tieTb.open(tmpTieTb));
  const auto innerTie = tb::chooseNextBoxHybridMove(tetrisReady, tb::parsePieceChar('I'),
                                                    tb::parsePieceChar('O'), tieTb);
  REQUIRE(innerTie.found);
  CHECK(innerTie.afterInTablebase);
  CHECK(innerTie.tablebaseQ == doctest::Approx(4.0));
  tieTb.close();
  std::remove(tmpTieTb.c_str());

  // With unloaded TB, hybrid falls back cleanly to pickBestNextBoxHeuristic with 2-ply r1+max_r2.
  const tb::MappedTablebase emptyTb;
  const auto hybFb = tb::chooseNextBoxHybridMove(lowBoard, tb::parsePieceChar('O'),
                                                 tb::parsePieceChar('I'), emptyTb);
  REQUIRE(hybFb.found);
  CHECK(hybFb.usedFallback);
  CHECK_FALSE(hybFb.afterInTablebase);
  CHECK(hybFb.transition.afterBoard == nbHeur.transition.afterBoard);
  CHECK(hybFb.tablebaseQ == doctest::Approx(2.0));

  hybridTb.close();
  std::remove(tmpHybridTb.c_str());

  // 4. Verify /moves?next=<piece> route validation & hybrid scoring.
  tb::ServerContext nbCtx;
  const auto badNext = tb::handleApiRequest("/moves", "board=0&piece=O&next=Q", nbCtx);
  CHECK(badNext.status == 400);
  const auto okNext = tb::handleApiRequest("/moves", "board=0&piece=O&next=I", nbCtx);
  CHECK(okNext.status == 200);
  CHECK(okNext.body.find("\"nextPieceChar\":\"I\"") != std::string::npos);
}
