#ifndef DAS_TABLEBASE_RT_NN_BOUNDARY_20ROW_HPP
#define DAS_TABLEBASE_RT_NN_BOUNDARY_20ROW_HPP

// Frozen TinyDasNet soft boundary for the reaction-time solver and RT agents:
//   B(b', n, k) = max(0, V_nn(b', n, k) - margin), and 0 when piece n cannot spawn on b'.
// 7-output nets are broadcast across DAS buckets; 7 * K-output nets are read per bucket.

#include "nnue_20row.hpp"
#include "solver_rt_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace das {

// True when `net` can supply values for `numBuckets` DAS buckets.
inline bool nnSupportsDasBuckets(const TinyDasNet &net, int numBuckets) noexcept {
  return isValidDasBucketCount(numBuckets)
         && net.isLoaded()
         && (net.outDim == NUM_PIECES || net.outDim == NUM_PIECES * numBuckets);
}

// Raw (un-margined, un-gated) NN predictions in [n * K + k] layout.
inline void evalNnRtRow(const TinyDasNet &net, const uint16_t rows[BOARD_ROWS], int numBuckets,
                        float out[MAX_RT_VALUES_PER_BOARD]) noexcept {
  if (!nnSupportsDasBuckets(net, numBuckets)) {
    const int nOut = (numBuckets > 0 && numBuckets <= MAX_DAS_BUCKETS) ? (NUM_PIECES * numBuckets)
                                                                       : MAX_RT_VALUES_PER_BOARD;
    for (int o = 0; o < nOut; ++o)
      out[o] = 0.0f;
    return;
  }
  if (net.outDim == NUM_PIECES) {
    float v[NUM_PIECES];
    net.evalAll(rows, v);
    for (int n = 0; n < NUM_PIECES; ++n)
      for (int k = 0; k < numBuckets; ++k)
        out[n * numBuckets + k] = v[n];
    return;
  }
  net.evalAllRt(rows, out);
}

struct NnSoftBoundaryEvalRt20 {
  static constexpr bool enabled = true;
  const TinyDasNet *net = nullptr;
  double margin = 0.15;

  void evalRowRt(const uint16_t afterRows[BOARD_ROWS], int numBuckets,
                 double outCont[MAX_RT_VALUES_PER_BOARD]) const noexcept {
    const int vpb = NUM_PIECES * numBuckets;
    if (net == nullptr || !nnSupportsDasBuckets(*net, numBuckets) || !std::isfinite(margin)) {
      for (int o = 0; o < vpb; ++o)
        outCont[o] = 0.0;
      return;
    }
    const double m = std::max(0.0, margin);
    float v[MAX_RT_VALUES_PER_BOARD];
    evalNnRtRow(*net, afterRows, numBuckets, v);
    const bool topClear = ((afterRows[0] | afterRows[1] | afterRows[2]) & FULL_ROW_MASK) == 0U;
    const Board20 bAfter = topClear ? Board20::empty() : Board20::fromRows(afterRows);
    for (int n = 0; n < NUM_PIECES; ++n) {
      const bool canSpawn = topClear || !collides20(bAfter, n, SPAWN_COL, PIECES_20[n].initialY, 0);
      for (int k = 0; k < numBuckets; ++k) {
        const int o = n * numBuckets + k;
        const double pred = static_cast<double>(v[o]);
        outCont[o] = (canSpawn && std::isfinite(pred) && pred > m) ? (pred - m) : 0.0;
      }
    }
  }
};

// Single-(piece, bucket) raw NN prediction; broadcasts 7-output nets across all buckets.
inline float evalNnPieceBucket(const TinyDasNet &net, const uint16_t rows[BOARD_ROWS], int piece,
                               int bucket, int numBuckets) noexcept {
  if (!nnSupportsDasBuckets(net, numBuckets)
      || piece < 0
      || piece >= NUM_PIECES
      || bucket < 0
      || bucket >= numBuckets) {
    return 0.0f;
  }
  if (net.outDim == NUM_PIECES) {
    return net.evalPiece(rows, piece);
  }
  return net.evalPieceBucket(rows, piece, bucket);
}

