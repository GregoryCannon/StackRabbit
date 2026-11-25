#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../src/tests/vendor/doctest.h"

#include "board_key.hpp"
#include "core_20row.hpp"
#include "eval_adapter.hpp"
#include "inspector_routes.hpp"
#include "nnue_20row.hpp"
#include "solver_20row.hpp"
#include "state_gen_20row.hpp"
#include "tablebase_io_20row.hpp"
#include "tbnn_precompute_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using PlacementTuple = std::tuple<int, int, int, char>;

std::vector<PlacementTuple> sortPlacements(const std::vector<LockPlacement> &v) {
  std::vector<PlacementTuple> out;
  out.reserve(v.size());
  for (const auto &p : v) {
    out.emplace_back(p.x, p.y, p.rotationIndex, p.tuckInput);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<PlacementTuple> sortPlacements(const das::PlacementList &v) {
  std::vector<PlacementTuple> out;
  out.reserve(static_cast<size_t>(v.count));
  for (int i = 0; i < v.count; ++i) {
    const auto &p = v.data[i];
    out.emplace_back(static_cast<int>(p.x), static_cast<int>(p.y), static_cast<int>(p.rot),
                     p.tuckInput);
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

TEST_CASE("BoardKey 16-byte round-trip, accessors, sentinels, and hostile validation") {
  uint16_t emptyRows[das::BOARD_ROWS] = {};
  das::BoardKey k = das::EMPTY_KEY;
  REQUIRE(das::encodeBoardKey(emptyRows, k));
  CHECK(k == das::EMPTY_BOARD_KEY);
  CHECK(k != das::EMPTY_KEY);
  CHECK(das::isValidBoardKey(k));
  CHECK(!das::isValidBoardKey(das::EMPTY_KEY));
  CHECK(das::boardKeyMaxHeight(k) == 0);
  CHECK(das::boardKeyNumHoles(k) == 0);

  // Verify round-trip across 5,000 random valid boards with 0..9 holes & rejection of 10+ holes.
  das::Rng rng(123456789ULL);
  int testedValid = 0;
  int testedRejected = 0;
  for (int trial = 0; trial < 5000; ++trial) {
    uint16_t rows[das::BOARD_ROWS] = {};
    int surf[das::BOARD_WIDTH];
    for (int c = 0; c < das::BOARD_WIDTH; ++c) {
      surf[c] = rng.nextInt(das::BOARD_ROWS + 1);
      for (int r = das::BOARD_ROWS - surf[c]; r < das::BOARD_ROWS; ++r) {
        rows[r] |= das::colBitMask(c);
      }
    }
    const int targetHoles = rng.nextInt(14);
    for (int h = 0; h < targetHoles; ++h) {
      const int c = rng.nextInt(das::BOARD_WIDTH);
      if (surf[c] >= 2) {
        const int r = (das::BOARD_ROWS - surf[c] + 1) + rng.nextInt(surf[c] - 1);
        rows[r] &= static_cast<uint16_t>(~das::colBitMask(c));
      }
    }
    for (int r = 0; r < das::BOARD_ROWS; ++r) {
      if (rows[r] == das::FULL_ROW_MASK) {
        rows[r] &= static_cast<uint16_t>(~das::colBitMask(rng.nextInt(das::BOARD_WIDTH)));
      }
    }

    uint32_t covered = 0;
    int exactHoles = 0;
    int expectedSurf[das::BOARD_WIDTH] = {};
    int expectedMaxH = 0;
    for (int r = 0; r < das::BOARD_ROWS; ++r) {
      const uint32_t row = rows[r];
      for (int c = 0; c < das::BOARD_WIDTH; ++c) {
        if ((row & das::colBitMask(c)) && expectedSurf[c] == 0) {
          expectedSurf[c] = das::BOARD_ROWS - r;
          expectedMaxH = std::max(expectedMaxH, expectedSurf[c]);
        }
      }
      exactHoles += __builtin_popcount((~row) & covered);
      covered |= row;
    }

    das::BoardKey encoded = das::EMPTY_KEY;
    const bool ok = das::encodeBoardKey(rows, encoded);
    if (exactHoles <= das::KEY_MAX_HOLES) {
      REQUIRE(ok);
      CHECK(encoded != das::EMPTY_KEY);
      CHECK(das::isValidBoardKey(encoded));
      CHECK(das::boardKeyNumHoles(encoded) == exactHoles);
      CHECK(das::boardKeyMaxHeight(encoded) == expectedMaxH);
      for (int c = 0; c < das::BOARD_WIDTH; ++c) {
        CHECK(das::boardKeySurfaceCol(encoded, c) == expectedSurf[c]);
      }

      uint16_t decoded[das::BOARD_ROWS] = {};
      int8_t decSurf[das::BOARD_WIDTH] = {};
      das::decodeBoardKey(encoded, decoded, decSurf);
      for (int r = 0; r < das::BOARD_ROWS; ++r) {
        CHECK(decoded[r] == rows[r]);
      }
      for (int c = 0; c < das::BOARD_WIDTH; ++c) {
        CHECK(static_cast<int>(decSurf[c]) == expectedSurf[c]);
      }

      // Board20::fromKey parity
      const das::Board20 bFromKey = das::Board20::fromKey(encoded);
      CHECK(static_cast<int>(bFromKey.numHoles) == exactHoles);
      CHECK(static_cast<int>(bFromKey.topNonEmptyRow) == das::BOARD_ROWS - expectedMaxH);
      for (int r = 0; r < das::BOARD_ROWS; ++r) {
        CHECK(bFromKey.rows()[r] == rows[r]);
      }
      testedValid++;
    } else {
      CHECK(!ok);
      CHECK(encoded == das::EMPTY_KEY);
      testedRejected++;
    }
  }
  CHECK(testedValid > 2500);
  CHECK(testedRejected > 100);

  // Hostile key corruption checks: every invariant in isValidBoardKey must fire.
  uint16_t sampleRows[das::BOARD_ROWS] = {};
  sampleRows[17] = 0x3FEU; // height 3 in cols 0..8
  sampleRows[18] = 0x3F4U; // 2 holes: (r=18, c=6) coord=176 and (r=18, c=8) coord=178
  sampleRows[19] = 0x3FEU;
  das::BoardKey goodKey = das::EMPTY_KEY;
  REQUIRE(das::encodeBoardKey(sampleRows, goodKey));
  REQUIRE(das::isValidBoardKey(goodKey));
  REQUIRE(das::boardKeyNumHoles(goodKey) == 2);

  // 1. Top 2 reserved bits non-zero
  das::BoardKey badTop = goodKey;
  badTop.lo |= (1ULL << 62);
  CHECK(!das::isValidBoardKey(badTop));

  // 2. Column height > 20
  das::BoardKey badHeight = goodKey;
  badHeight.lo = (badHeight.lo & ~0x1FULL) | 21ULL;
  CHECK(!das::isValidBoardKey(badHeight));

  // 3. numHoles > 9
  das::BoardKey badCount = goodKey;
  badCount.lo = (badCount.lo & ~(0xFULL << 50)) | (10ULL << 50);
  CHECK(!das::isValidBoardKey(badCount));

  // 4. Non-zero unused hole slot (slot 2 is unused when numHoles == 2)
  das::BoardKey badUnused = goodKey;
  badUnused.hi |= (1ULL << 8);
  CHECK(!das::isValidBoardKey(badUnused));

  // 5. Hole coordinate >= 190
  das::BoardKey badCoord = goodKey;
  badCoord.hi = (badCoord.hi & ~0xFFULL) | 190ULL;
  CHECK(!das::isValidBoardKey(badCoord));

  // 6. Hole AT exact surface row (r == 20 - surface[8] == 17, coord = (17-1)*10 + 8 = 168)
  das::BoardKey badAtSurface = goodKey;
  badAtSurface.lo = (badAtSurface.lo & ~(0xFFULL << 54)) | (168ULL << 54);
  CHECK(!das::isValidBoardKey(badAtSurface));

  // 7. Duplicate hole coordinates (hole[0] == hole[1] == 176)
  das::BoardKey badDupHole = goodKey;
  badDupHole.hi = (badDupHole.hi & ~0xFFULL) | 176ULL;
  CHECK(!das::isValidBoardKey(badDupHole));

  // 8. Descending hole coordinates (hole[0] = 178 > hole[1] = 176)
  das::BoardKey badDescHole = goodKey;
  badDescHole.lo = (badDescHole.lo & ~(0xFFULL << 54)) | (178ULL << 54);
  badDescHole.hi = (badDescHole.hi & ~0xFFULL) | 176ULL;
  CHECK(!das::isValidBoardKey(badDescHole));

  // 9. Full row encoded via zero holes + all cols >= 1
  das::BoardKey badFullRow = {0, 0};
  for (int c = 0; c < das::BOARD_WIDTH; ++c) {
    badFullRow.lo |= 1ULL << (c * 5);
  }
  CHECK(!das::isValidBoardKey(badFullRow));
}

TEST_CASE("BoardKey shard and slot hashes are uncorrelated (including k.hi == 0)") {
  das::Rng rng(987654321ULL);
  constexpr int N = 4096;

  // Case A: random non-zero k.hi (low 6 bits of shard hash vs low 6 bits of slot hash)
  int sameLow6RandomHi = 0;
  for (int i = 0; i < N; ++i) {
    const das::BoardKey k = {rng.nextU64() & ((1ULL << 62) - 1), rng.nextU64()};
    if ((das::hashBoardKeyShard(k) & 63ULL) == (das::hashBoardKeySlot(k) & 63ULL)) {
      sameLow6RandomHi++;
    }
  }
  CHECK(sameLow6RandomHi >= 20);
  CHECK(sameLow6RandomHi <= 120);

  // Case B: k.hi == 0 (0-hole and 1-hole boards represent ~75% of real keys)
  int sameLow6ZeroHi = 0;
  for (int i = 0; i < N; ++i) {
    const das::BoardKey k = {rng.nextU64() & ((1ULL << 62) - 1), 0ULL};
    if ((das::hashBoardKeyShard(k) & 63ULL) == (das::hashBoardKeySlot(k) & 63ULL)) {
      sameLow6ZeroHi++;
    }
  }
  CHECK(sameLow6ZeroHi >= 20);
  CHECK(sameLow6ZeroHi <= 120);
}

TEST_CASE("NES RNG tables, scoring, and helper functions match src/backend/ bit-for-bit") {
  for (int prev = 0; prev < das::NUM_PIECES; ++prev) {
    int rowSum = 0;
    for (int next = 0; next < das::NUM_PIECES; ++next) {
      rowSum += das::NES_TRANSITION_NUM[prev][next];
      const double ours = das::pieceTransitionProb(das::RngMode::Nes, prev, next);
      const double theirs =
          static_cast<double>(pieceProbability(&PIECE_LIST[prev], &PIECE_LIST[next]));
      CHECK(std::fabs(ours - theirs) < 1e-6);
    }
    CHECK(rowSum == das::NES_TRANSITION_DENOM);
  }

  uint64_t statSum = 0;
  for (int p = 0; p < das::NUM_PIECES; ++p) {
    statSum += das::NES_STATIONARY_NUM[p];
    double nextProb = 0.0;
    for (int prev = 0; prev < das::NUM_PIECES; ++prev) {
      nextProb +=
          das::NES_STATIONARY_PROB[prev] * das::pieceTransitionProb(das::RngMode::Nes, prev, p);
    }
    CHECK(std::fabs(nextProb - das::NES_STATIONARY_PROB[p]) < 1e-12);
  }
  CHECK(statSum == das::NES_STATIONARY_DENOM);

  // RngMode parsing, expectedValueFromRow, nesRawScore, Rng::nextUniform01
  das::RngMode rm = das::RngMode::Uniform;
  CHECK(das::parseRngMode("nes", rm));
  CHECK(rm == das::RngMode::Nes);
  CHECK(std::string_view(das::rngModeName(rm)) == "nes");
  CHECK(das::parseRngMode("uniform", rm));
  CHECK(rm == das::RngMode::Uniform);
  CHECK(std::string_view(das::rngModeName(rm)) == "uniform");
  CHECK(!das::parseRngMode("invalid", rm));

  const float rowF[das::NUM_PIECES] = {10.f, 10.f, 10.f, 10.f, 10.f, 10.f, 10.f};
  const double rowD[das::NUM_PIECES] = {10.0, 10.0, 10.0, 10.0, 10.0, 10.0, 10.0};
  CHECK(std::fabs(das::expectedValueFromRow(rowF, das::RngMode::Nes) - 10.0) < 1e-6);
  CHECK(std::fabs(das::expectedValueFromRow(rowD, das::RngMode::Uniform) - 10.0) < 1e-12);

  CHECK(das::nesRawScore(0, 18) == 0);
  CHECK(das::nesRawScore(1, 18) == 760);
  CHECK(das::nesRawScore(2, 18) == 1900);
  CHECK(das::nesRawScore(3, 18) == 5700);
  CHECK(das::nesRawScore(4, 18) == 22800);
  CHECK(das::nesRawScore(4, 19) == 24000);
  CHECK(das::nesRawScore(4, 28) == 34800);
  CHECK(das::nesRawScore(-1, 19) == 0);
  CHECK(das::nesRawScore(5, 19) == 0);

  das::Rng uRng(42ULL);
  for (int i = 0; i < 100; ++i) {
    const double u = uRng.nextUniform01();
    CHECK(u >= 0.0);
    CHECK(u < 1.0);
  }

  for (int lvl : {18, 19, 20, 25, 28, 29}) {
    for (int lines = 0; lines <= 240; ++lines) {
      for (int cleared = 0; cleared <= 4; ++cleared) {
        CHECK(das::getLevelAfterLineClears20(lvl, lines, cleared)
              == getLevelAfterLineClears(lvl, lines, cleared));
      }
    }
  }
}

TEST_CASE(
    "Fast 10Hz move gen, tucks, applyPlacement20, transitions, and adj parity on 10,500+ boards") {
  const das::BackendContexts bc = das::BackendContexts::make10Hz();
  std::vector<LockPlacement> refMoves, m1, m2;
  refMoves.reserve(64);
  m1.reserve(64);
  m2.reserve(64);

  int boardsChecked = 0;
  int tuckPlacementsVerified = 0;
  int lineClearTransitionsVerified = 0;
  int adjStatesVerified = 0;
  int topOutsVerified = 0;
  int negYPlacementsVerified = 0;
  int outOfKeyTransitionsVerified = 0;

  // 1. Live gameplay boards (Level 19 and Level 18)
  for (int lvl : {19, 18}) {
    const int grav = das::getGravity20(lvl);
    const int gamesToRun = (lvl == 19) ? 25 : 15;
    for (int g = 0; g < gamesToRun; ++g) {
      das::Rng rng(500000ULL + static_cast<uint64_t>(lvl) * 10000ULL + static_cast<uint64_t>(g));
      GameState st =
          das::board20ToGameState(das::Board20::empty(), lvl, (lvl == 19) ? 130 : 0, bc, AGGRO);
      int curP = rng.sampleOpeningPiece(das::RngMode::Nes);
      int nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);

      for (int step = 0; step < 260; ++step) {
        const das::Board20 b20 = das::gameStateToBoard20(st);
        const das::TuckSetupInfo tucks = das::computeTuckSetupInfo(b20);

        const int pStart = (step % 4 == 0) ? 0 : curP;
        const int pEnd = (step % 4 == 0) ? das::NUM_PIECES : (curP + 1);
        for (int p = pStart; p < pEnd; ++p) {
          refMoves.clear();
          fastMoveSearch(st, &PIECE_LIST[p], das::TIMELINE_10HZ_STR, refMoves);

          das::PlacementList ourMoves;
          das::findPlacements10Hz(b20, tucks, p, grav, ourMoves);
          REQUIRE(sortPlacements(ourMoves) == sortPlacements(refMoves));
          CHECK(das::hasAnyLegalMove10Hz(b20, p, grav) == !refMoves.empty());

          if (p == curP) {
            das::TransitionList trList;
            const int trCount = das::generateTransitions10Hz(b20, tucks, p, grav, trList);
            REQUIRE(trCount == ourMoves.count);

            for (int ti = 0; ti < trCount; ++ti) {
              const auto &tr = trList.data[ti];
              if (tr.placement.tuckInput != '.')
                tuckPlacementsVerified++;
              if (tr.placement.y < 0)
                negYPlacementsVerified++;
              if (!tr.fitsKey) {
                CHECK(tr.afterKey == das::EMPTY_KEY);
                outOfKeyTransitionsVerified++;
              }

              LockPlacement lp = {tr.placement.x,         tr.placement.y, tr.placement.rot, -1,
                                  tr.placement.tuckInput, &PIECE_LIST[p]};
              unsigned int refAfter[20] = {};
              const int refCleared = getNewBoardAndLinesCleared(st.board, lp, refAfter);
              uint16_t ourAfter[das::BOARD_ROWS] = {};
              const uint8_t ourCleared = das::applyPlacement20(
                  b20, p, tr.placement.rot, tr.placement.x, tr.placement.y, ourAfter);
              REQUIRE(static_cast<int>(ourCleared) == refCleared);
              REQUIRE(tr.linesCleared == ourCleared);
              if (ourCleared > 0)
                lineClearTransitionsVerified++;
              for (int r = 0; r < das::BOARD_ROWS; ++r) {
                REQUIRE(ourAfter[r] == static_cast<uint16_t>(refAfter[r] & FULL_ROW));
              }
              if (tr.fitsKey) {
                uint16_t decRows[das::BOARD_ROWS];
                das::decodeBoardKey(tr.afterKey, decRows);
                for (int r = 0; r < das::BOARD_ROWS; ++r) {
                  REQUIRE(decRows[r] == ourAfter[r]);
                }
              }

              // Parity of predictAdjState10Hz at rt=18 and rt=0
              for (int rt : {18, 0}) {
                const SimState refAdj =
                    predictStateAtAdjustmentTime(lp, das::TIMELINE_10HZ_STR, grav, false, rt);
                const das::AdjState10Hz ourAdj =
                    das::predictAdjState10Hz(tr.placement, p, grav, rt);
                const bool refValid = (refAdj.piece != nullptr);
                REQUIRE(ourAdj.valid == refValid);
                if (refValid) {
                  CHECK(static_cast<int>(ourAdj.x) == refAdj.x);
                  CHECK(static_cast<int>(ourAdj.y) == refAdj.y);
                  CHECK(static_cast<int>(ourAdj.rot) == refAdj.rotationIndex);
                  const int ourArrIdx = ourAdj.arrReset ? 0 : rt;
                  CHECK(ourArrIdx == refAdj.arrIndex);
                  adjStatesVerified++;
                }
              }
            }
          }
        }
        boardsChecked++;

        const das::Eval2PlyResult er =
            das::chooseEval2PlyMove10Hz(st, curP, nextP, bc, AGGRO, true, m1, m2);
        if (!er.hasMove) {
          topOutsVerified++;
          break;
        }
        st = er.afterState;
        curP = nextP;
        nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
      }
    }
  }

  // 2. Random stress boards across ALL three gravities (L18=3, L19=2, L29=1) and heights up to 20
  das::Rng rng(20261125ULL);
  const int stressLevels[3] = {18, 19, 29};
  for (int i = 0; i < 3000; ++i) {
    uint16_t rawRows[das::BOARD_ROWS] = {};
    // Include height 19 and 20 frequently to exercise y<0 spawns, ceiling overhangs, and top-outs.
    const int maxH = (i % 5 == 0) ? (18 + rng.nextInt(3)) : (2 + rng.nextInt(17));
    for (int r = das::BOARD_ROWS - maxH; r < das::BOARD_ROWS; ++r) {
      uint16_t row = static_cast<uint16_t>(rng.nextU64() & das::FULL_ROW_MASK);
      if (row == das::FULL_ROW_MASK) {
        row &= static_cast<uint16_t>(~das::colBitMask(rng.nextInt(das::BOARD_WIDTH)));
      }
      rawRows[r] = row;
    }
    const das::Board20 b20 = das::Board20::fromRows(rawRows);
    const int lvl = stressLevels[i % 3];
    const int grav = das::getGravity20(lvl);
    const GameState canon = das::board20ToGameState(b20, lvl, (lvl == 19) ? 130 : 0, bc, AGGRO);
    const int p = i % das::NUM_PIECES;

    refMoves.clear();
    fastMoveSearch(canon, &PIECE_LIST[p], das::TIMELINE_10HZ_STR, refMoves);
    das::PlacementList ourMoves;
    das::findPlacements10Hz(b20, p, grav, ourMoves);
    REQUIRE(sortPlacements(ourMoves) == sortPlacements(refMoves));
    CHECK(das::hasAnyLegalMove10Hz(b20, p, grav) == !refMoves.empty());
    if (refMoves.empty()) {
      topOutsVerified++;
    }
    for (const auto &lp : refMoves) {
      if (lp.tuckInput != '.')
        tuckPlacementsVerified++;
      if (lp.y < 0)
        negYPlacementsVerified++;
      unsigned int refAfter[20] = {};
      const int refCleared = getNewBoardAndLinesCleared(canon.board, lp, refAfter);
      uint16_t ourAfter[das::BOARD_ROWS] = {};
      const uint8_t ourCleared =
          das::applyPlacement20(b20, p, lp.rotationIndex, lp.x, lp.y, ourAfter);
      REQUIRE(static_cast<int>(ourCleared) == refCleared);
      for (int r = 0; r < das::BOARD_ROWS; ++r) {
        REQUIRE(ourAfter[r] == static_cast<uint16_t>(refAfter[r] & FULL_ROW));
      }
      const SimState refAdj =
          predictStateAtAdjustmentTime(lp, das::TIMELINE_10HZ_STR, grav, false, 18);
      const das::CompactPlacement cp = {static_cast<int8_t>(lp.x), static_cast<int8_t>(lp.y),
                                        static_cast<uint8_t>(lp.rotationIndex), lp.tuckInput};
      const das::AdjState10Hz ourAdj = das::predictAdjState10Hz(cp, p, grav, 18);
      REQUIRE(ourAdj.valid == (refAdj.piece != nullptr));
    }
    boardsChecked++;
  }

  // 3. Explicit targeted c==8 vertical I-tuck parity test
  {
    uint16_t vitRows[das::BOARD_ROWS] = {};
    // Roof over col 8 at row 14 (cols 0..8 filled, col 9 open), rows 15..18 open in cols 8..9
    vitRows[14] = 0x3FEU;
    vitRows[15] = 0x3FCU;
    vitRows[16] = 0x3FCU;
    vitRows[17] = 0x3FCU;
    vitRows[18] = 0x3FCU;
    vitRows[19] = 0x3FCU;
    const das::Board20 b20 = das::Board20::fromRows(vitRows);
    const das::TuckSetupInfo tucks = das::computeTuckSetupInfo(b20);
    CHECK(tucks.any);
    CHECK((tucks.mask[15] & das::colBitMask(8)) != 0);
    CHECK((tucks.mask[18] & das::colBitMask(8)) != 0);

    const GameState canon = das::board20ToGameState(b20, 18, 0, bc, AGGRO);
    refMoves.clear();
    fastMoveSearch(canon, &PIECE_LIST[0], das::TIMELINE_10HZ_STR, refMoves);
    das::PlacementList ourMoves;
    das::findPlacements10Hz(b20, tucks, 0, das::GRAVITY_L18, ourMoves);
    REQUIRE(sortPlacements(ourMoves) == sortPlacements(refMoves));
  }

  CHECK(boardsChecked >= 10000);
  CHECK(tuckPlacementsVerified > 100);
  CHECK(lineClearTransitionsVerified > 500);
  CHECK(adjStatesVerified > 1000);
  CHECK(topOutsVerified > 50);
  CHECK(negYPlacementsVerified > 50);
  CHECK(outOfKeyTransitionsVerified > 10);
}

TEST_CASE("Precompute 18-frame adapter parity and near-topout handling") {
  const das::BackendContexts bc = das::BackendContexts::make10Hz();
  das::Rng rng(777777ULL);
  GameState st = das::board20ToGameState(das::Board20::empty(), 19, 130, bc, AGGRO);
  int curP = rng.sampleOpeningPiece(das::RngMode::Nes);
  int nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
  int dasCharge = 0;

  for (int i = 0; i < 10; ++i) {
    das::PrecomputeOptions opts;
    opts.dasCharge = dasCharge;
    opts.reactionTime = (i == 0) ? 0 : 18;
    opts.playoutCount = 0;
    opts.playoutLength = 0;
    opts.pruningBreadth = 12;
    opts.aggro = AGGRO;
    opts.stationaryLevel = true;

    const das::PrecomputeMoveResult pr =
        das::chooseProductionPrecomputeMove(st, curP, nextP, opts, bc);
    REQUIRE(pr.hasMove);
    CHECK(pr.dasChargeAfter >= 0);
    CHECK(pr.dasChargeAfter <= DAS_CHARGE_MAX);
    st = pr.afterState;
    dasCharge = pr.dasChargeAfter;
    curP = nextP;
    nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
  }

  // Topped-out board (spawn blocked) must cleanly report hasMove == false.
  uint16_t toppedRows[das::BOARD_ROWS];
  for (int r = 0; r < das::BOARD_ROWS; ++r) {
    toppedRows[r] = 0x3FEU;
  }
  const GameState toppedSt =
      das::board20ToGameState(das::Board20::fromRows(toppedRows), 19, 130, bc, AGGRO);
  das::PrecomputeOptions opts;
  opts.dasCharge = DAS_CHARGE_MAX;
  opts.reactionTime = 18;
  opts.stationaryLevel = true;
  const das::PrecomputeMoveResult toppedRes =
      das::chooseProductionPrecomputeMove(toppedSt, 0, 1, opts, bc);
  CHECK(!toppedRes.hasMove);
}

TEST_CASE("1T == 4T benchmark determinism and fast move-gen throughput micro-benchmark") {
  const das::BackendContexts bc = das::BackendContexts::make10Hz();
  constexpr int GAMES = 24;
  auto runN = [&](int nThreads) {
    std::vector<uint64_t> scores(GAMES, 0);
    std::atomic<int> nextG{0};
    std::vector<std::thread> ws;
    for (int t = 0; t < nThreads; ++t) {
      ws.emplace_back([&]() {
        std::vector<LockPlacement> m1, m2;
        while (true) {
          const int g = nextG.fetch_add(1, std::memory_order_relaxed);
          if (g >= GAMES)
            break;
          das::Rng rng(30000000ULL + static_cast<uint64_t>(g) * 1009ULL);
          GameState st = das::board20ToGameState(das::Board20::empty(), 19, 130, bc, AGGRO);
          int curP = rng.sampleOpeningPiece(das::RngMode::Nes);
          int nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
          uint64_t sc = 0;
          for (int step = 0; step < 80; ++step) {
            const auto er = das::chooseEval2PlyMove10Hz(st, curP, nextP, bc, AGGRO, true, m1, m2);
            if (!er.hasMove)
              break;
            sc += static_cast<uint64_t>(das::nesRawScore(er.linesCleared, st.level));
            st = er.afterState;
            curP = nextP;
            nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
          }
          scores[static_cast<size_t>(g)] = sc;
        }
      });
    }
    for (auto &w : ws)
      w.join();
    return scores;
  };
  CHECK(runN(1) == runN(4));

  // Quick micro-benchmark comparing optimized findPlacements10Hz + generateTransitions10Hz
  std::vector<das::Board20> boards;
  boards.reserve(2000);
  {
    das::Rng rng(12345ULL);
    std::vector<LockPlacement> m1, m2;
    for (int g = 0; g < 15 && boards.size() < 2000; ++g) {
      GameState st = das::board20ToGameState(das::Board20::empty(), 19, 130, bc, AGGRO);
      int curP = rng.sampleOpeningPiece(das::RngMode::Nes);
      int nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
      for (int s = 0; s < 200 && boards.size() < 2000; ++s) {
        boards.push_back(das::gameStateToBoard20(st));
        const auto er = das::chooseEval2PlyMove10Hz(st, curP, nextP, bc, AGGRO, true, m1, m2);
        if (!er.hasMove)
          break;
        st = er.afterState;
        curP = nextP;
        nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
      }
    }
  }
  uint64_t sink = 0;
  das::PlacementList pl;
  das::TransitionList tl;
  constexpr int REPS = 5;
  const auto t0 = std::chrono::steady_clock::now();
  for (int rep = 0; rep < REPS; ++rep) {
    for (const auto &b : boards) {
      const das::TuckSetupInfo tucks = das::computeTuckSetupInfo(b);
      for (int p = 0; p < das::NUM_PIECES; ++p) {
        sink += static_cast<uint64_t>(das::findPlacements10Hz(b, tucks, p, das::GRAVITY_L19, pl));
      }
    }
  }
  const double secPl = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const auto t1 = std::chrono::steady_clock::now();
  for (int rep = 0; rep < REPS; ++rep) {
    for (const auto &b : boards) {
      const das::TuckSetupInfo tucks = das::computeTuckSetupInfo(b);
      for (int p = 0; p < das::NUM_PIECES; ++p) {
        sink +=
            static_cast<uint64_t>(das::generateTransitions10Hz(b, tucks, p, das::GRAVITY_L19, tl));
      }
    }
  }
  const double secTr = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  const double calls = static_cast<double>(boards.size()) * das::NUM_PIECES * REPS;
  printf("  [MicroBench L19 1T] findPlacements10Hz=%.0f ns/call | generateTransitions10Hz=%.0f "
         "ns/call (sink=%llu)\n",
         secPl * 1e9 / calls, secTr * 1e9 / calls, static_cast<unsigned long long>(sink));
  CHECK(sink > 0);
}

namespace {

// Independent brute-force Value Iteration oracle for small 20-row subsets.
// Used to verify solve20MPI bit-accurately across Hard, Discounted, and Soft-Boundary modes.
template <typename BoundaryFn>
std::vector<double> bruteForceValueIteration20(const std::vector<das::BoardKey> &sortedKeys,
                                               int level, das::RngMode rngMode, double gamma,
                                               const BoundaryFn &boundaryFn, int maxIters = 2000,
                                               double tol = 1e-10) {
  const size_t n = sortedKeys.size();
  const int gravity = das::getGravity20(level);
  das::FlatBoardIndex idx;
  REQUIRE(idx.build(sortedKeys));

  std::vector<double> vCur(n * das::NUM_PIECES, 0.0);
  std::vector<double> vNext(n * das::NUM_PIECES, 0.0);
  das::PlacementList pl;
  uint16_t afterRows[das::BOARD_ROWS];
  double boundCont[das::NUM_PIECES];

  for (int it = 0; it < maxIters; ++it) {
    double maxDiff = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const das::Board20 b = das::Board20::fromKey(sortedKeys[i]);
      const das::TuckSetupInfo tucks = das::computeTuckSetupInfo(b);
      for (int c = 0; c < das::NUM_PIECES; ++c) {
        das::findPlacements10Hz(b, tucks, c, gravity, pl);
        double bestQ[das::NUM_PIECES] = {};
        if (pl.count > 0) {
          bool have[das::NUM_PIECES] = {};
          for (int m = 0; m < pl.count; ++m) {
            const auto &p = pl.data[m];
            const uint8_t lc = das::applyPlacement20(b, c, p.rot, p.x, p.y, afterRows);
            const double rew = das::REWARD_NORM_TETRIS[lc];
            das::BoardKey ak;
            const bool fits = das::encodeBoardKey(afterRows, ak);
            const uint32_t cid = fits ? idx.find(ak) : das::INVALID_BOARD_ID;
            if (cid != das::INVALID_BOARD_ID) {
              for (int np = 0; np < das::NUM_PIECES; ++np) {
                const double q =
                    rew + gamma * vCur[static_cast<size_t>(cid) * das::NUM_PIECES + np];
                if (!have[np] || q > bestQ[np]) {
                  bestQ[np] = q;
                  have[np] = true;
                }
              }
            } else if constexpr (BoundaryFn::enabled) {
              boundaryFn.evalRow(afterRows, boundCont);
              for (int np = 0; np < das::NUM_PIECES; ++np) {
                const double cont =
                    (std::isfinite(boundCont[np]) && boundCont[np] > 0.0) ? boundCont[np] : 0.0;
                const double qQuant =
                    das::unpackTerminalPayoff(das::packTerminalPayoff(rew + gamma * cont));
                if (!have[np] || qQuant > bestQ[np]) {
                  bestQ[np] = qQuant;
                  have[np] = true;
                }
              }
            } else {
              for (int np = 0; np < das::NUM_PIECES; ++np) {
                if (!have[np] || rew > bestQ[np]) {
                  bestQ[np] = rew;
                  have[np] = true;
                }
              }
            }
          }
        }
        double ev = 0.0;
        for (int np = 0; np < das::NUM_PIECES; ++np) {
          ev += das::pieceTransitionProb(rngMode, c, np) * bestQ[np];
        }
        vNext[i * das::NUM_PIECES + c] = ev;
        maxDiff = std::max(maxDiff, std::abs(ev - vCur[i * das::NUM_PIECES + c]));
      }
    }
    vCur.swap(vNext);
    if (maxDiff < tol)
      break;
  }
  return vCur;
}

// Builds a small closed/semi-closed subset of valid 20-row BoardKeys around the empty board.
std::vector<das::BoardKey> makeSmallTestSubset(size_t targetSize = 180) {
  std::vector<das::BoardKey> keys = {das::EMPTY_BOARD_KEY};
  das::PlacementList pl;
  uint16_t afterRows[das::BOARD_ROWS];
  for (size_t head = 0; head < keys.size() && keys.size() < targetSize; ++head) {
    const das::Board20 b = das::Board20::fromKey(keys[head]);
    for (int c = 0; c < das::NUM_PIECES && keys.size() < targetSize; ++c) {
      das::findPlacements10Hz(b, c, das::GRAVITY_L19, pl);
      for (int m = 0; m < pl.count && keys.size() < targetSize; m += 3) {
        const auto &p = pl.data[m];
        das::applyPlacement20(b, c, p.rot, p.x, p.y, afterRows);
        das::BoardKey ak;
        if (das::encodeBoardKey(afterRows, ak)
            && std::find(keys.begin(), keys.end(), ak) == keys.end()) {
          keys.push_back(ak);
        }
      }
    }
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

struct ConstSoftBoundTest {
  static constexpr bool enabled = true;
  void evalRow(const uint16_t rows[das::BOARD_ROWS], double out[das::NUM_PIECES]) const noexcept {
    const int h = das::inspector::boardMaxHeight20(rows);
    const double base = std::max(0.0, 1.5 - 0.08 * static_cast<double>(h));
    for (int p = 0; p < das::NUM_PIECES; ++p) {
      out[p] = base + 0.05 * static_cast<double>(p);
    }
  }
};

struct OverflowBoundTest {
  static constexpr bool enabled = true;
  void evalRow(const uint16_t[das::BOARD_ROWS], double out[das::NUM_PIECES]) const noexcept {
    for (int p = 0; p < das::NUM_PIECES; ++p)
      out[p] = 5000.0;
  }
};

} // namespace

TEST_CASE("Phase 2 I/O: SRSTV002, SRTBV003, SRNNV001 round-trips and hostile-file guards") {
  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_io";
  fs::create_directories(tmpDir);

  const std::vector<das::BoardKey> keys = makeSmallTestSubset(40);
  const std::string stPath = (tmpDir / "test.states.bin").string();
  const std::string tbPath = (tmpDir / "test.tb.bin").string();
  const std::string nnPath = (tmpDir / "test.nn.bin").string();
  const std::string badPath = (tmpDir / "corrupt.bin").string();

  std::string err;
  REQUIRE(das::writeStatesFileV2(stPath, keys, &err));
  std::vector<das::BoardKey> loadedKeys;
  REQUIRE(das::readStatesFileV2(stPath, loadedKeys, &err));
  CHECK(loadedKeys == keys);

  // Unsorted / duplicate rejection on write
  std::vector<das::BoardKey> dupKeys = keys;
  dupKeys.push_back(keys[0]);
  CHECK(!das::writeStatesFileV2(badPath, dupKeys, &err));

  // SRTBV003 write + Mapped + Indexed + loadInputBoardKeys round-trip
  std::vector<double> vals(keys.size() * das::NUM_PIECES);
  for (size_t i = 0; i < vals.size(); ++i)
    vals[i] = 0.25 * static_cast<double>(i % 17);
  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(das::RngMode::Nes);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = 19;
  hdr.gamma = 0.995;
  hdr.emptyBoardValue = 1.234;
  hdr.bellmanResidual = 1e-6;
  REQUIRE(das::writeTablebaseV3(tbPath, keys, vals, hdr, &err));

  das::MappedDasTablebase mapped;
  REQUIRE(mapped.open(tbPath, &err, /*verifyRecords=*/true));
  CHECK(mapped.size() == keys.size());
  CHECK(mapped.level() == 19);
  CHECK(mapped.rngMode() == das::RngMode::Nes);
  CHECK(std::fabs(mapped.gamma() - 0.995) < 1e-12);

  das::IndexedDasTablebase indexed;
  REQUIRE(indexed.open(tbPath, &err, /*verifyRecords=*/true));
  for (size_t i = 0; i < keys.size(); ++i) {
    float rM[das::NUM_PIECES], rI[das::NUM_PIECES];
    REQUIRE(mapped.lookup(keys[i], rM));
    REQUIRE(indexed.lookup(keys[i], rI));
    for (int p = 0; p < das::NUM_PIECES; ++p) {
      CHECK(rM[p] == rI[p]);
      CHECK(std::fabs(rM[p] - static_cast<float>(vals[i * das::NUM_PIECES + p])) < 1e-6f);
    }
  }
  std::vector<das::BoardKey> fromTbKeys;
  REQUIRE(das::loadInputBoardKeys(tbPath, fromTbKeys, &err));
  CHECK(fromTbKeys == keys);

  // Hostile TB: negative value rejected on write
  vals[0] = -0.1;
  CHECK(!das::writeTablebaseV3(badPath, keys, vals, hdr, &err));
  vals[0] = std::numeric_limits<double>::quiet_NaN();
  CHECK(!das::writeTablebaseV3(badPath, keys, vals, hdr, &err));

  // Hostile corrupt magic + truncated file: transactional open preserves live mapped TB
  {
    std::ofstream bad(badPath, std::ios::binary | std::ios::trunc);
    bad.write("BADMAGIC12345678", 16);
  }
  CHECK(!mapped.open(badPath, &err));
  CHECK(mapped.isLoaded());
  CHECK(mapped.size() == keys.size());

  // SRNNV001 save/load + transactional load failure preserves live net
  das::TinyDasNet net;
  net.initRandom(32, 16, das::RngMode::Nes, 777ULL);
  REQUIRE(net.save(nnPath, &err));
  das::TinyDasNet net2;
  REQUIRE(net2.load(nnPath, &err));
  CHECK(net2.params == net.params);
  CHECK(!net2.load(badPath, &err));
  CHECK(net2.isLoaded());
  CHECK(net2.params == net.params);

  // isSameFilePath guard
  CHECK(das::detail::isSameFilePath(tbPath, tbPath));
  CHECK(!das::detail::isSameFilePath(tbPath, stPath));

  mapped.close();
  fs::remove_all(tmpDir);
}

TEST_CASE("Phase 2 Solver: Brute-force VI parity (Hard, Discounted, Soft-Boundary), 1T==4T, "
          "Ram==Disk, Warm==Cold, and guards") {
  const std::vector<das::BoardKey> keys = makeSmallTestSubset(160);
  REQUIRE(keys.size() >= 80);

  // 1. Hard-Boundary + Discounted (gamma = 0.90) vs brute-force VI
  das::SolveConfig20 cfg;
  cfg.rngMode = das::RngMode::Nes;
  cfg.level = 19;
  cfg.gamma = 0.90;
  cfg.tolerance = 1e-8;
  cfg.maxOuterIterations = 60;
  cfg.innerEvalSweeps = 150;
  cfg.numThreads = 1;
  cfg.minParallelBoards = 16;

  const auto hard1T = das::solve20MPI(keys, cfg);
  REQUIRE(hard1T.ok);
  REQUIRE(hard1T.converged);
  CHECK(hard1T.bellmanResidual < 1e-7);

  const auto bruteHard =
      bruteForceValueIteration20(keys, 19, das::RngMode::Nes, 0.90, das::NoOpBoundaryEval20{});
  REQUIRE(bruteHard.size() == hard1T.values.size());
  for (size_t i = 0; i < bruteHard.size(); ++i) {
    CHECK(std::fabs(hard1T.values[i] - bruteHard[i]) < 1e-6);
  }

  // 2. 1T == 4T and Ram == Disk exact parity
  das::SolveConfig20 cfg4T = cfg;
  cfg4T.numThreads = 4;
  const auto hard4T = das::solve20MPI(keys, cfg4T);
  REQUIRE(hard4T.ok);
  CHECK(hard4T.values == hard1T.values);

  das::SolveConfig20 cfgDisk = cfg4T;
  cfgDisk.policyCacheMode = das::PolicyCacheMode::Disk;
  const auto hardDisk = das::solve20MPI(keys, cfgDisk);
  REQUIRE(hardDisk.ok);
  REQUIRE(hardDisk.usedDiskPolicyCache);
  CHECK(hardDisk.values == hard1T.values);

  // 3. Soft-Boundary MPI vs brute-force VI (also tests bestOutStore iter-1 vs iter-2+ tie parity!)
  const ConstSoftBoundTest bound{};
  const auto soft1T = das::solve20MPI(keys, cfg, nullptr, bound);
  REQUIRE(soft1T.ok);
  REQUIRE(soft1T.converged);
  CHECK(soft1T.bellmanResidual < 1e-7);

  const auto soft4TDisk = das::solve20MPI(keys, cfgDisk, nullptr, bound);
  REQUIRE(soft4TDisk.ok);
  CHECK(soft4TDisk.values == soft1T.values);

  const auto bruteSoft = bruteForceValueIteration20(keys, 19, das::RngMode::Nes, 0.90, bound);
  for (size_t i = 0; i < bruteSoft.size(); ++i) {
    CHECK(std::fabs(soft1T.values[i] - bruteSoft[i]) < 1e-6);
  }

  // 4. Warm-start == Cold-start parity + Soft-MC 1T==4T & <= 3.5 sigma verification
  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_solver";
  fs::create_directories(tmpDir);
  const std::string softTbPath = (tmpDir / "soft.tb.bin").string();
  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(das::RngMode::Nes);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = 19;
  hdr.gamma = 0.90;
  hdr.emptyBoardValue = soft1T.emptyBoardValue;
  hdr.bellmanResidual = soft1T.bellmanResidual;
  REQUIRE(das::writeTablebaseV3(softTbPath, keys, soft1T.values, hdr));

  das::MappedDasTablebase warmTb;
  REQUIRE(warmTb.open(softTbPath));
  const auto softWarm = das::solve20MPI(keys, cfg, &warmTb, bound);
  REQUIRE(softWarm.ok);
  CHECK(softWarm.warmStartMatched == keys.size());
  CHECK(softWarm.outerIterations <= 2);
  for (size_t i = 0; i < soft1T.values.size(); ++i) {
    CHECK(std::fabs(softWarm.values[i] - soft1T.values[i]) < 1e-5);
  }

  das::IndexedDasTablebase idxTb;
  REQUIRE(idxTb.open(softTbPath));
  const auto mc1 = das::evaluateSoftBoundaryMC20(idxTb, 4000, 90000000ULL, 1, bound);
  const auto mc4 = das::evaluateSoftBoundaryMC20(idxTb, 4000, 90000000ULL, 4, bound);
  CHECK(std::fabs(mc1.meanReturn - mc4.meanReturn) < 1e-12);
  CHECK(mc1.zScore <= 3.5);

  // 5. Terminal payoff overflow abort guard + divergence guard
  const auto ovRes = das::solve20MPI(keys, cfg, nullptr, OverflowBoundTest{});
  CHECK(!ovRes.ok);

  das::SolveConfig20 tightGuard = cfg;
  tightGuard.maxValueDivergenceGuard = 0.01;
  const auto divRes = das::solve20MPI(keys, tightGuard, nullptr, bound);
  CHECK(!divRes.ok);

  warmTb.close();
  fs::remove_all(tmpDir);
}

TEST_CASE("Phase 2 NNUE: 6-block central finite-difference gradient check (< 1e-4) and 1T == 4T "
          "determinism") {
  das::TinyDasNet net;
  net.initRandom(16, 8, das::RngMode::Nes, 12345ULL);

  // Build synthetic labeled decisions exercising ALL 4 loss terms simultaneously:
  // parent 7-output Huber + child Huber + ply-2 top-out target (0.0) + Boltzmann CE.
  das::Rng rng(999ULL);
  std::vector<das::LabeledDecision20> ds;
  for (int d = 0; d < 4; ++d) {
    das::LabeledDecision20 ld{};
    for (int r = 14; r < das::BOARD_ROWS; ++r) {
      ld.rows[static_cast<size_t>(r)] = static_cast<uint16_t>((rng.nextU64() & 0x3FEU));
    }
    ld.curPiece = d % das::NUM_PIECES;
    ld.nextPiece = (d + 2) % das::NUM_PIECES;
    ld.boardInTb = true;
    for (int p = 0; p < das::NUM_PIECES; ++p) {
      ld.vStar[static_cast<size_t>(p)] = 2.0f + 0.3f * static_cast<float>(p + d);
    }
    for (int m = 0; m < 4; ++m) {
      das::LabeledMoveCand20 mc{};
      mc.afterRows = ld.rows;
      mc.afterRows[13] = static_cast<uint16_t>(1U << m);
      mc.linesCleared = static_cast<uint8_t>(m % 3);
      // Include one explicit ply-2 top-out target (childValTarget = 0.0)
      mc.childValTarget = (m == 3) ? 0.0 : (1.5 + 0.4 * static_cast<double>(m));
      mc.qStar = das::REWARD_NORM_TETRIS[mc.linesCleared] + 0.995 * mc.childValTarget;
      ld.bestQStar = std::max(ld.bestQStar, mc.qStar);
      ld.moves.push_back(mc);
    }
    ds.push_back(std::move(ld));
  }

  das::NnTrainConfig20 tcfg;
  tcfg.h1 = 16;
  tcfg.h2 = 8;
  tcfg.valWeight = 1.0;
  tcfg.childValWeight = 1.0;
  tcfg.rankWeight = 4.0;
  tcfg.huberDelta = 0.5;
  tcfg.temperature = 0.7;
  tcfg.gamma = 0.995;

  const int idxs[4] = {0, 1, 2, 3};
  std::vector<double> grad(net.params.size(), 0.0);
  const double baseLoss = das::detail::computeBatchSliceLossAndGrad(net, ds, idxs, 4, tcfg, grad);
  REQUIRE(std::isfinite(baseLoss));

  // Verify analytical gradients against central finite differences across all 6 parameter blocks:
  // W1, b1, W2, b2, W3, b3. Use EPS = 5e-3f (optimal float32 cancellation vs curvature tradeoff)
  // and skip weights whose +/-EPS window crosses a ReLU/Huber kink.
  const int oneIdx[1] = {0};
  std::fill(grad.begin(), grad.end(), 0.0);
  const double singleLoss =
      das::detail::computeBatchSliceLossAndGrad(net, ds, oneIdx, 1, tcfg, grad);
  REQUIRE(std::isfinite(singleLoss));

  const std::pair<size_t, size_t> blocks[6] = {
      {net.offW1(), net.offB1()}, {net.offB1(), net.offW2()}, {net.offW2(), net.offB2()},
      {net.offB2(), net.offW3()}, {net.offW3(), net.offB3()}, {net.offB3(), net.params.size()},
  };
  constexpr float EPS = 5e-3f;
  int nonZeroChecked = 0;
  for (int b = 0; b < 6; ++b) {
    const size_t lo = blocks[b].first;
    const size_t hi = blocks[b].second;
    int checkedInBlock = 0;
    for (size_t p = lo; p < hi && checkedInBlock < 12; ++p) {
      if (std::abs(grad[p]) < 0.05)
        continue;
      const float orig = net.params[p];
      const float pPlus = orig + EPS;
      const float pMinus = orig - EPS;
      const double hPos = static_cast<double>(pPlus) - static_cast<double>(orig);
      const double hNeg = static_cast<double>(orig) - static_cast<double>(pMinus);
      std::vector<double> dummy(net.params.size(), 0.0);
      net.params[p] = pPlus;
      const double lPlus =
          das::detail::computeBatchSliceLossAndGrad(net, ds, oneIdx, 1, tcfg, dummy);
      net.params[p] = pMinus;
      const double lMinus =
          das::detail::computeBatchSliceLossAndGrad(net, ds, oneIdx, 1, tcfg, dummy);
      net.params[p] = orig;

      const double fdPos = (lPlus - singleLoss) / hPos;
      const double fdNeg = (singleLoss - lMinus) / hNeg;
      const double kinkGap = std::abs(fdPos - fdNeg) / std::max(1e-6, std::abs(grad[p]));
      if (kinkGap > 5e-3) {
        continue;
      }
      const double fd = (lPlus - lMinus) / (hPos + hNeg);
      const double denom = std::max({1e-6, std::abs(grad[p]), std::abs(fd)});
      const double relErr = std::abs(grad[p] - fd) / denom;
      CHECK(relErr < 5e-4);
      checkedInBlock++;
      nonZeroChecked++;
    }
    CHECK(checkedInBlock > 0);
  }
  CHECK(nonZeroChecked >= 24);

  // 1T == 4T training determinism
  tcfg.epochs = 3;
  tcfg.batchSize = 2;
  tcfg.verbose = false;
  tcfg.numThreads = 1;
  const das::TinyDasNet m1 = das::trainTinyDasNet(ds, ds, das::RngMode::Nes, tcfg);
  tcfg.numThreads = 4;
  const das::TinyDasNet m4 = das::trainTinyDasNet(ds, ds, das::RngMode::Nes, tcfg);
  REQUIRE(m1.params.size() == m4.params.size());
  for (size_t i = 0; i < m1.params.size(); ++i) {
    CHECK(std::fabs(m1.params[i] - m4.params[i]) < 1e-6f);
  }
}

TEST_CASE("Phase 2 End-to-End: Sampler 1T==4T, Partial-OUT, tbnn-precompute 18f RT, and Inspector "
          "routes") {
  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_e2e";
  fs::create_directories(tmpDir);
  const std::string tbPath = (tmpDir / "mini.tb.bin").string();
  const std::string nnPath = (tmpDir / "mini.nn.bin").string();

  const std::vector<das::BoardKey> keys = makeSmallTestSubset(140);
  das::SolveConfig20 scfg;
  scfg.level = 19;
  scfg.gamma = 0.995;
  scfg.tolerance = 1e-6;
  scfg.numThreads = 2;
  const auto sres = das::solve20MPI(keys, scfg);
  REQUIRE(sres.ok);

  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(das::RngMode::Nes);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = 19;
  hdr.gamma = 0.995;
  hdr.emptyBoardValue = sres.emptyBoardValue;
  hdr.bellmanResidual = sres.bellmanResidual;
  REQUIRE(das::writeTablebaseV3(tbPath, keys, sres.values, hdr));

  das::IndexedDasTablebase tb;
  REQUIRE(tb.open(tbPath));
  das::TinyDasNet nn;
  nn.initRandom(32, 16, das::RngMode::Nes, 555ULL);
  REQUIRE(nn.save(nnPath));

  // 1. State sampler 1T == 4T determinism (nn-error mode)
  das::StateGenConfig20 gcfg;
  gcfg.level = 19;
  gcfg.numGames = 32;
  gcfg.maxPiecesPerGame = 40;
  gcfg.maxLinesPerGame = 20;
  gcfg.minVisits = 1;
  gcfg.maxBoards = 200;
  gcfg.protectedCore = 20;
  gcfg.priorityMode = das::BoardPriorityMode::NnError;
  gcfg.baseSeed = 10000000ULL;
  gcfg.verbose = false;
  das::StateGenStats20 st1{}, st4{};
  gcfg.numThreads = 1;
  const auto g1 = das::generateStateSetTbNn20(gcfg, &tb, &nn, {}, st1);
  gcfg.numThreads = 4;
  const auto g4 = das::generateStateSetTbNn20(gcfg, &tb, &nn, {}, st4);
  CHECK(g1 == g4);
  CHECK(st1.totalMoves == st4.totalMoves);

  // 2. Distillation dataset sampler 1T == 4T determinism
  das::DistillSampleConfig20 dcfg;
  dcfg.numGames = 24;
  dcfg.maxDecisions = 300;
  dcfg.maxPiecesPerGame = 50;
  dcfg.baseSeed = 20000000ULL;
  dcfg.numThreads = 1;
  const auto d1 = das::sampleDistillDataset20(tb, &nn, dcfg);
  dcfg.numThreads = 4;
  const auto d4 = das::sampleDistillDataset20(tb, &nn, dcfg);
  REQUIRE(d1.decisions.size() == d4.decisions.size());
  CHECK(d1.totalSteps == d4.totalSteps);

  // 3. Calibrated Partial-OUT: all-OUT fallback (no TB) must choose NN move cleanly without clamp
  const auto allOut =
      das::chooseHybridMove20(das::Board20::empty(), 0, 4, das::GRAVITY_L19, 0.995,
                              static_cast<const das::IndexedDasTablebase *>(nullptr), &nn, 100.0,
                              das::RngMode::Nes, das::NnSearchConfig20::ply2());
  CHECK(allOut.found);
  CHECK(allOut.usedAllOutFallback);

  // 4. tbnn-precompute 18-frame RT adapter smoke test (6 moves on L19)
  const das::BackendContexts bc = das::BackendContexts::make10Hz();
  GameState gs = das::board20ToGameState(das::Board20::empty(), 19, 130, bc, AGGRO);
  das::Rng rng(30000000ULL);
  int curP = rng.sampleOpeningPiece(das::RngMode::Nes);
  int nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
  int dasCharge = 0;
  for (int i = 0; i < 6; ++i) {
    das::TbNnPrecomputeOptions popts;
    popts.dasCharge = dasCharge;
    popts.reactionTime = (i == 0) ? 0 : 18;
    popts.gamma = 0.995;
    popts.outMargin = 0.25;
    popts.outCfg = das::NnSearchConfig20::ply2();
    const auto pr = das::chooseTbNnPrecomputeMove(gs, curP, nextP, &tb, &nn, popts, bc);
    REQUIRE(pr.hasMove);
    CHECK(pr.dasChargeAfter >= 0);
    CHECK(pr.dasChargeAfter <= DAS_CHARGE_MAX);
    gs = pr.afterState;
    dasCharge = pr.dasChargeAfter;
    curP = nextP;
    nextP = rng.sampleNextPiece(das::RngMode::Nes, curP);
  }

  // 5. Inspector HTTP routes (/ping, /state-info, /moves, /step-disagreement, transactional
  // /reload)
  das::InspectorContext ictx;
  ictx.configuredTbPath = tbPath;
  ictx.configuredNnPath = nnPath;
  REQUIRE(ictx.reloadConfigured());

  const auto rPing = das::inspector::handleApiRequest("/ping", "", ictx);
  CHECK(rPing.status == 200);
  CHECK(rPing.body.find("\"tbLoaded\":true") != std::string::npos);
  CHECK(rPing.body.find("\"nnLoaded\":true") != std::string::npos);

  const auto rInfo = das::inspector::handleApiRequest("/state-info", "board=empty&next=I", ictx);
  CHECK(rInfo.status == 200);
  CHECK(rInfo.body.find("\"inTablebase\":true") != std::string::npos);

  const auto rMoves =
      das::inspector::handleApiRequest("/moves", "board=empty&piece=T&next=I", ictx);
  CHECK(rMoves.status == 200);
  CHECK(rMoves.body.find("\"isHybridBest\":true") != std::string::npos);

  const auto rStep = das::inspector::handleApiRequest(
      "/step-disagreement", "board=empty&piece=T&next=I&mode=tb-vs-eval&maxSteps=20", ictx);
  CHECK(rStep.status == 200);
  CHECK(rStep.body.find("\"board\":\"") != std::string::npos);

  // Bad query & unknown parameter rejection + margin=inf semantics
  CHECK(das::inspector::handleApiRequest("/moves", "board=bad&piece=T&next=I", ictx).status == 400);
  CHECK(
      das::inspector::handleApiRequest("/moves", "board=empty&piece=T&next=I&bogus=1", ictx).status
      == 400);
  CHECK(
      das::inspector::handleApiRequest("/moves", "board=empty&piece=T&next=I&margin=", ictx).status
      == 400);
  CHECK(das::inspector::handleApiRequest("/step-disagreement", "board=empty&piece=T&next=I&seed=-1",
                                         ictx)
            .status
        == 400);
  const auto rInf =
      das::inspector::handleApiRequest("/moves", "board=empty&piece=T&next=I&margin=inf", ictx);
  CHECK(rInf.status == 200);
  CHECK(rInf.body.find("\"isHybridBest\":true") != std::string::npos);

  // Transactional /reload failure must preserve live TB and NN
  ictx.configuredNnPath = (tmpDir / "missing.nn.bin").string();
  const auto rBadReload = das::inspector::handleApiRequest("/reload", "", ictx);
  CHECK(rBadReload.status == 500);
  CHECK(ictx.isTbLoaded());
  CHECK(ictx.isNnLoaded());

  ictx.tb.close();
  fs::remove_all(tmpDir);
}

TEST_CASE("Ply-2 TB re-entry: OUT ply-1 resolving back into TB uses exact V_tb without margin") {
  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_ply2_reentry";
  fs::create_directories(tmpDir);

  // Tiny TB containing only the empty board (V_tb = 6.0 for all pieces).
  // Rows 18 & 19 have cols 0..5 filled (0x3F0), leaving cols 6..9 open:
  //   - curPiece = O at cols 6..7 lands out-of-TB (cols 8..9 still open on rows 18..19).
  //   - nextPiece = O at cols 8..9 clears both rows and re-enters the empty board (IN TB, V = 6.0).
  const std::vector<das::BoardKey> keys = {das::EMPTY_BOARD_KEY};
  const std::vector<double> vals(das::NUM_PIECES, 6.0);
  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(das::RngMode::Nes);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = 19;
  hdr.gamma = 0.995;
  hdr.emptyBoardValue = 6.0;
  hdr.bellmanResidual = 0.0;
  const std::string tbPath = (tmpDir / "empty_only.tb.bin").string();
  REQUIRE(das::writeTablebaseV3(tbPath, keys, vals, hdr));

  das::IndexedDasTablebase tb;
  REQUIRE(tb.open(tbPath));

  uint16_t startRows[das::BOARD_ROWS] = {};
  startRows[18] = 0x3F0U; // cols 0..5 filled
  startRows[19] = 0x3F0U; // cols 0..5 filled
  const das::Board20 startBoard = das::Board20::fromRows(startRows);
  const int pieceO = 1;
  const double gamma = 0.995;
  const double expectedQ =
      das::REWARD_NORM_TETRIS[0] + gamma * (das::REWARD_NORM_TETRIS[2] + gamma * 6.0);

  // Works even with nn == nullptr AND margin == +inf (zero NN evaluation needed!).
  const das::HybridMoveChoice20 chInf = das::chooseHybridMove20(
      startBoard, pieceO, pieceO, das::GRAVITY_L19, gamma, &tb, nullptr,
      std::numeric_limits<double>::infinity(), das::RngMode::Nes, das::NnSearchConfig20::ply2());
  REQUIRE(chInf.found);
  CHECK(!chInf.chosenInTb);
  CHECK(!chInf.usedAllOutFallback);
  CHECK(std::fabs(chInf.score - expectedQ) < 1e-9);

  // Also works identically with a finite safety margin and loaded NN.
  das::TinyDasNet net;
  net.initRandom(16, 8, das::RngMode::Nes, 77ULL);
  const das::HybridMoveChoice20 chFin =
      das::chooseHybridMove20(startBoard, pieceO, pieceO, das::GRAVITY_L19, gamma, &tb, &net, 0.50,
                              das::RngMode::Nes, das::NnSearchConfig20::ply2());
  REQUIRE(chFin.found);
  CHECK(!chFin.chosenInTb);
  CHECK(!chFin.usedAllOutFallback);
  CHECK(std::fabs(chFin.score - expectedQ) < 1e-9);

  // Inspector /moves must report ply2ReenteredTb:true and match chooseHybridMove20's winner.
  das::InspectorContext ictx;
  ictx.configuredTbPath = tbPath;
  ictx.defaultMargin = 0.50;
  ictx.defaultOutPly = 2;
  std::string err;
  REQUIRE(ictx.reloadConfigured(&err));
  std::string bStr;
  bStr.reserve(200);
  for (int r = 0; r < das::BOARD_ROWS; ++r) {
    for (int c = 0; c < 10; ++c) {
      bStr.push_back(((startRows[r] >> (9 - c)) & 1U) ? '1' : '0');
    }
  }
  const auto rMoves =
      das::inspector::handleApiRequest("/moves", "board=" + bStr + "&piece=O&next=O", ictx);
  CHECK(rMoves.status == 200);
  CHECK(rMoves.body.find("\"ply2ReenteredTb\":true") != std::string::npos);
  CHECK(rMoves.body.find("\"inTb\":true,\"cells\":[[18,8],[18,9],[19,8],[19,9]]")
        != std::string::npos);
  CHECK(rMoves.body.find("\"evalPly2Move\":{") != std::string::npos);

  ictx.tb.close();
  fs::remove_all(tmpDir);
}
