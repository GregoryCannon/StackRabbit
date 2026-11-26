// Standalone verification & micro-benchmark driver for das_rt_move_gen.hpp:
//   1. Multi-regime 0-mismatch parity tests vs. brute-force frame-by-frame reference
//      (pure DAS, quicktaps, direct mode, L29 grav=1, L18 grav=3, rt=0, dpadGap=0,
//      full 0..16 DAS classes, and searchRtMultiDas vs. searchRt payload parity).
//   2. Independent NES physics validator over reconstructed input sequences across 3,000 boards.
//   3. Legacy superset/diff audit vs. src/backend/full_move_search.cpp at 7.5 Hz ("X.......").
//   4. Throughput micro-benchmark (warmup, per-regime breakdown, memory footprint, 8T scaling)
//      comparing DasRtMoveTables::searchRt / searchRtMultiDas vs. legacy fullMoveSearch rt=18.

#include "das_rt_move_gen.hpp"
#include "das_rt_move_gen_ref.hpp"
#include "eval_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace das;

namespace {

// Generates a realistic diverse mix of boards: clean low/mid stacks, 1-9 hole overhang boards,
// high cliffs/pillars (H = 10..16) for pole vaults, and extreme ceiling boards (H = 12..20,
// including row-0 blocks + holes).
Board20 makeDiverseBoard(Rng &rng, int idx) {
  uint16_t rows[BOARD_ROWS] = {};
  const int kind = idx % 5;
  int surf[BOARD_WIDTH];

  if (kind == 0) {
    // Clean 0-hole low/mid stack (H = 0..10), right well.
    const int base = rng.nextInt(7);
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      surf[c] = (c == 9 && rng.nextInt(3) != 0) ? 0 : std::clamp(base + rng.nextInt(5) - 1, 0, 10);
    }
  } else if (kind == 1) {
    // Mid stack with 1..5 holes / overhangs.
    const int base = 2 + rng.nextInt(8);
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      surf[c] = std::clamp(base + rng.nextInt(5) - 2, 0, 13);
    }
  } else if (kind == 2) {
    // Tall board (H = 10..16) with central peaks/cliffs for pole vaults & pre-RT collisions.
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      const bool peak = (c == 2 || c == 3 || c == 6 || c == 7) && (rng.nextInt(2) == 0);
      surf[c] = peak ? (10 + rng.nextInt(7)) : (4 + rng.nextInt(7));
    }
  } else if (kind == 3) {
    // Extreme survival / ceiling (H = 6..20, including row-0 blocks) + overhangs.
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      surf[c] = (c >= 3 && c <= 6) ? (8 + rng.nextInt(10)) : (6 + rng.nextInt(15));
    }
  } else {
    // Jagged random columns (0..15) with deliberate overhang steps.
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      surf[c] = rng.nextInt(16);
    }
  }

  for (int c = 0; c < BOARD_WIDTH; ++c) {
    for (int r = BOARD_ROWS - surf[c]; r < BOARD_ROWS; ++r) {
      rows[r] |= colBitMask(c);
    }
  }

  if (kind != 0) {
    const int nHoles = (kind == 1) ? (1 + rng.nextInt(5)) : rng.nextInt(8);
    for (int h = 0; h < nHoles; ++h) {
      const int c = rng.nextInt(BOARD_WIDTH);
      if (surf[c] >= 2) {
        const int r = (BOARD_ROWS - surf[c] + 1) + rng.nextInt(surf[c] - 1);
        rows[r] &= static_cast<uint16_t>(~colBitMask(c));
      }
    }
  }

  // Ensure no pre-existing full rows.
  for (int r = 0; r < BOARD_ROWS; ++r) {
    if (rows[r] == FULL_ROW_MASK) {
      rows[r] &= static_cast<uint16_t>(~colBitMask(rng.nextInt(BOARD_WIDTH)));
    }
  }

  return Board20::fromRows(rows);
}

using MoveKey = std::tuple<int, int, int, int>; // rot, x, y, das