// Expected NN value over the next piece when `curPiece`'s lock leaves the next piece in `bucket`.
// Uses TinyDasNet::evalBucket so only the 7 Layer-3 outputs for `bucket` are computed.
inline double evalNnNextPieceExpectedRt(const TinyDasNet &net, const uint16_t rows[BOARD_ROWS],
                                        RngMode mode, int curPiece, int bucket,
                                        int numBuckets) noexcept {
  if (!nnSupportsDasBuckets(net, numBuckets)
      || curPiece < 0
      || curPiece >= NUM_PIECES
      || bucket < 0
      || bucket >= numBuckets) {
    return 0.0;
  }
  float v7[NUM_PIECES];
  net.evalBucket(rows, bucket, numBuckets, v7);
  double acc = 0.0;
  for (int nx = 0; nx < NUM_PIECES; ++nx) {
    const double val = std::max(0.0, static_cast<double>(v7[nx]));
    acc += pieceTransitionProb(mode, curPiece, nx) * val;
  }
  return acc;
}

namespace detail {

// 2-ply standalone NN reply value using DasRtMoveTables from (b2, nextPiece, startBucket),
// feeding each ply-2 placement's ending DAS bucket into the 3rd-piece NN expectation.
inline double bestReplyNn2PlyRt20(const Board20 &b2, int nextPiece, int startBucket, int numBuckets,
                                  const DasRtMoveTables &tables, const StepDiscountTable20 &disc,
                                  const TinyDasNet &net, RngMode mode, DasMoveList &pl2) noexcept {
  if (!nnSupportsDasBuckets(net, numBuckets)
      || nextPiece < 0
      || nextPiece >= NUM_PIECES
      || startBucket < 0
      || startBucket >= numBuckets) {
    return 0.0;
  }
  const int repDas = dasBucketRepresentativeCharge(startBucket, numBuckets);
  tables.searchMoves(b2, nextPiece, DasStartState::spawn(nextPiece, repDas), pl2);
  if (pl2.count == 0) {
    return 0.0;
  }
  uint16_t after2[BOARD_ROWS];
  double best2 = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < pl2.count; ++i) {
    const DasLockMove &p = pl2.data[i];
    const uint8_t lc2 = applyPlacement20(b2, nextPiece, p.rot, p.x, p.y, after2);
    const int k2 = dasChargeToBucket(p.dasChargeAfter, numBuckets);
    const double sc2 =
        REWARD_NORM_TETRIS[lc2]
        + disc.f[lc2] * evalNnNextPieceExpectedRt(net, after2, mode, nextPiece, k2, numBuckets);
    if (sc2 > best2) {
      best2 = sc2;
    }
  }
  return best2;
}

inline double bestReplyNn2PlyRt20(const uint16_t afterRows[BOARD_ROWS], int nextPiece,
                                  int startBucket, int numBuckets, const DasRtMoveTables &tables,
                                  double gamma, const TinyDasNet &net, RngMode mode,
                                  DasMoveList &pl2,
                                  DiscountMode discountMode = DiscountMode::PerPiece) noexcept {
  const Board20 b2 = Board20::fromRows(afterRows);
  const StepDiscountTable20 disc = StepDiscountTable20::make(gamma, discountMode);
  return bestReplyNn2PlyRt20(b2, nextPiece, startBucket, numBuckets, tables, disc, net, mode, pl2);
}

