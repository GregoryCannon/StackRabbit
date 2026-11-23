#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../src/tests/vendor/doctest.h"

#include "core.hpp"
#include "heuristic.hpp"
#include "server_routes.hpp"
#include "tablebase_io.hpp"

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