std::vector<MoveKey> toSortedKeys(const DasMoveList &ml) {
  std::vector<MoveKey> v;
  v.reserve(static_cast<size_t>(ml.count));
  for (int i = 0; i < ml.count; ++i) {
    const DasLockMove &m = ml.data[i];
    v.emplace_back(m.rot, m.x, m.y, m.dasChargeAfter);
  }
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<MoveKey> toSortedKeys(const std::vector<ref::RefMove> &rm) {
  std::vector<MoveKey> v;
  v.reserve(rm.size());
  for (const auto &m : rm) {
    v.emplace_back(m.rot, m.x, m.y, m.dasAfter);
  }
  std::sort(v.begin(), v.end());
  return v;
}

bool rtResultsEqual(const DasRtSearchResult &a, const DasRtSearchResult &b) {
  if (a.numUniqueCells != b.numUniqueCells
      || a.numEarlyMoves != b.numEarlyMoves
      || a.numAdjStates != b.numAdjStates
      || a.numAdjEdges != b.numAdjEdges) {
    return false;
  }
  std::vector<MoveKey> ea, eb;
  ea.reserve(static_cast<size_t>(a.numEarlyMoves));
  eb.reserve(static_cast<size_t>(b.numEarlyMoves));
  for (int i = 0; i < a.numEarlyMoves; ++i) {
    const CompactPlacement &ca = a.uniqueCells[a.earlyMoves[i].cellIdx];
    const CompactPlacement &cb = b.uniqueCells[b.earlyMoves[i].cellIdx];
    ea.emplace_back(ca.rot, ca.x, ca.y, a.earlyMoves[i].dasAfter);
    eb.emplace_back(cb.rot, cb.x, cb.y, b.earlyMoves[i].dasAfter);
  }
  std::sort(ea.begin(), ea.end());
  std::sort(eb.begin(), eb.end());
  if (ea != eb) {
    return false;
  }

  std::unordered_map<uint64_t, int> bPhiMap;
  bPhiMap.reserve(static_cast<size_t>(b.numAdjStates) * 2 + 1);
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
      const CompactPlacement &c = a.uniqueCells[a.adjEdges[e].cellIdx];
      ma.emplace_back(c.rot, c.x, c.y, a.adjEdges[e].dasAfter);
    }
    for (int e = b.adjMoveOffset[j]; e < b.adjMoveOffset[j + 1]; ++e) {
      const CompactPlacement &c = b.uniqueCells[b.adjEdges[e].cellIdx];
      mb.emplace_back(c.rot, c.x, c.y, b.adjEdges[e].dasAfter);
    }
    std::sort(ma.begin(), ma.end());
    std::sort(mb.begin(), mb.end());
    if (ma != mb) {
      return false;
    }
  }
  return true;
}