// 3-ply Top-K standalone NN reply value using DasRtMoveTables, threading exact ending DAS buckets
// from ply 1 -> ply 2 -> ply 3.
inline double bestReplyNn3PlyTopKRt20(const uint16_t afterRows[BOARD_ROWS], int nextPiece,
                                      int startBucket, int numBuckets,
                                      const DasRtMoveTables &tables, double gamma,
                                      const TinyDasNet &net, RngMode mode,
                                      int topK = NN_DEFAULT_3PLY_TOPK,
                                      DiscountMode discountMode = DiscountMode::PerPiece) noexcept {
  if (!nnSupportsDasBuckets(net, numBuckets)
      || nextPiece < 0
      || nextPiece >= NUM_PIECES
      || startBucket < 0
      || startBucket >= numBuckets) {
    return 0.0;
  }
  const Board20 b2 = Board20::fromRows(afterRows);
  const StepDiscountTable20 disc = StepDiscountTable20::make(gamma, discountMode);
  DasMoveList pl2;
  if (topK <= 0) {
    return bestReplyNn2PlyRt20(b2, nextPiece, startBucket, numBuckets, tables, disc, net, mode,
                               pl2);
  }
  const int repDas = dasBucketRepresentativeCharge(startBucket, numBuckets);
  tables.searchMoves(b2, nextPiece, DasStartState::spawn(nextPiece, repDas), pl2);
  if (pl2.count == 0) {
    return 0.0;
  }

  struct Ply2RtCand {
    double sc;
    int idx;
    uint8_t lc;
    uint8_t bucketAfter;
    uint16_t rows[BOARD_ROWS];
  };
  Ply2RtCand cands[DAS_MAX_LOCK_MOVES];
  for (int i = 0; i < pl2.count; ++i) {
    const DasLockMove &p = pl2.data[i];
    cands[i].idx = i;
    cands[i].lc = applyPlacement20(b2, nextPiece, p.rot, p.x, p.y, cands[i].rows);
    const int k2 = dasChargeToBucket(p.dasChargeAfter, numBuckets);
    cands[i].bucketAfter = static_cast<uint8_t>(k2);
    cands[i].sc =
        REWARD_NORM_TETRIS[cands[i].lc]
        + disc.f[cands[i].lc]
              * evalNnNextPieceExpectedRt(net, cands[i].rows, mode, nextPiece, k2, numBuckets);
  }
  const int keep = std::min(pl2.count, topK);
  if (keep < pl2.count) {
    std::partial_sort(cands, cands + keep, cands + pl2.count,
                      [](const Ply2RtCand &a, const Ply2RtCand &b) {
                        return (a.sc != b.sc) ? (a.sc > b.sc) : (a.idx < b.idx);
                      });
  }

  DasMoveList pl3;
  double best2 = -std::numeric_limits<double>::infinity();
  for (int k = 0; k < keep; ++k) {
    const Board20 bCand = Board20::fromRows(cands[k].rows);
    const int k2 = static_cast<int>(cands[k].bucketAfter);
    double expPly3 = 0.0;
    for (int p3 = 0; p3 < NUM_PIECES; ++p3) {
      const double b3 =
          bestReplyNn2PlyRt20(bCand, p3, k2, numBuckets, tables, disc, net, mode, pl3);
      expPly3 += pieceTransitionProb(mode, nextPiece, p3) * b3;
    }
    const double sc2 = REWARD_NORM_TETRIS[cands[k].lc] + disc.f[cands[k].lc] * expPly3;
    if (sc2 > best2) {
      best2 = sc2;
    }
  }
  return best2;
}

