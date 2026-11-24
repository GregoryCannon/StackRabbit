#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../src/tests/vendor/doctest.h"

#include "core.hpp"
#include "distill_data.hpp"
#include "distill_linear.hpp"
#include "heuristic.hpp"
#include "nextbox_gen.hpp"
#include "nextbox_solver.hpp"
#include "server_routes.hpp"
#include "state_generator.hpp"
#include "tablebase_io.hpp"
#include "tiny_nn.hpp"
#include "vi_solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

TEST_CASE("SRTBV002 Next-Box binary I/O, format detection, and O(1) IndexedTablebase parity") {
  // 1. Round-trip SRTBV002 write/read (Nes & Uniform), empty-board expectation, and format detect.
  const std::string tmpV1 = "build/test_fmt_v1.tb.bin";
  const std::string tmpV2 = "build/test_fmt_v2.tb.bin";
  const std::string tmpV2NoEmpty = "build/test_fmt_v2_noempty.tb.bin";
  const std::string tmpCorrupt = "build/test_fmt_v2_corrupt.tb.bin";

  const std::vector<uint64_t> boards = {0ULL, 0x0000000003ULL, 0x000000000FULL, 0x000000003FULL};
  std::vector<float> valsV1 = {10.5f, 9.25f, 8.75f, 7.125f};
  std::vector<float> valsV2(boards.size() * tb::NUM_PIECES, 0.0f);
  for (size_t i = 0; i < boards.size(); ++i) {
    for (int c = 0; c < tb::NUM_PIECES; ++c) {
      valsV2[i * tb::NUM_PIECES + static_cast<size_t>(c)] =
          static_cast<float>(50.0 - static_cast<double>(i) * 3.5 + static_cast<double>(c) * 0.75);
    }
  }

  REQUIRE(tb::writeTablebaseFile(tmpV1, boards, valsV1, 1.25e-8));
  REQUIRE(tb::writeNextBoxTablebaseFile(tmpV2, boards, valsV2, tb::RngMode::Nes, 2.5e-8));

  tb::TablebaseFormat fmt = tb::TablebaseFormat::Unknown;
  REQUIRE(tb::detectTablebaseFormat(tmpV1, fmt));
  CHECK(fmt == tb::TablebaseFormat::NnbV1);
  REQUIRE(tb::detectTablebaseFormat(tmpV2, fmt));
  CHECK(fmt == tb::TablebaseFormat::NextBoxV2);
  CHECK_FALSE(tb::detectTablebaseFormat("build/nonexistent_tb_file.bin", fmt));
  CHECK(fmt == tb::TablebaseFormat::Unknown);

  tb::MappedNextBoxTablebase mappedV2;
  REQUIRE(mappedV2.open(tmpV2, nullptr, /*verifySorted=*/true));
  CHECK(mappedV2.isLoaded());
  CHECK(mappedV2.size() == boards.size());
  CHECK(mappedV2.rngMode() == tb::RngMode::Nes);
  CHECK(mappedV2.bellmanResidual() == doctest::Approx(2.5e-8));

  double expectedEmptyV2 = 0.0;
  for (int c = 0; c < tb::NUM_PIECES; ++c) {
    expectedEmptyV2 += tb::openingPieceProb(tb::RngMode::Nes, c) * static_cast<double>(valsV2[c]);
  }
  CHECK(mappedV2.emptyBoardValue() == doctest::Approx(expectedEmptyV2));

  // Verify 0ULL absent writes emptyBoardValue == -1.0 sentinel.
  const std::vector<uint64_t> noEmptyBoards = {0x0000000003ULL, 0x000000000FULL};
  const std::vector<float> noEmptyVals(noEmptyBoards.size() * tb::NUM_PIECES, 12.0f);
  REQUIRE(tb::writeNextBoxTablebaseFile(tmpV2NoEmpty, noEmptyBoards, noEmptyVals,
                                        tb::RngMode::Uniform, 0.0));
  tb::MappedNextBoxTablebase mappedNoEmpty;
  REQUIRE(mappedNoEmpty.open(tmpV2NoEmpty));
  CHECK(mappedNoEmpty.rngMode() == tb::RngMode::Uniform);
  CHECK(mappedNoEmpty.emptyBoardValue() == doctest::Approx(-1.0));
  mappedNoEmpty.close();

  // 2. IndexedTablebase & IndexedNextBoxTablebase exact lookup parity vs Mapped versions.
  tb::MappedTablebase mappedV1;
  REQUIRE(mappedV1.open(tmpV1));
  tb::IndexedTablebase idxV1(mappedV1);
  CHECK(idxV1.isLoaded());
  CHECK(idxV1.size() == mappedV1.size());
  for (size_t i = 0; i < boards.size(); ++i) {
    float mv = -1.0f;
    float iv = -2.0f;
    CHECK(mappedV1.lookup(boards[i], mv));
    CHECK(idxV1.lookup(boards[i], iv));
    CHECK(mv == iv);
    CHECK(iv == valsV1[i]);
  }
  float dummy = 0.0f;
  CHECK_FALSE(idxV1.lookup(0x123456ULL, dummy));
  CHECK_FALSE(idxV1.lookup(~0ULL, dummy));

  tb::IndexedNextBoxTablebase idxV2(mappedV2);
  CHECK(idxV2.isLoaded());
  CHECK(idxV2.size() == mappedV2.size());
  CHECK(idxV2.rngMode() == tb::RngMode::Nes);
  for (size_t i = 0; i < boards.size(); ++i) {
    float mArr[tb::NUM_PIECES] = {};
    float iArr[tb::NUM_PIECES] = {};
    CHECK(mappedV2.lookup(boards[i], mArr));
    CHECK(idxV2.lookup(boards[i], iArr));
    for (int c = 0; c < tb::NUM_PIECES; ++c) {
      CHECK(mArr[c] == iArr[c]);
      float mSingle = -1.0f;
      float iSingle = -2.0f;
      CHECK(mappedV2.lookupPiece(boards[i], c, mSingle));
      CHECK(idxV2.lookupPiece(boards[i], c, iSingle));
      CHECK(mSingle == iSingle);
      CHECK(iSingle == valsV2[i * tb::NUM_PIECES + static_cast<size_t>(c)]);
    }
  }
  float missingArr[tb::NUM_PIECES] = {};
  CHECK_FALSE(idxV2.lookup(0x123456ULL, missingArr));
  CHECK_FALSE(idxV2.lookup(~0ULL, missingArr));
  CHECK_FALSE(idxV2.lookupPiece(0x123456ULL, 0, dummy));

  // Unloaded Indexed wrappers return false safely.
  tb::IndexedTablebase emptyIdxV1;
  CHECK_FALSE(emptyIdxV1.isLoaded());
  CHECK(emptyIdxV1.size() == 0);
  CHECK_FALSE(emptyIdxV1.lookup(0ULL, dummy));
  tb::IndexedNextBoxTablebase emptyIdxV2;
  CHECK_FALSE(emptyIdxV2.isLoaded());
  CHECK(emptyIdxV2.size() == 0);
  CHECK_FALSE(emptyIdxV2.lookup(0ULL, missingArr));

  // Verify chooseTablebaseMove, chooseNextBoxHybridMove, and selectRolloutTransition produce
  // bit-identical choices on Mapped vs Indexed tablebases when actually hitting in-TB boards.
  {
    std::vector<uint64_t> reachBoards = {0ULL};
    for (int c = 0; c < tb::NUM_PIECES; ++c) {
      tb::TransitionList tl1;
      tb::generateTransitions(0ULL, c, tl1);
      REQUIRE_FALSE(tl1.empty());
      reachBoards.push_back(tl1[0].afterBoard);
      tb::TransitionList tl2;
      tb::generateTransitions(tl1[0].afterBoard, (c + 1) % tb::NUM_PIECES, tl2);
      REQUIRE_FALSE(tl2.empty());
      reachBoards.push_back(tl2[0].afterBoard);
    }
    std::sort(reachBoards.begin(), reachBoards.end());
    reachBoards.erase(std::unique(reachBoards.begin(), reachBoards.end()), reachBoards.end());
    std::vector<float> reachVals(reachBoards.size());
    for (size_t i = 0; i < reachBoards.size(); ++i) {
      reachVals[i] = 10.0f + static_cast<float>(i) * 0.5f;
    }

    const std::string tmpReachV1 = "build/test_fmt_reach_v1.tb.bin";
    REQUIRE(tb::writeTablebaseFile(tmpReachV1, reachBoards, reachVals, 0.0));
    tb::MappedTablebase mReach;
    REQUIRE(mReach.open(tmpReachV1));
    tb::IndexedTablebase iReach(mReach);

    for (int c = 0; c < tb::NUM_PIECES; ++c) {
      const auto mChoice = tb::chooseTablebaseMove(0ULL, c, mReach, false, 1);
      const auto iChoice = tb::chooseTablebaseMove(0ULL, c, iReach, false, 1);
      CHECK(mChoice.afterInTablebase);
      CHECK_FALSE(mChoice.usedFallback);
      CHECK(mChoice.transition.afterBoard == iChoice.transition.afterBoard);
      CHECK(mChoice.tablebaseQ == iChoice.tablebaseQ);
      CHECK(mChoice.afterInTablebase == iChoice.afterInTablebase);

      const auto mHyb = tb::chooseNextBoxHybridMove(0ULL, c, (c + 1) % tb::NUM_PIECES, mReach);
      const auto iHyb = tb::chooseNextBoxHybridMove(0ULL, c, (c + 1) % tb::NUM_PIECES, iReach);
      CHECK(mHyb.afterInTablebase);
      CHECK_FALSE(mHyb.usedFallback);
      CHECK(mHyb.transition.afterBoard == iHyb.transition.afterBoard);
      CHECK(mHyb.tablebaseQ == iHyb.tablebaseQ);

      // Direct selectRolloutTransition parity under both greedy (eps=0) and explore (eps=1).
      tb::Rng rMap(500ULL + static_cast<uint64_t>(c));
      tb::Rng rIdx(500ULL + static_cast<uint64_t>(c));
      for (double eps : {0.0, 1.0}) {
        const auto mRoll =
            tb::selectRolloutTransition(0ULL, c, rMap, &mReach, 1, eps, 4, tb::EvalWeights{});
        const auto iRoll =
            tb::selectRolloutTransition(0ULL, c, rIdx, &iReach, 1, eps, 4, tb::EvalWeights{});
        CHECK(mRoll.transition.afterBoard == iRoll.transition.afterBoard);
        CHECK(mRoll.score == iRoll.score);
      }
    }

    // Verify rebuilding an already-populated IndexedTablebase and rebuilding from unloaded mapped.
    iReach.buildFromMapped(mappedV1);
    CHECK(iReach.size() == mappedV1.size());
    tb::MappedTablebase unloadedV1;
    iReach.buildFromMapped(unloadedV1);
    CHECK_FALSE(iReach.isLoaded());
    CHECK(iReach.size() == 0);

    mReach.close();
    std::remove(tmpReachV1.c_str());
  }

  // Verify 0-board SRTBV002 round-trip, move semantics, and IndexedNextBoxTablebase rebuild.
  {
    const std::string tmpZeroV2 = "build/test_fmt_v2_zero.tb.bin";
    REQUIRE(tb::writeNextBoxTablebaseFile(tmpZeroV2, {}, {}, tb::RngMode::Uniform, 0.0));
    tb::MappedNextBoxTablebase mZero;
    REQUIRE(mZero.open(tmpZeroV2));
    CHECK(mZero.isLoaded());
    CHECK(mZero.size() == 0);
    CHECK(mZero.emptyBoardValue() == doctest::Approx(tb::MISSING_BOARD_VALUE));
    CHECK_FALSE(mZero.lookup(0ULL, missingArr));
    CHECK_FALSE(mZero.lookupPiece(0ULL, 0, dummy));

    // Move constructor & move assignment
    tb::MappedNextBoxTablebase mMoved(std::move(mZero));
    CHECK(mMoved.isLoaded());
    CHECK_FALSE(mZero.isLoaded());
    CHECK(mZero.path().empty());

    tb::IndexedNextBoxTablebase iZero(mMoved);
    CHECK(iZero.isLoaded());
    CHECK(iZero.size() == 0);
    CHECK_FALSE(iZero.lookup(0ULL, missingArr));
    iZero.buildFromMapped(mZero); // Unloaded -> clears cleanly
    CHECK_FALSE(iZero.isLoaded());

    mMoved.close();
    std::remove(tmpZeroV2.c_str());
  }

  // 3. Corruption & validation rejection tests on writeNextBoxTablebaseFile &
  // MappedNextBoxTablebase.
  std::string err;
  CHECK_FALSE(
      tb::writeNextBoxTablebaseFile(tmpCorrupt, boards, {1.0f}, tb::RngMode::Uniform, 0.0, &err));
  std::vector<float> nanVals = valsV2;
  nanVals[3] = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(
      tb::writeNextBoxTablebaseFile(tmpCorrupt, boards, nanVals, tb::RngMode::Uniform, 0.0, &err));
  std::vector<float> negVals = valsV2;
  negVals[2] = -0.5f;
  CHECK_FALSE(
      tb::writeNextBoxTablebaseFile(tmpCorrupt, boards, negVals, tb::RngMode::Uniform, 0.0, &err));
  CHECK_FALSE(tb::writeNextBoxTablebaseFile(tmpCorrupt, {0x03ULL, 0x03ULL},
                                            std::vector<float>(14, 1.0f), tb::RngMode::Uniform, 0.0,
                                            &err));

  // Wrong magic (trying to open SRTBV001 as SRTBV002).
  tb::MappedNextBoxTablebase badOpen;
  CHECK_FALSE(badOpen.open(tmpV1, &err));

  // Corrupt header & record fields using exact struct offsets.
  {
    std::ifstream src(tmpV2, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(src)),
                            std::istreambuf_iterator<char>());
    REQUIRE(bytes.size()
            == sizeof(tb::NextBoxTablebaseHeader)
                   + boards.size() * sizeof(tb::PackedNextBoxRecord));

    const auto writeBytes = [&](const std::vector<char> &buf) {
      std::ofstream(tmpCorrupt, std::ios::binary | std::ios::trunc)
          .write(buf.data(), static_cast<std::streamsize>(buf.size()));
    };

    // Invalid rngMode = 2
    std::vector<char> badRng = bytes;
    const uint32_t badMode = 2U;
    std::memcpy(badRng.data() + offsetof(tb::NextBoxTablebaseHeader, rngMode), &badMode,
                sizeof(badMode));
    writeBytes(badRng);
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err));

    // Non-zero flags = 1
    std::vector<char> badFlags = bytes;
    const uint32_t nonZeroFlag = 1U;
    std::memcpy(badFlags.data() + offsetof(tb::NextBoxTablebaseHeader, flags), &nonZeroFlag,
                sizeof(nonZeroFlag));
    writeBytes(badFlags);
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err));

    // Negative emptyBoardValue != -1.0 sentinel
    std::vector<char> badEmpty = bytes;
    const double negEmpty = -2.5;
    std::memcpy(badEmpty.data() + offsetof(tb::NextBoxTablebaseHeader, emptyBoardValue), &negEmpty,
                sizeof(negEmpty));
    writeBytes(badEmpty);
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err));

    // Truncated header (< 40B) and truncated payload
    writeBytes(std::vector<char>(bytes.begin(), bytes.begin() + 20));
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err));
    writeBytes(std::vector<char>(bytes.begin(), bytes.end() - 4));
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err));

    // Exact 64-bit multiplication wrap-around trap: (2^62 + N) * 36 === N * 36 (mod 2^64).
    std::vector<char> badOverflow = bytes;
    const uint64_t hugeCount = 0x4000000000000000ULL + static_cast<uint64_t>(boards.size());
    std::memcpy(badOverflow.data() + offsetof(tb::NextBoxTablebaseHeader, numBoards), &hugeCount,
                sizeof(hugeCount));
    writeBytes(badOverflow);
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err));

    // Unsorted records & corrupt >40-bit board & negative record value caught by verifySorted=true.
    std::vector<char> badSort = bytes;
    const size_t rec0Off = sizeof(tb::NextBoxTablebaseHeader);
    const size_t rec1Off = rec0Off + sizeof(tb::PackedNextBoxRecord);
    std::swap_ranges(badSort.begin() + static_cast<std::ptrdiff_t>(rec0Off),
                     badSort.begin() + static_cast<std::ptrdiff_t>(rec1Off),
                     badSort.begin() + static_cast<std::ptrdiff_t>(rec1Off));
    writeBytes(badSort);
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err, /*verifySorted=*/true));

    std::vector<char> badRecNeg = bytes;
    const float negV = -1.0f;
    std::memcpy(badRecNeg.data() + rec0Off + 8, &negV, sizeof(negV));
    writeBytes(badRecNeg);
    CHECK_FALSE(badOpen.open(tmpCorrupt, &err, /*verifySorted=*/true));
  }

  mappedV1.close();
  mappedV2.close();
  std::remove(tmpV1.c_str());
  std::remove(tmpV2.c_str());
  std::remove(tmpV2NoEmpty.c_str());
  std::remove(tmpCorrupt.c_str());
}