int runParitySuite(const char *label, const MoveSearchConfig &cfg, int numBoards) {
  DasRtMoveTables tables(cfg);
  Rng rng(0x5EED3000ULL
          + (cfg.enableQuicktaps ? 100ULL : 0ULL)
          + (cfg.reactionTime * 7ULL)
          + (cfg.gravity * 31ULL)
          + (cfg.dpadGapFrames * 131ULL));
  constexpr int DAS_VALS[7] = {0, 4, 5, 10, 11, 15, 16};
  constexpr int BATCH_DAS[3] = {5, 10, 15};

  int mismatches = 0;
  int totalQueries = 0;
  int totalMoves = 0;
  int totalRtPhis = 0;
  int totalEarlyMoves = 0;

  DasMoveList fastMl;
  DasRtSearchResult rtRes;
  DasRtSearchResult multiRes[3];
  DasRtSearchResult singleCheck;
  RtSearchScratch scratch;
  std::vector<ref::RefMove> refMl;
  std::vector<DasStartState> refAdj;

  for (int bIdx = 0; bIdx < numBoards; ++bIdx) {
    const Board20 board = makeDiverseBoard(rng, bIdx);
    const int p = rng.nextInt(NUM_PIECES);
    const int d = DAS_VALS[rng.nextInt(7)];
    const DasStartState sp = DasStartState::spawn(p, d);
    totalQueries++;

    // 1. Single-state spawn parity + frame-RT capture in one reference call.
    tables.searchMoves(board, p, sp, fastMl);
    ref::referenceSearchMoves(board, p, sp, cfg, /*trackSeq=*/false, refMl,
                              cfg.reactionTime > 0 ? &refAdj : nullptr);
    const auto fk = toSortedKeys(fastMl);
    const auto rk = toSortedKeys(refMl);
    totalMoves += static_cast<int>(fk.size());
    if (fk != rk) {
      mismatches++;
      if (mismatches <= 5) {
        std::printf(
            "[MISMATCH %s spawn] board=%d piece=%c das=%d maxH=%d holes=%d fast=%zu ref=%zu\n",
            label, bIdx, PIECE_CHARS[p], d, BOARD_ROWS - board.topNonEmptyRow, board.numHoles,
            fk.size(), rk.size());
        for (const auto &k : fk) {
          if (!std::binary_search(rk.begin(), rk.end(), k)) {
            std::printf("  FAST-ONLY: rot=%d x=%d y=%d das=%d\n", std::get<0>(k), std::get<1>(k),
                        std::get<2>(k), std::get<3>(k));
          }
        }
        for (const auto &k : rk) {
          if (!std::binary_search(fk.begin(), fk.end(), k)) {
            std::printf("  REF-ONLY : rot=%d x=%d y=%d das=%d\n", std::get<0>(k), std::get<1>(k),
                        std::get<2>(k), std::get<3>(k));
          }
        }
      }
      continue;
    }

    // 2. Full 2-half RT parity (also checks rt == 0 contract!).
    tables.searchRt(board, p, d, rtRes, &scratch);
    if (cfg.reactionTime <= 0) {
      if (rtRes.numEarlyMoves != 0) {
        mismatches++;
        continue;
      }
      const bool spawnCollides = collides20(board, p, sp.x, sp.y, sp.rot);
      if (spawnCollides) {
        if (rtRes.numAdjStates != 0 || rtRes.numUniqueCells != 0 || rtRes.numAdjEdges != 0) {
          mismatches++;
        }
        continue;
      }
      if (rtRes.numAdjStates != 1 || rtRes.adjStates[0].packKey() != sp.packKey()) {
        mismatches++;
        continue;
      }
      std::vector<MoveKey> rtKeys;
      for (int e = rtRes.adjMoveOffset[0]; e < rtRes.adjMoveOffset[1]; ++e) {
        const CompactPlacement &c = rtRes.uniqueCells[rtRes.adjEdges[e].cellIdx];
        rtKeys.emplace_back(c.rot, c.x, c.y, rtRes.adjEdges[e].dasAfter);
      }
      std::sort(rtKeys.begin(), rtKeys.end());
      if (rtKeys != fk) {
        mismatches++;
      }
      continue;
    }

    // Compare reachable frame-RT adjState key sets.
    std::vector<uint64_t> fastPhiKeys, refPhiKeys;
    for (int i = 0; i < rtRes.numAdjStates; ++i) {
      fastPhiKeys.push_back(rtRes.adjStates[i].packKey());
    }
    for (const auto &st : refAdj) {
      refPhiKeys.push_back(st.packKey());
    }
    std::sort(fastPhiKeys.begin(), fastPhiKeys.end());
    std::sort(refPhiKeys.begin(), refPhiKeys.end());
    totalRtPhis += static_cast<int>(fastPhiKeys.size());
    totalEarlyMoves += rtRes.numEarlyMoves;

    if (fastPhiKeys != refPhiKeys) {
      mismatches++;
      if (mismatches <= 5) {
        std::printf(
            "[MISMATCH %s RT-phi] board=%d piece=%c das=%d maxH=%d fastPhi=%zu refPhi=%zu\n", label,
            bIdx, PIECE_CHARS[p], d, BOARD_ROWS - board.topNonEmptyRow, fastPhiKeys.size(),
            refPhiKeys.size());
      }
      continue;
    }

    // Compare early moves (< RT).
    const int minRtY =
        static_cast<int>(sp.y) + rt_detail::countGravityDrops(0, cfg.reactionTime, cfg.gravity);
    std::vector<MoveKey> fastEarly, refEarly;
    for (int i = 0; i < rtRes.numEarlyMoves; ++i) {
      const CompactPlacement &c = rtRes.uniqueCells[rtRes.earlyMoves[i].cellIdx];
      fastEarly.emplace_back(c.rot, c.x, c.y, rtRes.earlyMoves[i].dasAfter);
    }
    for (const auto &m : refMl) {
      if (m.y < minRtY) {
        refEarly.emplace_back(m.rot, m.x, m.y, m.dasAfter);
      }
    }
    std::sort(fastEarly.begin(), fastEarly.end());
    std::sort(refEarly.begin(), refEarly.end());
    if (fastEarly != refEarly) {
      mismatches++;
      if (mismatches <= 5) {
        std::printf("[MISMATCH %s RT-early] board=%d piece=%c das=%d fast=%zu ref=%zu\n", label,
                    bIdx, PIECE_CHARS[p], d, fastEarly.size(), refEarly.size());
      }
      continue;
    }

    // Check adjustment move sets per phi (all phis on every 5th board, 3 phis on others).
    const int stride = (bIdx % 5 == 0) ? 1 : std::max(1, rtRes.numAdjStates / 3);
    for (int i = 0; i < rtRes.numAdjStates; i += stride) {
      std::vector<MoveKey> fAdj;
      for (int e = rtRes.adjMoveOffset[i]; e < rtRes.adjMoveOffset[i + 1]; ++e) {
        const CompactPlacement &c = rtRes.uniqueCells[rtRes.adjEdges[e].cellIdx];
        fAdj.emplace_back(c.rot, c.x, c.y, rtRes.adjEdges[e].dasAfter);
      }
      std::sort(fAdj.begin(), fAdj.end());
      ref::referenceSearchMoves(board, p, rtRes.adjStates[i], cfg, false, refMl, nullptr);
      const auto rAdj = toSortedKeys(refMl);
      if (fAdj != rAdj) {
        mismatches++;
        if (mismatches <= 5) {
          const auto &phi = rtRes.adjStates[i];
          std::printf("[MISMATCH %s RT-adj] board=%d piece=%c das=%d "
                      "phi=(x=%d,y=%d,r=%d,d=%d,h=%d,dcd=%d,rcd=%d,scd=%d) fast=%zu ref=%zu\n",
                      label, bIdx, PIECE_CHARS[p], d, phi.x, phi.y, phi.rot, phi.dasCharge,
                      phi.heldDir, phi.dpadCooldown, phi.rotCooldown, phi.shiftCooldown,
                      fAdj.size(), rAdj.size());
          for (const auto &k : fAdj) {
            if (!std::binary_search(rAdj.begin(), rAdj.end(), k)) {
              std::printf("  FAST-ONLY: rot=%d x=%d y=%d das=%d\n", std::get<0>(k), std::get<1>(k),
                          std::get<2>(k), std::get<3>(k));
            }
          }
          for (const auto &k : rAdj) {
            if (!std::binary_search(fAdj.begin(), fAdj.end(), k)) {
              std::printf("  REF-ONLY : rot=%d x=%d y=%d das=%d\n", std::get<0>(k), std::get<1>(k),
                          std::get<2>(k), std::get<3>(k));
            }
          }
        }
        break;
      }
    }

    // 3. Full payload parity: searchRtMultiDas vs. independent searchRt on every 10th board.
    if (bIdx % 10 == 0) {
      tables.searchRtMultiDas(board, p, BATCH_DAS, 3, multiRes, &scratch);
      for (int bi = 0; bi < 3; ++bi) {
        tables.searchRt(board, p, BATCH_DAS[bi], singleCheck, &scratch);
        if (!rtResultsEqual(multiRes[bi], singleCheck)) {
          mismatches++;
          if (mismatches <= 5) {
            std::printf("[MISMATCH %s MultiDas-vs-Single] board=%d piece=%c das=%d\n", label, bIdx,
                        PIECE_CHARS[p], BATCH_DAS[bi]);
          }
          break;
        }
      }
    }
  }

  std::printf("[PARITY %-18s] boards=%d queries=%d moves=%d phis=%d early=%d pooledPhis=%zu "
              "preWalks=%zu mem=%.2fMB mismatches=%d\n",
              label, numBoards, totalQueries, totalMoves, totalRtPhis, totalEarlyMoves,
              tables.totalPooledAdjStates(), tables.totalPrecomputedWalks(),
              static_cast<double>(tables.memoryFootprintBytes()) / (1024.0 * 1024.0), mismatches);
  return mismatches;
}

