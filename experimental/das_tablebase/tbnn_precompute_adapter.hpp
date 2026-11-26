#ifndef DAS_TABLEBASE_TBNN_PRECOMPUTE_ADAPTER_HPP
#define DAS_TABLEBASE_TBNN_PRECOMPUTE_ADAPTER_HPP

// Configurable reaction-time + fullMoveSearch + DAS-penalty precompute harness powered by
// Hybrid TB + NNUE (read-only wrapper over src/backend/full_move_search.cpp and
// src/backend/precompute.cpp's analyzePrecomputeCollapse).

#include "eval_adapter.hpp"
#include "nnue_20row.hpp"
#include "rt_nn_boundary_20row.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace das {

// Scales normalized Tetris-equivalent values into PrecomputeState so precompute.cpp's
// -0.1 / -0.2 / -0.3 adjustment input costs act strictly as tiny human-friendly tie-breakers
// (0.0001 Tetris per extra input) rather than distorting Tetris/line-clear decisions.
inline constexpr double TBNN_PRECOMPUTE_SCALE = 1000.0;

struct TbNnPrecomputeOptions {
  int dasCharge = DAS_CHARGE_MAX; // -1 = tap mode, 0..16 = DAS mode
  int reactionTime = 18;          // 0..60 frames
  double gamma = 0.995;
  DiscountMode discountMode = DiscountMode::PerPiece;
  double outMargin = 0.25;
  NnSearchConfig20 outCfg = NnSearchConfig20::ply2();
  RngMode rngMode = RngMode::Nes;
  AggressionLevel aggro = AGGRO;
  bool stationaryLevel = true;
  bool applyDasPenalties = true;
};

struct TbNnPrecomputeResult {
  bool hasMove = false;
  int rot = 0;
  int x = 0;
  int y = 0;
  int dasChargeAfter = DAS_CHARGE_MAX;
  int linesCleared = 0;
  bool chosenInTb = false;
  bool usedAllOutFallback = false;
  // True if 3-ply NN search ran for any evaluated next-piece branch (all 7 when rt > 0, or
  // nextPieceIdx alone when rt == 0); usedAllOutFallback is scoped strictly to nextPieceIdx.
  bool usedPly3 = false;
  GameState afterState = {};
};

namespace detail {

inline uint32_t packLockCellKey(int rot, int x, int y) noexcept {
  return (static_cast<uint32_t>(rot & 3) << 16)
         | (static_cast<uint32_t>((x + 16) & 0xFF) << 8)
         | static_cast<uint32_t>((y + 16) & 0xFF);
}

inline int numOrientations20(const Piece *piece) noexcept {
  for (int r = 0; r < 4; ++r) {
    if (piece->rowsByRotation[r][0] == NONE) {
      return r;
    }
  }
  return 4;
}

inline int countInputsRel(int relRot, int relX) noexcept {
  return (relRot == 3) ? (1 + std::abs(relX)) : (relRot + std::abs(relX));
}

inline std::string encodeLockCellStr(int rot, int x, int y) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%d|%d|%d", rot, x, y);
  return std::string(buf);
}

inline PrecomputePossibility makePrecomputePossibility(const FullMovePossibility &poss,
                                                       int existingRot, int initialX, int initialY,
                                                       int numOrientations) {
  PrecomputePossibility out;
  out.rot = poss.rotationIndex;
  out.x = poss.x;
  out.y = poss.y;
  out.relRot = (poss.rotationIndex - existingRot + numOrientations) % numOrientations;
  out.relX = poss.x - initialX;
  out.relY = poss.y - initialY;
  out.inputSequence = poss.inputSequence;
  out.tuckInput = poss.tuckInput;
  out.dasChargeAfter = poss.dasChargeAfter;
  out.numLinesCleared = poss.numLinesCleared;
  return out;
}

inline int reactionSliceIdxLocal(const std::string &inputSeq, int reactionTime) {
  const bool holdsThroughAre = inputSeq.find('_') != std::string::npos;
  return holdsThroughAre ? reactionTime + 1 : reactionTime;
}

struct CachedSecondMoveScore {
  uint8_t inTb = 0;
  double inScore = 0.0;
  double rawScore = 0.0;
};

// Generation-stamped flat lookup table indexed by (rot, x, y) for O(1) zero-allocation
// per-(afterBoard, nextPiece) caching across DAS buckets (15, 10, 5).
struct FlatSecondMoveCache {
  static constexpr int ROT_COUNT = 4;
  static constexpr int X_OFFSET = 4;
  static constexpr int X_COUNT = 18; // covers x in [-4 .. 13]
  static constexpr int Y_OFFSET = 4;
  static constexpr int Y_COUNT = 26; // covers y in [-4 .. 21]
  static constexpr int SIZE = ROT_COUNT * X_COUNT * Y_COUNT;

  struct Entry {
    uint32_t gen = 0;
    CachedSecondMoveScore score{};
  };
  Entry entries[SIZE] = {};
  uint32_t curGen = 1;

  void clear() noexcept {
    if (++curGen == 0) {
      for (int i = 0; i < SIZE; ++i)
        entries[i].gen = 0;
      curGen = 1;
    }
  }

  static int index(int rot, int x, int y) noexcept {
    const int xi = x + X_OFFSET;
    const int yi = y + Y_OFFSET;
    if (xi < 0 || xi >= X_COUNT || yi < 0 || yi >= Y_COUNT)
      return -1;
    return ((rot & 3) * X_COUNT + xi) * Y_COUNT + yi;
  }
};