TEST_CASE("Next-Box V(b,c) MPI solver: packing, Ram vs Disk parity, thread determinism, warm-start,"
          " and exact MC verification") {
  // 1. Packed action round-trip tests.
  CHECK(tb::unpackNextBoxChild(tb::packNextBoxAction(tb::INVALID_BOARD_ID, 0))
        == tb::INVALID_BOARD_ID);
  CHECK(tb::unpackNextBoxReward(tb::packNextBoxAction(tb::INVALID_BOARD_ID, 4)) == 4);
  CHECK(tb::unpackNextBoxChild(tb::packNextBoxAction(0U, 0)) == 0U);
  CHECK(tb::unpackNextBoxChild(tb::packNextBoxAction(123456789U, 3)) == 123456789U);
  CHECK(tb::unpackNextBoxReward(tb::packNextBoxAction(123456789U, 3)) == 3);
  CHECK(tb::unpackNextBoxChild(tb::packNextBoxAction(tb::NB_ACTION_OUT_CHILD - 1U, 4))
        == tb::NB_ACTION_OUT_CHILD - 1U);

  tb::PolicyCacheMode pm = tb::PolicyCacheMode::Auto;
  CHECK(tb::parsePolicyCacheMode("auto", pm));
  CHECK(pm == tb::PolicyCacheMode::Auto);
  CHECK(tb::parsePolicyCacheMode("ram", pm));
  CHECK(pm == tb::PolicyCacheMode::Ram);
  CHECK(tb::parsePolicyCacheMode("disk", pm));
  CHECK(pm == tb::PolicyCacheMode::Disk);
  CHECK_FALSE(tb::parsePolicyCacheMode("ssd", pm));

  // 2. Verify exact Next-Box solver behavior across:
  //    (a) Exact parity vs an independent plain Jacobi VI reference on boards4x4 (Uniform & Nes).
  //    (b) Multi-chunk disk PolicyActionStore round-trip (> CHUNK_BOARDS) + Ram vs Disk & 1T vs 4T
  //        bit-for-bit determinism on boards2x10_1.
  //    (c) Monotone (subset) and non-monotone (superset) warm-start convergence & rngMode guard.
  //    (d) Direct chooseNextBoxTablebaseMove tie-breaking, fallback, top-out, Mapped vs Indexed
  //        parity, and evaluateNextBoxPolicy closed-world MC (<= 3.5 sigma) & edge cases.
  {
    // Multi-chunk disk PolicyActionStore check (CHUNK_BOARDS + 137 boards exercises non-zero
    // pwrite/pread offsets and partial tail chunk).
    constexpr size_t MULTI_BOARDS = tb::detail::PolicyActionStore::CHUNK_BOARDS + 137;
    tb::detail::PolicyActionStore store;
    std::string storeErr;
    REQUIRE(store.init(MULTI_BOARDS, /*useDisk=*/true, &storeErr));
    CHECK(store.isDisk());
    for (size_t off = 0; off < MULTI_BOARDS; off += tb::detail::PolicyActionStore::CHUNK_BOARDS) {
      const size_t cnt = std::min(MULTI_BOARDS - off, tb::detail::PolicyActionStore::CHUNK_BOARDS);
      uint32_t *wbuf = store.beginWriteChunk(off, cnt);
      for (size_t i = 0; i < cnt; ++i) {
        const uint32_t cid = static_cast<uint32_t>(off + i);
        const uint8_t rew = static_cast<uint8_t>((off + i) % 5);
        wbuf[i * tb::NB_ACTIONS_PER_BOARD] = tb::packNextBoxAction(cid, rew);
        wbuf[i * tb::NB_ACTIONS_PER_BOARD + tb::NB_ACTIONS_PER_BOARD - 1] =
            tb::packNextBoxAction(tb::INVALID_BOARD_ID, rew);
      }
      REQUIRE(store.finishWriteChunk(off, cnt, &storeErr));
    }
    const uint32_t *tail =
        store.readChunk(tb::detail::PolicyActionStore::CHUNK_BOARDS, 137, &storeErr);
    REQUIRE(tail != nullptr);
    for (size_t i = 0; i < 137; ++i) {
      const size_t absIdx = tb::detail::PolicyActionStore::CHUNK_BOARDS + i;
      const uint32_t firstWord = tail[i * tb::NB_ACTIONS_PER_BOARD];
      const uint32_t lastWord = tail[i * tb::NB_ACTIONS_PER_BOARD + tb::NB_ACTIONS_PER_BOARD - 1];
      CHECK(tb::unpackNextBoxChild(firstWord) == static_cast<uint32_t>(absIdx));
      CHECK(tb::unpackNextBoxReward(firstWord) == static_cast<uint8_t>(absIdx % 5));
      CHECK(tb::unpackNextBoxChild(lastWord) == tb::INVALID_BOARD_ID);
      CHECK(tb::unpackNextBoxReward(lastWord) == static_cast<uint8_t>(absIdx % 5));
    }
    store.close();
  }

  // Empty board set & innerEvalSweeps == 0 edge cases.
  {
    const tb::NextBoxSolveResult emptyRes = tb::solveNextBoxMPI({});
    CHECK(emptyRes.ok);
    CHECK(emptyRes.values.empty());
    CHECK(emptyRes.emptyBoardExpectedValue == tb::MISSING_BOARD_VALUE);
  }

  const std::vector<uint64_t> boards2x10_0 = tb::enumerateZeroSealedHoleBoards(0, 4, 2, 10);
  const std::vector<uint64_t> boards2x10_1 = tb::enumerateZeroSealedHoleBoards(1, 4, 2, 10);

  // Independent plain double-precision VI reference on a small 4x4 board set (fast & exact).
  const std::vector<uint64_t> boards4x4 = tb::enumerateZeroSealedHoleBoards(1, 4, 4, 4);
  const tb::FlatBoardIndex idx4x4(boards4x4);
  for (tb::RngMode mode : {tb::RngMode::Uniform, tb::RngMode::Nes}) {
    std::vector<double> refV(boards4x4.size() * tb::NUM_PIECES, 0.0);
    std::vector<double> refNext(boards4x4.size() * tb::NUM_PIECES, 0.0);
    tb::TransitionList tl;
    for (int iter = 0; iter < 500; ++iter) {
      double maxDiff = 0.0;
      for (size_t i = 0; i < boards4x4.size(); ++i) {
        for (int c = 0; c < tb::NUM_PIECES; ++c) {
          tb::generateTransitions(boards4x4[i], c, tl);
          double val = 0.0;
          if (!tl.empty()) {
            for (int n = 0; n < tb::NUM_PIECES; ++n) {
              double bestQ = 0.0;
              for (int m = 0; m < tl.size(); ++m) {
                const uint32_t cid = idx4x4.find(tl[m].afterBoard);
                const double cont =
                    (cid != tb::INVALID_BOARD_ID)
                        ? refV[static_cast<size_t>(cid) * tb::NUM_PIECES + static_cast<size_t>(n)]
                        : 0.0;
                const double q = static_cast<double>(tl[m].linesCleared) + cont;
                if (m == 0 || q > bestQ)
                  bestQ = q;
              }
              val += tb::pieceTransitionProb(mode, c, n) * bestQ;
            }
          }
          refNext[i * tb::NUM_PIECES + static_cast<size_t>(c)] = val;
          maxDiff =
              std::max(maxDiff, std::abs(val - refV[i * tb::NUM_PIECES + static_cast<size_t>(c)]));
        }
      }
      refV.swap(refNext);
      if (maxDiff <= 1e-11)
        break;
    }

    tb::NextBoxSolveConfig cfg{};
    cfg.rngMode = mode;
    cfg.policyCacheMode = tb::PolicyCacheMode::Ram;
    cfg.tolerance = 1e-10;
    cfg.innerEvalSweeps = 30;
    cfg.numThreads = 4;
    const tb::NextBoxSolveResult res = tb::solveNextBoxMPI(boards4x4, idx4x4, cfg);
    REQUIRE(res.ok);
    CHECK(res.finalBellmanResidual <= 1e-10);
    for (size_t k = 0; k < refV.size(); ++k) {
      CHECK(static_cast<double>(res.values[k]) == doctest::Approx(refV[k]).epsilon(1e-5));
    }
  }

  // Thread determinism (1T vs 4T) and Ram vs Disk bit-for-bit equality on boards2x10_1.
  tb::NextBoxSolveConfig cfgRam1{};
  cfgRam1.rngMode = tb::RngMode::Nes;
  cfgRam1.policyCacheMode = tb::PolicyCacheMode::Ram;
  cfgRam1.tolerance = 1e-9;
  cfgRam1.innerEvalSweeps = 25;
  cfgRam1.numThreads = 1;
  const tb::NextBoxSolveResult ram1 = tb::solveNextBoxMPI(boards2x10_1, cfgRam1);
  REQUIRE(ram1.ok);
  CHECK_FALSE(ram1.usedDiskPolicyCache);
  CHECK(ram1.finalBellmanResidual <= 1e-9);
  CHECK(ram1.emptyBoardExpectedValue > 0.5);

  tb::NextBoxSolveConfig cfgDisk4 = cfgRam1;
  cfgDisk4.policyCacheMode = tb::PolicyCacheMode::Disk;
  cfgDisk4.numThreads = 4;
  const tb::NextBoxSolveResult disk4 = tb::solveNextBoxMPI(boards2x10_1, cfgDisk4);
  REQUIRE(disk4.ok);
  CHECK(disk4.usedDiskPolicyCache);
  CHECK(disk4.emptyBoardExpectedValue == ram1.emptyBoardExpectedValue);
  CHECK(disk4.values == ram1.values);
  CHECK(disk4.outerIterations == ram1.outerIterations);

  // Monotone (subset -> superset) AND non-monotone (superset -> subset) warm-start verification,
  // plus innerEvalSweeps == 0 (pure Jacobi VI) check.
  const tb::NextBoxSolveResult subRes = tb::solveNextBoxMPI(boards2x10_0, cfgDisk4);
  REQUIRE(subRes.ok);
  const std::string tmpSubTb = "build/test_nb_sub.tb.bin";
  REQUIRE(tb::writeNextBoxTablebaseFile(tmpSubTb, boards2x10_0, subRes.values, tb::RngMode::Nes,
                                        subRes.finalBellmanResidual));
  tb::MappedNextBoxTablebase mappedSub;
  REQUIRE(mappedSub.open(tmpSubTb));

  const tb::NextBoxSolveResult warmRes = tb::solveNextBoxMPI(boards2x10_1, cfgDisk4, &mappedSub);
  REQUIRE(warmRes.ok);
  CHECK(warmRes.warmStartBoardsMatched == boards2x10_0.size());
  CHECK(warmRes.finalBellmanResidual <= 1e-9);
  CHECK(warmRes.emptyBoardExpectedValue
        == doctest::Approx(ram1.emptyBoardExpectedValue).epsilon(1e-7));
  CHECK(warmRes.emptyBoardExpectedValue >= subRes.emptyBoardExpectedValue - 1e-6);

  // Reverse (superset -> subset) warm-start: values must contract back down to subRes cleanly.
  const std::string tmpSupTb = "build/test_nb_sup.tb.bin";
  REQUIRE(tb::writeNextBoxTablebaseFile(tmpSupTb, boards2x10_1, ram1.values, tb::RngMode::Nes,
                                        ram1.finalBellmanResidual));
  tb::MappedNextBoxTablebase mappedSup;
  REQUIRE(mappedSup.open(tmpSupTb));
  const tb::NextBoxSolveResult revWarm = tb::solveNextBoxMPI(boards2x10_0, cfgDisk4, &mappedSup);
  REQUIRE(revWarm.ok);
  CHECK(revWarm.warmStartBoardsMatched == boards2x10_0.size());
  CHECK(revWarm.finalBellmanResidual <= 1e-9);
  CHECK(revWarm.emptyBoardExpectedValue
        == doctest::Approx(subRes.emptyBoardExpectedValue).epsilon(1e-7));
  mappedSup.close();
  std::remove(tmpSupTb.c_str());

  // innerEvalSweeps == 0 (pure VI) check on boards4x4.
  {
    tb::NextBoxSolveConfig pureViCfg = cfgRam1;
    pureViCfg.innerEvalSweeps = 0;
    pureViCfg.maxOuterIters = 500;
    const tb::NextBoxSolveResult pureRes = tb::solveNextBoxMPI(boards4x4, idx4x4, pureViCfg);
    REQUIRE(pureRes.ok);
    CHECK(pureRes.totalInnerSweeps == 0);
    CHECK(pureRes.finalBellmanResidual <= 1e-9);
  }

  // Mismatched RngMode on warm-start must fail explicitly.
  tb::NextBoxSolveConfig cfgWrongRng = cfgDisk4;
  cfgWrongRng.rngMode = tb::RngMode::Uniform;
  const tb::NextBoxSolveResult badWarm = tb::solveNextBoxMPI(boards2x10_1, cfgWrongRng, &mappedSub);
  CHECK_FALSE(badWarm.ok);

  // Direct chooseNextBoxTablebaseMove tests: closedWorld vs live tie-break, all-OUT fallback,
  // top-out, and Mapped vs Indexed parity.
  {
    tb::TransitionList oMoves;
    tb::generateTransitions(0ULL, tb::parsePieceChar('O'), oMoves);
    REQUIRE(oMoves.size() == 9);
    std::vector<uint64_t> tieKeys = {oMoves[0].afterBoard, oMoves[4].afterBoard};
    std::sort(tieKeys.begin(), tieKeys.end());
    std::vector<float> tieVals(tieKeys.size() * tb::NUM_PIECES, 15.0f);
    const std::string tmpNbTie = "build/test_nb_choose_tie.tb.bin";
    REQUIRE(tb::writeNextBoxTablebaseFile(tmpNbTie, tieKeys, tieVals, tb::RngMode::Nes, 0.0));
    tb::MappedNextBoxTablebase mTie;
    REQUIRE(mTie.open(tmpNbTie));
    tb::IndexedNextBoxTablebase iTie(mTie);

    const int oIdx = tb::parsePieceChar('O');
    const int iIdx = tb::parsePieceChar('I');
    const auto cwM = tb::chooseNextBoxTablebaseMove(0ULL, oIdx, iIdx, mTie, /*closedWorld=*/true);
    const auto cwI = tb::chooseNextBoxTablebaseMove(0ULL, oIdx, iIdx, iTie, /*closedWorld=*/true);
    REQUIRE(cwM.found);
    CHECK(cwM.transition.afterBoard == oMoves[0].afterBoard);
    CHECK(cwM.transition.afterBoard == cwI.transition.afterBoard);
    CHECK(cwM.tablebaseQ == doctest::Approx(15.0));

    const auto liveM =
        tb::chooseNextBoxTablebaseMove(0ULL, oIdx, iIdx, mTie, /*closedWorld=*/false);
    const auto liveI =
        tb::chooseNextBoxTablebaseMove(0ULL, oIdx, iIdx, iTie, /*closedWorld=*/false);
    REQUIRE(liveM.found);
    CHECK(liveM.afterInTablebase);
    CHECK_FALSE(liveM.usedFallback);
    CHECK(liveM.transition.afterBoard == liveI.transition.afterBoard);
    const double h0 = tb::scoreNextBoxHeuristicTransition(oMoves[0].afterBoard, 0, iIdx);
    const double h4 = tb::scoreNextBoxHeuristicTransition(oMoves[4].afterBoard, 0, iIdx);
    if (h0 != h4) {
      const uint64_t expected = (h4 > h0) ? oMoves[4].afterBoard : oMoves[0].afterBoard;
      CHECK(liveM.transition.afterBoard == expected);
    }

    // All-OUT fallback for piece 'I' on this O-only TB.
    const auto fb = tb::chooseNextBoxTablebaseMove(0ULL, iIdx, oIdx, iTie, /*closedWorld=*/false);
    REQUIRE(fb.found);
    CHECK(fb.usedFallback);
    CHECK_FALSE(fb.afterInTablebase);
    CHECK(fb.transition.afterBoard
          == tb::pickBestNextBoxHeuristic(0ULL, iIdx, oIdx).transition.afterBoard);

    // Top-out returns found == false.
    const uint64_t fullWall = mustParse("XXXXXXXXX.\n"
                                        "XXXXXXXXX.\n"
                                        "XXXXXXXXX.\n"
                                        "XXXXXXXXX.");
    CHECK_FALSE(tb::chooseNextBoxTablebaseMove(fullWall, oIdx, iIdx, iTie, false).found);

    // evaluateNextBoxPolicy edge cases: numGames=0, numGames=1, maxSteps=1.
    const auto ev0 = tb::evaluateNextBoxPolicy(iTie, tb::RngMode::Nes, false, 0, 1ULL, 1);
    CHECK(ev0.games == 0);
    const auto ev1 = tb::evaluateNextBoxPolicy(iTie, tb::RngMode::Nes, false, 1, 1ULL, 1);
    CHECK(ev1.games == 1);
    CHECK(ev1.stdErrLines == 0.0);
    const auto evCap = tb::evaluateNextBoxPolicy(iTie, tb::RngMode::Nes, false, 10, 1ULL, 2,
                                                 tb::EvalWeights{}, /*maxSteps=*/1);
    CHECK(evCap.stepCapHits == 10);

    mTie.close();
    std::remove(tmpNbTie.c_str());
  }

  // Closed-world MC & live fallback verification for BOTH Nes and Uniform on boards2x10_1.
  const std::string tmpFullNb = "build/test_nb_full.tb.bin";
  for (tb::RngMode mode : {tb::RngMode::Nes, tb::RngMode::Uniform}) {
    tb::NextBoxSolveConfig mCfg = cfgDisk4;
    mCfg.rngMode = mode;
    const tb::NextBoxSolveResult mRes = tb::solveNextBoxMPI(boards2x10_1, mCfg);
    REQUIRE(mRes.ok);
    REQUIRE(tb::writeNextBoxTablebaseFile(tmpFullNb, boards2x10_1, mRes.values, mode,
                                          mRes.finalBellmanResidual));
    tb::MappedNextBoxTablebase mappedNb;
    REQUIRE(mappedNb.open(tmpFullNb));
    tb::IndexedNextBoxTablebase idxNb(mappedNb);

    const tb::TablebaseEvalStats cw1 =
        tb::evaluateNextBoxPolicy(idxNb, mode, /*closedWorld=*/true, 3000, 4242ULL, 1);
    const tb::TablebaseEvalStats cw4 =
        tb::evaluateNextBoxPolicy(idxNb, mode, /*closedWorld=*/true, 3000, 4242ULL, 4);
    CHECK(cw1.meanLines == cw4.meanLines);
    CHECK(cw1.stdErrLines == cw4.stdErrLines);
    CHECK(cw1.inTbMovePct == cw4.inTbMovePct);
    CHECK(cw1.stepCapHits == 0);
    CHECK(tb::computeClosedWorldZScore(cw1.meanLines, cw1.stdErrLines, mRes.emptyBoardExpectedValue)
          <= 3.5);

    const tb::TablebaseEvalStats live =
        tb::evaluateNextBoxPolicy(idxNb, mode, /*closedWorld=*/false, 300, 4242ULL, 4);
    CHECK(live.stepCapHits == 0);
    CHECK(live.meanLines > cw1.meanLines);
    CHECK(live.inTbMovePct > 0.0);
    mappedNb.close();
  }

  mappedSub.close();
  std::remove(tmpSubTb.c_str());
  std::remove(tmpFullNb.c_str());
}