// 2-ply Hybrid OUT -> TB2 / NN evaluation for `nextPiece` on `b20` across all starting DAS buckets
// set in `bucketMask`. Evaluates highest bucket first and caches per-ply-2-cell (rot2, x2, y2)
// transitions, TB lookups, and NN expectations so shared ply-2 cells across buckets are evaluated
// at most once.
template <typename RtTbType>
inline void bestReplyHybrid2PlyRtMultiBucket20(
    const Board20 &b20, int nextPiece, uint8_t bucketMask, int numBuckets,
    const DasRtMoveTables &tables, const StepDiscountTable20 &disc, const RtTbType *rtTb,
    const TinyDasNet *nn, double safetyMargin, RngMode mode, DasMoveList &pl, bool &alreadyGrounded,
    Ply2HybridEval20 outByBucket[MAX_DAS_BUCKETS]) noexcept {
  for (int kb = 0; kb < numBuckets; ++kb) {
    outByBucket[kb] = Ply2HybridEval20{};
  }
  if (nextPiece < 0
      || nextPiece >= NUM_PIECES
      || !isValidDasBucketCount(numBuckets)
      || bucketMask == 0U) {
    return;
  }

  const bool hasTb = (rtTb != nullptr && rtTb->isLoaded());
  const bool hasNn = (nn != nullptr && nnSupportsDasBuckets(*nn, numBuckets));
  const bool finiteMargin = std::isfinite(safetyMargin);
  const double clampedMargin = finiteMargin ? std::max(0.0, safetyMargin) : 0.0;
  const double tieEps = 1e-12;
  const bool multiBucket = (bucketMask & (bucketMask - 1U)) != 0U;

  struct CachedPly2Cell {
    uint8_t lc = 0;
    uint8_t hitTb = 0;
    uint8_t nnFilledMask = 0;
    double qTb[MAX_DAS_BUCKETS] = {};
    double qGroundedNn[MAX_DAS_BUCKETS] = {};
    double qAllOutNn[MAX_DAS_BUCKETS] = {};
  };
  int16_t slotOf[rt_detail::LockMoveTable::TOTAL];
  if (multiBucket) {
    std::fill(std::begin(slotOf), std::end(slotOf), static_cast<int16_t>(-1));
  }
  CachedPly2Cell cache[DAS_MAX_LOCK_MOVES];
  int numCached = 0;

  uint16_t after2[BOARD_ROWS];
  float tbRow[MAX_RT_VALUES_PER_BOARD];

  // Iterate from highest bucket down to lowest so full DAS (most likely to hit TB2) runs first.
  for (int kb = numBuckets - 1; kb >= 0; --kb) {
    if (((bucketMask >> kb) & 1U) == 0U) {
      continue;
    }
    Ply2HybridEval20 &res = outByBucket[kb];
    const int repDas = dasBucketRepresentativeCharge(kb, numBuckets);
    tables.searchMoves(b20, nextPiece, DasStartState::spawn(nextPiece, repDas), pl);
    if (pl.count == 0) {
      continue;
    }
    res.hasMove = true;
    const bool needOutNn = hasNn && (!alreadyGrounded || finiteMargin);

    double bestGrounded = -std::numeric_limits<double>::infinity();
    double bestAllOut = -std::numeric_limits<double>::infinity();
    double bestTbOnly = -std::numeric_limits<double>::infinity();
    bool bestGroundedTb = false;
    CompactPlacement bestGroundedMove{};
    CompactPlacement bestAllOutMove{};
    uint8_t bestGroundedLc = 0;
    uint8_t bestAllOutLc = 0;

    for (int i = 0; i < pl.count; ++i) {
      const DasLockMove &p = pl.data[i];
      const CompactPlacement cp{p.x, p.y, p.rot, '-'};
      const int k2 = dasChargeToBucket(p.dasChargeAfter, numBuckets);

      uint8_t lc2 = 0;
      bool hitTb = false;
      double qGrounded = 0.0;
      double qAllOut = 0.0;

      const int cellIdx = multiBucket ? rt_detail::LockMoveTable::cellIndex(p.rot, p.x, p.y) : -1;
      int16_t slot = (multiBucket && cellIdx >= 0) ? slotOf[cellIdx] : -1;
      if (slot >= 0) {
        CachedPly2Cell &ce = cache[slot];
        lc2 = ce.lc;
        hitTb = (ce.hitTb != 0U);
        if (hitTb) {
          qGrounded = ce.qTb[k2];
          qAllOut = qGrounded;
        } else {
          const double rew2 = REWARD_NORM_TETRIS[lc2];
          qGrounded = rew2;
          qAllOut = rew2;
          if (needOutNn) {
            if (((ce.nnFilledMask >> k2) & 1U) == 0U) {
              applyPlacement20(b20, nextPiece, p.rot, p.x, p.y, after2);
              const double dFac2 = disc.f[lc2];
              const double evNn =
                  evalNnNextPieceExpectedRt(*nn, after2, mode, nextPiece, k2, numBuckets);
              ce.qAllOutNn[k2] = rew2 + dFac2 * evNn;
              ce.qGroundedNn[k2] =
                  finiteMargin ? (rew2 + dFac2 * std::max(0.0, evNn - clampedMargin)) : rew2;
              ce.nnFilledMask = static_cast<uint8_t>(ce.nnFilledMask | (1U << k2));
            }
            qAllOut = ce.qAllOutNn[k2];
            qGrounded = ce.qGroundedNn[k2];
          }
        }
      } else {
        lc2 = applyPlacement20(b20, nextPiece, p.rot, p.x, p.y, after2);
        const double rew2 = REWARD_NORM_TETRIS[lc2];
        const double dFac2 = disc.f[lc2];
        const int sr =
            (lc2 == 0)
                ? std::max(0, std::min(static_cast<int>(b20.topNonEmptyRow), static_cast<int>(p.y)))
                : 0;
        BoardKey key2{};
        qGrounded = rew2;
        qAllOut = rew2;

        if (hasTb && encodeBoardKey(after2, key2, sr) && rtTb->lookup(key2, tbRow)) {
          hitTb = true;
          if (multiBucket && cellIdx >= 0 && numCached < DAS_MAX_LOCK_MOVES) {
            slot = static_cast<int16_t>(numCached++);
            slotOf[cellIdx] = slot;
            CachedPly2Cell &ce = cache[slot];
            ce.lc = lc2;
            ce.hitTb = 1;
            for (int kb2 = 0; kb2 < numBuckets; ++kb2) {
              double ev = 0.0;
              for (int p3 = 0; p3 < NUM_PIECES; ++p3) {
                ev += pieceTransitionProb(mode, nextPiece, p3)
                      * static_cast<double>(tbRow[rtValueIndex(p3, kb2, numBuckets)]);
              }
              ce.qTb[kb2] = rew2 + dFac2 * ev;
            }
            qGrounded = ce.qTb[k2];
            qAllOut = qGrounded;
          } else {
            double evTb = 0.0;
            for (int p3 = 0; p3 < NUM_PIECES; ++p3) {
              evTb += pieceTransitionProb(mode, nextPiece, p3)
                      * static_cast<double>(tbRow[rtValueIndex(p3, k2, numBuckets)]);
            }
            qGrounded = rew2 + dFac2 * evTb;
            qAllOut = qGrounded;
          }
        } else {
          if (needOutNn) {
            const double evNn =
                evalNnNextPieceExpectedRt(*nn, after2, mode, nextPiece, k2, numBuckets);
            qAllOut = rew2 + dFac2 * evNn;
            qGrounded = finiteMargin ? (rew2 + dFac2 * std::max(0.0, evNn - clampedMargin)) : rew2;
          }
          if (multiBucket && cellIdx >= 0 && numCached < DAS_MAX_LOCK_MOVES) {
            slot = static_cast<int16_t>(numCached++);
            slotOf[cellIdx] = slot;
            CachedPly2Cell &ce = cache[slot];
            ce.lc = lc2;
            ce.hitTb = 0;
            if (needOutNn) {
              ce.nnFilledMask = static_cast<uint8_t>(1U << k2);
              ce.qAllOutNn[k2] = qAllOut;
              ce.qGroundedNn[k2] = qGrounded;
            }
          }
        }
      }

      if (hitTb) {
        res.anyInTb = true;
        if (qGrounded > bestTbOnly) {
          bestTbOnly = qGrounded;
        }
      }
      if (i == 0 || qGrounded > bestGrounded + tieEps) {
        bestGrounded = qGrounded;
        bestGroundedTb = hitTb;
        bestGroundedMove = cp;
        bestGroundedLc = lc2;
      } else if (std::abs(qGrounded - bestGrounded) <= tieEps && hitTb && !bestGroundedTb) {
        bestGrounded = qGrounded;
        bestGroundedTb = true;
        bestGroundedMove = cp;
        bestGroundedLc = lc2;
      }
      if (i == 0 || qAllOut > bestAllOut) {
        bestAllOut = qAllOut;
        bestAllOutMove = cp;
        bestAllOutLc = lc2;
      }
    }

    res.bestIfTbGrounded = bestGrounded;
    res.bestIfAllOut = bestAllOut;
    res.bestTbOnly = bestTbOnly;
    res.bestGroundedFromTb = bestGroundedTb;
    res.bestGroundedMove = bestGroundedMove;
    res.bestAllOutMove = bestAllOutMove;
    res.bestGroundedLc = bestGroundedLc;
    res.bestAllOutLc = bestAllOutLc;
    if (res.anyInTb) {
      alreadyGrounded = true;
    }
  }
}

} // namespace detail