// Evaluates best 2nd-piece continuation under a prescribed starting DAS charge (15, 10, or 5)
// using fast 1-ply TB + NNUE expectation over the 3rd piece, with zero-allocation flat caching
// across DAS buckets. When `forceGrounded` is true (because full DAS had an in-TB move), partial
// and min DAS stay in the margined regime so losing in-TB reach never underestimates DAS loss.
template <typename TbType>
inline double
bestSecondPieceScoreForDas(const GameState &after1, const Board20 &bAfter1, int nextPieceIdx,
                           int dasBucket, const StepDiscountTable20 &disc, const TbType *tb,
                           const TinyDasNet *nn, double outMargin, RngMode rngMode,
                           std::vector<FullMovePossibility> &buf, FlatSecondMoveCache &cache,
                           bool forceGrounded = false, bool *outAnyInTb = nullptr) {
  if (outAnyInTb != nullptr) {
    *outAnyInTb = false;
  }
  buf.clear();
  fullMoveSearch(after1, &PIECE_LIST[nextPieceIdx], TIMELINE_10HZ_STR, dasBucket,
                 /* trackInputSequences= */ false, buf);
  if (buf.empty()) {
    return 0.0;
  }

  const bool hasTb = (tb != nullptr && tb->isLoaded());
  const bool hasNn = (nn != nullptr && nn->isLoaded());
  const bool finiteMargin = std::isfinite(outMargin);
  const double clampedMargin = finiteMargin ? std::max(0.0, outMargin) : 0.0;

  uint16_t after2Rows[BOARD_ROWS];
  float tbRow[NUM_PIECES];
  bool anyInTb = false;
  double bestIn = -std::numeric_limits<double>::infinity();
  double bestRaw = -std::numeric_limits<double>::infinity();

  for (const FullMovePossibility &m2 : buf) {
    const int slot = FlatSecondMoveCache::index(m2.rotationIndex, m2.x, m2.y);
    CachedSecondMoveScore cs{};
    if (slot >= 0 && cache.entries[slot].gen == cache.curGen) {
      cs = cache.entries[slot].score;
    } else {
      const uint8_t lc2 =
          applyPlacement20(bAfter1, nextPieceIdx, m2.rotationIndex, m2.x, m2.y, after2Rows);
      const double rew2 = REWARD_NORM_TETRIS[lc2];
      const double dFac2 = disc.f[lc2];
      const int startRow =
          (lc2 == 0) ? std::max(0, std::min(static_cast<int>(bAfter1.topNonEmptyRow), m2.y)) : 0;
      BoardKey key2;
      const bool fitsKey = encodeBoardKey(after2Rows, key2, startRow);

      if (hasTb && fitsKey && tb->lookup(key2, tbRow)) {
        double ev = 0.0;
        for (int p3 = 0; p3 < NUM_PIECES; ++p3) {
          ev += pieceTransitionProb(rngMode, nextPieceIdx, p3) * static_cast<double>(tbRow[p3]);
        }
        cs.inTb = 1;
        cs.inScore = rew2 + dFac2 * ev;
        cs.rawScore = cs.inScore;
      } else {
        const double est =
            hasNn ? nn->evalNextPieceExpected(after2Rows, rngMode, nextPieceIdx) : 0.0;
        cs.inTb = 0;
        cs.rawScore = rew2 + dFac2 * est;
        // When margin is +inf, out-of-TB moves receive zero continuation if any in-TB move exists.
        cs.inScore =
            (hasNn && finiteMargin) ? (rew2 + dFac2 * std::max(0.0, est - clampedMargin)) : rew2;
      }
      if (slot >= 0) {
        cache.entries[slot].gen = cache.curGen;
        cache.entries[slot].score = cs;
      }
    }
    if (cs.inTb)
      anyInTb = true;
    if (cs.inScore > bestIn)
      bestIn = cs.inScore;
    if (cs.rawScore > bestRaw)
      bestRaw = cs.rawScore;
  }

  if (outAnyInTb != nullptr) {
    *outAnyInTb = anyInTb;
  }
  return (anyInTb || forceGrounded) ? bestIn : bestRaw;
}

} // namespace detail

