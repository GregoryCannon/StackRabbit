#ifndef DAS_TABLEBASE_TBNN_PRECOMPUTE_ADAPTER_HPP
#define DAS_TABLEBASE_TBNN_PRECOMPUTE_ADAPTER_HPP

// Configurable reaction-time + fullMoveSearch + DAS-penalty precompute harness powered by
// Hybrid TB + NNUE (read-only wrapper over src/backend/full_move_search.cpp and
// src/backend/precompute.cpp's analyzePrecomputeCollapse).

#include "eval_adapter.hpp"
#include "nnue_20row.hpp"

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
  double outMargin = 0.25;
  NnSearchConfig20 outCfg = NnSearchConfig20::ply2();
  RngMode rngMode = RngMode::Nes;
  AggressionLevel aggro = AGGRO;
  bool stationaryLevel = true;
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

// Evaluates best 2nd-piece continuation under a prescribed starting DAS charge (15, 10, or 5)
// using fast 1-ply TB + NNUE expectation over the 3rd piece, with per-(afterBoard, nextPiece)
// caching across DAS buckets and zero heap allocations per call.
template <typename TbType>
inline double
bestSecondPieceScoreForDas(const GameState &after1, const Board20 &bAfter1, int nextPieceIdx,
                           int dasBucket, double gamma, const TbType *tb, const TinyDasNet *nn,
                           double outMargin, RngMode rngMode, std::vector<FullMovePossibility> &buf,
                           std::unordered_map<uint32_t, CachedSecondMoveScore> &cache) {
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
    const uint32_t mKey = packLockCellKey(m2.rotationIndex, m2.x, m2.y);
    CachedSecondMoveScore cs{};
    const auto cit = cache.find(mKey);
    if (cit != cache.end()) {
      cs = cit->second;
    } else {
      const uint8_t lc2 =
          applyPlacement20(bAfter1, nextPieceIdx, m2.rotationIndex, m2.x, m2.y, after2Rows);
      const double rew2 = REWARD_NORM_TETRIS[lc2];
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
        cs.inScore = rew2 + gamma * ev;
        cs.rawScore = cs.inScore;
      } else {
        const double est =
            hasNn ? nn->evalNextPieceExpected(after2Rows, rngMode, nextPieceIdx) : 0.0;
        cs.inTb = 0;
        cs.rawScore = rew2 + gamma * est;
        // When margin is +inf, out-of-TB moves receive zero continuation if any in-TB move exists.
        cs.inScore =
            (hasNn && finiteMargin) ? (rew2 + gamma * std::max(0.0, est - clampedMargin)) : rew2;
      }
      cache.emplace(mKey, cs);
    }
    if (cs.inTb)
      anyInTb = true;
    if (cs.inScore > bestIn)
      bestIn = cs.inScore;
    if (cs.rawScore > bestRaw)
      bestRaw = cs.rawScore;
  }

  return anyInTb ? bestIn : bestRaw;
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

  bool anyTbGroundedForPiece[NUM_PIECES];
  for (int n = 0; n < NUM_PIECES; ++n) {
    anyTbGroundedForPiece[n] = anyInTbOverall;
  }
  if (opts.outCfg.ply == 2) {
    PlacementList pl2;
    for (UniqueCell &u : cells) {
      if (u.inTb)
        continue;
      for (int n = 0; n < NUM_PIECES; ++n) {
        u.ply2[n] = detail::bestReplyHybrid2Ply20(u.afterRows, n, gravity, opts.gamma, tb, nn,
                                                  opts.outMargin, opts.rngMode, pl2);
        if (u.ply2[n].anyInTb) {
          anyTbGroundedForPiece[n] = true;
        }
      }
    }
  }

  std::vector<FullMovePossibility> dasBuf;
  dasBuf.reserve(64);
  std::unordered_map<uint32_t, detail::CachedSecondMoveScore> dasScoreCache;
  dasScoreCache.reserve(64);

  for (UniqueCell &u : cells) {
    const double rew = REWARD_NORM_TETRIS[u.lc];
    const Board20 bAfter = Board20::fromRows(u.afterRows);
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
    const bool computeDas = pState.useDAS && (u.needsPartialDas || u.needsMinDas);
    if (computeDas) {
      afterGs.level = opts.stationaryLevel
                          ? state.level
                          : getLevelAfterLineClears20(state.level, state.lines, u.lc);
      afterGs.lines = opts.stationaryLevel ? state.lines : (state.lines + u.lc);
      for (int r = 0; r < BOARD_ROWS; ++r) {
        afterGs.board[r] = static_cast<unsigned int>(u.afterRows[r] & FULL_ROW_MASK);
      }
    }

    for (int n = 0; n < NUM_PIECES; ++n) {
      double qFull = rew;
      if (u.inTb) {
        qFull = rew + opts.gamma * static_cast<double>(u.tbRow[n]);
      } else if (opts.outCfg.ply == 2) {
        qFull = rew
                + opts.gamma
                      * (anyTbGroundedForPiece[n] ? u.ply2[n].bestIfTbGrounded
                                                  : u.ply2[n].bestIfAllOut);
      } else if (hasNn) {
        // When anyInTbOverall && !finiteMargin (+inf), out-of-TB moves get zero continuation.
        if (!anyInTbOverall || finiteMargin) {
          double estCont = 0.0;
          if (opts.outCfg.ply >= 3) {
            estCont = detail::bestReplyNn3PlyTopK20(u.afterRows, n, gravity, opts.gamma, *nn,
                                                    opts.rngMode, opts.outCfg.topK);
          } else {
            if (topClear || hasAnyLegalMove10Hz(bAfter, n, gravity)) {
              estCont = std::max(0.0, static_cast<double>(nnAllRow[n]));
            }
          }
          if (anyInTbOverall) {
            qFull = rew + opts.gamma * std::max(0.0, estCont - clampedMargin);
          } else {
            qFull = rew + opts.gamma * estCont;
          }
        }
      }
      pState.lockValues[n][u.keyStr] = qFull * TBNN_PRECOMPUTE_SCALE;

      if (computeDas) {
        dasScoreCache.clear();
        const double bestFull =
            detail::bestSecondPieceScoreForDas(afterGs, bAfter, n, 15, opts.gamma, tb, nn,
                                               opts.outMargin, opts.rngMode, dasBuf, dasScoreCache);
        double partPen = 0.0;
        double minPen = 0.0;
        if (u.needsPartialDas) {
          const double bestPart = detail::bestSecondPieceScoreForDas(
              afterGs, bAfter, n, 10, opts.gamma, tb, nn, opts.outMargin, opts.rngMode, dasBuf,
              dasScoreCache);
          partPen = std::min(0.0, bestPart - bestFull);
        }
        if (u.needsMinDas) {
          const double bestMin = detail::bestSecondPieceScoreForDas(
              afterGs, bAfter, n, 5, opts.gamma, tb, nn, opts.outMargin, opts.rngMode, dasBuf,
              dasScoreCache);
          minPen = std::min(0.0, bestMin - bestFull);
        }
        pState.partialPenalties[n][u.keyStr] = partPen * TBNN_PRECOMPUTE_SCALE;
        pState.minPenalties[n][u.keyStr] = minPen * TBNN_PRECOMPUTE_SCALE;
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
  res.usedAllOutFallback = !anyInTbOverall;

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

} // namespace das

#endif // DAS_TABLEBASE_TBNN_PRECOMPUTE_ADAPTER_HPP