TEST_CASE("Next-Box state generator determinism, frontier probing, and round monotonicity") {
  // 1. Thread determinism (1T vs 4T) for both Uniform and Nes modes without a tablebase.
  for (tb::RngMode mode : {tb::RngMode::Uniform, tb::RngMode::Nes}) {
    tb::NextBoxGenConfig cfg1{};
    cfg1.rngMode = mode;
    cfg1.numGames = 80;
    cfg1.baseSeed = 51515ULL;
    cfg1.epsilon = 0.08;
    cfg1.exploreTopK = 4;
    cfg1.minVisits = 1;
    cfg1.maxBoards = 2000;
    cfg1.siblingMinVisits = 3;
    cfg1.numThreads = 1;
    const tb::StateGenResult r1 = tb::generateNextBoxStateSpace(cfg1);

    tb::NextBoxGenConfig cfg4 = cfg1;
    cfg4.numThreads = 4;
    const tb::StateGenResult r4 = tb::generateNextBoxStateSpace(cfg4);

    CHECK(r1.sortedBoards == r4.sortedBoards);
    CHECK(r1.rawUniqueDiscovered == r4.rawUniqueDiscovered);
    CHECK(r1.siblingBoardsAdded == r4.siblingBoardsAdded);
    CHECK(r1.totalMovesSimulated == r4.totalMovesSimulated);
    REQUIRE_FALSE(r1.sortedBoards.empty());
    CHECK(r1.sortedBoards.front() == 0ULL);
    CHECK(std::is_sorted(r1.sortedBoards.begin(), r1.sortedBoards.end()));
    CHECK(std::adjacent_find(r1.sortedBoards.begin(), r1.sortedBoards.end())
          == r1.sortedBoards.end());
    CHECK(r1.sortedBoards.size() <= cfg1.maxBoards);
  }

  // 2. Frontier probing: verify both TB runner-up AND out-of-TB 2-ply heuristic winner are
  //    sampled, and greedy TB move is never repeated during exploration (even when TB and
  //    heuristic agree).
  {
    const int oIdx = tb::parsePieceChar('O');
    const int iIdx = tb::parsePieceChar('I');
    tb::TransitionList oMoves;
    tb::generateTransitions(0ULL, oIdx, oMoves);
    REQUIRE(oMoves.size() == 9);

    const tb::ChosenTransition heurWinner = tb::pickBestNextBoxHeuristic(0ULL, oIdx, iIdx);
    REQUIRE(heurWinner.found);

    // Put 4 non-heuristic-winner moves into the TB with distinct positive values.
    std::vector<std::pair<uint64_t, float>> entries;
    float val = 10.0f;
    uint64_t tbBestBoard = 0ULL;
    uint64_t tbRunnerUpBoard = 0ULL;
    for (int m = 0; m < oMoves.size() && entries.size() < 4; ++m) {
      if (oMoves[m].afterBoard == heurWinner.transition.afterBoard) {
        continue;
      }
      if (entries.empty()) {
        tbBestBoard = oMoves[m].afterBoard;
      } else if (entries.size() == 1) {
        tbRunnerUpBoard = oMoves[m].afterBoard;
      }
      entries.emplace_back(oMoves[m].afterBoard, val);
      val -= 1.0f;
    }
    REQUIRE(entries.size() == 4);
    std::sort(entries.begin(), entries.end());
    std::vector<uint64_t> tbBoards;
    std::vector<float> tbVals;
    for (const auto &e : entries) {
      tbBoards.push_back(e.first);
      for (int p = 0; p < tb::NUM_PIECES; ++p) {
        tbVals.push_back(e.second);
      }
    }

    const std::string tmpProbeTb = "build/test_nb_probe.tb.bin";
    REQUIRE(tb::writeNextBoxTablebaseFile(tmpProbeTb, tbBoards, tbVals, tb::RngMode::Nes, 0.0));
    tb::MappedNextBoxTablebase mProbe;
    REQUIRE(mProbe.open(tmpProbeTb));
    tb::IndexedNextBoxTablebase iProbe(mProbe);

    tb::Rng probeRng(777ULL);
    bool sawRunnerUp = false;
    bool sawHeurFrontier = false;
    for (int trial = 0; trial < 200; ++trial) {
      const tb::ChosenTransition ch =
          tb::selectNextBoxRolloutTransition(0ULL, oIdx, iIdx, probeRng, &iProbe, /*epsilon=*/1.0,
                                             /*exploreTopK=*/4, tb::EvalWeights{});
      REQUIRE(ch.found);
      CHECK(ch.transition.afterBoard != tbBestBoard);
      if (ch.transition.afterBoard == tbRunnerUpBoard) {
        sawRunnerUp = true;
      }
      if (ch.transition.afterBoard == heurWinner.transition.afterBoard) {
        sawHeurFrontier = true;
      }
    }
    CHECK(sawRunnerUp);
    CHECK(sawHeurFrontier);
    mProbe.close();
    std::remove(tmpProbeTb.c_str());

    // When TB greedy winner IS the heuristic winner, exploration must still never repeat it.
    std::vector<uint64_t> agreeBoards = {heurWinner.transition.afterBoard};
    std::vector<float> agreeVals(tb::NUM_PIECES, 20.0f);
    const std::string tmpAgreeTb = "build/test_nb_agree.tb.bin";
    REQUIRE(
        tb::writeNextBoxTablebaseFile(tmpAgreeTb, agreeBoards, agreeVals, tb::RngMode::Nes, 0.0));
    tb::MappedNextBoxTablebase mAgree;
    REQUIRE(mAgree.open(tmpAgreeTb));
    tb::IndexedNextBoxTablebase iAgree(mAgree);
    tb::Rng agreeRng(888ULL);
    for (int trial = 0; trial < 100; ++trial) {
      const tb::ChosenTransition ch =
          tb::selectNextBoxRolloutTransition(0ULL, oIdx, iIdx, agreeRng, &iAgree, /*epsilon=*/1.0,
                                             /*exploreTopK=*/4, tb::EvalWeights{});
      REQUIRE(ch.found);
      CHECK(ch.transition.afterBoard != heurWinner.transition.afterBoard);
    }

    // All-OUT (anyInTb == false) with a loaded TB: exploration falls back to heuristic probe and
    // must still never repeat the #1 heuristic move.
    const tb::ChosenTransition iHeurBest = tb::pickBestNextBoxHeuristic(0ULL, iIdx, oIdx);
    REQUIRE(iHeurBest.found);
    for (int trial = 0; trial < 60; ++trial) {
      const tb::ChosenTransition ch =
          tb::selectNextBoxRolloutTransition(0ULL, iIdx, oIdx, agreeRng, &iAgree, /*epsilon=*/1.0,
                                             /*exploreTopK=*/4, tb::EvalWeights{});
      REQUIRE(ch.found);
      CHECK(ch.transition.afterBoard != iHeurBest.transition.afterBoard);
    }

    // Exact tbQ tie-breaking parity: two moves tied at max tbQ = 15.0; both exploration branches
    // must match chooseNextBoxTablebaseMove(..., closedWorld=false) on rank 0 while allowing the
    // tied runner-up to be sampled.
    std::vector<uint64_t> tieBoards = {oMoves[0].afterBoard, oMoves[4].afterBoard};
    std::sort(tieBoards.begin(), tieBoards.end());
    std::vector<float> tieVals(tieBoards.size() * tb::NUM_PIECES, 15.0f);
    const std::string tmpTieTb = "build/test_nb_roll_tie.tb.bin";
    REQUIRE(tb::writeNextBoxTablebaseFile(tmpTieTb, tieBoards, tieVals, tb::RngMode::Nes, 0.0));
    tb::MappedNextBoxTablebase mTie;
    REQUIRE(mTie.open(tmpTieTb));
    tb::IndexedNextBoxTablebase iTie(mTie);
    const tb::TablebaseMoveChoice greedyTie =
        tb::chooseNextBoxTablebaseMove(0ULL, oIdx, iIdx, iTie, /*closedWorld=*/false);
    REQUIRE(greedyTie.found);
    const uint64_t otherTied = (greedyTie.transition.afterBoard == oMoves[0].afterBoard)
                                   ? oMoves[4].afterBoard
                                   : oMoves[0].afterBoard;
    bool sawOtherTied = false;
    for (int trial = 0; trial < 150; ++trial) {
      const tb::ChosenTransition ch =
          tb::selectNextBoxRolloutTransition(0ULL, oIdx, iIdx, agreeRng, &iTie, /*epsilon=*/1.0,
                                             /*exploreTopK=*/4, tb::EvalWeights{});
      REQUIRE(ch.found);
      CHECK(ch.transition.afterBoard != greedyTie.transition.afterBoard);
      if (ch.transition.afterBoard == otherTied) {
        sawOtherTied = true;
      }
    }
    CHECK(sawOtherTied);

    // Boundary cases: epsilon == 0, exploreTopK < 2, single legal move (tl.size() == 1), top-out.
    CHECK(tb::selectNextBoxRolloutTransition(0ULL, oIdx, iIdx, agreeRng, &iTie, 0.0, 4,
                                             tb::EvalWeights{})
              .transition.afterBoard
          == greedyTie.transition.afterBoard);
    CHECK(tb::selectNextBoxRolloutTransition(0ULL, oIdx, iIdx, agreeRng, &iTie, 1.0, 1,
                                             tb::EvalWeights{})
              .transition.afterBoard
          == greedyTie.transition.afterBoard);
    const uint64_t oneSlotBoard = mustParse("XXXXXXXXX.\n"
                                            "XXXXXXXXX.\n"
                                            "XXXXXXXXX.\n"
                                            "XXXXXXXXX.");
    const tb::ChosenTransition singleMove =
        tb::selectNextBoxRolloutTransition(oneSlotBoard, iIdx, oIdx, agreeRng, &iTie,
                                           /*epsilon=*/1.0, /*exploreTopK=*/4, tb::EvalWeights{});
    REQUIRE(singleMove.found);
    CHECK(singleMove.transition.linesCleared == 4);
    CHECK(singleMove.transition.afterBoard == 0ULL);
    CHECK_FALSE(tb::selectNextBoxRolloutTransition(oneSlotBoard, oIdx, iIdx, agreeRng, &iTie,
                                                   /*epsilon=*/1.0, /*exploreTopK=*/4,
                                                   tb::EvalWeights{})
                    .found);

    mTie.close();
    std::remove(tmpTieTb.c_str());
    mAgree.close();
    std::remove(tmpAgreeTb.c_str());
  }

  // 2b. Direct expandNextBoxCoreSiblings multi-next emission + dedup check and tight maxBoards cap.
  {
    const int oIdx = tb::parsePieceChar('O');
    tb::TransitionList oMoves;
    tb::generateTransitions(0ULL, oIdx, oMoves);
    REQUIRE(oMoves.size() == 9);
    const uint64_t heur1PlyBoard = tb::pickBestTransition1Ply(0ULL, oIdx).transition.afterBoard;

    std::vector<uint64_t> distinctMoves;
    for (int m = 0; m < oMoves.size() && distinctMoves.size() < 3; ++m) {
      if (oMoves[m].afterBoard != heur1PlyBoard) {
        distinctMoves.push_back(oMoves[m].afterBoard);
      }
    }
    REQUIRE(distinctMoves.size() == 3);
    std::sort(distinctMoves.begin(), distinctMoves.end());

    // Make move 0 win for next=0, move 1 win for next=1, move 2 win for next=2..6.
    std::vector<float> multiVals(distinctMoves.size() * tb::NUM_PIECES, 1.0f);
    multiVals[0 * tb::NUM_PIECES + 0] = 50.0f;
    multiVals[1 * tb::NUM_PIECES + 1] = 50.0f;
    for (int n = 2; n < tb::NUM_PIECES; ++n) {
      multiVals[2 * tb::NUM_PIECES + static_cast<size_t>(n)] = 50.0f;
    }
    const std::string tmpSibTb = "build/test_nb_sib.tb.bin";
    REQUIRE(
        tb::writeNextBoxTablebaseFile(tmpSibTb, distinctMoves, multiVals, tb::RngMode::Nes, 0.0));
    tb::MappedNextBoxTablebase mSib;
    REQUIRE(mSib.open(tmpSibTb));
    tb::IndexedNextBoxTablebase iSib(mSib);

    tb::NextBoxGenConfig sibCfg{};
    sibCfg.siblingMinVisits = 5;
    sibCfg.numThreads = 2;
    const std::vector<std::pair<uint64_t, uint32_t>> rawCore = {{0ULL, 5U}};
    const std::vector<uint64_t> sibs =
        tb::detail::expandNextBoxCoreSiblings(rawCore, sibCfg, &iSib);
    CHECK(std::is_sorted(sibs.begin(), sibs.end()));
    CHECK(std::adjacent_find(sibs.begin(), sibs.end()) == sibs.end());
    CHECK(std::binary_search(sibs.begin(), sibs.end(), heur1PlyBoard));
    for (uint64_t b : distinctMoves) {
      CHECK(std::binary_search(sibs.begin(), sibs.end(), b));
    }

    // Tight maxBoards cap with priorBoards + 0ULL preservation.
    tb::NextBoxGenConfig capCfg{};
    capCfg.rngMode = tb::RngMode::Nes;
    capCfg.numGames = 30;
    capCfg.baseSeed = 4242ULL;
    capCfg.minVisits = 1;
    capCfg.maxBoards = 10;
    capCfg.siblingMinVisits = 0;
    capCfg.numThreads = 2;
    const tb::StateGenResult capped =
        tb::generateNextBoxStateSpace(capCfg, nullptr, &distinctMoves);
    CHECK(capped.sortedBoards.size() == 10);
    CHECK(capped.sortedBoards.front() == 0ULL);
    CHECK(capped.priorBoardsMerged == distinctMoves.size());
    CHECK(std::includes(capped.sortedBoards.begin(), capped.sortedBoards.end(),
                        distinctMoves.begin(), distinctMoves.end()));

    mSib.close();
    std::remove(tmpSibTb.c_str());
  }

  // 3. Mini end-to-end iterative Next-Box loop (Round 0 -> Solve -> Round 1 with TB + merge ->
  //    Warm-Started Solve): verify superset growth, thread determinism with TB + merge,
  //    coverage determinism, and round-over-round value monotonicity V_1(0, c) >= V_0(0, c).
  {
    tb::NextBoxGenConfig gen0Cfg{};
    gen0Cfg.rngMode = tb::RngMode::Nes;
    gen0Cfg.numGames = 60;
    gen0Cfg.baseSeed = 9001ULL;
    gen0Cfg.epsilon = 0.06;
    gen0Cfg.minVisits = 2;
    gen0Cfg.maxBoards = 4000;
    gen0Cfg.siblingMinVisits = 4;
    gen0Cfg.numThreads = 4;
    const tb::StateGenResult gen0 = tb::generateNextBoxStateSpace(gen0Cfg);
    REQUIRE(gen0.sortedBoards.size() > 10);
    CHECK(gen0.sortedBoards.front() == 0ULL);

    tb::NextBoxSolveConfig solveCfg{};
    solveCfg.rngMode = tb::RngMode::Nes;
    solveCfg.policyCacheMode = tb::PolicyCacheMode::Ram;
    solveCfg.tolerance = 1e-8;
    solveCfg.innerEvalSweeps = 30;
    solveCfg.numThreads = 4;
    const tb::NextBoxSolveResult sol0 = tb::solveNextBoxMPI(gen0.sortedBoards, solveCfg);
    REQUIRE(sol0.ok);

    const std::string tmpR0Tb = "build/test_nb_r0.tb.bin";
    REQUIRE(tb::writeNextBoxTablebaseFile(tmpR0Tb, gen0.sortedBoards, sol0.values, tb::RngMode::Nes,
                                          sol0.finalBellmanResidual));
    tb::MappedNextBoxTablebase tb0;
    REQUIRE(tb0.open(tmpR0Tb));

    tb::NextBoxGenConfig gen1Cfg = gen0Cfg;
    gen1Cfg.baseSeed = 9002ULL;
    gen1Cfg.epsilon = 0.04;
    gen1Cfg.maxBoards = 10000;
    gen1Cfg.numThreads = 1;
    const tb::StateGenResult gen1T1 =
        tb::generateNextBoxStateSpace(gen1Cfg, &tb0, &gen0.sortedBoards);
    gen1Cfg.numThreads = 4;
    const tb::StateGenResult gen1T4 =
        tb::generateNextBoxStateSpace(gen1Cfg, &tb0, &gen0.sortedBoards);

    CHECK(gen1T1.sortedBoards == gen1T4.sortedBoards);
    CHECK(gen1T4.priorBoardsMerged == gen0.sortedBoards.size());
    CHECK(gen1T4.sortedBoards.size() >= gen0.sortedBoards.size());
    CHECK(std::includes(gen1T4.sortedBoards.begin(), gen1T4.sortedBoards.end(),
                        gen0.sortedBoards.begin(), gen0.sortedBoards.end()));

    const tb::CoverageReport cov0 =
        tb::measureNextBoxCoverage(gen1T4.sortedBoards, 0, 12345ULL, tb::RngMode::Nes, &tb0, 2);
    CHECK(cov0.games == 0);
    const tb::CoverageReport covHeur =
        tb::measureNextBoxCoverage(gen1T4.sortedBoards, 15, 12345ULL, tb::RngMode::Nes, nullptr, 2);
    CHECK(covHeur.games == 15);
    CHECK(covHeur.overallHitPct > 0.0);

    const tb::CoverageReport cov1 =
        tb::measureNextBoxCoverage(gen1T4.sortedBoards, 40, 12345ULL, tb::RngMode::Nes, &tb0, 1);
    const tb::CoverageReport cov4 =
        tb::measureNextBoxCoverage(gen1T4.sortedBoards, 40, 12345ULL, tb::RngMode::Nes, &tb0, 4);
    CHECK(cov1.meanLines == cov4.meanLines);
    CHECK(cov1.stdErrLines == cov4.stdErrLines);
    CHECK(cov1.overallHitPct == cov4.overallHitPct);
    CHECK(cov1.fullyCoveredGamePct == cov4.fullyCoveredGamePct);
    CHECK(cov1.byHoles.total == cov4.byHoles.total);
    CHECK(cov1.byHoles.inSet == cov4.byHoles.inSet);
    CHECK(cov1.overallHitPct > 0.0);

    const tb::NextBoxSolveResult sol1 = tb::solveNextBoxMPI(gen1T4.sortedBoards, solveCfg, &tb0);
    REQUIRE(sol1.ok);
    CHECK(sol1.warmStartBoardsMatched == gen0.sortedBoards.size());
    CHECK(sol1.emptyBoardExpectedValue >= sol0.emptyBoardExpectedValue - 1e-5);
    for (int c = 0; c < tb::NUM_PIECES; ++c) {
      CHECK(sol1.emptyBoardValues[static_cast<size_t>(c)]
            >= sol0.emptyBoardValues[static_cast<size_t>(c)] - 1e-5);
    }

    tb0.close();
    std::remove(tmpR0Tb.c_str());
  }
}