// Runs fullMoveSearch + mid-air adjustment search (at opts.reactionTime) + exact DAS-charge
// penalty buckets + StackRabbit's analyzePrecomputeCollapse using Hybrid TB + NNUE as leaf eval.
template <typename TbType>
inline TbNnPrecomputeResult
chooseTbNnPrecomputeMove(const GameState &state, int curPieceIdx, int nextPieceIdx,
                         const TbType *tb, const TinyDasNet *nn, const TbNnPrecomputeOptions &opts,
                         const BackendContexts &bc) {
  TbNnPrecomputeResult res{};
  const Piece *curPiece = &PIECE_LIST[curPieceIdx];
  const int numOrientations = detail::numOrientations20(curPiece);

  // Step 1: fullMoveSearch for current piece with real timeline, DAS charge, and reactionTime.
  std::vector<FullMovePossibility> initialResults;
  std::vector<AdjTimeSimStateSlot> captures;
  fullMoveSearch(state, curPiece, TIMELINE_10HZ_STR, opts.dasCharge,
                 /* trackInputSequences= */ true, initialResults, FullMoveSearchContext(),
                 opts.reactionTime, &captures);
  if (initialResults.empty()) {
    return res;
  }

  PrecomputeState pState;
  pState.reactionTime = opts.reactionTime;
  pState.useDAS = (opts.dasCharge != -1);

  // Step 2: construct phantom list matching src/backend/precompute.cpp exactly.
  if (opts.reactionTime == 0) {
    PrecomputePhantom phantom;
    phantom.hasAdjustmentState = true;
    phantom.adjustmentCtx = FullMoveSearchContext();
    phantom.adjustmentDasCharge = opts.dasCharge;
    pState.phantoms.push_back(std::move(phantom));
  } else {
    std::vector<PrecomputePossibility> initialPlacements;
    initialPlacements.reserve(initialResults.size());
    for (const FullMovePossibility &r : initialResults) {
      initialPlacements.push_back(
          detail::makePrecomputePossibility(r, 0, INITIAL_X, curPiece->initialY, numOrientations));
    }
    std::vector<size_t> order(initialPlacements.size());
    for (size_t i = 0; i < order.size(); ++i)
      order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return detail::countInputsRel(initialPlacements[a].relRot, initialPlacements[a].relX)
             < detail::countInputsRel(initialPlacements[b].relRot, initialPlacements[b].relX);
    });
    pState.phantoms.reserve(order.size());
    for (size_t idx : order) {
      const std::string &seq = initialPlacements[idx].inputSequence;
      PrecomputePhantom phantom;
      const int sliceIdx = std::min<int>(detail::reactionSliceIdxLocal(seq, opts.reactionTime),
                                         static_cast<int>(seq.size()));
      phantom.inputSequence = seq.substr(0, static_cast<size_t>(sliceIdx));
      phantom.hasInitialPlacement = true;
      phantom.initial = initialPlacements[idx];
      const AdjTimeSimStateSlot &slot = captures[idx];
      phantom.hasAdjustmentState = slot.hasCapture;
      if (slot.hasCapture) {
        const AdjTimeSimState &cap = slot.capture;
        phantom.adjustmentCtx.existingXOffset = cap.x - INITIAL_X;
        phantom.adjustmentCtx.existingYOffset = cap.y - curPiece->initialY;
        phantom.adjustmentCtx.framesAlreadyElapsed = cap.frameIndex;
        phantom.adjustmentCtx.existingRotation = cap.rotationIndex;
        phantom.adjustmentCtx.isAdjustment = true;
        phantom.adjustmentCtx.isArrContinued = cap.isArrContinued;
        phantom.adjustmentCtx.dasButtonHeld = cap.dasButtonHeld;
        phantom.adjustmentCtx.preAdjInputSequence = cap.preAdjInputSequence;
        phantom.adjustmentDasCharge = cap.dasCharge;
      }
      pState.phantoms.push_back(std::move(phantom));
    }
  }

  // Step 3: mid-air adjustment search for each phantom that captured an adjustment state.
  std::vector<FullMovePossibility> adjResults;
  for (PrecomputePhantom &phantom : pState.phantoms) {
    if (!phantom.hasAdjustmentState)
      continue;
    adjResults.clear();
    fullMoveSearch(state, curPiece, TIMELINE_10HZ_STR, phantom.adjustmentDasCharge,
                   /* trackInputSequences= */ true, adjResults, phantom.adjustmentCtx,
                   /* reactionTime= */ -1, /* adjTimeCaptures= */ nullptr);
    phantom.possibleAdjustments.reserve(adjResults.size());
    for (const FullMovePossibility &r : adjResults) {
      phantom.possibleAdjustments.push_back(detail::makePrecomputePossibility(
          r, phantom.adjustmentCtx.existingRotation,
          INITIAL_X + phantom.adjustmentCtx.existingXOffset,
          curPiece->initialY + phantom.adjustmentCtx.existingYOffset, numOrientations));
    }
  }

  // Step 4: collect unique reachable lock cells (rot, x, y) and evaluate via Hybrid TB + NNUE.
  struct UniqueCell {
    int rot = 0;
    int x = 0;
    int y = 0;
    std::string keyStr;
    uint8_t lc = 0;
    uint16_t afterRows[BOARD_ROWS] = {};
    BoardKey afterKey = EMPTY_KEY;
    bool fitsKey = false;
    bool inTb = false;
    float tbRow[NUM_PIECES] = {};
    detail::Ply2HybridEval20 ply2[NUM_PIECES] = {};
    bool needsPartialDas = false;
    bool needsMinDas = false;
  };
  std::unordered_map<uint32_t, size_t> cellToIdx;
  std::vector<UniqueCell> cells;
  auto registerCell = [&](int rot, int x, int y, int dasAfter) {
    const uint32_t packed = detail::packLockCellKey(rot, x, y);
    auto it = cellToIdx.find(packed);
    size_t idx = 0;
    if (it == cellToIdx.end()) {
      idx = cells.size();
      cellToIdx.emplace(packed, idx);
      UniqueCell u{};
      u.rot = rot;
      u.x = x;
      u.y = y;
      u.keyStr = detail::encodeLockCellStr(rot, x, y);
      cells.push_back(std::move(u));
    } else {
      idx = it->second;
    }
    if (pState.useDAS) {
      if (dasAfter >= 10 && dasAfter < 15) {
        cells[idx].needsPartialDas = true;
      } else if (dasAfter < 10) {
        cells[idx].needsMinDas = true;
      }
    }
  };

  for (const PrecomputePhantom &ph : pState.phantoms) {
    if (ph.hasInitialPlacement) {
      registerCell(ph.initial.rot, ph.initial.x, ph.initial.y, ph.initial.dasChargeAfter);
    }
    for (const PrecomputePossibility &adj : ph.possibleAdjustments) {
      registerCell(adj.rot, adj.x, adj.y, adj.dasChargeAfter);
    }
  }

  const Board20 board20 = gameStateToBoard20(state);
  const int gravity = getGravity20(state.level);
  const bool hasTb = (tb != nullptr && tb->isLoaded());
  const bool hasNn = (nn != nullptr && nn->isLoaded());
  const bool finiteMargin = std::isfinite(opts.outMargin);
  const double clampedMargin = finiteMargin ? std::max(0.0, opts.outMargin) : 0.0;

  bool anyInTbOverall = false;
  for (UniqueCell &u : cells) {
    u.lc = applyPlacement20(board20, curPieceIdx, u.rot, u.x, u.y, u.afterRows);
    const int startRow =
        (u.lc == 0) ? std::max(0, std::min(static_cast<int>(board20.topNonEmptyRow), u.y)) : 0;
    u.fitsKey = encodeBoardKey(u.afterRows, u.afterKey, startRow);
    if (hasTb && u.fitsKey && tb->lookup(u.afterKey, u.tbRow)) {
      u.inTb = true;
      anyInTbOverall = true;
    }
  }

  const StepDiscountTable20 disc = StepDiscountTable20::make(opts.gamma, opts.discountMode);

  const bool usePly2HybridPath = opts.outCfg.usesPly2HybridPath(hasTb);
  const bool isHighRisk = opts.outCfg.isPly3HighRisk(board20);
  const bool needSelectivePly3 = usePly2HybridPath && opts.outCfg.ply >= 3 && hasNn;
  const bool needRiskPrescreen = needSelectivePly3 && isHighRisk && finiteMargin;

  bool anyTbGroundedForPiece[NUM_PIECES];
  double bestTbGroundedForPiece[NUM_PIECES];
  double bestAllOutForPiece[NUM_PIECES];
  for (int n = 0; n < NUM_PIECES; ++n) {
    anyTbGroundedForPiece[n] = anyInTbOverall;
    bestTbGroundedForPiece[n] = -std::numeric_limits<double>::infinity();
    bestAllOutForPiece[n] = -std::numeric_limits<double>::infinity();
  }
  if (usePly2HybridPath) {
    PlacementList pl2;
    for (UniqueCell &u : cells) {
      const double rew = REWARD_NORM_TETRIS[u.lc];
      const double dFac = disc.f[u.lc];
      if (u.inTb) {
        if (needRiskPrescreen) {
          for (int n = 0; n < NUM_PIECES; ++n) {
            const double q = rew + dFac * static_cast<double>(u.tbRow[n]);
            if (q > bestTbGroundedForPiece[n]) {
              bestTbGroundedForPiece[n] = q;
            }
          }
        }
        continue;
      }
      const Board20 bAfter = Board20::fromRows(u.afterRows);
      const TuckSetupInfo tucks = computeTuckSetupInfo(bAfter);
      for (int n = 0; n < NUM_PIECES; ++n) {
        u.ply2[n] =
            detail::bestReplyHybrid2Ply20(bAfter, tucks, n, gravity, disc, tb, nn, opts.outMargin,
                                          opts.rngMode, pl2, anyTbGroundedForPiece[n]);
        if (u.ply2[n].anyInTb) {
          anyTbGroundedForPiece[n] = true;
          if (needRiskPrescreen) {
            const double tbOnlyQ = rew + dFac * u.ply2[n].bestTbOnly;
            if (tbOnlyQ > bestTbGroundedForPiece[n]) {
              bestTbGroundedForPiece[n] = tbOnlyQ;
            }
          }
        }
        if (needSelectivePly3) {
          const double aoQ = rew + dFac * u.ply2[n].bestIfAllOut;
          if (aoQ > bestAllOutForPiece[n]) {
            bestAllOutForPiece[n] = aoQ;
          }
        }
      }
    }
  }

  for (int n = 0; n < NUM_PIECES; ++n) {
    pState.lockValues[n].reserve(cells.size());
  }

  std::vector<FullMovePossibility> dasBuf;
  dasBuf.reserve(64);
  detail::FlatSecondMoveCache dasScoreCache;
  const double tieEps = 1e-12;
  bool anyPly3Used = false;

  for (UniqueCell &u : cells) {
    const double rew = REWARD_NORM_TETRIS[u.lc];
    const bool topClear =
        ((u.afterRows[0] | u.afterRows[1] | u.afterRows[2]) & FULL_ROW_MASK) == 0U;

    // For ply == 1 out-of-TB boards, compute all 7 NN outputs in a single forward pass.
    float nnAllRow[NUM_PIECES] = {};
    const bool needNn1 =
        !u.inTb && hasNn && opts.outCfg.ply == 1 && (!anyInTbOverall || finiteMargin);
    if (needNn1) {
      nn->evalAll(u.afterRows, nnAllRow);
    }

    GameState afterGs{};
    const bool computeDas =
        opts.applyDasPenalties && pState.useDAS && (u.needsPartialDas || u.needsMinDas);
    const bool needBoardAfter = computeDas || (needNn1 && !topClear);
    const Board20 bAfter = needBoardAfter ? Board20::fromRows(u.afterRows) : Board20::empty();
    if (computeDas) {
      afterGs.level = opts.stationaryLevel
                          ? state.level
                          : getLevelAfterLineClears20(state.level, state.lines, u.lc);
      afterGs.lines = opts.stationaryLevel ? state.lines : (state.lines + u.lc);
      for (int r = 0; r < BOARD_ROWS; ++r) {
        afterGs.board[r] = static_cast<unsigned int>(u.afterRows[r] & FULL_ROW_MASK);
      }
    }

    const double dFac = disc.f[u.lc];
    for (int n = 0; n < NUM_PIECES; ++n) {
      double qFull = rew;
      if (u.inTb) {
        qFull = rew + dFac * static_cast<double>(u.tbRow[n]);
      } else if (usePly2HybridPath) {
        const bool groundedPiece = anyTbGroundedForPiece[n];
        qFull = rew + dFac * (groundedPiece ? u.ply2[n].bestIfTbGrounded : u.ply2[n].bestIfAllOut);
        // At rt == 0 (single phantom), collapse only reads nextPieceIdx, avoiding 7x 3-ply work.
        const bool pieceNeedsPly3 = (opts.reactionTime > 0) || (n == nextPieceIdx);
        if (needSelectivePly3 && pieceNeedsPly3 && u.ply2[n].hasMove) {
          if (!groundedPiece) {
            if (qFull + NN_3PLY_PRESCREEN_SLACK >= bestAllOutForPiece[n]) {
              anyPly3Used = true;
              const double est3 =
                  detail::bestReplyNn3PlyTopK20(u.afterRows, n, gravity, opts.gamma, *nn,
                                                opts.rngMode, opts.outCfg.topK, opts.discountMode);
              qFull = rew + dFac * est3;
            }
          } else if (needRiskPrescreen
                     && !u.ply2[n].bestGroundedFromTb
                     && qFull + NN_3PLY_PRESCREEN_SLACK >= bestTbGroundedForPiece[n]) {
            anyPly3Used = true;
            const double est3 =
                detail::bestReplyNn3PlyTopK20(u.afterRows, n, gravity, opts.gamma, *nn,
                                              opts.rngMode, opts.outCfg.topK, opts.discountMode);
            double cont = std::max(0.0, est3 - clampedMargin);
            // 3-ply NN leaves skip TB lookup, so floor against any exact ply-2 TB re-entry.
            if (u.ply2[n].anyInTb && u.ply2[n].bestTbOnly >= cont - tieEps) {
              cont = u.ply2[n].bestTbOnly;
            }
            qFull = rew + dFac * cont;
          }
        }
      } else if (hasNn) {
        // When anyInTbOverall && !finiteMargin (+inf), out-of-TB moves get zero continuation.
        if (!anyInTbOverall || finiteMargin) {
          double estCont = 0.0;
          if (opts.outCfg.ply >= 3) {
            anyPly3Used = true;
            estCont =
                detail::bestReplyNn3PlyTopK20(u.afterRows, n, gravity, opts.gamma, *nn,
                                              opts.rngMode, opts.outCfg.topK, opts.discountMode);
          } else {
            if (topClear || hasAnyLegalMove10Hz(bAfter, n, gravity)) {
              estCont = std::max(0.0, static_cast<double>(nnAllRow[n]));
            }
          }
          if (anyInTbOverall) {
            qFull = rew + dFac * std::max(0.0, estCont - clampedMargin);
          } else {
            qFull = rew + dFac * estCont;
          }
        }
      }
      pState.lockValues[n][u.keyStr] = qFull * TBNN_PRECOMPUTE_SCALE;

      if (computeDas) {
        dasScoreCache.clear();
        bool fullAnyInTb = false;
        const double bestFull = detail::bestSecondPieceScoreForDas(
            afterGs, bAfter, n, 15, disc, tb, nn, opts.outMargin, opts.rngMode, dasBuf,
            dasScoreCache, /*forceGrounded=*/false, &fullAnyInTb);
        double partPen = 0.0;
        double minPen = 0.0;
        if (u.needsPartialDas) {
          const double bestPart = detail::bestSecondPieceScoreForDas(
              afterGs, bAfter, n, 10, disc, tb, nn, opts.outMargin, opts.rngMode, dasBuf,
              dasScoreCache, /*forceGrounded=*/fullAnyInTb);
          partPen = std::min(0.0, bestPart - bestFull);
        }
        if (u.needsMinDas) {
          const double bestMin = detail::bestSecondPieceScoreForDas(
              afterGs, bAfter, n, 5, disc, tb, nn, opts.outMargin, opts.rngMode, dasBuf,
              dasScoreCache, /*forceGrounded=*/fullAnyInTb);
          minPen = std::min(0.0, bestMin - bestFull);
        }
        pState.partialPenalties[n][u.keyStr] = dFac * partPen * TBNN_PRECOMPUTE_SCALE;
        pState.minPenalties[n][u.keyStr] = dFac * minPen * TBNN_PRECOMPUTE_SCALE;
      }
    }
  }

  // Step 5: StackRabbit's exact precompute collapse.
  const std::vector<PrecomputePhantomBreakdown> ranked =
      analyzePrecomputeCollapse(pState, curPiece);
  if (ranked.empty() || !ranked[0].isWinner) {
    return res;
  }
  const PrecomputePhantomBreakdown &winner = ranked[0];
  const PrecomputePieceBreakdown &pb = winner.byPiece[nextPieceIdx];

  int chosenRot = 0, chosenX = 0, chosenY = 0, chosenDas = DAS_CHARGE_MAX;
  if (pb.valid) {
    chosenRot = pb.rot;
    chosenX = pb.x;
    chosenY = pb.y;
    chosenDas = pb.dasChargeAfter;
  } else if (winner.hasInitialPlacement) {
    chosenRot = winner.rot;
    chosenX = winner.x;
    chosenY = winner.y;
    chosenDas = winner.dasChargeAfter;
  } else {
    return res;
  }

  const uint32_t chosenPacked = detail::packLockCellKey(chosenRot, chosenX, chosenY);
  const auto cit = cellToIdx.find(chosenPacked);
  if (cit != cellToIdx.end()) {
    res.chosenInTb = cells[cit->second].inTb;
  }
  res.usedAllOutFallback =
      !(usePly2HybridPath ? anyTbGroundedForPiece[nextPieceIdx] : anyInTbOverall);
  res.usedPly3 = anyPly3Used;

  const EvalContext ctx = getEvalContext(state, bc.lookup, false, opts.aggro);
  LockPlacement lp = {chosenX, chosenY, chosenRot, -1, NO_TUCK_NOTATION, curPiece};
  GameState after = advanceGameState(state, lp, &ctx);
  const int cleared = after.lines - state.lines;
  const int nextLines = opts.stationaryLevel ? state.lines : after.lines;
  const int nextLevel = opts.stationaryLevel ? state.level : after.level;

  res.hasMove = true;
  res.rot = chosenRot;
  res.x = chosenX;
  res.y = chosenY;
  res.dasChargeAfter = chosenDas;
  res.linesCleared = cleared;
  res.afterState =
      board20ToGameState(gameStateToBoard20(after), nextLevel, nextLines, bc, opts.aggro);
  return res;
}