int runPhysicsValidationSuite(const char *label, const MoveSearchConfig &cfg, int numBoards) {
  Rng rng(0xA11CE777ULL + (cfg.enableQuicktaps ? 55ULL : 0ULL) + (cfg.gravity * 17ULL));
  constexpr int DAS_VALS[7] = {0, 4, 5, 10, 11, 15, 16};
  int failures = 0;
  int validatedSeqs = 0;
  int validatedAdjSeqs = 0;
  std::vector<ref::RefMove> moves;
  std::vector<DasStartState> adjStates;

  for (int bIdx = 0; bIdx < numBoards; ++bIdx) {
    const Board20 board = makeDiverseBoard(rng, bIdx);
    const int p = rng.nextInt(NUM_PIECES);
    const int d = DAS_VALS[rng.nextInt(7)];
    const DasStartState sp = DasStartState::spawn(p, d);

    ref::referenceSearchMoves(board, p, sp, cfg, /*trackSeq=*/true, moves, &adjStates);
    for (const auto &m : moves) {
      const auto vr = ref::validateNesInputSequence(board, p, sp, m.seq, cfg);
      validatedSeqs++;
      if (!vr.ok
          || vr.finalX != m.x
          || vr.finalY != m.y
          || vr.finalRot != m.rot
          || vr.finalDas != m.dasAfter) {
        failures++;
        if (failures <= 5) {
          std::printf("[PHYS-FAIL %s spawn] b=%d p=%c d=%d seq='%s' err='%s' got=(%d,%d,%d,d=%d) "
                      "exp=(%d,%d,%d,d=%d) tuck=%c\n",
                      label, bIdx, PIECE_CHARS[p], d, m.seq.c_str(), vr.error.c_str(), vr.finalRot,
                      vr.finalX, vr.finalY, vr.finalDas, m.rot, m.x, m.y, m.dasAfter, m.tuckInput);
        }
      }
    }

    const int stride = std::max(1, static_cast<int>(adjStates.size()) / 3);
    for (size_t i = 0; i < adjStates.size(); i += static_cast<size_t>(stride)) {
      const DasStartState &phi = adjStates[i];
      ref::referenceSearchMoves(board, p, phi, cfg, /*trackSeq=*/true, moves, nullptr);
      for (const auto &m : moves) {
        const auto vr = ref::validateNesInputSequence(board, p, phi, m.seq, cfg);
        validatedAdjSeqs++;
        if (!vr.ok
            || vr.finalX != m.x
            || vr.finalY != m.y
            || vr.finalRot != m.rot
            || vr.finalDas != m.dasAfter) {
          failures++;
          if (failures <= 5) {
            std::printf(
                "[PHYS-FAIL %s adj] b=%d p=%c phi=(x=%d,y=%d,r=%d,d=%d,h=%d,dcd=%d,rcd=%d,scd=%d) "
                "seq='%s' err='%s' got=(%d,%d,%d,d=%d) exp=(%d,%d,%d,d=%d) tuck=%c\n",
                label, bIdx, PIECE_CHARS[p], phi.x, phi.y, phi.rot, phi.dasCharge, phi.heldDir,
                phi.dpadCooldown, phi.rotCooldown, phi.shiftCooldown, m.seq.c_str(),
                vr.error.c_str(), vr.finalRot, vr.finalX, vr.finalY, vr.finalDas, m.rot, m.x, m.y,
                m.dasAfter, m.tuckInput);
          }
        }
      }
    }
  }

  std::printf("[PHYSICS %-16s] boards=%d spawnSeqs=%d adjSeqs=%d failures=%d\n", label, numBoards,
              validatedSeqs, validatedAdjSeqs, failures);
  return failures;
}