TEST_CASE("Server routes Next-Box (SRTBV002) auto-detection, /ping, /moves, /state-info, and "
          "reload") {
  const std::string nbNesPath = "build/test_srv_nb_nes.tb.bin";
  const std::string nbUniPath = "build/test_srv_nb_uni.tb.bin";
  const std::string nnbPath = "build/test_srv_nnb.tb.bin";
  const std::string badPath = "build/test_srv_corrupt.tb.bin";

  // Build an exact solved 4x4 SRTBV002 tablebase under NES RNG (includes empty board 0ULL),
  // a synthetic single-board SRTBV002 under Uniform RNG (for disagreement + reload switching),
  // and a 1-board SRTBV001 NNB tablebase.
  const std::vector<uint64_t> boards4x4 = tb::enumerateZeroSealedHoleBoards(1, 4, 4, 4);
  tb::NextBoxSolveConfig sCfg{};
  sCfg.rngMode = tb::RngMode::Nes;
  sCfg.policyCacheMode = tb::PolicyCacheMode::Ram;
  sCfg.tolerance = 1e-9;
  sCfg.innerEvalSweeps = 30;
  sCfg.numThreads = 2;
  const tb::NextBoxSolveResult solved4x4 = tb::solveNextBoxMPI(boards4x4, sCfg);
  REQUIRE(solved4x4.ok);
  REQUIRE(tb::writeNextBoxTablebaseFile(nbNesPath, boards4x4, solved4x4.values, tb::RngMode::Nes,
                                        solved4x4.finalBellmanResidual));

  // Synthetic Uniform SRTBV002 with a boosted non-heuristic I placement for next=O.
  tb::TransitionList emptyIMoves;
  tb::generateTransitions(0ULL, tb::parsePieceChar('I'), emptyIMoves);
  REQUIRE(emptyIMoves.size() == 17);
  const uint64_t boostedI = emptyIMoves[16].afterBoard;
  std::vector<float> boostVals(tb::NUM_PIECES, 1.0f);
  boostVals[tb::parsePieceChar('O')] = 123.5f;
  REQUIRE(
      tb::writeNextBoxTablebaseFile(nbUniPath, {boostedI}, boostVals, tb::RngMode::Uniform, 0.0));
  REQUIRE(tb::writeTablebaseFile(nnbPath, {boostedI}, {77.0f}, 0.0));

  // Write a truncated invalid file to test reload failure rollback.
  {
    std::ofstream badOut(badPath, std::ios::binary | std::ios::trunc);
    REQUIRE(badOut.is_open());
    badOut.write("BADMAGIC", 8);
    REQUIRE(badOut.good());
  }

  auto extractJsonNumberAfter = [](const std::string &json, const std::string &needle,
                                   size_t startPos = 0) -> double {
    const size_t pos = json.find(needle, startPos);
    REQUIRE(pos != std::string::npos);
    const char *numStart = json.c_str() + pos + needle.size();
    char *endPtr = nullptr;
    const double val = std::strtod(numStart, &endPtr);
    REQUIRE(endPtr != numStart);
    return val;
  };

  tb::ServerContext ctx;
  const auto pingNone = tb::handleApiRequest("/ping", "", ctx);
  CHECK(pingNone.status == 200);
  CHECK(pingNone.body.find("\"tablebaseLoaded\":false") != std::string::npos);
  CHECK(pingNone.body.find("\"tablebaseFormat\":\"none\"") != std::string::npos);
  CHECK(pingNone.body.find("\"rngMode\":null") != std::string::npos);

  // 1. Auto-detect SRTBV002 (NES) on openTablebase & verify /ping.
  ctx.configuredTbPath = nbNesPath;
  std::string err;
  REQUIRE(ctx.openTablebase(ctx.configuredTbPath, &err));
  CHECK(ctx.activeFormat() == tb::TablebaseFormat::NextBoxV2);
  const auto pingNes = tb::handleApiRequest("/ping", "", ctx);
  CHECK(pingNes.status == 200);
  CHECK(pingNes.body.find("\"tablebaseLoaded\":true") != std::string::npos);
  CHECK(pingNes.body.find("\"tablebaseFormat\":\"nextbox\"") != std::string::npos);
  CHECK(pingNes.body.find("\"rngMode\":\"nes\"") != std::string::npos);
  CHECK(pingNes.body.find("\"boardCount\":" + std::to_string(boards4x4.size()))
        != std::string::npos);

  // 2. Strict /moves validation for SRTBV002: missing or invalid 'next' rejected with 400,
  //    and valid /moves reports piece-specific V(b, c) (I vs O).
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=I", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=I&next=Q", ctx).status == 400);
  CHECK(tb::handleApiRequest("/moves", "board=0&piece=I&next=7", ctx).status == 400);
  CHECK(tb::handleApiRequest("/state-info", "board=0&next=Q", ctx).status == 400);

  const auto movesNbI = tb::handleApiRequest("/moves", "board=0&piece=I&next=O", ctx);
  const auto movesNbO = tb::handleApiRequest("/moves", "board=0&piece=O&next=I", ctx);
  CHECK(movesNbI.status == 200);
  CHECK(movesNbO.status == 200);
  CHECK(movesNbI.body.find("\"tablebaseFormat\":\"nextbox\"") != std::string::npos);
  CHECK(movesNbI.body.find("\"nextPiece\":1,\"nextPieceChar\":\"O\"") != std::string::npos);
  CHECK(movesNbI.body.find("\"inTablebase\":true") != std::string::npos);
  CHECK(extractJsonNumberAfter(movesNbI.body, "\"boardValue\":")
        == doctest::Approx(solved4x4.emptyBoardValues[tb::parsePieceChar('I')]).epsilon(1e-5));
  CHECK(extractJsonNumberAfter(movesNbO.body, "\"boardValue\":")
        == doctest::Approx(solved4x4.emptyBoardValues[tb::parsePieceChar('O')]).epsilon(1e-5));

  // 3. /state-info with and without 'next' on solved SRTBV002: numerically verify boardValue,
  //    boardValues[0..6], per-piece tablebaseValue & bellmanBackupValue, and top-level backup.
  const auto infoWithNext = tb::handleApiRequest("/state-info", "board=0&next=O", ctx);
  CHECK(infoWithNext.status == 200);
  CHECK(infoWithNext.body.find("\"tablebaseFormat\":\"nextbox\"") != std::string::npos);
  CHECK(infoWithNext.body.find("\"rngMode\":\"nes\"") != std::string::npos);
  CHECK(infoWithNext.body.find("\"nextPiece\":1,\"nextPieceChar\":\"O\"") != std::string::npos);
  CHECK(extractJsonNumberAfter(infoWithNext.body, "\"boardValue\":")
        == doctest::Approx(solved4x4.emptyBoardExpectedValue).epsilon(1e-5));
  const size_t topBackupPos = infoWithNext.body.rfind("\"bellmanBackupValue\":");
  REQUIRE(topBackupPos != std::string::npos);
  CHECK(extractJsonNumberAfter(infoWithNext.body, "\"bellmanBackupValue\":", topBackupPos)
        == doctest::Approx(solved4x4.emptyBoardExpectedValue).epsilon(1e-5));
  for (int c = 0; c < tb::NUM_PIECES; ++c) {
    const std::string pTag = "\"piece\":" + std::to_string(c) + ",";
    const size_t pPos = infoWithNext.body.find(pTag);
    REQUIRE(pPos != std::string::npos);
    CHECK(extractJsonNumberAfter(infoWithNext.body, "\"tablebaseValue\":", pPos)
          == doctest::Approx(solved4x4.emptyBoardValues[static_cast<size_t>(c)]).epsilon(1e-5));
    CHECK(extractJsonNumberAfter(infoWithNext.body, "\"bellmanBackupValue\":", pPos)
          == doctest::Approx(solved4x4.emptyBoardValues[static_cast<size_t>(c)]).epsilon(1e-5));
  }

  const auto infoNoNext = tb::handleApiRequest("/state-info", "board=0", ctx);
  CHECK(infoNoNext.status == 200);
  CHECK(infoNoNext.body.find("\"boardValues\":[") != std::string::npos);
  CHECK(infoNoNext.body.find("\"disagree\":true") == std::string::npos);

  // 3b. Top-out board under SRTBV002: /moves returns empty list; /state-info reports
  //     bestTablebaseQ:null, bellmanBackupValue:0, allInTablebase:false, disagree:false for O.
  const uint64_t topOutWall = mustParse("XXXXXXXXX.\n"
                                        "XXXXXXXXX.\n"
                                        "XXXXXXXXX.\n"
                                        "XXXXXXXXX.");
  const std::string topOutHex = tb::formatBoardHex(topOutWall);
  const auto topOutMoves =
      tb::handleApiRequest("/moves", "board=" + topOutHex + "&piece=O&next=I", ctx);
  CHECK(topOutMoves.status == 200);
  CHECK(topOutMoves.body.find("\"moves\":[]") != std::string::npos);
  const auto topOutInfo =
      tb::handleApiRequest("/state-info", "board=" + topOutHex + "&next=I", ctx);
  CHECK(topOutInfo.status == 200);
  const size_t oPos = topOutInfo.body.find("\"pieceChar\":\"O\"");
  REQUIRE(oPos != std::string::npos);
  CHECK(topOutInfo.body.find("\"numTransitions\":0", oPos) != std::string::npos);
  CHECK(topOutInfo.body.find("\"bestTablebaseQ\":null", oPos) != std::string::npos);
  CHECK(extractJsonNumberAfter(topOutInfo.body, "\"bellmanBackupValue\":", oPos) == 0.0);
  CHECK(topOutInfo.body.find("\"allInTablebase\":false,\"disagree\":false", oPos)
        != std::string::npos);

  // 4. Reload switching to synthetic Uniform SRTBV002 -> verify exact Q=123.5, disagreement on I,
  //    and all-OUT fallback (disagree:false) on O.
  ctx.configuredTbPath = nbUniPath;
  const auto reloadUni = tb::handleApiRequest("/reload", "", ctx);
  CHECK(reloadUni.status == 200);
  CHECK(reloadUni.body.find("\"tablebaseFormat\":\"nextbox\"") != std::string::npos);
  CHECK(reloadUni.body.find("\"rngMode\":\"uniform\"") != std::string::npos);
  CHECK(reloadUni.body.find("\"emptyBoardValue\":null") != std::string::npos);

  const auto movesUni = tb::handleApiRequest("/moves", "board=0&piece=I&next=O", ctx);
  CHECK(movesUni.status == 200);
  CHECK(movesUni.body.find("\"tablebaseQ\":123.5") != std::string::npos);
  const auto infoUni = tb::handleApiRequest("/state-info", "board=0&next=O", ctx);
  CHECK(infoUni.status == 200);
  CHECK(infoUni.body.find("\"boardValues\":null") != std::string::npos);
  CHECK(infoUni.body.find("\"bestTablebaseQ\":123.5") != std::string::npos);
  CHECK(infoUni.body.find("\"disagree\":true") != std::string::npos);
  const size_t oUniPos = infoUni.body.find("\"pieceChar\":\"O\"");
  REQUIRE(oUniPos != std::string::npos);
  CHECK(infoUni.body.find("\"anyInTablebase\":false,\"allInTablebase\":false,\"disagree\":false",
                          oUniPos)
        != std::string::npos);

  // 5. Failed /reload must return 500 and keep the previously loaded Uniform SRTBV002 active.
  ctx.configuredTbPath = badPath;
  CHECK(tb::handleApiRequest("/reload", "", ctx).status == 500);
  CHECK(ctx.activeFormat() == tb::TablebaseFormat::NextBoxV2);
  CHECK(ctx.nextBoxTablebase.rngMode() == tb::RngMode::Uniform);

  // 6. Reload switching from SRTBV002 back to SRTBV001 NNB closes NextBox mmap and restores NNB
  //    endpoints (including /moves without 'next' and /state-info with optional 'next').
  ctx.configuredTbPath = nnbPath;
  const auto reloadNnb = tb::handleApiRequest("/reload", "", ctx);
  CHECK(reloadNnb.status == 200);
  CHECK(reloadNnb.body.find("\"tablebaseFormat\":\"nnb\"") != std::string::npos);
  CHECK(reloadNnb.body.find("\"rngMode\":\"uniform\"") != std::string::npos);
  CHECK_FALSE(ctx.nextBoxTablebase.isLoaded());
  CHECK(ctx.tablebase.isLoaded());

  const auto movesNnb = tb::handleApiRequest("/moves", "board=0&piece=I", ctx);
  CHECK(movesNnb.status == 200);
  CHECK(movesNnb.body.find("\"tablebaseQ\":77") != std::string::npos);
  const auto infoHybrid = tb::handleApiRequest("/state-info", "board=0&next=O", ctx);
  CHECK(infoHybrid.status == 200);
  CHECK(infoHybrid.body.find("\"tablebaseFormat\":\"nnb\"") != std::string::npos);
  CHECK(infoHybrid.body.find("\"nextPiece\":1,\"nextPieceChar\":\"O\"") != std::string::npos);

  ctx.closeAll();
  std::remove(nbNesPath.c_str());
  std::remove(nbUniPath.c_str());
  std::remove(nnbPath.c_str());
  std::remove(badPath.c_str());
}