// Native V_RT(b', n, d') + DasRtMoveTables reaction-time move selector:
//   - Generates early locks + reaction-time phantoms and their post-RT adjustment moves in one pass
//     via DasRtMoveTables (`ws.search`).
//   - Scores each unique lock cell (rot, x, y) directly from `rtTb->lookup` in O(1) across all
//     (nextPiece n, ending DAS bucket k') with zero 2nd-piece `fullMoveSearch` penalty sweeps.
//   - For out-of-TB cells, supports 1-ply multi-bucket NNUE, 2-ply `OUT -> TB2 / NN` re-entry
//     (threading exact 2nd-piece ending DAS buckets into `rtTb` / `nn`), and selective 3-ply Top-K.
template <typename RtTbType>
inline TbNnPrecomputeResult
chooseTbNnRtMove(const GameState &state, int curPieceIdx, int nextPieceIdx, const RtTbType *rtTb,
                 const TinyDasNet *nn, const DasRtMoveTables &tables, RtDecisionWorkspace &ws,
                 const TbNnPrecomputeOptions &opts, const BackendContexts &bc) {
  TbNnPrecomputeResult res{};
  if (curPieceIdx < 0
      || curPieceIdx >= NUM_PIECES
      || nextPieceIdx < 0
      || nextPieceIdx >= NUM_PIECES
      || opts.dasCharge < 0) {
    return res;
  }

  const bool hasTb = (rtTb != nullptr && rtTb->isLoaded());
  const int K =
      hasTb ? rtTb->numDasBuckets()
            : ((nn != nullptr && nn->isLoaded() && nn->numDasBuckets() >= 2) ? nn->numDasBuckets()
                                                                             : MAX_DAS_BUCKETS);
  if (!isValidDasBucketCount(K)) {
    return res;
  }
  const bool hasNn = (nn != nullptr && nnSupportsDasBuckets(*nn, K));
  const int vpb = NUM_PIECES * K;
  const int startDas = std::clamp(opts.dasCharge, 0, DAS_CHARGE_MAX);
  const Board20 board20 = gameStateToBoard20(state);
  const bool zeroRt = (opts.reactionTime <= 0);

  ws.search(tables, board20, curPieceIdx, &startDas, 1, /*forceZeroRt=*/zeroRt);
  const int nCells = ws.numCells();
  if (nCells == 0 || nCells > DAS_MAX_LOCK_MOVES) {
    return res;
  }

  struct RtCellMeta {
    uint8_t lc = 0;
    uint8_t bucketMask = 0;
    int8_t bestOverallDas = 0;
    bool fitsKey = false;
    bool inTb = false;
    BoardKey afterKey = EMPTY_KEY;
    uint16_t afterRows[BOARD_ROWS] = {};
    float tbRow[MAX_RT_VALUES_PER_BOARD] = {};
  };
  RtCellMeta cells[DAS_MAX_LOCK_MOVES];

  const DasRtSearchResult &sr = ws.result(0);
  for (int e = 0; e < sr.numEarlyMoves; ++e) {
    const int cl = ws.localCellOf(0, sr.earlyMoves[e].cellIdx);
    const int dasAfter = static_cast<int>(sr.earlyMoves[e].dasAfter);
    const int kb = dasChargeToBucket(dasAfter, K);
    RtCellMeta &u = cells[cl];
    u.bucketMask = static_cast<uint8_t>(u.bucketMask | (1U << kb));
    if (dasAfter > u.bestOverallDas) {
      u.bestOverallDas = static_cast<int8_t>(dasAfter);
    }
  }
  const int totalAdjEdges = sr.adjMoveOffset[sr.numAdjStates];
  for (int e = 0; e < totalAdjEdges; ++e) {
    const int cl = ws.localCellOf(0, sr.adjEdges[e].cellIdx);
    const int dasAfter = static_cast<int>(sr.adjEdges[e].dasAfter);
    const int kb = dasChargeToBucket(dasAfter, K);
    RtCellMeta &u = cells[cl];
    u.bucketMask = static_cast<uint8_t>(u.bucketMask | (1U << kb));
    if (dasAfter > u.bestOverallDas) {
      u.bestOverallDas = static_cast<int8_t>(dasAfter);
    }
  }
  if (zeroRt) {
    for (int i = 0; i < nCells; ++i) {
      RtCellMeta &u = cells[i];
      u.bucketMask = static_cast<uint8_t>(1U << dasChargeToBucket(u.bestOverallDas, K));
    }
  }

  const StepDiscountTable20 disc = StepDiscountTable20::make(opts.gamma, opts.discountMode);
  const int nLo = zeroRt ? nextPieceIdx : 0;
  const int nHi = zeroRt ? nextPieceIdx : (NUM_PIECES - 1);
  double bestInTbForPiece[NUM_PIECES];
  for (int n = 0; n < NUM_PIECES; ++n) {
    bestInTbForPiece[n] = -std::numeric_limits<double>::infinity();
  }

  bool anyInTbOverall = false;
  for (int i = 0; i < nCells; ++i) {
    RtCellMeta &u = cells[i];
    u.lc = ws.computeAfterRows(board20, curPieceIdx, i, u.afterRows);
    const int startRow = (u.lc == 0)
                             ? std::max(0, std::min(static_cast<int>(board20.topNonEmptyRow),
                                                    static_cast<int>(ws.cell(i).y)))
                             : 0;
    u.fitsKey = encodeBoardKey(u.afterRows, u.afterKey, startRow);
    if (hasTb && u.fitsKey && rtTb->lookup(u.afterKey, u.tbRow)) {
      u.inTb = true;
      anyInTbOverall = true;
      const double rew = REWARD_NORM_TETRIS[u.lc];
      const double dFac = disc.f[u.lc];
      for (int n = nLo; n <= nHi; ++n) {
        for (int kb = 0; kb < K; ++kb) {
          if (((u.bucketMask >> kb) & 1U) == 0U)
            continue;
          const double q = rew + dFac * static_cast<double>(u.tbRow[rtValueIndex(n, kb, K)]);
          if (q > bestInTbForPiece[n]) {
            bestInTbForPiece[n] = q;
          }
        }
      }
    }
  }

  const bool finiteMargin = std::isfinite(opts.outMargin);
  const double clampedMargin = finiteMargin ? std::max(0.0, opts.outMargin) : 0.0;
  const bool usePly2HybridPath = opts.outCfg.usesPly2HybridPath(hasTb);
  const bool isHighRisk = opts.outCfg.isPly3HighRisk(board20);
  const bool needSelectivePly3 = usePly2HybridPath && opts.outCfg.ply >= 3 && hasNn;
  const bool needRiskPrescreen = needSelectivePly3 && isHighRisk && finiteMargin;

  bool anyTbGroundedForPiece[NUM_PIECES];
  double bestTbGroundedForPiece[NUM_PIECES];
  double bestAllOutForPiece[NUM_PIECES];
  for (int n = 0; n < NUM_PIECES; ++n) {
    anyTbGroundedForPiece[n] = anyInTbOverall;
    bestTbGroundedForPiece[n] = bestInTbForPiece[n];
    bestAllOutForPiece[n] = -std::numeric_limits<double>::infinity();
  }

  detail::Ply2HybridEval20 ply2[DAS_MAX_LOCK_MOVES][NUM_PIECES][MAX_DAS_BUCKETS];
  if (usePly2HybridPath) {
    DasMoveList pl2;
    for (int i = 0; i < nCells; ++i) {
      const RtCellMeta &u = cells[i];
      if (u.inTb) {
        continue;
      }
      const double rew = REWARD_NORM_TETRIS[u.lc];
      const double dFac = disc.f[u.lc];
      const Board20 bAfter = Board20::fromRows(u.afterRows);
      for (int n = nLo; n <= nHi; ++n) {
        detail::bestReplyHybrid2PlyRtMultiBucket20(bAfter, n, u.bucketMask, K, tables, disc, rtTb,
                                                   nn, opts.outMargin, opts.rngMode, pl2,
                                                   anyTbGroundedForPiece[n], ply2[i][n]);
        for (int kb = 0; kb < K; ++kb) {
          if (((u.bucketMask >> kb) & 1U) == 0U)
            continue;
          const auto &p2 = ply2[i][n][kb];
          if (p2.anyInTb && needRiskPrescreen) {
            const double tbOnlyQ = rew + dFac * p2.bestTbOnly;
            if (tbOnlyQ > bestTbGroundedForPiece[n]) {
              bestTbGroundedForPiece[n] = tbOnlyQ;
            }
          }
          if (needSelectivePly3) {
            const double aoQ = rew + dFac * p2.bestIfAllOut;
            if (aoQ > bestAllOutForPiece[n]) {
              bestAllOutForPiece[n] = aoQ;
            }
          }
        }
      }
    }
  }

  const double tieEps = 1e-12;
  bool anyPly3Used = false;

  for (int i = 0; i < nCells; ++i) {
    const RtCellMeta &u = cells[i];
    const double rew = REWARD_NORM_TETRIS[u.lc];
    const double dFac = disc.f[u.lc];
    double *qc = ws.q(i);
    for (int o = 0; o < vpb; ++o) {
      qc[o] = rew;
    }

    if (u.inTb) {
      for (int o = 0; o < vpb; ++o) {
        qc[o] = rew + dFac * static_cast<double>(u.tbRow[o]);
      }
      continue;
    }

    float nnRtRow[MAX_RT_VALUES_PER_BOARD] = {};
    const bool needNn1 =
        !usePly2HybridPath && hasNn && opts.outCfg.ply == 1 && (!anyInTbOverall || finiteMargin);
    const bool topClear =
        ((u.afterRows[0] | u.afterRows[1] | u.afterRows[2]) & FULL_ROW_MASK) == 0U;
    const Board20 bAfter = (!usePly2HybridPath && hasNn && (!topClear || opts.outCfg.ply >= 2))
                               ? Board20::fromRows(u.afterRows)
                               : Board20::empty();
    if (needNn1) {
      evalNnRtRow(*nn, u.afterRows, K, nnRtRow);
    }

    for (int n = nLo; n <= nHi; ++n) {
      const bool canSpawn1 =
          !needNn1 || topClear || !collides20(bAfter, n, SPAWN_COL, PIECES_20[n].initialY, 0);
      for (int kb = 0; kb < K; ++kb) {
        if (((u.bucketMask >> kb) & 1U) == 0U)
          continue;
        const int o = rtValueIndex(n, kb, K);
        double qVal = rew;
        if (usePly2HybridPath) {
          const auto &p2 = ply2[i][n][kb];
          const bool groundedPiece = anyTbGroundedForPiece[n];
          qVal = rew + dFac * (groundedPiece ? p2.bestIfTbGrounded : p2.bestIfAllOut);
          if (needSelectivePly3 && p2.hasMove) {
            if (!groundedPiece) {
              if (qVal + NN_3PLY_PRESCREEN_SLACK >= bestAllOutForPiece[n]) {
                anyPly3Used = true;
                const double est3 = detail::bestReplyNn3PlyTopKRt20(
                    u.afterRows, n, kb, K, tables, opts.gamma, *nn, opts.rngMode, opts.outCfg.topK,
                    opts.discountMode);
                qVal = rew + dFac * est3;
              }
            } else if (needRiskPrescreen
                       && !p2.bestGroundedFromTb
                       && qVal + NN_3PLY_PRESCREEN_SLACK >= bestTbGroundedForPiece[n]) {
              anyPly3Used = true;
              const double est3 = detail::bestReplyNn3PlyTopKRt20(
                  u.afterRows, n, kb, K, tables, opts.gamma, *nn, opts.rngMode, opts.outCfg.topK,
                  opts.discountMode);
              double cont = std::max(0.0, est3 - clampedMargin);
              if (p2.anyInTb && p2.bestTbOnly >= cont - tieEps) {
                cont = p2.bestTbOnly;
              }
              qVal = rew + dFac * cont;
            }
          }
        } else if (hasNn && (!anyInTbOverall || finiteMargin)) {
          double estCont = 0.0;
          if (opts.outCfg.ply >= 3) {
            DasMoveList pl2Local;
            if (anyInTbOverall) {
              const double est2 = detail::bestReplyNn2PlyRt20(bAfter, n, kb, K, tables, disc, *nn,
                                                              opts.rngMode, pl2Local);
              if (rew + dFac * std::max(0.0, est2 - clampedMargin) + NN_3PLY_PRESCREEN_SLACK
                  < bestInTbForPiece[n]) {
                estCont = est2;
              } else {
                anyPly3Used = true;
                estCont = detail::bestReplyNn3PlyTopKRt20(u.afterRows, n, kb, K, tables, opts.gamma,
                                                          *nn, opts.rngMode, opts.outCfg.topK,
                                                          opts.discountMode);
              }
            } else {
              anyPly3Used = true;
              estCont = detail::bestReplyNn3PlyTopKRt20(u.afterRows, n, kb, K, tables, opts.gamma,
                                                        *nn, opts.rngMode, opts.outCfg.topK,
                                                        opts.discountMode);
            }
          } else if (opts.outCfg.ply == 2) {
            DasMoveList pl2Local;
            estCont = detail::bestReplyNn2PlyRt20(bAfter, n, kb, K, tables, disc, *nn, opts.rngMode,
                                                  pl2Local);
          } else if (canSpawn1) {
            estCont = std::max(0.0, static_cast<double>(nnRtRow[o]));
          }
          qVal = anyInTbOverall ? (rew + dFac * std::max(0.0, estCont - clampedMargin))
                                : (rew + dFac * estCont);
        }
        qc[o] = qVal;
      }
    }
  }

  int chosenCell = -1;
  int chosenDas = DAS_CHARGE_MAX;
  if (zeroRt) {
    double bestQ = -std::numeric_limits<double>::infinity();
    bool bestInTb = false;
    for (int i = 0; i < nCells; ++i) {
      const RtCellMeta &u = cells[i];
      const int dasAfter = static_cast<int>(u.bestOverallDas);
      const int kb = dasChargeToBucket(dasAfter, K);
      const double q = ws.q(i)[rtValueIndex(nextPieceIdx, kb, K)];
      const bool strictlyBetter = (chosenCell < 0 || q > bestQ + tieEps);
      const bool tieBetter =
          !strictlyBetter
          && (std::abs(q - bestQ) <= tieEps)
          && ((u.inTb && !bestInTb) || (u.inTb == bestInTb && dasAfter > chosenDas));
      if (strictlyBetter || tieBetter) {
        chosenCell = i;
        chosenDas = dasAfter;
        bestQ = q;
        bestInTb = u.inTb;
      }
    }
  } else {
    const auto transProb = detail::buildTransitionProbMatrix(opts.rngMode);
    const RtDecisionWorkspace::Choice ch =
        ws.chooseAction(0, K, transProb[static_cast<size_t>(curPieceIdx)]);
    if (!ch.any) {
      return res;
    }
    chosenCell = ch.cellForNext[nextPieceIdx];
    chosenDas = ch.dasAfterForNext[nextPieceIdx];
  }
  if (chosenCell < 0) {
    return res;
  }

  const RtDecisionWorkspace::Cell &cl = ws.cell(chosenCell);
  const RtCellMeta &chosenMeta = cells[chosenCell];
  res.chosenInTb = chosenMeta.inTb;
  res.usedAllOutFallback =
      !(usePly2HybridPath ? anyTbGroundedForPiece[nextPieceIdx] : anyInTbOverall);
  res.usedPly3 = anyPly3Used;

  const EvalContext ctx = getEvalContext(state, bc.lookup, false, opts.aggro);
  LockPlacement lp = {static_cast<int>(cl.x), static_cast<int>(cl.y),  static_cast<int>(cl.rot), -1,
                      NO_TUCK_NOTATION,       &PIECE_LIST[curPieceIdx]};
  GameState after = advanceGameState(state, lp, &ctx);
  const int cleared = after.lines - state.lines;
  const int nextLines = opts.stationaryLevel ? state.lines : after.lines;
  const int nextLevel = opts.stationaryLevel ? state.level : after.level;

  res.hasMove = true;
  res.rot = static_cast<int>(cl.rot);
  res.x = static_cast<int>(cl.x);
  res.y = static_cast<int>(cl.y);
  res.dasChargeAfter = chosenDas;
  res.linesCleared = cleared;
  res.afterState =
      board20ToGameState(gameStateToBoard20(after), nextLevel, nextLines, bc, opts.aggro);
  return res;
}

} // namespace das

#endif // DAS_TABLEBASE_TBNN_PRECOMPUTE_ADAPTER_HPP