// Deterministic (1T == MT) on-policy live-RT + DAS-bucket trajectory sampler for supervised
// multi-bucket NNUE distillation from an SRTBV004 tablebase.
template <typename RtTbType>
inline DistillDataset20 sampleDistillDatasetRt20(const RtTbType &tb, const DasRtMoveTables &tables,
                                                 const TinyDasNet *guideNn,
                                                 const DistillSampleConfig20 &cfg) {
  DistillDataset20 out{};
  if (!tb.isLoaded() || cfg.numGames <= 0 || cfg.maxDecisions <= 0) {
    return out;
  }
  const int K = tb.numDasBuckets();
  const int vpb = NUM_PIECES * K;
  const RngMode rngMode = tb.rngMode();
  const StepDiscountTable20 disc = StepDiscountTable20::make(tb.gamma(), tb.discountMode());
  const auto transProb = detail::buildTransitionProbMatrix(rngMode);
  const int threads = std::max(1, cfg.numThreads);
  const int dangerMod = std::clamp(static_cast<int>(std::lround(cfg.dangerFrac * 100.0)), 0, 100);
  const int recoverMod =
      std::clamp(static_cast<int>(std::lround(cfg.recoverFrac * 100.0)), 0, 100 - dangerMod);
  const int chunkGames = std::max(32, threads * 4);
  const bool hasGuide = (guideNn != nullptr && nnSupportsDasBuckets(*guideNn, K));

  out.decisions.reserve(static_cast<size_t>(cfg.maxDecisions));
  std::vector<std::vector<LabeledDecision20>> perGame(static_cast<size_t>(chunkGames));
  std::vector<uint64_t> perSteps(static_cast<size_t>(chunkGames), 0);
  std::vector<uint64_t> perSkipped(static_cast<size_t>(chunkGames), 0);
  detail::PersistentWorkerPool pool(threads);

  for (int baseG = 0;
       baseG < cfg.numGames && static_cast<int>(out.decisions.size()) < cfg.maxDecisions;
       baseG += chunkGames) {
    const int batchCount = std::min(chunkGames, cfg.numGames - baseG);
    for (int i = 0; i < batchCount; ++i) {
      perGame[static_cast<size_t>(i)].clear();
      perSteps[static_cast<size_t>(i)] = 0;
      perSkipped[static_cast<size_t>(i)] = 0;
    }

    std::atomic<int> nextLocal{0};
    pool.run([&](int /*tid*/) {
      auto ws = std::make_unique<RtDecisionWorkspace>();
      std::vector<std::array<uint16_t, BOARD_ROWS>> cellRows(RtDecisionWorkspace::MAX_CELLS);
      std::vector<int> bestBucketForCell(RtDecisionWorkspace::MAX_CELLS, 0);
      std::vector<uint8_t> cellIsGuidedOut(RtDecisionWorkspace::MAX_CELLS, 0);
      std::vector<double> inTbQ(hasGuide ? static_cast<size_t>(RtDecisionWorkspace::MAX_CELLS)
                                               * MAX_RT_VALUES_PER_BOARD
                                         : 0U,
                                0.0);
      std::vector<double> rawNnQ(hasGuide ? static_cast<size_t>(RtDecisionWorkspace::MAX_CELLS)
                                                * MAX_RT_VALUES_PER_BOARD
                                          : 0U,
                                 0.0);
      std::vector<LabeledMoveCand20> candsBuf;
      candsBuf.reserve(RtDecisionWorkspace::MAX_CELLS);
      std::vector<int> order(RtDecisionWorkspace::MAX_CELLS);
      float parentRow[MAX_RT_VALUES_PER_BOARD];
      float childRow[MAX_RT_VALUES_PER_BOARD];
      float nnRow[MAX_RT_VALUES_PER_BOARD];

      while (true) {
        const int local = nextLocal.fetch_add(1, std::memory_order_relaxed);
        if (local >= batchCount)
          break;
        const int g = baseG + local;
        const int gMod = static_cast<int>((static_cast<uint64_t>(g) * 37ULL) % 100ULL);
        const bool isDanger = (gMod < dangerMod);
        const bool isRecover = (!isDanger && gMod < dangerMod + recoverMod);
        const double eps = isDanger ? cfg.dangerEpsilon : (isRecover ? cfg.recoverEpsilon : 0.0);

        Rng rng(cfg.baseSeed + static_cast<uint64_t>(g) * 1009ULL);
        int curP = rng.sampleOpeningPiece(rngMode);
        int nextP = rng.sampleNextPiece(rngMode, curP);
        int bucket = K - 1;
        Board20 board = Board20::empty();
        auto &gameVec = perGame[static_cast<size_t>(local)];
        uint64_t steps = 0;
        uint64_t skipped = 0;

        for (int step = 0; step < cfg.maxPiecesPerGame; ++step) {
          const int repDas = dasBucketRepresentativeCharge(bucket, K);
          ws->search(tables, board, curP, &repDas, 1);
          const int nCells = ws->numCells();
          if (nCells == 0)
            break;

          // Record the best ending DAS bucket reaching each unique lock cell.
          const DasRtSearchResult &sr = ws->result(0);
          for (int i = 0; i < nCells; ++i) {
            bestBucketForCell[static_cast<size_t>(i)] = 0;
          }
          for (int e = 0; e < sr.numEarlyMoves; ++e) {
            const int cl = ws->localCellOf(0, sr.earlyMoves[e].cellIdx);
            const int kb = dasChargeToBucket(sr.earlyMoves[e].dasAfter, K);
            if (kb > bestBucketForCell[static_cast<size_t>(cl)]) {
              bestBucketForCell[static_cast<size_t>(cl)] = kb;
            }
          }
          const int totalAdjEdges = sr.adjMoveOffset[sr.numAdjStates];
          for (int e = 0; e < totalAdjEdges; ++e) {
            const int cl = ws->localCellOf(0, sr.adjEdges[e].cellIdx);
            const int kb = dasChargeToBucket(sr.adjEdges[e].dasAfter, K);
            if (kb > bestBucketForCell[static_cast<size_t>(cl)]) {
              bestBucketForCell[static_cast<size_t>(cl)] = kb;
            }
          }

          double bestQ = -std::numeric_limits<double>::infinity();
          double minQ = std::numeric_limits<double>::infinity();
          bool hasTerminalCand = false;
          bool anyInTb = false;
          candsBuf.clear();

          for (int i = 0; i < nCells; ++i) {
            auto &afterRows = cellRows[static_cast<size_t>(i)];
            const uint8_t lc = ws->computeAfterRows(board, curP, i, afterRows.data());
            const double rew = REWARD_NORM_TETRIS[lc];
            const double dFac = disc.f[lc];
            const int startRow = (lc == 0)
                                     ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                            static_cast<int>(ws->cell(i).y)))
                                     : 0;
            BoardKey afterKey;
            const bool fitsKey = encodeBoardKey(afterRows.data(), afterKey, startRow);
            const bool inTb = fitsKey && tb.lookup(afterKey, childRow);
            const int kb = bestBucketForCell[static_cast<size_t>(i)];
            const int outIdx = rtValueIndex(nextP, kb, K);
            double *qc = ws->q(i);
            cellIsGuidedOut[static_cast<size_t>(i)] = 0;

            if (inTb) {
              anyInTb = true;
              for (int o = 0; o < vpb; ++o) {
                qc[o] = rew + dFac * static_cast<double>(childRow[o]);
              }
              const double targetVal = static_cast<double>(childRow[outIdx]);
              const double qNext = rew + dFac * targetVal;
              LabeledMoveCand20 cand{};
              cand.afterRows = afterRows;
              cand.linesCleared = lc;
              cand.outIdx = static_cast<uint8_t>(outIdx);
              cand.qStar = qNext;
              cand.childValTarget = targetVal;
              candsBuf.push_back(cand);
              if (qNext > bestQ)
                bestQ = qNext;
              if (qNext < minQ)
                minQ = qNext;
            } else {
              for (int o = 0; o < vpb; ++o) {
                qc[o] = rew;
              }
              const bool topClear =
                  ((afterRows[0] | afterRows[1] | afterRows[2]) & FULL_ROW_MASK) == 0U;
              const Board20 bAfter =
                  topClear ? Board20::empty() : Board20::fromRows(afterRows.data());
              const bool nextCanPlay =
                  topClear || !collides20(bAfter, nextP, SPAWN_COL, PIECES_20[nextP].initialY, 0);
              if (!nextCanPlay && cfg.includeTerminalOutMoves) {
                hasTerminalCand = true;
                LabeledMoveCand20 cand{};
                cand.afterRows = afterRows;
                cand.linesCleared = lc;
                cand.outIdx = static_cast<uint8_t>(outIdx);
                cand.qStar = rew;
                cand.childValTarget = 0.0;
                candsBuf.push_back(cand);
                if (rew > bestQ)
                  bestQ = rew;
                if (rew < minQ)
                  minQ = rew;
              }
              if (hasGuide && fitsKey) {
                cellIsGuidedOut[static_cast<size_t>(i)] = 1;
                double *inDst = &inTbQ[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD];
                double *rawDst = &rawNnQ[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD];
                for (int o = 0; o < vpb; ++o) {
                  inDst[o] = rew;
                  rawDst[o] = rew;
                }
                evalNnRtRow(*guideNn, afterRows.data(), K, nnRow);
                for (int n = 0; n < NUM_PIECES; ++n) {
                  const bool canSpawn =
                      (n == nextP)
                          ? nextCanPlay
                          : (topClear
                             || !collides20(bAfter, n, SPAWN_COL, PIECES_20[n].initialY, 0));
                  if (!canSpawn)
                    continue;
                  for (int k = 0; k < K; ++k) {
                    const int o = rtValueIndex(n, k, K);
                    const double est = std::max(0.0, static_cast<double>(nnRow[o]));
                    rawDst[o] = rew + dFac * est;
                    inDst[o] = rew + dFac * std::max(0.0, est - cfg.rolloutMargin);
                  }
                }
              }
            }
          }

          if (hasGuide) {
            for (int i = 0; i < nCells; ++i) {
              if (cellIsGuidedOut[static_cast<size_t>(i)] == 0U)
                continue;
              const double *src = anyInTb
                                      ? &inTbQ[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD]
                                      : &rawNnQ[static_cast<size_t>(i) * MAX_RT_VALUES_PER_BOARD];
              std::memcpy(ws->q(i), src, static_cast<size_t>(vpb) * sizeof(double));
            }
          }

          const bool hasRank = (candsBuf.size() >= 2 && bestQ > minQ + 1e-9);
          if (!hasRank && !hasTerminalCand) {
            skipped++;
          } else {
            LabeledDecision20 sample{};
            std::memcpy(sample.rows.data(), board.rows(), sizeof(uint16_t) * BOARD_ROWS);
            sample.curPiece = curP;
            sample.nextPiece = nextP;
            sample.hasRankingSignal = hasRank;
            sample.vStarDim = static_cast<uint8_t>(vpb);
            BoardKey parentKey;
            if (encodeBoardKey(board.rows(), parentKey, board.topNonEmptyRow)
                && tb.lookup(parentKey, parentRow)) {
              sample.boardInTb = true;
              for (int o = 0; o < vpb; ++o) {
                sample.vStar[static_cast<size_t>(o)] = parentRow[o];
              }
            }
            sample.bestQStar = bestQ;
            sample.moves.assign(candsBuf.begin(), candsBuf.end());
            gameVec.push_back(std::move(sample));
          }

          // Advance rollout state (board, curP, nextP, bucket).
          int pickCell = 0;
          int pickBucket = bestBucketForCell[0];
          if (eps > 0.0 && nCells > 1 && rng.nextUniform01() < eps) {
            if (isDanger) {
              pickCell = rng.nextInt(nCells);
              pickBucket = bestBucketForCell[static_cast<size_t>(pickCell)];
            } else {
              for (int i = 0; i < nCells; ++i)
                order[static_cast<size_t>(i)] = i;
              const int hi = std::min(nCells, std::max(2, cfg.exploreTopK));
              std::partial_sort(
                  order.begin(), order.begin() + hi, order.begin() + nCells, [&](int a, int b) {
                    const double sa =
                        ws->q(a)[rtValueIndex(nextP, bestBucketForCell[static_cast<size_t>(a)], K)];
                    const double sb =
                        ws->q(b)[rtValueIndex(nextP, bestBucketForCell[static_cast<size_t>(b)], K)];
                    return (sa != sb) ? (sa > sb) : (a < b);
                  });
              pickCell = order[static_cast<size_t>(1 + rng.nextInt(hi - 1))];
              pickBucket = bestBucketForCell[static_cast<size_t>(pickCell)];
            }
          } else {
            const RtDecisionWorkspace::Choice ch =
                ws->chooseAction(0, K, transProb[static_cast<size_t>(curP)]);
            if (!ch.any)
              break;
            pickCell = ch.cellForNext[nextP];
            pickBucket = ch.bucketForNext[nextP];
          }
          board = Board20::fromRows(cellRows[static_cast<size_t>(pickCell)].data());
          bucket = pickBucket;
          steps++;
          curP = nextP;
          nextP = rng.sampleNextPiece(rngMode, curP);
        }

        perSteps[static_cast<size_t>(local)] = steps;
        perSkipped[static_cast<size_t>(local)] = skipped;
      }
    });

    for (int local = 0; local < batchCount; ++local) {
      out.totalSteps += perSteps[static_cast<size_t>(local)];
      out.skippedIncomplete += perSkipped[static_cast<size_t>(local)];
      for (auto &s : perGame[static_cast<size_t>(local)]) {
        if (static_cast<int>(out.decisions.size()) >= cfg.maxDecisions)
          break;
        out.decisions.push_back(std::move(s));
      }
      if (static_cast<int>(out.decisions.size()) >= cfg.maxDecisions)
        break;
    }
  }
  return out;
}

} // namespace das

#endif // DAS_TABLEBASE_RT_NN_BOUNDARY_20ROW_HPP