TEST_CASE("Linear distillation: Ridge recovery, gradient check, V2 features, and 1T/4T sampling") {
  // 1. Exact Ridge regression recovery on synthetic noiseless linear data + singular guard.
  {
    std::vector<double> singA = {1.0, 2.0, 2.0, 4.0};
    std::vector<double> singB = {3.0, 6.0};
    std::vector<double> sol;
    CHECK_FALSE(tb::solveLinearSystem(singA, singB, 2, sol));
    CHECK_FALSE(tb::solveLinearSystem({}, {}, 0, sol));

    const double trueIntercept = 42.5;
    const std::array<double, 3> trueW = {-7.25, 3.5, 1.125};
    std::vector<tb::LabeledDecisionSample> syn;
    for (int i = 0; i < 64; ++i) {
      tb::LabeledDecisionSample s{};
      s.board = static_cast<uint64_t>(i);
      s.boardInTb = true;
      const double x0 = static_cast<double>(i % 5);
      const double x1 = static_cast<double>((i / 5) % 4);
      const double x2 = static_cast<double>((i * 7 + 3) % 11);
      s.expectedVStar = trueIntercept + trueW[0] * x0 + trueW[1] * x1 + trueW[2] * x2;
      syn.push_back(s);
    }
    const auto synFeat = [](uint64_t b, double *out) {
      const int i = static_cast<int>(b);
      out[0] = static_cast<double>(i % 5);
      out[1] = static_cast<double>((i / 5) % 4);
      out[2] = static_cast<double>((i * 7 + 3) % 11);
    };
    const tb::RidgeRegressionResult fit = tb::fitRidgeValueModel(syn, 3, 0.0, synFeat);
    REQUIRE(fit.ok);
    CHECK(fit.intercept == doctest::Approx(trueIntercept).epsilon(1e-9));
    for (int j = 0; j < 3; ++j) {
      CHECK(fit.weights[static_cast<size_t>(j)]
            == doctest::Approx(trueW[static_cast<size_t>(j)]).epsilon(1e-9));
    }
    CHECK(fit.r2 == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(fit.rmse == doctest::Approx(0.0).epsilon(1e-9));
  }

  // 2. V2 residual-guided feature extraction sanity on explicit ASCII boards:
  //    (a) Buried row-0 pinhole under 2 covering rows.
  const uint64_t buriedBoard = mustParse("..........\n"
                                         "XXXX......\n"
                                         "XXXX......\n"
                                         "XX.X......");
  double fBuried[tb::V2_NUM_BOARD_FEATURES] = {};
  tb::extractV2BoardFeatures(buriedBoard, fBuried);
  CHECK(fBuried[tb::V2_SEALED_HOLES] == doctest::Approx(1.0));
  CHECK(fBuried[tb::V2_OVERHANGS] == doctest::Approx(0.0));
  CHECK(fBuried[tb::V2_HAS_ANY_HOLE] == doctest::Approx(1.0));
  CHECK(fBuried[tb::V2_HOLE_COLUMNS] == doctest::Approx(1.0));
  CHECK(fBuried[tb::V2_HOLE_ROWS] == doctest::Approx(1.0));
  CHECK(fBuried[tb::V2_COVERED_MINO_COUNT] == doctest::Approx(2.0));
  CHECK(fBuried[tb::V2_BURIED_DEPTH_2PLUS] == doctest::Approx(1.0));
  CHECK(fBuried[tb::V2_COVER_ROW_MISSING] == doctest::Approx(6.0));
  CHECK(fBuried[tb::V2_MIN_HOLE_ROW_PENALTY] == doctest::Approx(3.0));
  CHECK(fBuried[tb::V2_TOTAL_CELLS] == doctest::Approx(11.0));
  CHECK(fBuried[tb::V2_MAX_HEIGHT] == doctest::Approx(3.0));
  CHECK(fBuried[tb::V2_ADJ_HIGH_COL_PAIRS] == doctest::Approx(3.0));

  //    (b) Contiguous 3-hole cavity under one nearly-full covering row (1 cell missing).
  const uint64_t cavityBoard = mustParse("..........\n"
                                         "..........\n"
                                         "XXXXXXX.XX\n"
                                         "XXXX...XXX");
  double fCavity[tb::V2_NUM_BOARD_FEATURES] = {};
  tb::extractV2BoardFeatures(cavityBoard, fCavity);
  CHECK(fCavity[tb::V2_SEALED_HOLES] == doctest::Approx(3.0));
  CHECK(fCavity[tb::V2_HAS_ANY_HOLE] == doctest::Approx(1.0));
  CHECK(fCavity[tb::V2_HOLE_COLUMNS] == doctest::Approx(3.0));
  CHECK(fCavity[tb::V2_HOLE_ROWS] == doctest::Approx(1.0));
  CHECK(fCavity[tb::V2_COVERED_MINO_COUNT] == doctest::Approx(3.0));
  CHECK(fCavity[tb::V2_BURIED_DEPTH_2PLUS] == doctest::Approx(0.0));
  CHECK(fCavity[tb::V2_COVER_ROW_MISSING] == doctest::Approx(1.0));
  CHECK(fCavity[tb::V2_MIN_HOLE_ROW_PENALTY] == doctest::Approx(3.0));

  //    (c) Filled cell below a hole (excluded from coveredMinoCount) + stacked holes + deep well.
  const uint64_t stackedHoleBoard = mustParse("X.X.......\n"
                                              "X.X.......\n"
                                              "..X.......\n"
                                              "X.X.......");
  double fStacked[tb::V2_NUM_BOARD_FEATURES] = {};
  tb::extractV2BoardFeatures(stackedHoleBoard, fStacked);
  CHECK(fStacked[tb::V2_HOLE_COLUMNS] == doctest::Approx(1.0));
  CHECK(fStacked[tb::V2_COVERED_MINO_COUNT] == doctest::Approx(2.0)); // Rows 2&3, NOT row 0
  CHECK(fStacked[tb::V2_BURIED_DEPTH_2PLUS] == doctest::Approx(1.0));
  CHECK(fStacked[tb::V2_MIN_HOLE_ROW_PENALTY] == doctest::Approx(2.0)); // Lowest hole in row 1
  CHECK(fStacked[tb::V2_WELL_COUNT] == doctest::Approx(1.0));           // Col 1 depth 4
  CHECK(fStacked[tb::V2_MAX_WELL_DEPTH] == doctest::Approx(4.0));
  CHECK(fStacked[tb::V2_EDGE_HEIGHT_SUM] == doctest::Approx(4.0));
  CHECK(fStacked[tb::V2_CENTER_HEIGHT_SUM] == doctest::Approx(0.0));

  //    (d) Empty board transitions, zeroed hole metrics, and V1/V2 prefix parity across boards.
  double fEmpty[tb::V2_NUM_BOARD_FEATURES] = {};
  tb::extractV2BoardFeatures(0ULL, fEmpty);
  CHECK(fEmpty[tb::V2_HAS_ANY_HOLE] == doctest::Approx(0.0));
  CHECK(fEmpty[tb::V2_COVER_ROW_MISSING] == doctest::Approx(0.0));
  CHECK(fEmpty[tb::V2_MIN_HOLE_ROW_PENALTY] == doctest::Approx(0.0));
  CHECK(fEmpty[tb::V2_ROW_TRANSITIONS] == doctest::Approx(8.0));
  CHECK(fEmpty[tb::V2_COL_TRANSITIONS] == doctest::Approx(10.0));

  for (uint64_t b : {0ULL, buriedBoard, cavityBoard, stackedHoleBoard}) {
    double v1f[tb::V1_NUM_BOARD_FEATURES] = {};
    double v2f[tb::V2_NUM_BOARD_FEATURES] = {};
    tb::extractV1BoardFeatures(b, v1f);
    tb::extractV2BoardFeatures(b, v2f);
    for (int j = 0; j < tb::V1_NUM_BOARD_FEATURES; ++j) {
      CHECK(v1f[j] == doctest::Approx(v2f[j]));
    }
  }

  // 3. Deterministic 1T vs 4T trajectory sampling on a small solved Next-Box TB, plus exact
  //    finite-difference gradient verification, trainer smoke test, and hostile/edge guards.
  const std::vector<uint64_t> boards2x10 = tb::enumerateZeroSealedHoleBoards(1, 4, 2, 10);
  tb::NextBoxSolveConfig solveCfg{};
  solveCfg.rngMode = tb::RngMode::Nes;
  solveCfg.tolerance = 1e-8;
  solveCfg.innerEvalSweeps = 20;
  solveCfg.numThreads = 4;
  const tb::NextBoxSolveResult solved2x10 = tb::solveNextBoxMPI(boards2x10, solveCfg);
  REQUIRE(solved2x10.ok);

  const std::string tmpDistillTb = "build/test_distill_2x10.tb.bin";
  REQUIRE(tb::writeNextBoxTablebaseFile(tmpDistillTb, boards2x10, solved2x10.values,
                                        tb::RngMode::Nes, solved2x10.finalBellmanResidual));
  tb::MappedNextBoxTablebase mappedTb;
  REQUIRE(mappedTb.open(tmpDistillTb));
  tb::IndexedNextBoxTablebase idxTb(mappedTb);

  tb::DistillSampleConfig sCfg{};
  sCfg.numGames = 40;
  sCfg.maxDecisions = 120;
  sCfg.baseSeed = 9001ULL;
  sCfg.numThreads = 1;
  const tb::DistillDataset ds1 = tb::sampleDistillDataset(idxTb, sCfg);
  sCfg.numThreads = 4;
  const tb::DistillDataset ds4 = tb::sampleDistillDataset(idxTb, sCfg);

  REQUIRE_FALSE(ds1.decisions.empty());
  REQUIRE(ds1.decisions.size() == ds4.decisions.size());
  CHECK(ds1.totalStepsSimulated == ds4.totalStepsSimulated);
  CHECK(ds1.skippedIncomplete == ds4.skippedIncomplete);
  for (size_t i = 0; i < ds1.decisions.size(); ++i) {
    CHECK(ds1.decisions[i].board == ds4.decisions[i].board);
    CHECK(ds1.decisions[i].curPiece == ds4.decisions[i].curPiece);
    CHECK(ds1.decisions[i].nextPiece == ds4.decisions[i].nextPiece);
    CHECK(ds1.decisions[i].moves.size() == ds4.decisions[i].moves.size());
  }

  // Zero-game/decision and requireAllMovesInTb filtering checks.
  {
    tb::DistillSampleConfig zeroCfg = sCfg;
    zeroCfg.numGames = 0;
    CHECK(tb::sampleDistillDataset(idxTb, zeroCfg).decisions.empty());
    zeroCfg.numGames = 10;
    zeroCfg.maxDecisions = 0;
    CHECK(tb::sampleDistillDataset(idxTb, zeroCfg).decisions.empty());

    tb::DistillSampleConfig strictCfg = sCfg;
    strictCfg.requireAllMovesInTb = true;
    const tb::DistillDataset dsStrict = tb::sampleDistillDataset(idxTb, strictCfg);
    CHECK(dsStrict.skippedIncomplete >= ds1.skippedIncomplete);
  }

  // Oracle ranker + degenerate empty-move decision guard in evaluateMoveRanker.
  const tb::RankingEvalMetrics oracleM =
      tb::evaluateMoveRanker(ds1.decisions, [&](uint64_t afterB, int lc, int nxt) {
        float v = 0.0f;
        idxTb.lookupPiece(afterB, nxt, v);
        return static_cast<double>(lc) + static_cast<double>(v);
      });
  CHECK(oracleM.top1ExactPct == doctest::Approx(100.0));
  CHECK(oracleM.meanRegret == doctest::Approx(0.0).epsilon(1e-9));
  {
    std::vector<tb::LabeledDecisionSample> emptyMoves(3);
    const tb::RankingEvalMetrics em =
        tb::evaluateMoveRanker(emptyMoves, [](uint64_t, int, int) { return 0.0; });
    CHECK(em.numDecisions == 0);
    CHECK(em.p95Regret == 0.0);
  }

  // Ridge edge guards: negative/NaN lambda, empty samples, constant-target R^2, residual outliers.
  {
    CHECK_FALSE(tb::fitRidgeValueModel({}, 3, 1.0, tb::extractV1BoardFeatures).ok);
    CHECK_FALSE(tb::fitRidgeValueModel(ds1.decisions, tb::V1_NUM_BOARD_FEATURES, -1.0,
                                       tb::extractV1BoardFeatures)
                    .ok);
    CHECK_FALSE(tb::fitRidgeValueModel(ds1.decisions, tb::V1_NUM_BOARD_FEATURES,
                                       std::numeric_limits<double>::quiet_NaN(),
                                       tb::extractV1BoardFeatures)
                    .ok);
    std::vector<tb::LabeledDecisionSample> constTarget(4);
    for (auto &s : constTarget) {
      s.board = 0ULL;
      s.boardInTb = true;
      s.expectedVStar = 5.0;
    }
    tb::RidgeRegressionResult constRes{};
    constRes.r2 = 0.99;
    tb::evaluateRidgeFit(
        constTarget, 1, 9.0, {0.0}, [](uint64_t, double *o) { o[0] = 0.0; }, constRes);
    CHECK(constRes.r2 == doctest::Approx(0.0)); // Wrong constant prediction -> R^2 == 0, not 1
    tb::evaluateRidgeFit({}, 1, 0.0, {0.0}, [](uint64_t, double *o) { o[0] = 0.0; }, constRes);
    CHECK(constRes.rmse == 0.0);

    // Outlier collection deduplicates repeated boards and orders over/under tails correctly.
    std::vector<tb::LabeledDecisionSample> outSamples(4);
    outSamples[0].board = 0ULL;
    outSamples[0].boardInTb = true;
    outSamples[0].expectedVStar = 10.0;
    outSamples[1].board = 0ULL; // Duplicate board
    outSamples[1].boardInTb = true;
    outSamples[1].expectedVStar = 10.0;
    outSamples[2].board = buriedBoard;
    outSamples[2].boardInTb = true;
    outSamples[2].expectedVStar = 2.0;
    outSamples[3].board = cavityBoard;
    outSamples[3].boardInTb = true;
    outSamples[3].expectedVStar = 25.0;
    std::vector<tb::ResidualOutlier> over, under;
    tb::collectResidualOutliers(outSamples, tb::V1_NUM_BOARD_FEATURES, 10.0,
                                std::vector<double>(tb::V1_NUM_BOARD_FEATURES, 0.0), 5,
                                tb::extractV1BoardFeatures, over, under);
    REQUIRE(over.size() == 3);
    REQUIRE(under.size() == 3);
    CHECK(over[0].board == buriedBoard);  // pred=10, V*=2 -> err=+8
    CHECK(under[0].board == cavityBoard); // pred=10, V*=25 -> err=-15
  }

  // Finite-difference numerical gradient check across all 12 parameters (intercept, lineClear, 10
  // V1 features) for both fixLineClearToOne = false and fixLineClearToOne = true, plus
  // useExpectedTarget=false and extreme Q/score magnitudes (overflow-free softmax).
  std::vector<size_t> order(ds1.decisions.size());
  std::iota(order.begin(), order.end(), 0);
  const size_t checkN = std::min<size_t>(order.size(), 25);
  std::vector<double> w(tb::V1_NUM_BOARD_FEATURES + tb::LINEAR_PARAM_BOARD_OFFSET, 0.0);
  w[tb::LINEAR_PARAM_INTERCEPT] = 12.3;
  w[tb::LINEAR_PARAM_LINE_CLEAR] = 1.25;
  for (int j = 0; j < tb::V1_NUM_BOARD_FEATURES; ++j) {
    w[static_cast<size_t>(j + tb::LINEAR_PARAM_BOARD_OFFSET)] =
        -0.35 + 0.09 * static_cast<double>(j);
  }
  for (bool fixLc : {false, true}) {
    std::vector<double> grad;
    std::vector<double> dummyGrad;
    tb::computeLinearJointLossAndGrad(ds1.decisions, order, 0, checkN, w, /*temperature=*/0.8,
                                      /*valueLossWeight=*/0.05, /*l2Reg=*/1e-3,
                                      /*useExpectedTarget=*/!fixLc, fixLc,
                                      tb::extractV1BoardFeatures, grad);
    constexpr double EPS = 1e-6;
    for (size_t k = 0; k < w.size(); ++k) {
      if (fixLc && k == static_cast<size_t>(tb::LINEAR_PARAM_LINE_CLEAR)) {
        CHECK(grad[k] == 0.0);
        continue;
      }
      std::vector<double> wp = w;
      std::vector<double> wm = w;
      wp[k] += EPS;
      wm[k] -= EPS;
      const double lp =
          tb::computeLinearJointLossAndGrad(ds1.decisions, order, 0, checkN, wp, 0.8, 0.05, 1e-3,
                                            !fixLc, fixLc, tb::extractV1BoardFeatures, dummyGrad);
      const double lm =
          tb::computeLinearJointLossAndGrad(ds1.decisions, order, 0, checkN, wm, 0.8, 0.05, 1e-3,
                                            !fixLc, fixLc, tb::extractV1BoardFeatures, dummyGrad);
      const double numGrad = (lp - lm) / (2.0 * EPS);
      const double denom = std::max(1.0, std::abs(numGrad));
      CHECK(std::abs(grad[k] - numGrad) / denom < 1e-6);
    }
  }
  {
    // Extreme-magnitude Q/score softmax stability check (must remain finite without NaN/Inf).
    tb::LabeledDecisionSample extreme{};
    extreme.board = 0ULL;
    extreme.moves.push_back({0ULL, 0, 1e4, 1e4});
    extreme.moves.push_back({buriedBoard, 1, -1e4, -1e4});
    std::vector<tb::LabeledDecisionSample> exVec = {extreme};
    std::vector<size_t> exIdx = {0};
    std::vector<double> exGrad;
    std::vector<double> bigW = w;
    bigW[tb::LINEAR_PARAM_LINE_CLEAR] = 1e4;
    const double exLoss = tb::computeLinearJointLossAndGrad(
        exVec, exIdx, 0, 1, bigW, 1.0, 0.0, 0.0, false, false, tb::extractV1BoardFeatures, exGrad);
    CHECK(std::isfinite(exLoss));
    for (double g : exGrad) {
      CHECK(std::isfinite(g));
    }
  }

  // Lightweight smoke test for trainLinearRegretRanking (finite outputs & fixLineClearToOne).
  {
    tb::LinearRankingConfig rCfg{};
    rCfg.epochs = 3;
    rCfg.batchSize = 32;
    rCfg.fixLineClearToOne = true;
    const std::vector<double> zeroW(tb::V1_NUM_BOARD_FEATURES, 0.0);
    const tb::LinearRankingResult rRes =
        tb::trainLinearRegretRanking(ds1.decisions, ds1.decisions, tb::V1_NUM_BOARD_FEATURES, 1.0,
                                     zeroW, 2.5, rCfg, tb::extractV1BoardFeatures);
    CHECK(rRes.lineClearWeight == doctest::Approx(1.0));
    CHECK(std::isfinite(rRes.intercept));
    CHECK(std::isfinite(rRes.bestValLoss));
    CHECK(rRes.boardWeights.size() == tb::V1_NUM_BOARD_FEATURES);
  }

  // 4. defaultV2LinearEval() edge cases: clamping at 0, ply-2 top-out return, top-out avoidance.
  {
    tb::LinearBoardEvalV1 negEval{};
    negEval.intercept = -50.0;
    CHECK(negEval.evalBoard(0ULL) == 0.0);
  }
  const tb::LinearBoardEvalV2 v2 = tb::defaultV2LinearEval();
  const uint64_t tetrisReady = mustParse("XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.");
  CHECK_FALSE(
      v2.pickBestNextBox2Ply(tetrisReady, tb::parsePieceChar('O'), tb::parsePieceChar('I')).found);
  CHECK(v2.scoreNextBox2Ply(tetrisReady, 2, tb::parsePieceChar('O')) == doctest::Approx(2.0));
  const uint64_t narrowO = mustParse("XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...");
  const auto avoidO =
      v2.pickBestNextBox2Ply(narrowO, tb::parsePieceChar('I'), tb::parsePieceChar('O'));
  REQUIRE(avoidO.found);
  tb::TransitionList oAfter;
  tb::generateTransitions(avoidO.transition.afterBoard, tb::parsePieceChar('O'), oAfter);
  CHECK_FALSE(oAfter.empty());

  mappedTb.close();
  std::remove(tmpDistillTb.c_str());
}

TEST_CASE("TinyNextBoxNet: gradient check, SRNNV001 I/O corruption guards, and 1T/4T parity") {
  // 1. Exact central finite-difference gradient check across ALL 6 parameter blocks
  //    (W1, b1, W2, b2, W3, b3) with parent Huber value, child Huber value, Boltzmann ranking CE,
  //    and L2 weight decay all active simultaneously.
  const uint64_t b0 = 0ULL;
  const uint64_t b1 = mustParse("..........\n"
                                "..........\n"
                                "XXXX......\n"
                                "XXXX....XX");
  const uint64_t b2 = mustParse("..........\n"
                                "XXXX......\n"
                                "XXXX......\n"
                                "XX.X......");
  const uint64_t b3 = mustParse("..........\n"
                                "..........\n"
                                "XXXXXXX.XX\n"
                                "XXXX...XXX");

  tb::LabeledDecisionSample s1{};
  s1.board = b1;
  s1.curPiece = 0;
  s1.nextPiece = 2;
  s1.boardInTb = true;
  s1.vStar = {12.5f, 11.0f, 14.2f, 10.5f, 9.8f, 13.1f, 11.4f};
  s1.moves.push_back({b0, 1, 20.5, 20.5});
  s1.moves.push_back({b2, 0, 5.2, 5.2});
  s1.moves.push_back({b3, 0, 11.8, 11.8});

  tb::LabeledDecisionSample s2{};
  s2.board = b2;
  s2.curPiece = 3;
  s2.nextPiece = 5;
  s2.boardInTb = true;
  s2.vStar = {3.2f, 4.1f, 2.9f, 5.0f, 3.7f, 6.4f, 4.8f};
  s2.moves.push_back({b1, 0, 13.1, 13.1});
  s2.moves.push_back({b3, 1, 9.5, 9.5});

  const std::vector<tb::LabeledDecisionSample> synDecisions = {s1, s2};
  const std::vector<size_t> order = {0, 1};

  tb::TinyNextBoxNet net;
  net.initRandom(/*hidden1=*/8, /*hidden2=*/6, tb::RngMode::Nes, 424242ULL, 8.0f);
  tb::NnLossConfig lossCfg{};
  lossCfg.huberDelta = 2.0;
  lossCfg.valueWeight = 1.0;
  lossCfg.childValueWeight = 0.75;
  lossCfg.rankWeight = 2.5;
  lossCfg.temperature = 1.2;
  lossCfg.weightDecay = 1e-2;

  std::vector<float> analyticGrad;
  std::vector<float> dummyGrad;
  tb::NnBatchScratch scratch;
  const tb::NnLossBreakdown baseLoss = tb::computeNnSliceLossAndGrad(
      net, synDecisions, order, 0, synDecisions.size(), lossCfg, analyticGrad, scratch, true);
  CHECK(std::isfinite(baseLoss.totalLoss));
  REQUIRE(analyticGrad.size() == net.params.size());

  // Check representative indices in every single parameter block (W1, b1, W2, b2, W3, b3) using
  // central differences; skip any parameter sitting right on a ReLU kink (detected by asymmetric
  // one-sided slopes differing markedly).
  const std::vector<std::pair<size_t, size_t>> blocks = {
      {net.offW1(), static_cast<size_t>(net.inDim) * static_cast<size_t>(net.h1)},
      {net.offB1(), static_cast<size_t>(net.h1)},
      {net.offW2(), static_cast<size_t>(net.h1) * static_cast<size_t>(net.h2)},
      {net.offB2(), static_cast<size_t>(net.h2)},
      {net.offW3(), static_cast<size_t>(net.outDim) * static_cast<size_t>(net.h2)},
      {net.offB3(), static_cast<size_t>(net.outDim)},
  };
  constexpr float EPS = 1e-3f;
  for (const auto &blk : blocks) {
    const size_t step = std::max<size_t>(1, blk.second / 12);
    size_t checkedInBlock = 0;
    for (size_t off = 0; off < blk.second; off += step) {
      const size_t idx = blk.first + off;
      const float orig = net.params[idx];
      net.params[idx] = orig + EPS;
      const double lp =
          tb::computeNnSliceLossAndGrad(net, synDecisions, order, 0, synDecisions.size(), lossCfg,
                                        dummyGrad, scratch, true)
              .totalLoss;
      net.params[idx] = orig - EPS;
      const double lm =
          tb::computeNnSliceLossAndGrad(net, synDecisions, order, 0, synDecisions.size(), lossCfg,
                                        dummyGrad, scratch, true)
              .totalLoss;
      net.params[idx] = orig;
      const double slopeP = (lp - baseLoss.totalLoss) / static_cast<double>(EPS);
      const double slopeM = (baseLoss.totalLoss - lm) / static_cast<double>(EPS);
      if (std::abs(slopeP - slopeM) > 1e-2 * std::max(1.0, std::abs(slopeP))) {
        continue; // ReLU/Huber non-differentiable kink within +-EPS
      }
      const double numGrad = (lp - lm) / (2.0 * static_cast<double>(EPS));
      const double denom = std::max(1.0, std::abs(numGrad));
      CHECK(std::abs(static_cast<double>(analyticGrad[idx]) - numGrad) / denom < 2e-3);
      ++checkedInBlock;
    }
    CHECK(checkedInBlock > 0);
  }

  // 2. evalPiece vs evalAll exact parity + SRNNV001 save/load round-trip & corruption rejection.
  for (uint64_t b : {b0, b1, b2, b3}) {
    float allV[tb::NUM_PIECES] = {};
    net.evalAll(b, allV);
    for (int p = 0; p < tb::NUM_PIECES; ++p) {
      CHECK(net.evalPiece(b, p) == doctest::Approx(allV[p]).epsilon(1e-6));
    }
  }

  const std::string tmpNnPath = "build/test_tiny_nn.nn.bin";
  const std::string tmpCorruptNn = "build/test_tiny_nn_corrupt.nn.bin";
  std::string err;
  REQUIRE(net.save(tmpNnPath, &err));

  tb::TinyNextBoxNet loaded;
  REQUIRE(loaded.load(tmpNnPath, &err));
  CHECK(loaded.isLoaded());
  CHECK(loaded.inDim == net.inDim);
  CHECK(loaded.h1 == net.h1);
  CHECK(loaded.h2 == net.h2);
  CHECK(loaded.outDim == net.outDim);
  CHECK(loaded.rngMode == net.rngMode);
  CHECK(loaded.params == net.params);

  // Save guards: uninitialized net and NaN weight must be rejected.
  tb::TinyNextBoxNet emptyNet;
  CHECK_FALSE(emptyNet.save(tmpCorruptNn, &err));
  tb::TinyNextBoxNet nanNet = net;
  nanNet.params[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(nanNet.save(tmpCorruptNn, &err));

  // Corrupt binary header/payload tests.
  {
    std::ifstream src(tmpNnPath, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(src)),
                            std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() == net.byteSize());
    const auto writeBytes = [&](const std::vector<char> &buf) {
      std::ofstream(tmpCorruptNn, std::ios::binary | std::ios::trunc)
          .write(buf.data(), static_cast<std::streamsize>(buf.size()));
    };

    tb::TinyNextBoxNet bad;
    // Wrong magic
    auto badMagic = bytes;
    badMagic[0] = 'X';
    writeBytes(badMagic);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    // Wrong featureVersion
    auto badVer = bytes;
    const uint32_t v99 = 99U;
    std::memcpy(badVer.data() + offsetof(tb::TinyNnHeader, featureVersion), &v99, sizeof(v99));
    writeBytes(badVer);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    // Zero/oversized h1 & h2, bad inDim/outDim
    auto badH1 = bytes;
    const uint32_t hugeH1 = tb::NN_MAX_HIDDEN_DIM + 1;
    std::memcpy(badH1.data() + offsetof(tb::TinyNnHeader, h1), &hugeH1, sizeof(hugeH1));
    writeBytes(badH1);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    auto zeroH1 = bytes;
    const uint32_t zeroU = 0U;
    std::memcpy(zeroH1.data() + offsetof(tb::TinyNnHeader, h1), &zeroU, sizeof(zeroU));
    writeBytes(zeroH1);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    auto zeroH2 = bytes;
    std::memcpy(zeroH2.data() + offsetof(tb::TinyNnHeader, h2), &zeroU, sizeof(zeroU));
    writeBytes(zeroH2);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    auto badInDim = bytes;
    const uint32_t badIn = 72U;
    std::memcpy(badInDim.data() + offsetof(tb::TinyNnHeader, inDim), &badIn, sizeof(badIn));
    writeBytes(badInDim);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    auto badOutDim = bytes;
    const uint32_t badOut = 6U;
    std::memcpy(badOutDim.data() + offsetof(tb::TinyNnHeader, outDim), &badOut, sizeof(badOut));
    writeBytes(badOutDim);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    // Invalid rngMode
    auto badRng = bytes;
    const uint32_t badR = 2U;
    std::memcpy(badRng.data() + offsetof(tb::TinyNnHeader, rngMode), &badR, sizeof(badR));
    writeBytes(badRng);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    // Truncated header, truncated payload, bloated payload, nonexistent file
    writeBytes(std::vector<char>(bytes.begin(), bytes.begin() + 16));
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));
    writeBytes(std::vector<char>(bytes.begin(), bytes.end() - 4));
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));
    auto bloated = bytes;
    bloated.push_back(0);
    writeBytes(bloated);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));
    CHECK_FALSE(bad.load("build/nonexistent_tiny_nn.nn.bin", &err));

    // Non-finite float weights (NaN & +Inf) + transactional preservation of loaded net on failure
    auto nanPayload = bytes;
    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(nanPayload.data() + sizeof(tb::TinyNnHeader), &nanVal, sizeof(nanVal));
    writeBytes(nanPayload);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    auto infPayload = bytes;
    const float infVal = std::numeric_limits<float>::infinity();
    std::memcpy(infPayload.data() + sizeof(tb::TinyNnHeader), &infVal, sizeof(infVal));
    writeBytes(infPayload);
    CHECK_FALSE(bad.load(tmpCorruptNn, &err));

    tb::TinyNextBoxNet infNet = net;
    infNet.params[1] = -infVal;
    CHECK_FALSE(infNet.save(tmpCorruptNn, &err));

    // Failed load must not corrupt an already-loaded network.
    const std::vector<float> savedParams = loaded.params;
    CHECK_FALSE(loaded.load(tmpCorruptNn, &err));
    CHECK(loaded.isLoaded());
    CHECK(loaded.params == savedParams);
  }

  // Uniform RngMode save/load round-trip.
  {
    tb::TinyNextBoxNet uniNet;
    REQUIRE(uniNet.initRandom(8, 6, tb::RngMode::Uniform, 999ULL, 1.5f));
    const std::string tmpUniNn = "build/test_tiny_nn_uni.nn.bin";
    REQUIRE(uniNet.save(tmpUniNn, &err));
    tb::TinyNextBoxNet uniLoaded;
    REQUIRE(uniLoaded.load(tmpUniNn, &err));
    CHECK(uniLoaded.rngMode == tb::RngMode::Uniform);
    CHECK(uniLoaded.params == uniNet.params);
    std::remove(tmpUniNn.c_str());
  }

  // 3. Bitwise extractNnFeatures parity vs extractV2BoardFeatures + fast hasAnyLegalTransition
  //    exact parity vs generateTransitions across 128 boards x 7 pieces.
  std::vector<uint64_t> testBoards = {b0, b1, b2, b3};
  tb::Rng bRng(1234ULL);
  for (int i = 0; i < 124; ++i) {
    testBoards.push_back(bRng.nextU64() & tb::VALID_BOARD_MASK);
  }
  for (uint64_t tbBoard : testBoards) {
    float nnF[tb::NN_INPUT_DIM] = {};
    double v2F[tb::V2_NUM_BOARD_FEATURES] = {};
    tb::extractNnFeatures(tbBoard, nnF);
    tb::extractV2BoardFeatures(tbBoard, v2F);
    CHECK(nnF[tb::NN_FEAT_GLOBALS_OFF + 21] * 30.0f
          == doctest::Approx(v2F[tb::V2_ROW_TRANSITIONS]).epsilon(1e-4));
    CHECK(nnF[tb::NN_FEAT_GLOBALS_OFF + 22] * 30.0f
          == doctest::Approx(v2F[tb::V2_COL_TRANSITIONS]).epsilon(1e-4));
    CHECK(nnF[tb::NN_FEAT_GLOBALS_OFF + 5] * 10.0f
          == doctest::Approx(v2F[tb::V2_COVERED_MINO_COUNT]).epsilon(1e-4));
    CHECK(nnF[tb::NN_FEAT_GLOBALS_OFF + 6] * 6.0f
          == doctest::Approx(v2F[tb::V2_BURIED_DEPTH_2PLUS]).epsilon(1e-4));
    for (int p = 0; p < tb::NUM_PIECES; ++p) {
      tb::TransitionList tl;
      tb::generateTransitions(tbBoard, p, tl);
      CHECK(tb::hasAnyLegalTransition(tbBoard, p) == !tl.empty());
    }
  }

  // 4. 1T vs 4T inference parity across boards, evalNextPieceExpected Markov + per-piece clamping,
  //    evaluateNnValueAccuracy, hostile loss configs, and top-out / ply-2 avoidance checks.
  std::vector<std::array<float, tb::NUM_PIECES>> out1T(testBoards.size());
  std::vector<std::array<float, tb::NUM_PIECES>> out4T(testBoards.size());
  for (size_t i = 0; i < testBoards.size(); ++i) {
    loaded.evalAll(testBoards[i], out1T[i].data());
  }
  tb::detail::runWorkerPool(4, testBoards.size(), 1, [&](int tid) {
    for (size_t i = static_cast<size_t>(tid); i < testBoards.size(); i += 4) {
      loaded.evalAll(testBoards[i], out4T[i].data());
    }
  });
  CHECK(out1T == out4T);

  // Per-piece max(0, v) clamping + exact Markov transition weights in evalNextPieceExpected.
  {
    tb::TinyNextBoxNet biasNet;
    REQUIRE(biasNet.initRandom(4, 4, tb::RngMode::Nes, 7ULL, 0.0f));
    std::fill(biasNet.params.begin(), biasNet.params.end(), 0.0f);
    // Piece 0 = -10.0 (must clamp to 0.0), piece 1 = +14.0, others = 0.0.
    biasNet.params[biasNet.offB3() + 0] = -10.0f;
    biasNet.params[biasNet.offB3() + 1] = 14.0f;
    CHECK(biasNet.evalNextPieceExpected(0ULL, tb::RngMode::Uniform, 0)
          == doctest::Approx(14.0 / 7.0).epsilon(1e-6));
    const double expectedNes = tb::pieceTransitionProb(tb::RngMode::Nes, 0, 1) * 14.0;
    CHECK(biasNet.evalNextPieceExpected(0ULL, tb::RngMode::Nes, 0)
          == doctest::Approx(expectedNes).epsilon(1e-6));

    // evaluateNnValueAccuracy on synthetic exact targets + empty/unloaded guards.
    tb::LabeledDecisionSample vs{};
    vs.board = 0ULL;
    vs.boardInTb = true;
    vs.vStar = {-10.0f, 14.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const tb::NnValueEvalMetrics exactAcc = tb::evaluateNnValueAccuracy(biasNet, {vs});
    CHECK(exactAcc.numSamples == 1);
    CHECK(exactAcc.mae == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(tb::evaluateNnValueAccuracy(emptyNet, {vs}).numSamples == 0);
  }

  // Hostile loss config guards (NaN / negative / OOB slice / unloaded net).
  {
    tb::NnLossConfig badCfg = lossCfg;
    badCfg.temperature = std::numeric_limits<double>::quiet_NaN();
    CHECK(
        tb::computeNnSliceLossAndGrad(loaded, synDecisions, order, 0, 2, badCfg, dummyGrad, scratch)
            .totalLoss
        == 0.0);
    badCfg = lossCfg;
    badCfg.valueWeight = -1.0;
    CHECK(
        tb::computeNnSliceLossAndGrad(loaded, synDecisions, order, 0, 2, badCfg, dummyGrad, scratch)
            .totalLoss
        == 0.0);
    CHECK(tb::computeNnSliceLossAndGrad(loaded, synDecisions, order, 0, 99, lossCfg, dummyGrad,
                                        scratch)
              .totalLoss
          == 0.0);
    CHECK_FALSE(tb::chooseNextBoxNnMove1Ply(0ULL, 0, 1, emptyNet).found);
    CHECK_FALSE(tb::chooseNextBoxNnMove2Ply(0ULL, 0, 1, emptyNet, tb::RngMode::Nes).found);
  }

  // Top-out and ply-2 top-out avoidance checks (narrowO: only vertical I in col 7 lets O survive).
  const uint64_t tetrisReady = mustParse("XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.");
  CHECK_FALSE(tb::chooseNextBoxNnMove1Ply(tetrisReady, tb::parsePieceChar('O'),
                                          tb::parsePieceChar('I'), loaded)
                  .found);
  CHECK_FALSE(tb::chooseNextBoxNnMove2Ply(tetrisReady, tb::parsePieceChar('O'),
                                          tb::parsePieceChar('I'), loaded, tb::RngMode::Nes)
                  .found);
  CHECK(tb::scoreNextBoxNn2Ply(tetrisReady, 2, tb::parsePieceChar('O'), loaded, tb::RngMode::Nes)
        == doctest::Approx(2.0));

  const uint64_t narrowO = mustParse("XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...");
  const auto avoidO = tb::chooseNextBoxNnMove2Ply(
      narrowO, tb::parsePieceChar('I'), tb::parsePieceChar('O'), loaded, tb::RngMode::Nes);
  REQUIRE(avoidO.found);
  CHECK(tb::hasAnyLegalTransition(avoidO.transition.afterBoard, tb::parsePieceChar('O')));

  std::remove(tmpNnPath.c_str());
  std::remove(tmpCorruptNn.c_str());
}

TEST_CASE("Calibrated partial-OUT fallback: +inf parity, margin clamping, and edge cases") {
  // Build a tiny Next-Box TB containing only ONE O move from empty board (low value = 5.0),
  // leaving the other 8 O moves out-of-TB.
  tb::TransitionList oMoves;
  tb::generateTransitions(0ULL, tb::parsePieceChar('O'), oMoves);
  REQUIRE(oMoves.size() == 9);
  const uint64_t inTbBoard = oMoves[0].afterBoard;

  const std::string tmpPartialTb = "build/test_partial_out.tb.bin";
  const std::vector<float> tbVals = {5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f};
  REQUIRE(tb::writeNextBoxTablebaseFile(tmpPartialTb, {inTbBoard}, tbVals, tb::RngMode::Nes, 0.0));
  tb::MappedNextBoxTablebase mappedTb;
  REQUIRE(mappedTb.open(tmpPartialTb));
  tb::IndexedNextBoxTablebase idxTb(mappedTb);

  // Constant-output NN predicting V_hat = 12.0 on every board/piece.
  tb::TinyNextBoxNet constNet;
  REQUIRE(constNet.initRandom(4, 4, tb::RngMode::Nes, 11ULL, 0.0f));
  std::fill(constNet.params.begin(), constNet.params.end(), 0.0f);
  for (int p = 0; p < tb::NUM_PIECES; ++p) {
    constNet.params[constNet.offB3() + static_cast<size_t>(p)] = 12.0f;
  }

  const int curO = tb::parsePieceChar('O');
  const int nxtI = tb::parsePieceChar('I');
  const double inf = std::numeric_limits<double>::infinity();

  // 1. safetyMargin == +inf, -inf, NaN, or nullptr NN must match chooseNextBoxTablebaseMove
  //    bit-for-bit whenever at least one move is in-TB.
  const tb::TablebaseMoveChoice refTb =
      tb::chooseNextBoxTablebaseMove(0ULL, curO, nxtI, idxTb, /*closedWorld=*/false);
  const tb::TablebaseMoveChoice infChoice = tb::chooseNextBoxHybridFallbackMove(
      0ULL, curO, nxtI, idxTb, &constNet, inf, tb::RngMode::Nes, /*outUse2Ply=*/false);
  const tb::TablebaseMoveChoice negInfChoice = tb::chooseNextBoxHybridFallbackMove(
      0ULL, curO, nxtI, idxTb, &constNet, -inf, tb::RngMode::Nes, /*outUse2Ply=*/false);
  const tb::TablebaseMoveChoice nanChoice = tb::chooseNextBoxHybridFallbackMove(
      0ULL, curO, nxtI, idxTb, &constNet, std::numeric_limits<double>::quiet_NaN(),
      tb::RngMode::Nes, /*outUse2Ply=*/false);
  const tb::TablebaseMoveChoice nullChoice = tb::chooseNextBoxHybridFallbackMove(
      0ULL, curO, nxtI, idxTb, nullptr, 0.0, tb::RngMode::Nes, /*outUse2Ply=*/false);
  CHECK(infChoice.transition.afterBoard == refTb.transition.afterBoard);
  CHECK(infChoice.tablebaseQ == doctest::Approx(refTb.tablebaseQ));
  CHECK(infChoice.afterInTablebase);
  CHECK_FALSE(infChoice.usedFallback);
  CHECK(negInfChoice.transition.afterBoard == refTb.transition.afterBoard);
  CHECK(nanChoice.transition.afterBoard == refTb.transition.afterBoard);
  CHECK(nullChoice.transition.afterBoard == refTb.transition.afterBoard);

  // 2. Partial-OUT margin behavior (1-ply & 2-ply):
  //    in-TB child has V = 5.0; out-of-TB children have V_hat = 12.0.
  //    - margin = 2.0  -> out-of-TB scores 10.0 > 5.0 -> escapes to out-of-TB (usedFallback=false)
  //    - margin = -5.0 -> negative finite margin clamps to 0.0 -> out-of-TB scores 12.0 > 5.0
  //    - margin = 7.0  -> exact tie (12 - 7 == 5.0) -> prefers in-TB move
  //    - margin = 50.0 -> out-of-TB continuation clamps at 0.0, stays in-TB
  const auto m2 = tb::chooseNextBoxHybridFallbackMove(0ULL, curO, nxtI, idxTb, &constNet, 2.0,
                                                      tb::RngMode::Nes, /*outUse2Ply=*/false);
  CHECK(m2.found);
  CHECK_FALSE(m2.afterInTablebase);
  CHECK_FALSE(m2.usedFallback);
  CHECK(m2.tablebaseQ == doctest::Approx(10.0));

  const auto m2Ply2 = tb::chooseNextBoxHybridFallbackMove(0ULL, curO, nxtI, idxTb, &constNet, 2.0,
                                                          tb::RngMode::Nes, /*outUse2Ply=*/true);
  CHECK(m2Ply2.found);
  CHECK_FALSE(m2Ply2.afterInTablebase);
  CHECK(m2Ply2.tablebaseQ == doctest::Approx(10.0));

  const auto mNeg = tb::chooseNextBoxHybridFallbackMove(0ULL, curO, nxtI, idxTb, &constNet, -5.0,
                                                        tb::RngMode::Nes, /*outUse2Ply=*/false);
  CHECK(mNeg.found);
  CHECK_FALSE(mNeg.afterInTablebase);
  CHECK(mNeg.tablebaseQ == doctest::Approx(12.0));

  const auto m7 = tb::chooseNextBoxHybridFallbackMove(0ULL, curO, nxtI, idxTb, &constNet, 7.0,
                                                      tb::RngMode::Nes, /*outUse2Ply=*/false);
  CHECK(m7.found);
  CHECK(m7.afterInTablebase);
  CHECK(m7.transition.afterBoard == inTbBoard);
  CHECK(m7.tablebaseQ == doctest::Approx(5.0));

  const auto m50 = tb::chooseNextBoxHybridFallbackMove(0ULL, curO, nxtI, idxTb, &constNet, 50.0,
                                                       tb::RngMode::Nes, /*outUse2Ply=*/false);
  CHECK(m50.found);
  CHECK(m50.afterInTablebase);
  CHECK(m50.transition.afterBoard == inTbBoard);

  // 2b. Verify >= 0.0 clamping directly AND out-of-TB line-clear > in-TB value under +inf:
  //     lowClearBoard allows O to clear 2 lines (afterBoard=0ULL, out-of-TB) or drop off-center
  //     (0 lines, placed in TB with V = 0.5). Under +inf, out-of-TB clear (Q=2.0) beats in-TB
  //     (Q=0.5), matching chooseNextBoxTablebaseMove; with huge margin=50, Q is still >= 2.0.
  const uint64_t lowClearBoard = mustParse("..........\n"
                                           "..........\n"
                                           "XXXX..XXXX\n"
                                           "XXXX..XXXX");
  tb::TransitionList clearMoves;
  tb::generateTransitions(lowClearBoard, curO, clearMoves);
  uint64_t nonClearBoard = 0ULL;
  for (int i = 0; i < clearMoves.size(); ++i) {
    if (clearMoves[i].linesCleared == 0) {
      nonClearBoard = clearMoves[i].afterBoard;
      break;
    }
  }
  REQUIRE(nonClearBoard != 0ULL);
  const std::string tmpClampTb = "build/test_partial_clamp.tb.bin";
  REQUIRE(tb::writeNextBoxTablebaseFile(tmpClampTb, {nonClearBoard},
                                        {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f},
                                        tb::RngMode::Nes, 0.0));
  tb::MappedNextBoxTablebase mappedClamp;
  REQUIRE(mappedClamp.open(tmpClampTb));
  tb::IndexedNextBoxTablebase idxClamp(mappedClamp);
  const auto refClear =
      tb::chooseNextBoxTablebaseMove(lowClearBoard, curO, nxtI, idxClamp, /*closedWorld=*/false);
  const auto infClear = tb::chooseNextBoxHybridFallbackMove(
      lowClearBoard, curO, nxtI, idxClamp, &constNet, inf, tb::RngMode::Nes, /*outUse2Ply=*/false);
  CHECK(infClear.transition.afterBoard == refClear.transition.afterBoard);
  CHECK(infClear.transition.linesCleared == 2);
  CHECK(infClear.tablebaseQ == doctest::Approx(2.0));
  const auto clampClear = tb::chooseNextBoxHybridFallbackMove(
      lowClearBoard, curO, nxtI, idxClamp, &constNet, 50.0, tb::RngMode::Nes, /*outUse2Ply=*/false);
  CHECK(clampClear.transition.linesCleared == 2);
  CHECK(clampClear.tablebaseQ == doctest::Approx(2.0)); // max(0, 12-50) == 0, never negative!
  mappedClamp.close();
  std::remove(tmpClampTb.c_str());

  // 2c. 1-ply partial-OUT ply-2 survival guard: on narrowO (cur=I, next=O), moves leaving no
  //     legal O placement get continuation 0.0 even when constNet predicts 12.0.
  const uint64_t narrowO = mustParse("XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...\n"
                                     "XXXXXXX...");
  const auto surv1Ply = tb::chooseNextBoxHybridFallbackMove(
      narrowO, nxtI, curO, idxTb, &constNet, 2.0, tb::RngMode::Nes, /*outUse2Ply=*/false);
  REQUIRE(surv1Ply.found);
  CHECK(surv1Ply.usedFallback); // All narrowO children are out-of-TB

  // 3. All-OUT fallback with a NON-CONSTANT NN must pick the exact same move as
  //    chooseNextBoxNnMove2Ply even when safetyMargin = 50.0 (no clamping loss of ordering).
  tb::TinyNextBoxNet randNet;
  REQUIRE(randNet.initRandom(8, 6, tb::RngMode::Nes, 777ULL, 5.0f));
  const auto allOut = tb::chooseNextBoxHybridFallbackMove(0ULL, nxtI, curO, idxTb, &randNet, 50.0,
                                                          tb::RngMode::Nes, /*outUse2Ply=*/true);
  const auto pureNn2 = tb::chooseNextBoxNnMove2Ply(0ULL, nxtI, curO, randNet, tb::RngMode::Nes);
  CHECK(allOut.found);
  CHECK(allOut.usedFallback);
  CHECK_FALSE(allOut.afterInTablebase);
  CHECK(allOut.transition.afterBoard == pureNn2.transition.afterBoard);

  // 4. Unloaded TB, out-of-range pieces (across loaded NN, nullptr NN, unloaded TB), and top-out.
  tb::IndexedNextBoxTablebase emptyTb;
  const auto unloadedRes = tb::chooseNextBoxHybridFallbackMove(
      0ULL, curO, nxtI, emptyTb, &constNet, 2.0, tb::RngMode::Nes, /*outUse2Ply=*/true);
  CHECK(unloadedRes.found);
  CHECK(unloadedRes.usedFallback);

  CHECK_FALSE(tb::chooseNextBoxHybridFallbackMove(0ULL, -1, nxtI, idxTb, nullptr, inf,
                                                  tb::RngMode::Nes, /*outUse2Ply=*/true)
                  .found);
  CHECK_FALSE(tb::chooseNextBoxHybridFallbackMove(0ULL, curO, 7, idxTb, nullptr, inf,
                                                  tb::RngMode::Nes, /*outUse2Ply=*/true)
                  .found);
  CHECK_FALSE(tb::chooseNextBoxHybridFallbackMove(0ULL, curO, 7, emptyTb, nullptr, inf,
                                                  tb::RngMode::Nes, /*outUse2Ply=*/true)
                  .found);
  CHECK_FALSE(tb::chooseNextBoxHybridFallbackMove(0ULL, curO, -1, idxTb, &constNet, 2.0,
                                                  tb::RngMode::Nes, /*outUse2Ply=*/true)
                  .found);

  const uint64_t tetrisReady = mustParse("XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.\n"
                                         "XXXXXXXXX.");
  CHECK_FALSE(tb::chooseNextBoxHybridFallbackMove(tetrisReady, curO, nxtI, idxTb, &constNet, 2.0,
                                                  tb::RngMode::Nes, /*outUse2Ply=*/true)
                  .found);

  // 5. generateNextBoxStateSpaceWithPolicy 1T vs 4T bit-for-bit determinism with custom NN policy.
  tb::NextBoxGenConfig gCfg{};
  gCfg.rngMode = tb::RngMode::Nes;
  gCfg.numGames = 24;
  gCfg.maxPiecesPerGame = 30;
  gCfg.baseSeed = 4242ULL;
  gCfg.epsilon = 0.05;
  gCfg.minVisits = 1;
  gCfg.siblingMinVisits = 0;
  auto nnPolicyCb = [&](uint64_t b, int c, int n, tb::Rng &rng,
                        const tb::IndexedNextBoxTablebase *fastTb) {
    return tb::selectNextBoxRolloutWithPolicy(
        b, c, n, rng, fastTb, gCfg.epsilon, gCfg.exploreTopK,
        [&](uint64_t gb, int gc, int gn, const tb::IndexedNextBoxTablebase *) {
          return tb::chooseNextBoxNnMove1Ply(gb, gc, gn, randNet);
        },
        [&](const tb::Transition &t) {
          return tb::scoreNextBoxNn1Ply(t.afterBoard, t.linesCleared, n, randNet);
        });
  };
  gCfg.numThreads = 1;
  const auto g1 = tb::generateNextBoxStateSpaceWithPolicy(gCfg, &mappedTb, nullptr, nnPolicyCb);
  gCfg.numThreads = 4;
  const auto g4 = tb::generateNextBoxStateSpaceWithPolicy(gCfg, &mappedTb, nullptr, nnPolicyCb);
  CHECK_FALSE(g1.sortedBoards.empty());
  CHECK(g1.sortedBoards == g4.sortedBoards);

  mappedTb.close();
  std::remove(tmpPartialTb.c_str());
}
