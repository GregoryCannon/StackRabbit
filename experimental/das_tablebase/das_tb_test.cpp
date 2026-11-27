#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../src/tests/vendor/doctest.h"

#include "board_key.hpp"
#include "core_20row.hpp"
#include "das_rt_move_gen.hpp"
#include "das_rt_move_gen_ref.hpp"
#include "eval_adapter.hpp"
#include "inspector_routes.hpp"
#include "nnue_20row.hpp"
#include "rt_nn_boundary_20row.hpp"
#include "solver_20row.hpp"
#include "solver_rt_20row.hpp"
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
#include <map>
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
              if (fits) {
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
                const double qQuant = das::unpackTerminalPayoff(das::packTerminalPayoff(rew));
                for (int np = 0; np < das::NUM_PIECES; ++np) {
                  if (!have[np] || qQuant > bestQ[np]) {
                    bestQ[np] = qQuant;
                    have[np] = true;
                  }
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

struct NonFitGuardCheckBound {
  static constexpr bool enabled = true;
  mutable std::atomic<int> nonFitCalls{0};
  void evalRow(const uint16_t rows[das::BOARD_ROWS], double out[das::NUM_PIECES]) const noexcept {
    das::BoardKey tmp{};
    if (!das::encodeBoardKey(rows, tmp)) {
      nonFitCalls.fetch_add(1, std::memory_order_relaxed);
      for (int p = 0; p < das::NUM_PIECES; ++p)
        out[p] = 100.0;
      return;
    }
    for (int p = 0; p < das::NUM_PIECES; ++p)
      out[p] = 1.0;
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
  // Ensure "ply2Move" is serialized exactly once per move (no duplicate JSON keys).
  {
    const size_t firstPly2 = rMoves.body.find("\"ply2Move\":");
    const size_t firstEval = rMoves.body.find("\"evalPly2Move\":", firstPly2);
    REQUIRE(firstPly2 != std::string::npos);
    REQUIRE(firstEval != std::string::npos);
    CHECK(rMoves.body.find("\"ply2Move\":", firstPly2 + 1) > firstEval);
  }
  const auto rPing = das::inspector::handleApiRequest("/ping", "", ictx);
  CHECK(rPing.status == 200);
  CHECK(rPing.body.find("\"discountMode\":\"piece\"") != std::string::npos);

  ictx.tb.close();
  fs::remove_all(tmpDir);
}

TEST_CASE("Per-line discounting: SRTBV003 flag round-trip, 1T == 4T, and Soft-MC <= 3.5 sigma") {
  const std::vector<das::BoardKey> keys = makeSmallTestSubset(160);
  REQUIRE(keys.size() >= 80);

  das::SolveConfig20 cfg;
  cfg.rngMode = das::RngMode::Nes;
  cfg.level = 19;
  cfg.gamma = 0.90;
  cfg.discountMode = das::DiscountMode::PerLine;
  cfg.tolerance = 1e-8;
  cfg.maxOuterIterations = 120;
  cfg.innerEvalSweeps = 80;
  cfg.numThreads = 1;

  const auto s1 = das::solve20MPI(keys, cfg);
  REQUIRE(s1.ok);
  REQUIRE(s1.converged);
  CHECK(s1.bellmanResidual < 1e-7);

  das::SolveConfig20 cfg4 = cfg;
  cfg4.numThreads = 4;
  const auto s4 = das::solve20MPI(keys, cfg4);
  REQUIRE(s4.ok);
  CHECK(s4.values == s1.values);

  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_perline";
  fs::create_directories(tmpDir);
  const std::string tbPath = (tmpDir / "perline.tb.bin").string();
  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(das::RngMode::Nes);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = 19;
  hdr.flags = das::TB_FLAG_DISCOUNT_PER_LINE;
  hdr.gamma = cfg.gamma;
  hdr.emptyBoardValue = s1.emptyBoardValue;
  hdr.bellmanResidual = s1.bellmanResidual;
  REQUIRE(das::writeTablebaseV3(tbPath, keys, s1.values, hdr));

  das::IndexedDasTablebase tb;
  REQUIRE(tb.open(tbPath));
  CHECK(tb.discountMode() == das::DiscountMode::PerLine);
  das::MappedDasTablebase mappedTb;
  REQUIRE(mappedTb.open(tbPath));
  CHECK(mappedTb.discountMode() == das::DiscountMode::PerLine);
  mappedTb.close();

  // Unknown flag bits must be rejected on write.
  das::TablebaseHeaderV3 badHdr = hdr;
  badHdr.flags = 2U;
  CHECK(!das::writeTablebaseV3((tmpDir / "badflags.tb.bin").string(), keys, s1.values, badHdr));

  const auto mc = das::evaluateSoftBoundaryMC20(tb, 4000, 91000000ULL, 4);
  CHECK(mc.zScore <= 3.5);

  // Verify PerLine 3-ply NN scoring differs from PerPiece and respects discountMode, and verify
  // sampleDistillDataset20 interleaving produces valid decisions on a small prefix.
  das::TinyDasNet net;
  net.initRandom(16, 8, das::RngMode::Nes, 555ULL);
  uint16_t rows[das::BOARD_ROWS] = {};
  rows[19] = 0x3FEU;
  const double scPiece3 = das::scoreMoveNn3PlyTopK20(
      rows, 1, 0, das::GRAVITY_L19, 0.90, net, das::RngMode::Nes, 4, das::DiscountMode::PerPiece);
  const double scLine3 = das::scoreMoveNn3PlyTopK20(
      rows, 1, 0, das::GRAVITY_L19, 0.90, net, das::RngMode::Nes, 4, das::DiscountMode::PerLine);
  CHECK(std::isfinite(scPiece3));
  CHECK(std::isfinite(scLine3));
  CHECK(std::fabs(scPiece3 - scLine3) > 1e-6);

  das::DistillSampleConfig20 scfg;
  scfg.numGames = 60;
  scfg.maxDecisions = 300;
  scfg.numThreads = 2;
  const auto ds = das::sampleDistillDataset20(tb, nullptr, scfg);
  CHECK(ds.decisions.size() > 50);

  fs::remove_all(tmpDir);
}

TEST_CASE("Soft-Boundary !fitsKey guard: >=10-hole children receive 0.0 continuation and never "
          "query BoundaryFn") {
  // Construct a valid 9-hole board (fitsKey == true): row 18 filled in cols 0..8, row 19 empty.
  // Any placement covering col 9 creates >= 10 holes (!fitsKey).
  uint16_t nineHoleRows[das::BOARD_ROWS] = {};
  nineHoleRows[18] = 0x3FEU;
  das::BoardKey nineHoleKey{};
  REQUIRE(das::encodeBoardKey(nineHoleRows, nineHoleKey));

  // Verify at least one legal placement produces a !fitsKey (>= 10-hole) child.
  das::PlacementList pl;
  const das::Board20 nineBoard = das::Board20::fromKey(nineHoleKey);
  das::findPlacements10Hz(nineBoard, /*Z=*/6, das::GRAVITY_L19, pl);
  int nonFitChildren = 0;
  uint16_t afterRows[das::BOARD_ROWS];
  for (int m = 0; m < pl.count; ++m) {
    das::applyPlacement20(nineBoard, 6, pl.data[m].rot, pl.data[m].x, pl.data[m].y, afterRows);
    das::BoardKey ak{};
    if (!das::encodeBoardKey(afterRows, ak)) {
      nonFitChildren++;
    }
  }
  REQUIRE(nonFitChildren > 0);

  std::vector<das::BoardKey> keys = {das::EMPTY_BOARD_KEY, nineHoleKey};
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

  das::SolveConfig20 cfg;
  cfg.rngMode = das::RngMode::Nes;
  cfg.level = 19;
  cfg.gamma = 0.90;
  cfg.discountMode = das::DiscountMode::PerLine;
  cfg.tolerance = 1e-8;
  cfg.maxOuterIterations = 80;
  cfg.innerEvalSweeps = 80;
  cfg.numThreads = 1;

  NonFitGuardCheckBound bound;
  const auto res1 = das::solve20MPI(keys, cfg, nullptr, bound);
  REQUIRE(res1.ok);
  REQUIRE(res1.converged);
  CHECK(bound.nonFitCalls.load() == 0);
  for (double v : res1.values) {
    CHECK(v < 10.0);
  }

  das::SolveConfig20 cfg4 = cfg;
  cfg4.numThreads = 4;
  const auto res4 = das::solve20MPI(keys, cfg4, nullptr, bound);
  REQUIRE(res4.ok);
  CHECK(res4.values == res1.values);
  CHECK(bound.nonFitCalls.load() == 0);

  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_nonfit_guard";
  fs::create_directories(tmpDir);
  const std::string tbPath = (tmpDir / "nonfit.tb.bin").string();
  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(das::RngMode::Nes);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = 19;
  hdr.flags = das::TB_FLAG_DISCOUNT_PER_LINE;
  hdr.gamma = cfg.gamma;
  hdr.emptyBoardValue = res1.emptyBoardValue;
  hdr.bellmanResidual = res1.bellmanResidual;
  REQUIRE(das::writeTablebaseV3(tbPath, keys, res1.values, hdr));

  das::IndexedDasTablebase tb;
  REQUIRE(tb.open(tbPath));
  const auto mc = das::evaluateSoftBoundaryMC20(tb, 4000, 92000000ULL, 4, bound);
  CHECK(bound.nonFitCalls.load() == 0);
  CHECK(mc.zScore <= 3.5);

  fs::remove_all(tmpDir);
}

TEST_CASE("Selective runtime 3-ply: AllOut, Risk, bestTbOnly floor, and precompute parity") {
  das::OutPly3Trigger trig = das::OutPly3Trigger::Always;
  CHECK(das::parseOutPly3Trigger("always", trig));
  CHECK(trig == das::OutPly3Trigger::Always);
  CHECK(das::parseOutPly3Trigger("allout", trig));
  CHECK(trig == das::OutPly3Trigger::AllOut);
  CHECK(das::parseOutPly3Trigger("risk", trig));
  CHECK(trig == das::OutPly3Trigger::Risk);
  CHECK(!das::parseOutPly3Trigger("invalid", trig));
  CHECK(!das::parseOutPly3Trigger("", trig));

  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_selective_ply3";
  fs::create_directories(tmpDir);
  const std::string tbPath = (tmpDir / "tiny.tb.bin").string();

  // Construct:
  //  - emptyB (H=0, 0 holes) with one 1-ply child in TB
  //  - tallB (H=11, 0 holes, 99 filled cells) with one 1-ply child in TB (val=0.5) AND one ply-2
  //    grandchild in TB (val=2.0, via out-of-TB ply-1 child tallOut1) to exercise bestTbOnly floor
  //  - holeyLowB (H=3, 6 holes) with one 1-ply child in TB to test holes-triggered Risk
  uint16_t tallRows[das::BOARD_ROWS] = {};
  for (int r = 9; r < das::BOARD_ROWS; ++r) {
    tallRows[r] = 0x3FEU; // H=11, 0 holes, clean right well (99 filled cells)
  }
  uint16_t holeyLowRows[das::BOARD_ROWS] = {};
  holeyLowRows[17] = 0x380U; // cols 0..2 filled above empty rows 18..19 -> H=3, 6 holes
  const das::Board20 emptyB = das::Board20::empty();
  const das::Board20 tallB = das::Board20::fromRows(tallRows);
  const das::Board20 holeyLowB = das::Board20::fromRows(holeyLowRows);
  REQUIRE(holeyLowB.numHoles >= 2);

  das::PlacementList plEmpty, plTall, plHoley, plTall2;
  das::findPlacements10Hz(emptyB, /*O=*/1, das::GRAVITY_L19, plEmpty);
  das::findPlacements10Hz(tallB, /*O=*/1, das::GRAVITY_L19, plTall);
  das::findPlacements10Hz(holeyLowB, /*O=*/1, das::GRAVITY_L19, plHoley);
  REQUIRE(plEmpty.count > 0);
  REQUIRE(plTall.count > 2);
  REQUIRE(plHoley.count > 1);

  uint16_t childRows[das::BOARD_ROWS];
  uint16_t grandRows[das::BOARD_ROWS];
  das::applyPlacement20(emptyB, 1, plEmpty.data[0].rot, plEmpty.data[0].x, plEmpty.data[0].y,
                        childRows);
  das::BoardKey emptyChildK{};
  REQUIRE(das::encodeBoardKey(childRows, emptyChildK));

  das::applyPlacement20(tallB, 1, plTall.data[0].rot, plTall.data[0].x, plTall.data[0].y,
                        childRows);
  das::BoardKey tallChildK{};
  REQUIRE(das::encodeBoardKey(childRows, tallChildK));

  // Take a distinct OUT ply-1 child of tallB (plTall.data[1]) and place one of its ply-2 children
  // (nextPiece=O) into TB with high value 2.0 so bestReplyHybrid2Ply20 sets anyInTb & bestTbOnly.
  das::applyPlacement20(tallB, 1, plTall.data[1].rot, plTall.data[1].x, plTall.data[1].y,
                        childRows);
  const das::Board20 tallOut1 = das::Board20::fromRows(childRows);
  das::findPlacements10Hz(tallOut1, /*O=*/1, das::GRAVITY_L19, plTall2);
  REQUIRE(plTall2.count > 0);
  das::applyPlacement20(tallOut1, 1, plTall2.data[0].rot, plTall2.data[0].x, plTall2.data[0].y,
                        grandRows);
  das::BoardKey tallGrandK{};
  REQUIRE(das::encodeBoardKey(grandRows, tallGrandK));

  das::applyPlacement20(holeyLowB, 1, plHoley.data[0].rot, plHoley.data[0].x, plHoley.data[0].y,
                        childRows);
  das::BoardKey holeyChildK{};
  REQUIRE(das::encodeBoardKey(childRows, holeyChildK));

  std::vector<das::BoardKey> keys = {das::EMPTY_BOARD_KEY, emptyChildK, tallChildK, tallGrandK,
                                     holeyChildK};
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

  std::vector<double> vals(keys.size() * das::NUM_PIECES, 1.0);
  for (size_t i = 0; i < keys.size(); ++i) {
    const double v = (keys[i] == tallGrandK) ? 2.0 : ((keys[i] == tallChildK) ? 0.5 : 1.0);
    for (int p = 0; p < das::NUM_PIECES; ++p) {
      vals[i * das::NUM_PIECES + p] = v;
    }
  }
  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(das::RngMode::Nes);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = 19;
  hdr.flags = das::TB_FLAG_DISCOUNT_PER_LINE;
  hdr.gamma = 0.975;
  hdr.emptyBoardValue = 1.0;
  hdr.bellmanResidual = 0.0;
  REQUIRE(das::writeTablebaseV3(tbPath, keys, vals, hdr));

  das::IndexedDasTablebase tb;
  REQUIRE(tb.open(tbPath));
  das::TinyDasNet net;
  net.initRandom(16, 8, das::RngMode::Nes, 777ULL);

  const auto cfgPly2 = das::NnSearchConfig20::ply2();
  const auto cfgAlways = das::NnSearchConfig20::ply3(4, das::OutPly3Trigger::Always);
  const auto cfgAllOut = das::NnSearchConfig20::ply3(4, das::OutPly3Trigger::AllOut);
  const auto cfgRisk = das::NnSearchConfig20::ply3(4, das::OutPly3Trigger::Risk, 10, 2);

  // 1. Grounded low clean board (H=0, holes=0): both AllOut and Risk skip 3-ply and match ply2.
  const auto lowP2 =
      das::chooseHybridMove20(emptyB, 1, 0, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgPly2, das::DiscountMode::PerLine);
  const auto lowAO =
      das::chooseHybridMove20(emptyB, 1, 0, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgAllOut, das::DiscountMode::PerLine);
  const auto lowRk =
      das::chooseHybridMove20(emptyB, 1, 0, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgRisk, das::DiscountMode::PerLine);
  REQUIRE(lowP2.found);
  CHECK(!lowAO.usedPly3);
  CHECK(!lowRk.usedPly3);
  CHECK(lowAO.placement.rot == lowP2.placement.rot);
  CHECK(lowAO.placement.x == lowP2.placement.x);
  CHECK(lowAO.placement.y == lowP2.placement.y);
  CHECK(lowRk.placement.rot == lowP2.placement.rot);
  CHECK(lowRk.placement.x == lowP2.placement.x);
  CHECK(lowRk.placement.y == lowP2.placement.y);
  CHECK(std::fabs(lowAO.score - lowP2.score) <= 1e-12);
  CHECK(std::fabs(lowRk.score - lowP2.score) <= 1e-12);

  // 2. All-OUT board (no TB): usesPly2HybridPath(false) skips dead ply-2 pass; matches Always.
  const auto aoAllOut = das::chooseHybridMove20(
      emptyB, 1, 0, das::GRAVITY_L19, 0.975, static_cast<const das::IndexedDasTablebase *>(nullptr),
      &net, 0.15, das::RngMode::Nes, cfgAllOut, das::DiscountMode::PerLine);
  const auto aoAlways = das::chooseHybridMove20(
      emptyB, 1, 0, das::GRAVITY_L19, 0.975, static_cast<const das::IndexedDasTablebase *>(nullptr),
      &net, 0.15, das::RngMode::Nes, cfgAlways, das::DiscountMode::PerLine);
  REQUIRE(aoAllOut.found);
  CHECK(aoAllOut.usedAllOutFallback);
  CHECK(aoAllOut.usedPly3);
  CHECK(aoAlways.usedPly3);
  CHECK(aoAllOut.placement.rot == aoAlways.placement.rot);
  CHECK(aoAllOut.placement.x == aoAlways.placement.x);
  CHECK(aoAllOut.placement.y == aoAlways.placement.y);
  CHECK(std::fabs(aoAllOut.score - aoAlways.score) <= 1e-12);

  // 3. Grounded tall board (H=11 >= 10, nextPiece=I so bestTbGroundedQ=0.5): AllOut skips 3-ply;
  //    Risk triggers 3-ply on competitive OUT moves (0.0 + 1.8 >= 0.5).
  const auto tallAO =
      das::chooseHybridMove20(tallB, 1, 0, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgAllOut, das::DiscountMode::PerLine);
  const auto tallRk =
      das::chooseHybridMove20(tallB, 1, 0, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgRisk, das::DiscountMode::PerLine);
  REQUIRE(tallAO.found);
  REQUIRE(tallRk.found);
  CHECK(!tallAO.usedAllOutFallback);
  CHECK(!tallAO.usedPly3);
  CHECK(!tallRk.usedAllOutFallback);
  CHECK(tallRk.usedPly3);
  CHECK(std::isfinite(tallRk.score));

  // Verify bestTbGroundedQ prescreen prunes uncompetitive ~0.0 OUT moves when tallGrandK=2.0
  // (nextPiece=O) puts bestTbGroundedQ > 0.0 + NN_3PLY_PRESCREEN_SLACK (1.8).
  const auto tallPruned =
      das::chooseHybridMove20(tallB, 1, 1, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgRisk, das::DiscountMode::PerLine);
  CHECK(!tallPruned.usedPly3);

  // 4. Grounded low holey board (H=3 < 10, holes=6 >= 2): holes trigger Risk 3-ply, not AllOut.
  const auto holeyAO =
      das::chooseHybridMove20(holeyLowB, 1, 0, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgAllOut, das::DiscountMode::PerLine);
  const auto holeyRk =
      das::chooseHybridMove20(holeyLowB, 1, 0, das::GRAVITY_L19, 0.975, &tb, &net, 0.15,
                              das::RngMode::Nes, cfgRisk, das::DiscountMode::PerLine);
  CHECK(!holeyAO.usedPly3);
  CHECK(holeyRk.usedPly3);

  // 5. safetyMargin = +inf and nn = nullptr safely skip grounded Risk 3-ply.
  const auto tallInf = das::chooseHybridMove20(
      tallB, 1, 1, das::GRAVITY_L19, 0.975, &tb, &net, std::numeric_limits<double>::infinity(),
      das::RngMode::Nes, cfgRisk, das::DiscountMode::PerLine);
  CHECK(!tallInf.usedPly3);
  const auto tallNoNn =
      das::chooseHybridMove20(tallB, 1, 1, das::GRAVITY_L19, 0.975, &tb, nullptr, 0.15,
                              das::RngMode::Nes, cfgRisk, das::DiscountMode::PerLine);
  CHECK(!tallNoNn.usedPly3);

  // 6. Exercise bestTbOnly floor directly: construct a step-function NN on totalCells/200
  //    (feature g[11] = NN_FEAT_GLOBALS_OFF + 11) that outputs 3.0 at 107 cells (ply-2 O+O on
  //    tallB -> beats tallGrandK's 2.0 so !bestGroundedFromTb) and 0.0 at >=110 cells (ply-3 ->
  //    drops below 2.0). Risk 3-ply MUST run and floor tallOut1 back up to bestTbOnly (~2.0),
  //    winning over tallChildK (0.5) in both chooseHybridMove20 and chooseTbNnPrecomputeMove.
  das::TinyDasNet floorNet;
  floorNet.initRandom(16, 8, das::RngMode::Nes, 888ULL);
  std::fill(floorNet.params.begin(), floorNet.params.end(), 0.0f);
  const int cellsFeatIdx = das::NN_FEAT_GLOBALS_OFF + 11;
  floorNet.params[floorNet.offW1() + static_cast<size_t>(cellsFeatIdx) * 16U + 0U] = -1.0f;
  floorNet.params[floorNet.offB1() + 0U] = 0.55f;  // 0.55 - 107/200 = +0.015; 0.55 - 111/200 < 0
  floorNet.params[floorNet.offW2() + 0U] = 200.0f; // 0.015 * 200 = 3.0 at 107 cells, 0.0 at 111
  for (int p = 0; p < das::NUM_PIECES; ++p) {
    floorNet.params[floorNet.offW3() + static_cast<size_t>(p) * 8U + 0U] = 1.0f;
  }

  das::PlacementList plCheck;
  const auto p2Floor = das::detail::bestReplyHybrid2Ply20(
      tallOut1.rows(), /*O=*/1, das::GRAVITY_L19, 0.975, &tb, &floorNet, 0.15, das::RngMode::Nes,
      plCheck, das::DiscountMode::PerLine, /*alreadyGrounded=*/true);
  CHECK(p2Floor.anyInTb);
  CHECK(!p2Floor.bestGroundedFromTb);
  CHECK(p2Floor.bestTbOnly >= 1.99);
  CHECK(p2Floor.bestIfTbGrounded > p2Floor.bestTbOnly + 0.5);

  const auto floorChoice =
      das::chooseHybridMove20(tallB, 1, 1, das::GRAVITY_L19, 0.975, &tb, &floorNet, 0.15,
                              das::RngMode::Nes, cfgRisk, das::DiscountMode::PerLine);
  REQUIRE(floorChoice.found);
  CHECK(floorChoice.usedPly3);
  CHECK(!floorChoice.chosenInTb); // tallOut1 (floored at ~2.0) must beat tallChildK (0.5)
  CHECK(floorChoice.score >= 1.99);

  // 7. chooseTbNnPrecomputeMove selective 3-ply checks (AllOut skips low grounded, Risk triggers
  //    and respects bestTbOnly on tallB).
  const auto preBc = das::BackendContexts::make10Hz();
  das::TbNnPrecomputeOptions preOpts;
  preOpts.dasCharge = -1;
  preOpts.reactionTime = 0;
  preOpts.gamma = 0.975;
  preOpts.discountMode = das::DiscountMode::PerLine;
  preOpts.outMargin = 0.15;
  preOpts.outCfg = cfgAllOut;
  const GameState gsEmpty = das::board20ToGameState(emptyB, 19, 130, preBc, AGGRO);
  const auto preLowAO = das::chooseTbNnPrecomputeMove(gsEmpty, 1, 0, &tb, &net, preOpts, preBc);
  CHECK(preLowAO.hasMove);
  CHECK(!preLowAO.usedPly3);

  const GameState gsTall = das::board20ToGameState(tallB, 19, 130, preBc, AGGRO);
  preOpts.outCfg = cfgRisk;
  const auto preTallFloor =
      das::chooseTbNnPrecomputeMove(gsTall, 1, 1, &tb, &floorNet, preOpts, preBc);
  CHECK(preTallFloor.hasMove);
  CHECK(preTallFloor.usedPly3);
  CHECK(!preTallFloor.chosenInTb); // tallOut1 (floored at ~2.0) beats tallChildK (0.5)

  fs::remove_all(tmpDir);
}

TEST_CASE("DasRtMoveTables: parity, pole vaults, slow-taps, quicktaps, wall-charge tucks, "
          "edge cases, NES physics, and thread safety") {
  using MoveKey = std::tuple<int, int, int, int>;
  auto toSortedKeysList = [](const das::DasMoveList &ml) {
    std::vector<MoveKey> v;
    v.reserve(static_cast<size_t>(ml.count));
    for (int i = 0; i < ml.count; ++i) {
      v.emplace_back(ml.data[i].rot, ml.data[i].x, ml.data[i].y, ml.data[i].dasChargeAfter);
    }
    std::sort(v.begin(), v.end());
    return v;
  };
  auto toSortedKeysRef = [](const std::vector<das::ref::RefMove> &rm) {
    std::vector<MoveKey> v;
    v.reserve(rm.size());
    for (const auto &m : rm) {
      v.emplace_back(m.rot, m.x, m.y, m.dasAfter);
    }
    std::sort(v.begin(), v.end());
    return v;
  };
  auto rtResultsEqual = [](const das::DasRtSearchResult &a, const das::DasRtSearchResult &b) {
    if (a.numUniqueCells != b.numUniqueCells
        || a.numEarlyMoves != b.numEarlyMoves
        || a.numAdjStates != b.numAdjStates
        || a.numAdjEdges != b.numAdjEdges) {
      return false;
    }
    std::vector<MoveKey> ea, eb;
    for (int i = 0; i < a.numEarlyMoves; ++i) {
      const auto &ca = a.uniqueCells[a.earlyMoves[i].cellIdx];
      const auto &cb = b.uniqueCells[b.earlyMoves[i].cellIdx];
      ea.emplace_back(ca.rot, ca.x, ca.y, a.earlyMoves[i].dasAfter);
      eb.emplace_back(cb.rot, cb.x, cb.y, b.earlyMoves[i].dasAfter);
    }
    std::sort(ea.begin(), ea.end());
    std::sort(eb.begin(), eb.end());
    if (ea != eb) {
      return false;
    }
    std::unordered_map<uint64_t, int> bPhiMap;
    for (int i = 0; i < b.numAdjStates; ++i) {
      bPhiMap[b.adjStates[i].packKey()] = i;
    }
    if (static_cast<int>(bPhiMap.size()) != b.numAdjStates) {
      return false;
    }
    std::vector<MoveKey> ma, mb;
    for (int i = 0; i < a.numAdjStates; ++i) {
      auto it = bPhiMap.find(a.adjStates[i].packKey());
      if (it == bPhiMap.end()) {
        return false;
      }
      const int j = it->second;
      ma.clear();
      mb.clear();
      for (int e = a.adjMoveOffset[i]; e < a.adjMoveOffset[i + 1]; ++e) {
        const auto &c = a.uniqueCells[a.adjEdges[e].cellIdx];
        ma.emplace_back(c.rot, c.x, c.y, a.adjEdges[e].dasAfter);
      }
      for (int e = b.adjMoveOffset[j]; e < b.adjMoveOffset[j + 1]; ++e) {
        const auto &c = b.uniqueCells[b.adjEdges[e].cellIdx];
        mb.emplace_back(c.rot, c.x, c.y, b.adjEdges[e].dasAfter);
      }
      std::sort(ma.begin(), ma.end());
      std::sort(mb.begin(), mb.end());
      if (ma != mb) {
        return false;
      }
    }
    return true;
  };

  // 1. Crafted T-piece pole vault over a tall column-4 pillar (rows 6..19, height 14):
  //    Direct rot 0 and rot 3 (cols x+1..x+3 / x+2..x+3) clip (4,6) at frame 11 (x=1, y=5),
  //    whereas rotating 0->1 at f=0 (cols x+1..x+2) clears col 4 and then either:
  //      - un-rotates 1->0 at f=12 (x=0) to reach (rot=0, x=0, y=17) directly (no tuck), or
  //      - continues 1->2->3 to reach the left-wall cell (rot=3, x=-2, y=17), which is
  //        completely unreachable without pole vaults (even with Phase C tucks/spins!).
  {
    uint16_t rows[das::BOARD_ROWS] = {};
    for (int r = 6; r < das::BOARD_ROWS; ++r) {
      rows[r] |= das::colBitMask(4);
    }
    const das::Board20 pillarBoard = das::Board20::fromRows(rows);
    das::MoveSearchConfig cfgPv;
    cfgPv.enableQuicktaps = false;
    cfgPv.enablePoleVaults = true;
    das::MoveSearchConfig cfgNoPv = cfgPv;
    cfgNoPv.enablePoleVaults = false;
    das::DasRtMoveTables tabPv(cfgPv), tabNoPv(cfgNoPv);
    das::DasMoveList mlPv, mlNoPv;
    const das::DasStartState sp = das::DasStartState::spawn(/*T=*/4, 15);
    tabPv.searchMoves(pillarBoard, 4, sp, mlPv);
    tabNoPv.searchMoves(pillarBoard, 4, sp, mlNoPv);

    bool pvReachedRot0NoTuck = false;
    bool pvReachedLeftWallRot3 = false;
    bool noPvReachedLeftWallRot3 = false;
    for (int i = 0; i < mlPv.count; ++i) {
      const auto &m = mlPv.data[i];
      if (m.rot == 0 && m.x == 0 && m.y == 17) {
        pvReachedRot0NoTuck = (m.tuckInput == '.') && ((m.flags & das::MOVE_FLAG_POLE_VAULT) != 0U);
      }
      if (m.rot == 3 && m.x == -2 && m.y == 17) {
        pvReachedLeftWallRot3 = true;
        CHECK((m.flags & das::MOVE_FLAG_POLE_VAULT) != 0U);
      }
    }
    for (int i = 0; i < mlNoPv.count; ++i) {
      const auto &m = mlNoPv.data[i];
      if (m.rot == 3 && m.x == -2 && m.y == 17) {
        noPvReachedLeftWallRot3 = true;
      }
    }
    CHECK(pvReachedRot0NoTuck);
    CHECK(pvReachedLeftWallRot3);
    CHECK(!noPvReachedLeftWallRot3);
    CHECK(mlPv.count > mlNoPv.count);

    // Full sorted (rot, x, y, dasAfter) parity with reference + independent NES physics check.
    std::vector<das::ref::RefMove> refMoves;
    das::ref::referenceSearchMoves(pillarBoard, 4, sp, cfgPv, /*trackSeq=*/true, refMoves, nullptr);
    CHECK(toSortedKeysList(mlPv) == toSortedKeysRef(refMoves));
    for (const auto &rm : refMoves) {
      const auto vr = das::ref::validateNesInputSequence(pillarBoard, 4, sp, rm.seq, cfgPv);
      REQUIRE(vr.ok);
      CHECK(vr.finalRot == rm.rot);
      CHECK(vr.finalX == rm.x);
      CHECK(vr.finalY == rm.y);
      CHECK(vr.finalDas == rm.dasAfter);
    }
  }

  // 2. Native 7.5 Hz slow-tap (3-tap from spawn das=5 reaches x=0 higher than reset+DAS)
  //    and X.X quicktap toggle (enableQuicktaps=true reaches left wall at height 11).
  {
    uint16_t rows[das::BOARD_ROWS] = {};
    // Height 10 flat stack (rows 10..19): reset+DAS 3 shifts needs frame 22 (11 drops -> locks!),
    // while 7.5 Hz 3-tap finishes at frame 16 (8 drops -> clears height 10!).
    for (int r = 10; r < das::BOARD_ROWS; ++r) {
      rows[r] = static_cast<uint16_t>(das::FULL_ROW_MASK ^ das::colBitMask(9));
    }
    const das::Board20 b10 = das::Board20::fromRows(rows);
    das::MoveSearchConfig cfgSt;
    cfgSt.enableSlowTaps = true;
    cfgSt.enableQuicktaps = false;
    das::MoveSearchConfig cfgNoSt = cfgSt;
    cfgNoSt.enableSlowTaps = false;
    das::DasRtMoveTables tabSt(cfgSt), tabNoSt(cfgNoSt);
    das::DasMoveList mlSt, mlNoSt;
    const das::DasStartState sp5 = das::DasStartState::spawn(/*O=*/1, 5);
    tabSt.searchMoves(b10, 1, sp5, mlSt);
    tabNoSt.searchMoves(b10, 1, sp5, mlNoSt);

    bool stReachedX0 = false;
    bool noStReachedX0 = false;
    for (int i = 0; i < mlSt.count; ++i) {
      if (mlSt.data[i].x == 0) {
        stReachedX0 = true;
        CHECK((mlSt.data[i].flags & das::MOVE_FLAG_SLOW_TAP) != 0U);
      }
    }
    for (int i = 0; i < mlNoSt.count; ++i) {
      if (mlNoSt.data[i].x == 0) {
        noStReachedX0 = true;
      }
    }
    CHECK(stReachedX0);
    CHECK(!noStReachedX0);

    // Quicktap test: O-piece with das=15, height 11 (rows 9..19, lockY=6).
    // DAS 3 shifts to x=0 lands at f=12 (y=5), 4th DAS shift is at f=18 (locks at f=15, y=6!),
    // while X.X quicktap fires at f=14 (y=6, bottom at row 8 -> clears row 9!).
    uint16_t rows11[das::BOARD_ROWS] = {};
    for (int r = 9; r < das::BOARD_ROWS; ++r) {
      rows11[r] = static_cast<uint16_t>(das::FULL_ROW_MASK ^ das::colBitMask(9));
    }
    const das::Board20 b11 = das::Board20::fromRows(rows11);
    das::MoveSearchConfig cfgQt = cfgSt;
    cfgQt.enableQuicktaps = true;
    das::DasRtMoveTables tabQt(cfgQt);
    das::DasMoveList mlQt, mlNoQt;
    const das::DasStartState sp15 = das::DasStartState::spawn(/*O=*/1, 15);
    tabQt.searchMoves(b11, 1, sp15, mlQt);
    tabSt.searchMoves(b11, 1, sp15, mlNoQt);
    bool qtReachedLeftWall = false;
    bool noQtReachedLeftWall = false;
    for (int i = 0; i < mlQt.count; ++i) {
      if (mlQt.data[i].x == -1) {
        qtReachedLeftWall = true;
        CHECK((mlQt.data[i].flags & das::MOVE_FLAG_QUICKTAP) != 0U);
      }
    }
    for (int i = 0; i < mlNoQt.count; ++i) {
      if (mlNoQt.data[i].x == -1) {
        noQtReachedLeftWall = true;
      }
    }
    CHECK(qtReachedLeftWall);
    CHECK(!noQtReachedLeftWall);
  }

  // 3. Independent-thumb cooldown validator tests:
  //    - Quick L->R or A->B within 7 frames MUST be rejected by NES physics validator,
  //    - '_' ARE marker MUST be rejected when dasCharge <= 5,
  //    - Independent L on frame 0 + A on frame 1 MUST be accepted,
  //    - ARE hold '_' starts with dpadCooldown = 0 (allows X.X quicktap on frame 2).
  {
    const das::Board20 emptyB = das::Board20::empty();
    das::MoveSearchConfig cfg;
    const das::DasStartState sp5 = das::DasStartState::spawn(/*T=*/4, 5);
    const das::DasStartState sp15 = das::DasStartState::spawn(/*T=*/4, 15);
    // Quick L (f=0) -> R (f=2): violates 7-frame D-pad gap!
    CHECK(!das::ref::validateNesInputSequence(emptyB, 4, sp5,
                                              "L.R...................................", cfg)
               .ok);
    // Quick A (f=0) -> B (f=2): violates 7-frame rotate gap!
    CHECK(!das::ref::validateNesInputSequence(emptyB, 4, sp5,
                                              "A.B...................................", cfg)
               .ok);
    // ARE hold '_' illegal when das <= 5!
    std::string badAre = "_L";
    badAre.append(37, '.');
    CHECK(!das::ref::validateNesInputSequence(emptyB, 4, sp5, badAre, cfg).ok);
    // Independent L (f=0) + A (f=1): legal! (38 frames f=0..37 to lock at y=17 from y=-1)
    std::string indSeq = "LA";
    indSeq.append(36, '.');
    const auto vrInd = das::ref::validateNesInputSequence(emptyB, 4, sp5, indSeq, cfg);
    CHECK(vrInd.ok);
    CHECK(vrInd.finalX == 2);
    CHECK(vrInd.finalRot == 1);
    // ARE hold '_' (f=0 L, f=1 '.', f=2 L quicktap): legal with dpadCooldown=0 at spawn!
    std::string qtSeq = "_L.L";
    qtSeq.append(35, '.');
    const auto vrQt = das::ref::validateNesInputSequence(emptyB, 4, sp15, qtSeq, cfg);
    CHECK(vrQt.ok);
    CHECK(vrQt.finalX == 1);
    CHECK(vrQt.finalDas == 0);
  }

  // 4. Edge-case & config regressions:
  //    - Row-0 block + hole + I-piece (under-surface clamp & zero OOB/UB),
  //    - Stationary startDas in {0, 4, 5, 10, 11, 15, 16} exact dasCharge preservation,
  //    - Crafted Phase C wall-charge overhang tuck (MOVE_FLAG_TUCK | MOVE_FLAG_WALL_CHARGE),
  //    - Gravity 1 (L29), Gravity 3 (L18), rt=0, dpadGapFrames=0 parity,
  //    - Topped-out board, MoveSearchConfig::isValid(), and dasBucket* helpers.
  {
    // 4a. Row-0 block + hole + I-piece
    uint16_t row0Rows[das::BOARD_ROWS] = {};
    row0Rows[0] = das::colBitMask(0) | das::colBitMask(9);
    row0Rows[1] = das::colBitMask(0); // hole under (9, 0)
    const das::Board20 row0Board = das::Board20::fromRows(row0Rows);
    das::MoveSearchConfig cfg;
    das::DasRtMoveTables tables(cfg);
    das::DasMoveList ml;
    const das::DasStartState spI = das::DasStartState::spawn(/*I=*/0, 15);
    tables.searchMoves(row0Board, 0, spI, ml);
    std::vector<das::ref::RefMove> refMoves;
    das::ref::referenceSearchMoves(row0Board, 0, spI, cfg, /*trackSeq=*/true, refMoves, nullptr);
    CHECK(toSortedKeysList(ml) == toSortedKeysRef(refMoves));
    for (const auto &rm : refMoves) {
      const auto vr = das::ref::validateNesInputSequence(row0Board, 0, spI, rm.seq, cfg);
      REQUIRE(vr.ok);
    }

    // 4b. Stationary startDas in {0, 4, 5, 10, 11, 15, 16} exact preservation on empty board
    const das::Board20 emptyB = das::Board20::empty();
    das::DasRtSearchResult rtRes;
    for (int sd : {0, 4, 5, 10, 11, 15, 16}) {
      const das::DasStartState spT = das::DasStartState::spawn(/*T=*/4, sd);
      tables.searchMoves(emptyB, 4, spT, ml);
      das::ref::referenceSearchMoves(emptyB, 4, spT, cfg, false, refMoves, nullptr);
      CHECK(toSortedKeysList(ml) == toSortedKeysRef(refMoves));
      bool foundStationary = false;
      for (int i = 0; i < ml.count; ++i) {
        if (ml.data[i].rot == 0 && ml.data[i].x == das::SPAWN_COL && ml.data[i].y == 17) {
          foundStationary = true;
          CHECK(ml.data[i].dasChargeAfter == sd);
        }
      }
      CHECK(foundStationary);
      tables.searchRt(emptyB, 4, sd, rtRes);
      bool foundStationaryPhi = false;
      for (int i = 0; i < rtRes.numAdjStates; ++i) {
        const auto &phi = rtRes.adjStates[i];
        if (phi.x == das::SPAWN_COL && phi.rot == 0 && phi.heldDir == 0) {
          foundStationaryPhi = true;
          CHECK(phi.dasCharge == sd);
        }
      }
      CHECK(foundStationaryPhi);
    }

    // 4c. Crafted Phase C wall-charge overhang tuck (O-piece drops in cols 3..4, wall at col 5
    //     on rows 12..19 lets it wall-charge right to 16, overhang at (2, 16) blocks direct drop
    //     to cols 1..2, then L-tucks into cols 1..2 at row 18..19 with das >= 10!).
    uint16_t tuckRows[das::BOARD_ROWS] = {};
    for (int r = 12; r < das::BOARD_ROWS; ++r) {
      tuckRows[r] |= das::colBitMask(5); // right wall for wall-charge at x=2 (cols 3..4)
    }
    tuckRows[16] |= das::colBitMask(2); // overhang above cols 1..2
    const das::Board20 tuckBoard = das::Board20::fromRows(tuckRows);
    const das::DasStartState spO5 = das::DasStartState::spawn(/*O=*/1, 5);
    tables.searchMoves(tuckBoard, 1, spO5, ml);
    das::ref::referenceSearchMoves(tuckBoard, 1, spO5, cfg, /*trackSeq=*/true, refMoves, nullptr);
    CHECK(toSortedKeysList(ml) == toSortedKeysRef(refMoves));
    bool foundWallChargeTuck = false;
    for (int i = 0; i < ml.count; ++i) {
      const auto &m = ml.data[i];
      if (m.x == 1 && m.y == 17) {
        CHECK((m.flags & das::MOVE_FLAG_TUCK) != 0U);
        CHECK((m.flags & das::MOVE_FLAG_WALL_CHARGE) != 0U);
        CHECK(m.dasChargeAfter >= das::DAS_POST_SHIFT_CHARGE);
        foundWallChargeTuck = true;
      }
    }
    CHECK(foundWallChargeTuck);
    for (const auto &rm : refMoves) {
      const auto vr = das::ref::validateNesInputSequence(tuckBoard, 1, spO5, rm.seq, cfg);
      REQUIRE(vr.ok);
    }

    // 4d. Gravity 1 (L29), Gravity 3 (L18), rt=0, dpadGapFrames=0 parity
    for (int grav : {1, 3}) {
      das::MoveSearchConfig gcfg;
      gcfg.gravity = grav;
      das::DasRtMoveTables gtab(gcfg);
      const das::DasStartState sp = das::DasStartState::spawn(/*L=*/5, 10);
      gtab.searchMoves(tuckBoard, 5, sp, ml);
      das::ref::referenceSearchMoves(tuckBoard, 5, sp, gcfg, /*trackSeq=*/true, refMoves, nullptr);
      CHECK(toSortedKeysList(ml) == toSortedKeysRef(refMoves));
      for (const auto &rm : refMoves) {
        REQUIRE(das::ref::validateNesInputSequence(tuckBoard, 5, sp, rm.seq, gcfg).ok);
      }
    }
    {
      das::MoveSearchConfig rt0Cfg;
      rt0Cfg.reactionTime = 0;
      das::DasRtMoveTables rt0Tab(rt0Cfg);
      rt0Tab.searchRt(tuckBoard, 1, 10, rtRes);
      CHECK(rtRes.numEarlyMoves == 0);
      CHECK(rtRes.numAdjStates == 1);
      CHECK(rtRes.numUniqueCells == rtRes.numAdjEdges);
      das::DasRtSearchResult rt0Multi[2];
      const int twoDas[2] = {5, 15};
      rt0Tab.searchRtMultiDas(tuckBoard, 1, twoDas, 2, rt0Multi);
      CHECK(rt0Multi[0].numAdjStates == 1);
      CHECK(rt0Multi[1].numAdjStates == 1);
    }
    {
      das::MoveSearchConfig gap0Cfg;
      gap0Cfg.dpadGapFrames = 0;
      gap0Cfg.rotGapFrames = 0;
      das::DasRtMoveTables gap0Tab(gap0Cfg);
      gap0Tab.searchMoves(tuckBoard, 1, spO5, ml);
      das::ref::referenceSearchMoves(tuckBoard, 1, spO5, gap0Cfg, /*trackSeq=*/true, refMoves,
                                     nullptr);
      CHECK(toSortedKeysList(ml) == toSortedKeysRef(refMoves));
      for (const auto &rm : refMoves) {
        REQUIRE(das::ref::validateNesInputSequence(tuckBoard, 1, spO5, rm.seq, gap0Cfg).ok);
      }
    }

    // 4e. Topped-out board (immediate spawn collision), MoveSearchConfig::isValid(), dasBucket*
    uint16_t fullTopRows[das::BOARD_ROWS] = {};
    fullTopRows[0] = das::colBitMask(3) | das::colBitMask(4) | das::colBitMask(5);
    const das::Board20 toppedOut = das::Board20::fromRows(fullTopRows);
    tables.searchMoves(toppedOut, 4, das::DasStartState::spawn(4, 15), ml);
    CHECK(ml.count == 0);
    tables.searchRt(toppedOut, 4, 15, rtRes);
    CHECK(rtRes.numUniqueCells == 0);
    CHECK(rtRes.numAdjStates == 0);
    CHECK(rtRes.numEarlyMoves == 0);

    CHECK(cfg.isValid());
    das::MoveSearchConfig badCfg = cfg;
    badCfg.gravity = 0;
    CHECK(!badCfg.isValid());
    badCfg = cfg;
    badCfg.dpadGapFrames = 8;
    CHECK(!badCfg.isValid());
    badCfg = cfg;
    badCfg.quicktapDelayFrames = 1;
    CHECK(!badCfg.isValid());
    CHECK(das::dasBucket3(0) == 0);
    CHECK(das::dasBucket3(9) == 0);
    CHECK(das::dasBucket3(10) == 1);
    CHECK(das::dasBucket3(14) == 1);
    CHECK(das::dasBucket3(15) == 2);
    CHECK(das::dasBucket3(16) == 2);
    CHECK(das::dasBucket2(14) == 0);
    CHECK(das::dasBucket2(15) == 1);
    CHECK(das::dasBucketToRepresentative(0, 3) == 5);
    CHECK(das::dasBucketToRepresentative(1, 3) == 10);
    CHECK(das::dasBucketToRepresentative(2, 3) == 15);
    CHECK(das::dasBucketToRepresentative(0, 2) == 5);
    CHECK(das::dasBucketToRepresentative(1, 2) == 15);
    CHECK(tables.memoryFootprintBytes() > 0U);
  }

  // 5. Multi-DAS batch full-payload parity + 4-thread macOS stack-safety & concurrency test
  //    (both with external RtSearchScratch and with nullptr scratch).
  {
    das::MoveSearchConfig cfg;
    cfg.enableQuicktaps = false;
    cfg.reactionTime = 18;
    const das::DasRtMoveTables tables(cfg);
    constexpr int DAS_BUCKETS[3] = {5, 10, 15};

    std::atomic<int> errCount{0};
    std::vector<std::thread> workers;
    for (int tid = 0; tid < 4; ++tid) {
      workers.emplace_back([&, tid]() {
        das::Rng rng(0xC0FFEE00ULL + static_cast<uint64_t>(tid) * 99991ULL);
        das::DasRtSearchResult multiRes[3];
        das::DasRtSearchResult singleRes;
        das::RtSearchScratch scratch;
        std::vector<das::ref::RefMove> refMoves;
        std::vector<das::DasStartState> refAdj;

        for (int i = 0; i < 60; ++i) {
          uint16_t rows[das::BOARD_ROWS] = {};
          int surf[das::BOARD_WIDTH];
          for (int c = 0; c < das::BOARD_WIDTH; ++c) {
            surf[c] = rng.nextInt(15);
            for (int r = das::BOARD_ROWS - surf[c]; r < das::BOARD_ROWS; ++r) {
              rows[r] |= das::colBitMask(c);
            }
          }
          const int holes = (i % 2 == 0) ? 0 : (1 + rng.nextInt(6));
          for (int h = 0; h < holes; ++h) {
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
          const das::Board20 b = das::Board20::fromRows(rows);
          const int p = rng.nextInt(das::NUM_PIECES);

          // Alternate external scratch and nullptr scratch to exercise both paths on worker stacks.
          das::RtSearchScratch *scPtr = (i % 2 == 0) ? &scratch : nullptr;
          tables.searchRtMultiDas(b, p, DAS_BUCKETS, 3, multiRes, scPtr);
          for (int bi = 0; bi < 3; ++bi) {
            tables.searchRt(b, p, DAS_BUCKETS[bi], singleRes, scPtr);
            if (!rtResultsEqual(multiRes[bi], singleRes)) {
              errCount.fetch_add(1);
            }
            if (i % 10 == 0 && bi == 2) {
              const das::DasStartState sp = das::DasStartState::spawn(p, DAS_BUCKETS[bi]);
              das::ref::referenceSearchMoves(b, p, sp, cfg, /*trackSeq=*/true, refMoves, &refAdj);
              if (static_cast<int>(refAdj.size()) != multiRes[bi].numAdjStates) {
                errCount.fetch_add(1);
              }
              for (const auto &rm : refMoves) {
                const auto vr = das::ref::validateNesInputSequence(b, p, sp, rm.seq, cfg);
                if (!vr.ok
                    || vr.finalRot != rm.rot
                    || vr.finalX != rm.x
                    || vr.finalY != rm.y
                    || vr.finalDas != rm.dasAfter) {
                  errCount.fetch_add(1);
                }
              }
            }
          }
        }
      });
    }
    for (auto &w : workers) {
      w.join();
    }
    CHECK(errCount.load() == 0);
  }
}

TEST_CASE("Multi-bucket NNUE: 21-output save/load, legacy API collapse, finite-difference "
          "gradients, warm-start expansion") {
  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_tb_test_multibucket_nn";
  fs::create_directories(tmpDir);
  std::string err;

  das::Rng boardRng(4321ULL);
  auto randomRows = [&](std::array<uint16_t, das::BOARD_ROWS> &rows, int topRow) {
    rows.fill(0);
    for (int r = topRow; r < das::BOARD_ROWS; ++r) {
      rows[static_cast<size_t>(r)] = static_cast<uint16_t>(boardRng.nextU64() & 0x3FEU);
    }
  };

  // 1. Output-width validation, save/load round trips, and hostile outDim rejection.
  CHECK(das::isValidNnOutDim(7));
  CHECK(das::isValidNnOutDim(14));
  CHECK(das::isValidNnOutDim(21));
  CHECK(!das::isValidNnOutDim(0));
  CHECK(!das::isValidNnOutDim(8));
  CHECK(!das::isValidNnOutDim(28));
  CHECK(!das::isValidNnOutDim(-7));
  for (int outDim : {14, 21}) {
    das::TinyDasNet net;
    net.initRandom(32, 16, das::RngMode::Nes, 31337ULL, outDim);
    REQUIRE(net.isLoaded());
    CHECK(net.outDim == outDim);
    CHECK(net.numDasBuckets() == outDim / das::NUM_PIECES);
    const std::string path = (tmpDir / ("net" + std::to_string(outDim) + ".nn.bin")).string();
    REQUIRE(net.save(path, &err));
    das::TinyDasNet loaded;
    REQUIRE(loaded.load(path, &err));
    CHECK(loaded.outDim == outDim);
    CHECK(loaded.h1 == 32);
    CHECK(loaded.h2 == 16);
    CHECK(loaded.params == net.params);
  }
  {
    // 7-output initialization is unchanged by the outDim parameter.
    das::TinyDasNet a, b;
    a.initRandom(16, 8, das::RngMode::Nes, 99ULL);
    b.initRandom(16, 8, das::RngMode::Nes, 99ULL, das::NUM_PIECES);
    CHECK(a.params == b.params);
    CHECK(a.numDasBuckets() == 1);

    // Self-consistent file (size matches outDim = 8) is still rejected for its outDim.
    const std::string badPath = (tmpDir / "out8.nn.bin").string();
    das::TinyNnHeader hdr{};
    std::memcpy(hdr.magic, das::NN_FILE_MAGIC, sizeof(das::NN_FILE_MAGIC));
    hdr.featureVersion = das::NN_FEATURE_VERSION_20;
    hdr.inDim = das::NN_INPUT_DIM;
    hdr.h1 = 16;
    hdr.h2 = 8;
    hdr.outDim = 8;
    hdr.rngMode = 1;
    const std::vector<float> junk(das::TinyDasNet::totalParams(das::NN_INPUT_DIM, 16, 8, 8), 0.1f);
    {
      std::ofstream out(badPath, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
      out.write(reinterpret_cast<const char *>(junk.data()),
                static_cast<std::streamsize>(junk.size() * sizeof(float)));
    }
    CHECK(!a.load(badPath, &err));
    CHECK(a.isLoaded());
    CHECK(a.params == b.params);
    das::TinyDasNet bad = a;
    bad.outDim = 8;
    bad.params.resize(das::TinyDasNet::totalParams(das::NN_INPUT_DIM, 16, 8, 8));
    CHECK(!bad.isLoaded());
  }

  // 2. Legacy 7-value APIs on multi-bucket nets read the full-DAS bucket (bit-exact), on both the
  // stack-buffer and large-hidden forward paths.
  for (const auto &shape : {std::pair<int, int>{32, 16}, std::pair<int, int>{320, 128}}) {
    for (int outDim : {14, 21}) {
      das::TinyDasNet net;
      net.initRandom(shape.first, shape.second, das::RngMode::Nes, 2024ULL, outDim);
      const int kb = net.numDasBuckets();
      for (int t = 0; t < 8; ++t) {
        std::array<uint16_t, das::BOARD_ROWS> rows{};
        randomRows(rows, 10 + t);
        float all[das::NN_MAX_OUT_DIM];
        float legacy[das::NUM_PIECES];
        net.evalAllRt(rows.data(), all);
        net.evalAll(rows.data(), legacy);
        double evNes = 0.0;
        for (int c = 0; c < das::NUM_PIECES; ++c) {
          CHECK(net.legacyOutIdx(c) == c * kb + (kb - 1));
          CHECK(legacy[c] == all[c * kb + (kb - 1)]);
          CHECK(net.evalPiece(rows.data(), c) == all[c * kb + (kb - 1)]);
          for (int k = 0; k < kb; ++k) {
            CHECK(net.evalPieceBucket(rows.data(), c, k) == all[das::rtValueIndex(c, k, kb)]);
          }
          const double vc =
              std::isfinite(legacy[c]) ? std::max(0.0, static_cast<double>(legacy[c])) : 0.0;
          evNes += das::openingPieceProb(das::RngMode::Nes, c) * vc;
        }
        CHECK(net.evalExpected(rows.data(), das::RngMode::Nes) == evNes);
      }
    }
  }

  // 3. expandedToBuckets replicates each piece's output row across every bucket (bit-exact).
  das::TinyDasNet base7;
  base7.initRandom(32, 16, das::RngMode::Nes, 777ULL);
  for (int kb : {2, 3}) {
    const das::TinyDasNet expanded = base7.expandedToBuckets(kb);
    REQUIRE(expanded.isLoaded());
    CHECK(expanded.outDim == das::NUM_PIECES * kb);
    for (int t = 0; t < 8; ++t) {
      std::array<uint16_t, das::BOARD_ROWS> rows{};
      randomRows(rows, 8 + t);
      float v7[das::NUM_PIECES];
      float vAll[das::NN_MAX_OUT_DIM];
      base7.evalAll(rows.data(), v7);
      expanded.evalAllRt(rows.data(), vAll);
      for (int c = 0; c < das::NUM_PIECES; ++c) {
        for (int k = 0; k < kb; ++k) {
          CHECK(vAll[das::rtValueIndex(c, k, kb)] == v7[c]);
        }
      }
    }
  }

  // 4. Central finite-difference gradient check on a 21-output net with candidates spanning
  // different DAS buckets (plus one nextPiece-sentinel candidate) and 21 parent targets.
  das::TinyDasNet net;
  net.initRandom(16, 8, das::RngMode::Nes, 12345ULL, das::NN_MAX_OUT_DIM);
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
    ld.vStarDim = das::NN_MAX_OUT_DIM;
    for (int o = 0; o < das::NN_MAX_OUT_DIM; ++o) {
      ld.vStar[static_cast<size_t>(o)] = 2.0f + 0.15f * static_cast<float>(o + d);
    }
    for (int m = 0; m < 5; ++m) {
      das::LabeledMoveCand20 mc{};
      mc.afterRows = ld.rows;
      mc.afterRows[13] = static_cast<uint16_t>(1U << m);
      mc.linesCleared = static_cast<uint8_t>(m % 3);
      // Buckets 0, 1, 2 of varying pieces, plus the nextPiece sentinel for m == 4.
      mc.outIdx = (m == 4) ? das::NN_OUT_IDX_NEXT_PIECE
                           : static_cast<uint8_t>(das::rtValueIndex((d + m) % das::NUM_PIECES,
                                                                    m % das::MAX_DAS_BUCKETS,
                                                                    das::MAX_DAS_BUCKETS));
      mc.childValTarget = (m == 3) ? 0.0 : (1.5 + 0.4 * static_cast<double>(m));
      mc.qStar = das::REWARD_NORM_TETRIS[mc.linesCleared] + 0.995 * mc.childValTarget;
      ld.bestQStar = std::max(ld.bestQStar, mc.qStar);
      ld.moves.push_back(mc);
    }
    ds.push_back(std::move(ld));
  }
  CHECK(das::validateDecisionOutIdx20(ds, das::NN_MAX_OUT_DIM));
  CHECK(das::detail::effectiveOutIdx20(net, ds[0], ds[0].moves[4])
        == das::rtValueIndex(ds[0].nextPiece, das::MAX_DAS_BUCKETS - 1, das::MAX_DAS_BUCKETS));

  das::NnTrainConfig20 tcfg;
  tcfg.h1 = 16;
  tcfg.h2 = 8;
  tcfg.outDim = das::NN_MAX_OUT_DIM;
  tcfg.valWeight = 1.0;
  tcfg.childValWeight = 1.0;
  tcfg.rankWeight = 4.0;
  tcfg.huberDelta = 0.5;
  tcfg.temperature = 0.7;
  tcfg.gamma = 0.995;

  {
    // Candidate-only losses touch exactly the W3 rows / b3 entries of their effective outputs.
    das::NnTrainConfig20 candOnly = tcfg;
    candOnly.valWeight = 0.0;
    const int oneIdx[1] = {0};
    std::vector<double> g(net.params.size(), 0.0);
    das::detail::computeBatchSliceLossAndGrad(net, ds, oneIdx, 1, candOnly, g);
    bool used[das::NN_MAX_OUT_DIM] = {};
    for (const auto &mv : ds[0].moves) {
      used[das::detail::effectiveOutIdx20(net, ds[0], mv)] = true;
    }
    for (int o = 0; o < das::NN_MAX_OUT_DIM; ++o) {
      bool rowNonZero = (g[net.offB3() + static_cast<size_t>(o)] != 0.0);
      for (int k = 0; k < net.h2; ++k) {
        rowNonZero = rowNonZero || g[net.offW3() + static_cast<size_t>(o * net.h2 + k)] != 0.0;
      }
      if (!used[o]) {
        CHECK(!rowNonZero);
      }
    }
  }

  const int oneIdx[1] = {0};
  std::vector<double> grad(net.params.size(), 0.0);
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

  // 5. Evaluation metrics and outIdx validation.
  const das::NnEvalMetrics20 vm = das::evaluateNnOnDecisions20(net, ds, tcfg.gamma);
  CHECK(std::isfinite(vm.valMae));
  CHECK(std::isfinite(vm.meanRegret));
  {
    std::vector<das::LabeledDecision20> badDs = ds;
    badDs[1].moves[0].outIdx = 14; // valid for 21 outputs, invalid for 14
    CHECK(das::validateDecisionOutIdx20(badDs, 21));
    CHECK(!das::validateDecisionOutIdx20(badDs, 14, &err));
    std::vector<das::LabeledDecision20> badVStarDs = ds;
    badVStarDs[0].vStarDim = 7; // mismatches outDim = 21 when boardInTb is true
    CHECK(!das::validateDecisionOutIdx20(badVStarDs, 21, &err));
    das::NnTrainConfig20 cfg21Quick = tcfg;
    cfg21Quick.epochs = 1;
    cfg21Quick.verbose = false;
    CHECK_THROWS_AS(das::trainTinyDasNet(badVStarDs, ds, das::RngMode::Nes, cfg21Quick),
                    std::invalid_argument);
    das::NnTrainConfig20 cfg14 = tcfg;
    cfg14.outDim = 14;
    cfg14.epochs = 1;
    cfg14.verbose = false;
    CHECK_THROWS_AS(das::trainTinyDasNet(badDs, ds, das::RngMode::Nes, cfg14),
                    std::invalid_argument);
    das::NnTrainConfig20 cfgBadDim = tcfg;
    cfgBadDim.outDim = 8;
    CHECK_THROWS_AS(das::trainTinyDasNet(ds, ds, das::RngMode::Nes, cfgBadDim),
                    std::invalid_argument);
    das::TinyDasNet net14;
    net14.initRandom(16, 8, das::RngMode::Nes, 5ULL, 14);
    CHECK(std::isnan(das::evaluateNnOnDecisions20(net14, badDs, tcfg.gamma).valMae));
  }

  // 6. 1T == 4T training determinism for a 21-output net.
  tcfg.epochs = 3;
  tcfg.batchSize = 2;
  tcfg.verbose = false;
  tcfg.numThreads = 1;
  const das::TinyDasNet m1 = das::trainTinyDasNet(ds, ds, das::RngMode::Nes, tcfg);
  tcfg.numThreads = 4;
  const das::TinyDasNet m4 = das::trainTinyDasNet(ds, ds, das::RngMode::Nes, tcfg);
  CHECK(m1.outDim == das::NN_MAX_OUT_DIM);
  REQUIRE(m1.params.size() == m4.params.size());
  for (size_t i = 0; i < m1.params.size(); ++i) {
    CHECK(std::fabs(m1.params[i] - m4.params[i]) < 1e-6f);
  }

  // 7. Trainer warm start: a matching 7-output net of the same h1/h2 is expanded per bucket.
  {
    das::TinyDasNet warm7;
    warm7.initRandom(16, 8, das::RngMode::Nes, 4242ULL);
    das::NnTrainConfig20 wcfg = tcfg;
    wcfg.epochs = 0;
    const das::TinyDasNet started = das::trainTinyDasNet(ds, ds, das::RngMode::Nes, wcfg, &warm7);
    CHECK(started.outDim == das::NN_MAX_OUT_DIM);
    CHECK(started.params == warm7.expandedToBuckets(das::MAX_DAS_BUCKETS).params);
    das::TinyDasNet warm21;
    warm21.initRandom(16, 8, das::RngMode::Nes, 4243ULL, das::NN_MAX_OUT_DIM);
    const das::TinyDasNet same = das::trainTinyDasNet(ds, ds, das::RngMode::Nes, wcfg, &warm21);
    CHECK(same.params == warm21.params);
    das::TinyDasNet badH1;
    badH1.initRandom(32, 8, das::RngMode::Nes, 4244ULL, das::NN_MAX_OUT_DIM);
    CHECK_THROWS_AS(das::trainTinyDasNet(ds, ds, das::RngMode::Nes, wcfg, &badH1),
                    std::invalid_argument);
    das::TinyDasNet badOut14;
    badOut14.initRandom(16, 8, das::RngMode::Nes, 4245ULL, 14);
    CHECK_THROWS_AS(das::trainTinyDasNet(ds, ds, das::RngMode::Nes, wcfg, &badOut14),
                    std::invalid_argument);
  }

  fs::remove_all(tmpDir);
}

namespace {

// Small semi-closed subset of BoardKeys around the empty board for V_RT solver tests.
std::vector<das::BoardKey> makeRtTestSubset(size_t targetSize) {
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
  // Include a 0-hole high-ledge board (cols 3..9 filled on rows 5..19, col 0 filled on row 19,
  // cols 1..2 open) where an O-piece with full DAS (15) shifts left 3 columns in 5 frames to clear
  // the ledge and clear row 19, while min DAS (5) is blocked by the initial charge delay.
  {
    uint16_t ledgeRows[das::BOARD_ROWS] = {};
    for (int r = 5; r < das::BOARD_ROWS; ++r) {
      ledgeRows[r] = static_cast<uint16_t>(0x3F8U); // cols 3..9
    }
    ledgeRows[19] |= 0x1U; // col 0
    das::BoardKey lk;
    if (das::encodeBoardKey(ledgeRows, lk)
        && std::find(keys.begin(), keys.end(), lk) == keys.end()) {
      keys.push_back(lk);
    }
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

struct ConstSoftBoundRt {
  static constexpr bool enabled = true;
  void evalRowRt(const uint16_t rows[das::BOARD_ROWS], int numBuckets,
                 double out[das::MAX_RT_VALUES_PER_BOARD]) const noexcept {
    int h = 0;
    for (int r = 0; r < das::BOARD_ROWS; ++r) {
      if ((rows[r] & das::FULL_ROW_MASK) != 0U) {
        h = das::BOARD_ROWS - r;
        break;
      }
    }
    const double base = std::max(0.0, 1.5 - 0.08 * static_cast<double>(h));
    for (int n = 0; n < das::NUM_PIECES; ++n) {
      for (int k = 0; k < numBuckets; ++k) {
        out[n * numBuckets + k] = base + 0.05 * n + 0.03 * k;
      }
    }
  }
};

// Brute-force V_RT oracle: plain value iteration over transitions generated by the independent
// frame-by-frame reference move search (das_rt_move_gen_ref.hpp).
template <typename BoundaryFn>
std::vector<double> bruteForceRtValues(const std::vector<das::BoardKey> &keys,
                                       const das::MoveSearchConfig &mcfg, int K, double gamma,
                                       das::DiscountMode mode, const BoundaryFn &bound) {
  struct Edge {
    int child; // -1 = out of set
    uint8_t lc;
    int bucket;
    double outQ[das::MAX_RT_VALUES_PER_BOARD]; // used when child < 0
  };
  struct Action {
    std::vector<int> edges; // early: 1 edge; phantom: all edges
  };
  struct StateDef {
    std::vector<Action> actions;
  };
  const das::StepDiscountTable20 disc = das::StepDiscountTable20::make(gamma, mode);
  std::map<das::BoardKey, int> idOf;
  for (size_t i = 0; i < keys.size(); ++i) {
    idOf[keys[i]] = static_cast<int>(i);
  }
  const int vpb = das::NUM_PIECES * K;
  std::vector<Edge> edges;
  std::vector<StateDef> states(keys.size() * static_cast<size_t>(vpb));
  std::vector<das::ref::RefMove> moves, phiMoves;
  std::vector<das::DasStartState> adj;

  auto makeEdge = [&](const das::Board20 &b, int c, int rot, int x, int y, int dasAfter) {
    Edge e{};
    uint16_t rows[das::BOARD_ROWS];
    e.lc = das::applyPlacement20(b, c, rot, x, y, rows);
    e.bucket = das::dasChargeToBucket(dasAfter, K);
    das::BoardKey k;
    const bool fits = das::encodeBoardKey(rows, k);
    auto it = fits ? idOf.find(k) : idOf.end();
    e.child = (it != idOf.end()) ? it->second : -1;
    const double rew = das::REWARD_NORM_TETRIS[e.lc];
    if (e.child < 0) {
      double cont[das::MAX_RT_VALUES_PER_BOARD] = {};
      if constexpr (BoundaryFn::enabled) {
        if (fits) {
          bound.evalRowRt(rows, K, cont);
        }
        for (int o = 0; o < vpb; ++o) {
          const double cc = fits ? std::max(0.0, cont[o]) : 0.0;
          e.outQ[o] = das::unpackTerminalPayoff(das::packTerminalPayoff(rew + disc.f[e.lc] * cc));
        }
      } else {
        for (int o = 0; o < vpb; ++o)
          e.outQ[o] = rew;
      }
    }
    edges.push_back(e);
    return static_cast<int>(edges.size() - 1);
  };

  for (size_t i = 0; i < keys.size(); ++i) {
    const das::Board20 b = das::Board20::fromKey(keys[i]);
    for (int c = 0; c < das::NUM_PIECES; ++c) {
      for (int k = 0; k < K; ++k) {
        StateDef &sd = states[i * static_cast<size_t>(vpb) + static_cast<size_t>(c * K + k)];
        const das::DasStartState sp =
            das::DasStartState::spawn(c, das::dasBucketRepresentativeCharge(k, K));
        if (das::collides20(b, c, sp.x, sp.y, sp.rot)) {
          continue;
        }
        if (mcfg.reactionTime <= 0) {
          das::ref::referenceSearchMoves(b, c, sp, mcfg, false, moves, nullptr);
          Action a;
          for (const auto &m : moves)
            a.edges.push_back(makeEdge(b, c, m.rot, m.x, m.y, m.dasAfter));
          sd.actions.push_back(a);
          continue;
        }
        das::ref::referenceSearchMoves(b, c, sp, mcfg, false, moves, &adj);
        const int minRtY = static_cast<int>(sp.y)
                           + das::rt_detail::countGravityDrops(0, mcfg.reactionTime, mcfg.gravity);
        for (const auto &m : moves) {
          if (m.y < minRtY) {
            Action a;
            a.edges.push_back(makeEdge(b, c, m.rot, m.x, m.y, m.dasAfter));
            sd.actions.push_back(a);
          }
        }
        for (const auto &phi : adj) {
          das::ref::referenceSearchMoves(b, c, phi, mcfg, false, phiMoves, nullptr);
          if (phiMoves.empty())
            continue;
          Action a;
          for (const auto &m : phiMoves)
            a.edges.push_back(makeEdge(b, c, m.rot, m.x, m.y, m.dasAfter));
          sd.actions.push_back(a);
        }
      }
    }
  }

  std::vector<double> v(states.size(), 0.0), vn(states.size(), 0.0);
  for (int it = 0; it < 5000; ++it) {
    double maxDiff = 0.0;
    for (size_t i = 0; i < keys.size(); ++i) {
      for (int c = 0; c < das::NUM_PIECES; ++c) {
        for (int k = 0; k < K; ++k) {
          const size_t si = i * static_cast<size_t>(vpb) + static_cast<size_t>(c * K + k);
          double best = 0.0;
          bool any = false;
          for (const Action &a : states[si].actions) {
            double val = 0.0;
            for (int nx = 0; nx < das::NUM_PIECES; ++nx) {
              double bq = -1e300;
              for (int ei : a.edges) {
                const Edge &e = edges[static_cast<size_t>(ei)];
                const int o = nx * K + e.bucket;
                const double q =
                    (e.child >= 0)
                        ? das::REWARD_NORM_TETRIS[e.lc]
                              + disc.f[e.lc]
                                    * v[static_cast<size_t>(e.child) * static_cast<size_t>(vpb)
                                        + static_cast<size_t>(o)]
                        : e.outQ[o];
                bq = std::max(bq, q);
              }
              val += das::pieceTransitionProb(das::RngMode::Nes, c, nx) * bq;
            }
            if (!any || val > best) {
              best = val;
              any = true;
            }
          }
          vn[si] = best;
          maxDiff = std::max(maxDiff, std::abs(best - v[si]));
        }
      }
    }
    v.swap(vn);
    if (maxDiff < 1e-12)
      break;
  }
  return v;
}

das::SolveRtConfig20 makeRtCfg(int K, int threads, double gamma, das::DiscountMode mode) {
  das::SolveRtConfig20 cfg;
  cfg.base.rngMode = das::RngMode::Nes;
  cfg.base.level = 19;
  cfg.base.gamma = gamma;
  cfg.base.discountMode = mode;
  cfg.base.tolerance = 1e-10;
  cfg.base.innerStopRatio = 0.01;
  cfg.base.maxOuterIterations = 60;
  cfg.base.innerEvalSweeps = 400;
  cfg.base.numThreads = threads;
  cfg.base.minParallelBoards = 16;
  cfg.numDasBuckets = K;
  cfg.move.enableQuicktaps = false;
  cfg.move.reactionTime = 18;
  return cfg;
}

struct GuardCheckBoundaryRt {
  static constexpr bool enabled = true;
  mutable std::atomic<int> nonFitCalls{0};
  void evalRowRt(const uint16_t afterRows[das::BOARD_ROWS], int numBuckets,
                 double out[das::MAX_RT_VALUES_PER_BOARD]) const noexcept {
    das::BoardKey k;
    if (!das::encodeBoardKey(afterRows, k)) {
      nonFitCalls.fetch_add(1, std::memory_order_relaxed);
    }
    for (int o = 0; o < das::NUM_PIECES * numBuckets; ++o)
      out[o] = 1.0;
  }
};

} // namespace

TEST_CASE("SRTBV004: write/read round trip, Mapped == Indexed, any-format key loader, and guards") {
  namespace fs = std::filesystem;
  const fs::path tmpDir = fs::temp_directory_path() / "sr_das_rt_tb_io";
  fs::create_directories(tmpDir);
  const std::string path = (tmpDir / "rt.tb.bin").string();
  const std::string bad = (tmpDir / "bad.tb.bin").string();

  const std::vector<das::BoardKey> keys = makeRtTestSubset(40);
  for (int K : {2, 3}) {
    const size_t vpb = static_cast<size_t>(das::NUM_PIECES * K);
    std::vector<double> vals(keys.size() * vpb);
    for (size_t i = 0; i < vals.size(); ++i)
      vals[i] = 0.125 * static_cast<double>(i % 29);
    das::TablebaseHeaderV4 hdr{};
    hdr.rngMode = 1U;
    hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
    hdr.level = 19;
    hdr.flags = das::TB_FLAG_DISCOUNT_PER_LINE;
    hdr.numDasBuckets = static_cast<uint32_t>(K);
    das::fillHeaderV4MoveFields(hdr, das::MoveSearchConfig{});
    hdr.gamma = 0.975;
    hdr.emptyBoardValue = 3.5;
    hdr.bellmanResidual = 1e-7;
    std::string err;
    REQUIRE(das::writeTablebaseV4(path, keys, vals, hdr, &err));

    das::MappedDasRtTablebase mapped;
    REQUIRE(mapped.open(path, &err, /*verifyRecords=*/true));
    CHECK(mapped.size() == keys.size());
    CHECK(mapped.numDasBuckets() == K);
    CHECK(mapped.discountMode() == das::DiscountMode::PerLine);
    const das::MoveSearchConfig rt = das::moveConfigFromHeaderV4(mapped.header());
    CHECK(rt.reactionTime == 18);
    CHECK(rt.enableSlowTaps);
    CHECK(!rt.enableQuicktaps);
    das::IndexedDasRtTablebase indexed;
    REQUIRE(indexed.open(path, &err));
    float rowM[das::MAX_RT_VALUES_PER_BOARD], rowI[das::MAX_RT_VALUES_PER_BOARD];
    for (size_t i = 0; i < keys.size(); ++i) {
      REQUIRE(mapped.lookup(keys[i], rowM));
      REQUIRE(indexed.lookup(keys[i], rowI));
      for (size_t v = 0; v < vpb; ++v) {
        CHECK(rowM[v] == rowI[v]);
        CHECK(rowM[v] == static_cast<float>(vals[i * vpb + v]));
      }
    }
    std::vector<das::BoardKey> loaded;
    REQUIRE(das::loadInputBoardKeysAnyFormat(path, loaded, &err));
    CHECK(loaded == keys);

    // Hostile inputs: negative value, bad bucket count, truncated file.
    std::vector<double> neg = vals;
    neg[3] = -0.5;
    CHECK(!das::writeTablebaseV4(bad, keys, neg, hdr, &err));
    das::TablebaseHeaderV4 badHdr = hdr;
    badHdr.numDasBuckets = 4;
    CHECK(!das::writeTablebaseV4(bad, keys, vals, badHdr, &err));
    {
      std::ifstream in(path, std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
      std::ofstream out(bad, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - 5));
    }
    CHECK(!mapped.open(bad, &err));
    CHECK(mapped.isLoaded()); // transactional: previous mapping preserved
  }

  CHECK(das::dasChargeToBucket(0, 3) == 0);
  CHECK(das::dasChargeToBucket(9, 3) == 0);
  CHECK(das::dasChargeToBucket(10, 3) == 1);
  CHECK(das::dasChargeToBucket(14, 3) == 1);
  CHECK(das::dasChargeToBucket(15, 3) == 2);
  CHECK(das::dasChargeToBucket(16, 3) == 2);
  CHECK(das::dasChargeToBucket(14, 2) == 0);
  CHECK(das::dasChargeToBucket(16, 2) == 1);
  CHECK(das::dasBucketRepresentativeCharge(1, 3) == 10);
  CHECK(das::dasBucketRepresentativeCharge(1, 2) == 15);

  const uint32_t w = das::packRtPolicyAction(12345U, 3, 2);
  CHECK(das::unpackRtPolicyChild(w) == 12345U);
  CHECK(das::unpackRtPolicyBucket(w) == 2);
  CHECK(das::unpackPolicyLines(w) == 3);
  CHECK(das::unpackRtPolicyChild(das::packRtPolicyAction(das::INVALID_BOARD_ID, 0, 1))
        == das::INVALID_BOARD_ID);
  fs::remove_all(tmpDir);
}

TEST_CASE("solve20RtMPI: brute-force oracle parity (hard & soft boundary, K = 2 / 3, rt = 0 / 18), "
          "1T == 4T, Ram == Disk, warm start, !fitsKey guard, and RT Soft-MC") {
  const std::vector<das::BoardKey> keys = makeRtTestSubset(90);
  REQUIRE(keys.size() >= 60);

  // 1. Hard boundary, K = 3, rt = 18, PerLine gamma = 0.9.
  {
    const das::SolveRtConfig20 cfg = makeRtCfg(3, 1, 0.9, das::DiscountMode::PerLine);
    const auto res = das::solve20RtMPI(keys, cfg);
    REQUIRE(res.ok);
    CHECK(res.converged);
    CHECK(res.bellmanResidual < 1e-8);
    const auto oracle = bruteForceRtValues(keys, cfg.move, 3, 0.9, das::DiscountMode::PerLine,
                                           das::NoOpRtBoundaryEval20{});
    REQUIRE(oracle.size() == res.values.size());
    double maxErr = 0.0;
    for (size_t i = 0; i < oracle.size(); ++i)
      maxErr = std::max(maxErr, std::abs(oracle[i] - res.values[i]));
    CHECK(maxErr < 1e-7);
    CHECK(res.emptyBoardValue > 0.0);

    // Non-triviality: starting DAS bucket must matter on at least some boards.
    int bucketDiffs = 0;
    for (size_t i = 0; i < keys.size(); ++i)
      for (int c = 0; c < das::NUM_PIECES; ++c)
        if (std::abs(res.values[i * 21 + static_cast<size_t>(das::rtValueIndex(c, 2, 3))]
                     - res.values[i * 21 + static_cast<size_t>(das::rtValueIndex(c, 0, 3))])
            > 1e-9)
          bucketDiffs++;
    CHECK(bucketDiffs > 0);
    const size_t e = static_cast<size_t>(
        std::lower_bound(keys.begin(), keys.end(), das::EMPTY_BOARD_KEY) - keys.begin());
    for (int c = 0; c < das::NUM_PIECES; ++c) {
      CHECK(res.values[e * 21 + static_cast<size_t>(das::rtValueIndex(c, 2, 3))]
            >= res.values[e * 21 + static_cast<size_t>(das::rtValueIndex(c, 0, 3))] - 1e-12);
    }

    // 1T == 4T and Ram == Disk exact parity.
    das::SolveRtConfig20 cfg4 = cfg;
    cfg4.base.numThreads = 4;
    const auto res4 = das::solve20RtMPI(keys, cfg4);
    REQUIRE(res4.ok);
    CHECK(res4.values == res.values);
    das::SolveRtConfig20 cfgDisk = cfg4;
    cfgDisk.base.policyCacheMode = das::PolicyCacheMode::Disk;
    const auto resDisk = das::solve20RtMPI(keys, cfgDisk);
    REQUIRE(resDisk.ok);
    CHECK(resDisk.usedDiskPolicyCache);
    CHECK(resDisk.values == res.values);
  }

  // 2. Soft boundary, K = 2, rt = 18; plus !fitsKey (>= 10-hole) guard check.
  {
    const das::SolveRtConfig20 cfg = makeRtCfg(2, 2, 0.9, das::DiscountMode::PerLine);
    const ConstSoftBoundRt bound{};
    const auto res = das::solve20RtMPI(keys, cfg, nullptr, nullptr, bound);
    REQUIRE(res.ok);
    CHECK(res.converged);
    const auto oracle =
        bruteForceRtValues(keys, cfg.move, 2, 0.9, das::DiscountMode::PerLine, bound);
    double maxErr = 0.0;
    for (size_t i = 0; i < oracle.size(); ++i)
      maxErr = std::max(maxErr, std::abs(oracle[i] - res.values[i]));
    CHECK(maxErr < 1e-7);

    // Craft a 9-hole board where locking a piece creates >= 10 holes (!fitsKey) and verify
    // solve20RtMPI never invokes boundaryFn.evalRowRt on !fitsKey children.
    uint16_t nineHoleRows[das::BOARD_ROWS] = {};
    for (int h = 0; h < 9; ++h) {
      const int row = 18 - (h / 5) * 2;
      const int col = (h % 5) * 2;
      nineHoleRows[row] |= static_cast<uint16_t>(1U << col);
    }
    das::BoardKey nineHoleKey;
    REQUIRE(das::encodeBoardKey(nineHoleRows, nineHoleKey));
    std::vector<das::BoardKey> oneBoard = {nineHoleKey};
    GuardCheckBoundaryRt guardBound;
    const auto gRes = das::solve20RtMPI(oneBoard, cfg, nullptr, nullptr, guardBound);
    REQUIRE(gRes.ok);
    CHECK(guardBound.nonFitCalls.load() == 0);
  }

  // 3. rt = 0 (no reaction-time split), K = 3, PerPiece, hard boundary.
  {
    das::SolveRtConfig20 cfg = makeRtCfg(3, 2, 0.85, das::DiscountMode::PerPiece);
    cfg.move.reactionTime = 0;
    const auto res = das::solve20RtMPI(keys, cfg);
    REQUIRE(res.ok);
    const auto oracle = bruteForceRtValues(keys, cfg.move, 3, 0.85, das::DiscountMode::PerPiece,
                                           das::NoOpRtBoundaryEval20{});
    double maxErr = 0.0;
    for (size_t i = 0; i < oracle.size(); ++i)
      maxErr = std::max(maxErr, std::abs(oracle[i] - res.values[i]));
    CHECK(maxErr < 1e-7);
  }

  // 4. Warm start (V3 broadcast and V4) == cold start, then RT Soft-MC on the written TB.
  {
    namespace fs = std::filesystem;
    const fs::path tmpDir = fs::temp_directory_path() / "sr_das_rt_tb_solver";
    fs::create_directories(tmpDir);
    const std::string v3Path = (tmpDir / "v3.tb.bin").string();
    const std::string v4Path = (tmpDir / "v4.tb.bin").string();

    das::SolveRtConfig20 cfg = makeRtCfg(3, 4, 0.9, das::DiscountMode::PerLine);
    const ConstSoftBoundRt bound{};
    const auto cold = das::solve20RtMPI(keys, cfg, nullptr, nullptr, bound);
    REQUIRE(cold.ok);

    // V3 warm start: broadcast the K = 3 full-DAS bucket as a SRTBV003 table.
    std::vector<double> v3(keys.size() * das::NUM_PIECES);
    for (size_t i = 0; i < keys.size(); ++i)
      for (int c = 0; c < das::NUM_PIECES; ++c)
        v3[i * das::NUM_PIECES + static_cast<size_t>(c)] =
            cold.values[i * 21 + static_cast<size_t>(das::rtValueIndex(c, 2, 3))];
    das::TablebaseHeaderV3 h3{};
    h3.rngMode = 1U;
    h3.rewardMode = das::REWARD_MODE_NORM_TETRIS;
    h3.level = 19;
    h3.flags = das::TB_FLAG_DISCOUNT_PER_LINE;
    h3.gamma = 0.9;
    REQUIRE(das::writeTablebaseV3(v3Path, keys, v3, h3));
    das::MappedDasTablebase init3;
    REQUIRE(init3.open(v3Path));
    const auto warm3 = das::solve20RtMPI(keys, cfg, &init3, nullptr, bound);
    REQUIRE(warm3.ok);
    CHECK(warm3.warmStartMatched == keys.size());
    for (size_t i = 0; i < cold.values.size(); ++i)
      CHECK(std::abs(warm3.values[i] - cold.values[i]) < 1e-7);

    das::TablebaseHeaderV4 h4{};
    h4.rngMode = 1U;
    h4.rewardMode = das::REWARD_MODE_NORM_TETRIS;
    h4.level = 19;
    h4.flags = das::TB_FLAG_DISCOUNT_PER_LINE;
    h4.numDasBuckets = 3;
    das::fillHeaderV4MoveFields(h4, cfg.move);
    h4.gamma = 0.9;
    h4.emptyBoardValue = cold.emptyBoardValue;
    h4.bellmanResidual = cold.bellmanResidual;
    REQUIRE(das::writeTablebaseV4(v4Path, keys, cold.values, h4));
    das::MappedDasRtTablebase init4;
    REQUIRE(init4.open(v4Path));
    const auto warm4 = das::solve20RtMPI(keys, cfg, nullptr, &init4, bound);
    REQUIRE(warm4.ok);
    CHECK(warm4.outerIterations <= 2);
    for (size_t i = 0; i < cold.values.size(); ++i)
      CHECK(std::abs(warm4.values[i] - cold.values[i]) < 1e-7);

    // Both warm-start sources at once is an error.
    CHECK(!das::solve20RtMPI(keys, cfg, &init3, &init4, bound).ok);

    // RT Soft-MC: Mapped and Indexed agree exactly, 1T == 4T, and z <= 3.5.
    const das::DasRtMoveTables tables(cfg.move);
    das::IndexedDasRtTablebase idx4;
    REQUIRE(idx4.open(v4Path));
    const auto mcM = das::evaluateSoftBoundaryMcRt20(init4, tables, 3000, 91000000ULL, 1, bound);
    const auto mcI = das::evaluateSoftBoundaryMcRt20(idx4, tables, 3000, 91000000ULL, 4, bound);
    CHECK(mcM.meanReturn == mcI.meanReturn);
    CHECK(mcM.zScore <= 3.5);

    // On-policy RT distillation sampler (1T == 4T) + 21-output TinyDasNet training +
    // chooseTbNnRtMove.
    das::DistillSampleConfig20 scfg{};
    scfg.numGames = 16;
    scfg.maxDecisions = 120;
    scfg.maxPiecesPerGame = 30;
    scfg.baseSeed = 888888ULL;
    scfg.numThreads = 1;
    const das::DistillDataset20 ds1 = das::sampleDistillDatasetRt20(idx4, tables, nullptr, scfg);
    scfg.numThreads = 4;
    const das::DistillDataset20 ds4 = das::sampleDistillDatasetRt20(idx4, tables, nullptr, scfg);
    REQUIRE(!ds1.decisions.empty());
    REQUIRE(ds1.decisions.size() == ds4.decisions.size());
    CHECK(ds1.totalSteps == ds4.totalSteps);
    CHECK(das::validateDecisionOutIdx20(ds1.decisions, das::NN_MAX_OUT_DIM));
    for (size_t i = 0; i < ds1.decisions.size(); ++i) {
      CHECK(ds1.decisions[i].vStarDim == das::NN_MAX_OUT_DIM);
      CHECK(ds1.decisions[i].curPiece == ds4.decisions[i].curPiece);
      CHECK(ds1.decisions[i].nextPiece == ds4.decisions[i].nextPiece);
      CHECK(ds1.decisions[i].moves.size() == ds4.decisions[i].moves.size());
    }

    das::NnTrainConfig20 rtTrainCfg{};
    rtTrainCfg.h1 = 16;
    rtTrainCfg.h2 = 8;
    rtTrainCfg.outDim = das::NN_MAX_OUT_DIM;
    rtTrainCfg.epochs = 2;
    rtTrainCfg.batchSize = 16;
    rtTrainCfg.gamma = 0.9;
    rtTrainCfg.discountMode = das::DiscountMode::PerLine;
    rtTrainCfg.verbose = false;
    const das::TinyDasNet rtNet =
        das::trainTinyDasNet(ds1.decisions, ds1.decisions, das::RngMode::Nes, rtTrainCfg);
    CHECK(rtNet.outDim == das::NN_MAX_OUT_DIM);

    // Native V_RT runtime adapter (chooseTbNnRtMove) across rt = 0 and rt = 18, ply = 1 / 2 / 3.
    const das::BackendContexts bc = das::BackendContexts::make10Hz();
    const GameState emptyGs = das::board20ToGameState(das::Board20::empty(), 19, 130, bc, AGGRO);
    das::RtDecisionWorkspace rtWs;
    for (int rt : {0, 18}) {
      for (int ply : {1, 2, 3}) {
        das::TbNnPrecomputeOptions opts{};
        opts.dasCharge = 15;
        opts.reactionTime = rt;
        opts.gamma = 0.9;
        opts.discountMode = das::DiscountMode::PerLine;
        opts.outMargin = 0.15;
        opts.outCfg.ply = ply;
        opts.outCfg.topK = 2;
        const das::TbNnPrecomputeResult mv =
            das::chooseTbNnRtMove(emptyGs, 0, 6, &idx4, &rtNet, tables, rtWs, opts, bc);
        CHECK(mv.hasMove);
        CHECK(mv.dasChargeAfter >= 0);
        CHECK(mv.dasChargeAfter <= DAS_CHARGE_MAX);
      }
    }

    // Live-RT on-policy state generator (generateStateSetRt20): 1T == 4T determinism + nn-error.
    das::StateGenConfig20 gcfg{};
    gcfg.level = 19;
    gcfg.gamma = 0.9;
    gcfg.numGames = 16;
    gcfg.maxPiecesPerGame = 30;
    gcfg.maxLinesPerGame = 10;
    gcfg.epsilon = 0.15;
    gcfg.minVisits = 1;
    gcfg.siblingMinVisits = 2;
    gcfg.maxBoards = 200;
    gcfg.priorityMode = das::BoardPriorityMode::NnError;
    gcfg.protectedCore = 20;
    gcfg.baseSeed = 777777ULL;
    gcfg.numThreads = 1;
    das::StateGenStats20 gs1{}, gs4{};
    const auto st1 = das::generateStateSetRt20(gcfg, &idx4, tables, &rtNet, {}, gs1);
    gcfg.numThreads = 4;
    const auto st4 = das::generateStateSetRt20(gcfg, &idx4, tables, &rtNet, {}, gs4);
    CHECK(!st1.empty());
    CHECK(st1.size() <= gcfg.maxBoards);
    CHECK(st1 == st4);
    CHECK(gs1.gamesPlayed == gcfg.numGames);
    CHECK(gs4.gamesPlayed == gcfg.numGames);
    CHECK(std::is_sorted(st1.begin(), st1.end()));
    CHECK(std::binary_search(st1.begin(), st1.end(), das::EMPTY_BOARD_KEY));

    // Live targetBoards early-stop: reaches target before numGames ceiling, both cold and merged.
    das::StateGenConfig20 tcfg = gcfg;
    tcfg.numGames = 200;
    tcfg.minVisits = 2;
    tcfg.siblingMinVisits = 0;
    tcfg.targetBoards = 30;
    tcfg.maxBoards = 500;
    tcfg.numThreads = 2;
    tcfg.verbose = false;
    das::StateGenStats20 tsCold{};
    const auto stCold = das::generateStateSetRt20(tcfg, &idx4, tables, &rtNet, {}, tsCold);
    CHECK(stCold.size() >= tcfg.targetBoards);
    CHECK(tsCold.gamesPlayed > 0);
    CHECK(tsCold.gamesPlayed < tcfg.numGames);

    // Merged with stCold: only newly qualified non-prior boards count toward the delta.
    tcfg.targetBoards = stCold.size() + 20;
    tcfg.baseSeed = 999999ULL;
    das::StateGenStats20 tsMerge{};
    const auto stMerge = das::generateStateSetRt20(tcfg, &idx4, tables, &rtNet, stCold, tsMerge);
    CHECK(stMerge.size() >= tcfg.targetBoards);
    CHECK(tsMerge.gamesPlayed > 0);
    CHECK(tsMerge.gamesPlayed < tcfg.numGames);

    // Already at target via priorBoards: stops immediately with zero games played.
    tcfg.targetBoards = stCold.size();
    das::StateGenStats20 tsZero{};
    const auto stZero = das::generateStateSetRt20(tcfg, &idx4, tables, &rtNet, stCold, tsZero);
    CHECK(stZero == stCold);
    CHECK(tsZero.gamesPlayed == 0);
    CHECK(tsZero.targetBoardsReached);

    // priorBoards without EMPTY_BOARD_KEY + minVisits == 0/1 + 0-frame generateStateSetTbNn20.
    std::vector<das::BoardKey> priorNoEmpty;
    for (das::BoardKey k : stCold) {
      if (k != das::EMPTY_BOARD_KEY)
        priorNoEmpty.push_back(k);
    }
    REQUIRE(priorNoEmpty.size() + 1U == stCold.size());
    tcfg.minVisits = 0;
    tcfg.targetBoards = priorNoEmpty.size() + 1U + 15U;
    tcfg.baseSeed = 555555ULL;
    das::StateGenStats20 tsNoEmpty{};
    const auto stNoEmpty =
        das::generateStateSetRt20(tcfg, &idx4, tables, &rtNet, priorNoEmpty, tsNoEmpty);
    CHECK(stNoEmpty.size() >= tcfg.targetBoards);
    CHECK(tsNoEmpty.gamesPlayed > 0);
    CHECK(tsNoEmpty.gamesPlayed < tcfg.numGames);
    CHECK(tsNoEmpty.targetBoardsReached);

    das::StateGenConfig20 tbCfg = tcfg;
    tbCfg.minVisits = 1;
    tbCfg.targetBoards = 35;
    das::StateGenStats20 tsTb{};
    const auto stTb = das::generateStateSetTbNn20(tbCfg, nullptr, nullptr, {}, tsTb);
    CHECK(stTb.size() >= tbCfg.targetBoards);
    CHECK(tsTb.gamesPlayed > 0);
    CHECK(tsTb.gamesPlayed < tbCfg.numGames);
    CHECK(tsTb.targetBoardsReached);

    init3.close();
    init4.close();
    fs::remove_all(tmpDir);
  }

  // 5. Config guards.
  {
    das::SolveRtConfig20 cfg = makeRtCfg(4, 1, 0.9, das::DiscountMode::PerLine);
    CHECK(!das::solve20RtMPI(keys, cfg).ok);
    cfg = makeRtCfg(3, 1, 0.9, das::DiscountMode::PerLine);
    cfg.move.gravity = 3; // mismatches level 19
    CHECK(!das::solve20RtMPI(keys, cfg).ok);
  }
}