void runLegacyAudit(int numBoards) {
  MoveSearchConfig cfg;
  cfg.enableQuicktaps = false;
  DasRtMoveTables tables(cfg);

  Rng rng(0x1E6AC775ULL);
  constexpr int DAS_VALS[3] = {5, 10, 15};

  int legacyTotal = 0;
  int newTotal = 0;
  int legacyMissingCell = 0;
  int legacyHigherDas = 0;
  int newExtraCells = 0;
  int newHigherDas = 0;
  int extraSlowTap = 0;
  int extraPoleVault = 0;
  int extraTuck = 0;
  int extraOther = 0;

  const BackendContexts bc = BackendContexts::make10Hz();
  DasMoveList fastMl;
  std::vector<FullMovePossibility> legRes;

  for (int bIdx = 0; bIdx < numBoards; ++bIdx) {
    const Board20 board = makeDiverseBoard(rng, bIdx);
    const int p = rng.nextInt(NUM_PIECES);
    const int d = DAS_VALS[rng.nextInt(3)];

    GameState gs = board20ToGameState(board, 19, 0, bc, AGGRO);
    const Piece *srPiece = &PIECE_LIST[p];
    legRes.clear();
    fullMoveSearch(gs, srPiece, "X.......", d, false, legRes);

    auto keyOf = [](int r, int x, int y) { return ((r * 16) + (x + 4)) * 32 + (y + 4); };
    std::unordered_map<int, int> legMap;
    for (const auto &m : legRes) {
      const int k = keyOf(m.rotationIndex, m.x, m.y);
      auto it = legMap.find(k);
      if (it == legMap.end() || m.dasChargeAfter > it->second) {
        legMap[k] = m.dasChargeAfter;
      }
    }

    tables.searchMoves(board, p, DasStartState::spawn(p, d), fastMl);
    std::unordered_map<int, DasLockMove> newMap;
    for (int i = 0; i < fastMl.count; ++i) {
      const auto &m = fastMl.data[i];
      newMap[keyOf(m.rot, m.x, m.y)] = m;
    }

    legacyTotal += static_cast<int>(legMap.size());
    newTotal += static_cast<int>(newMap.size());

    for (const auto &kv : legMap) {
      auto it = newMap.find(kv.first);
      if (it == newMap.end()) {
        legacyMissingCell++;
        if (legacyMissingCell <= 5) {
          const int y = (kv.first % 32) - 4;
          const int rx = kv.first / 32;
          const int x = (rx % 16) - 4;
          const int r = rx / 16;
          std::printf("  [LEGACY-ONLY] b=%d p=%c d=%d rot=%d x=%d y=%d das=%d maxH=%d holes=%d\n",
                      bIdx, PIECE_CHARS[p], d, r, x, y, kv.second,
                      BOARD_ROWS - board.topNonEmptyRow, board.numHoles);
        }
      } else if (kv.second > it->second.dasChargeAfter) {
        legacyHigherDas++;
        if (legacyHigherDas <= 5) {
          std::printf("  [LEGACY-HIGHER-DAS] b=%d p=%c d=%d cell=(%d,%d,%d) legDas=%d newDas=%d\n",
                      bIdx, PIECE_CHARS[p], d, it->second.rot, it->second.x, it->second.y,
                      kv.second, it->second.dasChargeAfter);
        }
      }
    }

    for (const auto &kv : newMap) {
      auto it = legMap.find(kv.first);
      if (it == legMap.end()) {
        newExtraCells++;
        const uint8_t fl = kv.second.flags;
        if (fl & MOVE_FLAG_POLE_VAULT) {
          extraPoleVault++;
        } else if (fl & MOVE_FLAG_SLOW_TAP) {
          extraSlowTap++;
        } else if (fl & MOVE_FLAG_TUCK) {
          extraTuck++;
        } else {
          extraOther++;
        }
      } else if (kv.second.dasChargeAfter > it->second) {
        newHigherDas++;
      }
    }
  }

  std::printf(
      "[LEGACY-AUDIT 7.5Hz] boards=%d legMoves=%d newMoves=%d | legOnly=%d legHigherDas=%d | "
      "newExtra=%d (slowTap=%d poleVault=%d tuck=%d other=%d) newHigherDas=%d\n",
      numBoards, legacyTotal, newTotal, legacyMissingCell, legacyHigherDas, newExtraCells,
      extraSlowTap, extraPoleVault, extraTuck, extraOther, newHigherDas);
}

void runThroughputBenchmark(int numBoards) {
  MoveSearchConfig cfg;
  cfg.enableQuicktaps = false;
  DasRtMoveTables tables(cfg);

  Rng rng(0xBE4C4001ULL);
  std::vector<Board20> boards;
  std::vector<GameState> srBoards;
  std::vector<int> pieces;
  std::vector<int> dasVals;
  std::vector<int> regimes;
  constexpr int DAS_VALS[3] = {5, 10, 15};

  const BackendContexts bc = BackendContexts::make10Hz();
  boards.reserve(static_cast<size_t>(numBoards));
  srBoards.reserve(static_cast<size_t>(numBoards));
  for (int i = 0; i < numBoards; ++i) {
    // Realistic mix: 60% clean/low-mid (regime 0), 25% 1-5 holes (regime 1), 15% tall (regime 2).
    const int k = (i % 20 < 12) ? 0 : ((i % 20 < 17) ? 1 : 2);
    Board20 b = makeDiverseBoard(rng, k);
    boards.push_back(b);
    srBoards.push_back(board20ToGameState(b, 19, 0, bc, AGGRO));
    pieces.push_back(rng.nextInt(NUM_PIECES));
    dasVals.push_back(DAS_VALS[rng.nextInt(3)]);
    regimes.push_back(k);
  }

  // Warmup pass (populates caches and stabilizes CPU frequency).
  DasRtSearchResult rtRes;
  DasRtSearchResult multiRes[3];
  RtSearchScratch scratch;
  for (int i = 0; i < std::min(200, numBoards); ++i) {
    tables.searchRt(boards[static_cast<size_t>(i)], pieces[static_cast<size_t>(i)],
                    dasVals[static_cast<size_t>(i)], rtRes, &scratch);
    tables.searchRtMultiDas(boards[static_cast<size_t>(i)], pieces[static_cast<size_t>(i)],
                            DAS_VALS, 3, multiRes, &scratch);
  }

  // 1. Single-DAS searchRt (with reusable thread-local scratch).
  uint64_t newChecksum = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < numBoards; ++i) {
    tables.searchRt(boards[static_cast<size_t>(i)], pieces[static_cast<size_t>(i)],
                    dasVals[static_cast<size_t>(i)], rtRes, &scratch);
    newChecksum += static_cast<uint64_t>(rtRes.numUniqueCells + rtRes.numAdjEdges);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double newUs =
      std::chrono::duration<double, std::micro>(t1 - t0).count() / static_cast<double>(numBoards);

  // 2. Legacy fullMoveSearch rt=18 (spawn + deduped frame-18 adjustments).
  std::vector<FullMovePossibility> legSpawn, legAdj;
  std::vector<AdjTimeSimStateSlot> captures;
  uint64_t legChecksum = 0;
  const auto t2 = std::chrono::steady_clock::now();
  for (int i = 0; i < numBoards; ++i) {
    const Piece *srP = &PIECE_LIST[pieces[static_cast<size_t>(i)]];
    legSpawn.clear();
    captures.clear();
    fullMoveSearch(srBoards[static_cast<size_t>(i)], srP, "X.......",
                   dasVals[static_cast<size_t>(i)], false, legSpawn, FullMoveSearchContext(), 18,
                   &captures);
    legChecksum += legSpawn.size();

    std::vector<std::string> seenSeq;
    for (const auto &slot : captures) {
      if (!slot.hasCapture) {
        continue;
      }
      bool dup = false;
      for (const auto &s : seenSeq) {
        if (s == slot.capture.preAdjInputSequence) {
          dup = true;
          break;
        }
      }
      if (dup) {
        continue;
      }
      seenSeq.push_back(slot.capture.preAdjInputSequence);
      FullMoveSearchContext ctx;
      ctx.existingXOffset = slot.capture.x - INITIAL_X;
      ctx.existingYOffset = slot.capture.y - srP->initialY;
      ctx.framesAlreadyElapsed = slot.capture.frameIndex;
      ctx.existingRotation = slot.capture.rotationIndex;
      ctx.isAdjustment = true;
      ctx.isArrContinued = slot.capture.isArrContinued;
      ctx.dasButtonHeld = slot.capture.dasButtonHeld;
      ctx.preAdjInputSequence = slot.capture.preAdjInputSequence;
      legAdj.clear();
      fullMoveSearch(srBoards[static_cast<size_t>(i)], srP, "X.......", slot.capture.dasCharge,
                     false, legAdj, ctx);
      legChecksum += legAdj.size();
    }
  }
  const auto t3 = std::chrono::steady_clock::now();
  const double legUs =
      std::chrono::duration<double, std::micro>(t3 - t2).count() / static_cast<double>(numBoards);

  // 3. Batch searchRtMultiDas across {5, 10, 15} (1T) + per-regime latency breakdown.
  uint64_t multiChecksum = 0;
  double regimeUs[3] = {0.0, 0.0, 0.0};
  int regimeCnt[3] = {0, 0, 0};
  const auto t6 = std::chrono::steady_clock::now();
  for (int i = 0; i < numBoards; ++i) {
    const auto r0 = std::chrono::steady_clock::now();
    tables.searchRtMultiDas(boards[static_cast<size_t>(i)], pieces[static_cast<size_t>(i)],
                            DAS_VALS, 3, multiRes, &scratch);
    const auto r1 = std::chrono::steady_clock::now();
    const int k = regimes[static_cast<size_t>(i)];
    regimeUs[k] += std::chrono::duration<double, std::micro>(r1 - r0).count();
    regimeCnt[k]++;
    multiChecksum += static_cast<uint64_t>(
        multiRes[0].numAdjEdges + multiRes[1].numAdjEdges + multiRes[2].numAdjEdges);
  }
  const auto t7 = std::chrono::steady_clock::now();
  const double multiUsPerDas = std::chrono::duration<double, std::micro>(t7 - t6).count()
                               / static_cast<double>(numBoards * 3);

  // 4. Single-call spawn searchMoves (rt=0).
  DasMoveList ml;
  uint64_t spawnChecksum = 0;
  const auto t4 = std::chrono::steady_clock::now();
  for (int i = 0; i < numBoards; ++i) {
    const int p = pieces[static_cast<size_t>(i)];
    tables.searchMoves(boards[static_cast<size_t>(i)], p,
                       DasStartState::spawn(p, dasVals[static_cast<size_t>(i)]), ml);
    spawnChecksum += static_cast<uint64_t>(ml.count);
  }
  const auto t5 = std::chrono::steady_clock::now();
  const double spawnUs =
      std::chrono::duration<double, std::micro>(t5 - t4).count() / static_cast<double>(numBoards);

  // 5. 8-thread aggregate throughput for searchRtMultiDas (measures L2/L3 scaling).
  constexpr int NUM_THREADS = 8;
  constexpr int REPS_8T = 4;
  std::atomic<uint64_t> mtChecksum{0};
  const auto t8 = std::chrono::steady_clock::now();
  {
    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);
    for (int tid = 0; tid < NUM_THREADS; ++tid) {
      workers.emplace_back([&, tid]() {
        DasRtSearchResult localRes[3];
        RtSearchScratch localSc;
        uint64_t acc = 0;
        for (int rep = 0; rep < REPS_8T; ++rep) {
          for (int i = tid; i < numBoards; i += NUM_THREADS) {
            tables.searchRtMultiDas(boards[static_cast<size_t>(i)], pieces[static_cast<size_t>(i)],
                                    DAS_VALS, 3, localRes, &localSc);
            acc += static_cast<uint64_t>(
                localRes[0].numAdjEdges + localRes[1].numAdjEdges + localRes[2].numAdjEdges);
          }
        }
        mtChecksum.fetch_add(acc, std::memory_order_relaxed);
      });
    }
    for (auto &w : workers) {
      w.join();
    }
  }
  const auto t9 = std::chrono::steady_clock::now();
  const double mtTotalSec = std::chrono::duration<double>(t9 - t8).count();
  const double mtBoardsPerSec =
      static_cast<double>(numBoards * REPS_8T) / std::max(1e-9, mtTotalSec);
  const double mtUsPer3Das = (mtTotalSec * 1e6) / static_cast<double>(numBoards * REPS_8T);

  std::printf(
      "[BENCH %d boards | mem=%.2fMB | scratch=%zuB]\n"
      "  1T: new_rt18=%.2f us/call | multi3_rt18=%.2f us/das (%.2f us/3das) | "
      "legacy_rt18=%.2f us/call | speedup=%.1fx (multi3=%.1fx) | spawn_only=%.2f us\n"
      "  Regimes (us/3das): clean=%.2f (%d) | holey=%.2f (%d) | tall=%.2f (%d)\n"
      "  8T: %.2f us/3das wall (%.1fk board-pieces/s, %.1fx 8T scaling, "
      "chk=%llu/%llu/%llu/%llu/%llu)\n",
      numBoards, static_cast<double>(tables.memoryFootprintBytes()) / (1024.0 * 1024.0),
      sizeof(RtSearchScratch), newUs, multiUsPerDas, multiUsPerDas * 3.0, legUs,
      legUs / std::max(0.001, newUs), legUs / std::max(0.001, multiUsPerDas), spawnUs,
      regimeUs[0] / std::max(1, regimeCnt[0]), regimeCnt[0],
      regimeUs[1] / std::max(1, regimeCnt[1]), regimeCnt[1],
      regimeUs[2] / std::max(1, regimeCnt[2]), regimeCnt[2], mtUsPer3Das, mtBoardsPerSec / 1000.0,
      (multiUsPerDas * 3.0) / std::max(0.001, mtUsPer3Das),
      static_cast<unsigned long long>(newChecksum), static_cast<unsigned long long>(multiChecksum),
      static_cast<unsigned long long>(legChecksum), static_cast<unsigned long long>(spawnChecksum),
      static_cast<unsigned long long>(mtChecksum.load()));
}

} // namespace

int main() {
  int totalErr = 0;

  // 1. 10,000-board parity suite: Pure DAS (enableQuicktaps=false), L19 grav=2, rt=18.
  {
    MoveSearchConfig cfg;
    cfg.enableQuicktaps = false;
    cfg.reactionTime = 18;
    totalErr += runParitySuite("pureDAS_rt18", cfg, 10000);
  }

  // 2. 5,000-board parity suite: Quicktaps enabled (X.X), L19 grav=2, rt=18.
  {
    MoveSearchConfig cfg;
    cfg.enableQuicktaps = true;
    cfg.reactionTime = 18;
    totalErr += runParitySuite("quicktap_rt18", cfg, 5000);
  }

  // 3. 2,000-board parity suite: Direct mode (no pole vaults / no slow taps), rt=18.
  {
    MoveSearchConfig cfg;
    cfg.enableSlowTaps = false;
    cfg.enablePoleVaults = false;
    cfg.enableQuicktaps = false;
    cfg.reactionTime = 18;
    totalErr += runParitySuite("direct_rt18", cfg, 2000);
  }

  // 4. Multi-gravity, rt=0, and dpadGap=0 parity suites.
  {
    MoveSearchConfig cfg;
    cfg.gravity = 1; // L29
    cfg.reactionTime = 18;
    totalErr += runParitySuite("L29_grav1_rt18", cfg, 2000);
  }
  {
    MoveSearchConfig cfg;
    cfg.gravity = 3; // L18
    cfg.reactionTime = 18;
    totalErr += runParitySuite("L18_grav3_rt18", cfg, 2000);
  }
  {
    MoveSearchConfig cfg;
    cfg.gravity = 2;
    cfg.reactionTime = 0;
    totalErr += runParitySuite("pureDAS_rt0", cfg, 1000);
  }
  {
    MoveSearchConfig cfg;
    cfg.gravity = 2;
    cfg.reactionTime = 18;
    cfg.dpadGapFrames = 0;
    cfg.rotGapFrames = 0;
    totalErr += runParitySuite("zeroGap_rt18", cfg, 1000);
  }

  // 5. Independent NES physics validator across 3,000 boards (L19 qt=0/1, L29, L18).
  {
    MoveSearchConfig cfg;
    cfg.enableQuicktaps = false;
    totalErr += runPhysicsValidationSuite("L19_qt0", cfg, 1000);
    cfg.enableQuicktaps = true;
    totalErr += runPhysicsValidationSuite("L19_qt1", cfg, 1000);
    cfg.enableQuicktaps = false;
    cfg.gravity = 1;
    totalErr += runPhysicsValidationSuite("L29_grav1", cfg, 500);
    cfg.gravity = 3;
    totalErr += runPhysicsValidationSuite("L18_grav3", cfg, 500);
  }

  // 6. Legacy superset/diff audit vs. fullMoveSearch at 7.5 Hz ("X.......").
  runLegacyAudit(2000);

  // 7. Throughput micro-benchmark vs. legacy fullMoveSearch rt=18.
  runThroughputBenchmark(2000);

  return (totalErr == 0) ? 0 : 1;
}
