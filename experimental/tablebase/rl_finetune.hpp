#ifndef TABLEBASE_RL_FINETUNE_HPP
#define TABLEBASE_RL_FINETUNE_HPP

#include "distill_data.hpp"
#include "tiny_nn.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace tb {

// Hard board-count ceiling enforced on any tablebase anchor used during small-TB RL fine-tuning.
inline constexpr uint64_t RL_MAX_ANCHOR_TB_BOARDS = 2500000ULL;

// Per-worker direct-mapped cache storing exact IEEE-754 double results for both 2-ply
// bestReplyNn2Ply (key bit 44 = 1) and leaf evalNextPieceExpected (key bit 44 = 0). Preserves
// 100% bit-for-bit 1T == MT determinism when targetNet is frozen.
struct Eval2PlyCache {
  static constexpr int INDEX_BITS = 15;
  static constexpr size_t NUM_SLOTS = size_t{1} << INDEX_BITS; // 32,768 slots (512 KB/worker)
  static constexpr uint64_t FIB_HASH_MULT = 11400714819323198485ULL;

  struct Entry {
    uint64_t key = ~0ULL;
    double val = 0.0;
  };

  std::vector<Entry> slots;

  void init() {
    if (slots.size() != NUM_SLOTS) {
      slots.assign(NUM_SLOTS, Entry{});
    }
  }

  double bestReply(uint64_t board, int piece, const TinyNextBoxNet &net, RngMode mode,
                   TransitionList &tl) noexcept {
    assert(slots.size() == NUM_SLOTS);
    const uint64_t key =
        (1ULL << 44) | (static_cast<uint64_t>(piece) << 40) | (board & VALID_BOARD_MASK);
    const size_t idx = static_cast<size_t>((key * FIB_HASH_MULT) >> (64 - INDEX_BITS));
    if (slots[idx].key == key) {
      return slots[idx].val;
    }
    generateTransitions(board, piece, tl);
    double best = 0.0;
    if (!tl.empty()) {
      best = -std::numeric_limits<double>::infinity();
      for (int i = 0; i < tl.size(); ++i) {
        const uint64_t leafKey =
            (static_cast<uint64_t>(piece) << 40) | (tl[i].afterBoard & VALID_BOARD_MASK);
        const size_t lIdx = static_cast<size_t>((leafKey * FIB_HASH_MULT) >> (64 - INDEX_BITS));
        double expV = 0.0;
        if (slots[lIdx].key == leafKey) {
          expV = slots[lIdx].val;
        } else {
          expV = net.evalNextPieceExpected(tl[i].afterBoard, mode, piece);
          slots[lIdx].key = leafKey;
          slots[lIdx].val = expV;
        }
        const double sc = static_cast<double>(tl[i].linesCleared) + expV;
        if (sc > best) {
          best = sc;
        }
      }
    }
    slots[idx].key = key;
    slots[idx].val = best;
    return best;
  }
};

struct RlMoveTarget {
  uint64_t afterBoard = 0ULL;
  uint8_t linesCleared = 0;
  double q2Ply = 0.0; // r_m + bestReplyNn2Ply(afterBoard, nextPiece, targetNet, rngMode)
};

struct RlDecisionSample {
  uint64_t board = 0ULL;
  int curPiece = 0;
  int nextPiece = 0;
  double tdReturn = 0.0; // Backward TD(lambda) return G_t^(lambda)
  std::vector<RlMoveTarget> moves;
};

struct RlTrajectoryStep {
  uint64_t board = 0ULL;
  int curPiece = 0;
  int nextPiece = 0;
  uint8_t linesCleared = 0;
  double bootstrapNextV = 0.0; // Clamped max(0, V_target(b_{t+1}, n_t))
  std::vector<RlMoveTarget> moves;
};

// Computes exact backward TD(lambda) returns G_t^(lambda) over a game trajectory:
//   - At real top-out (t == T - 1, !truncatedAtCap): G_{T-1} = r_{T-1} (exact 0.0 terminal value).
//   - At step-cap truncation (t == T - 1, truncatedAtCap): G_{T-1} = r_{T-1} + max(0, terminalV).
//   - For t < T - 1: G_t = r_t + (1 - lambda) * max(0, bootstrapNextV_t) + lambda * G_{t+1}.
// When lambda == 1.0 and !truncatedAtCap, G_t equals the exact unbiased suffix sum of lines
// cleared.
inline void computeTdLambdaReturns(const std::vector<RlTrajectoryStep> &steps, double lambda,
                                   bool truncatedAtCap, double terminalBootstrapV,
                                   std::vector<double> &outReturns) {
  const size_t n = steps.size();
  outReturns.assign(n, 0.0);
  if (n == 0) {
    return;
  }
  const double lam = std::isfinite(lambda) ? std::clamp(lambda, 0.0, 1.0) : 1.0;
  const double termV =
      (truncatedAtCap && std::isfinite(terminalBootstrapV) && terminalBootstrapV > 0.0)
          ? terminalBootstrapV
          : 0.0;

  double gNext = 0.0;
  for (size_t i = n; i > 0; --i) {
    const size_t t = i - 1;
    const double r = static_cast<double>(steps[t].linesCleared);
    if (t + 1 == n) {
      gNext = r + termV;
    } else {
      const double rawBoot = steps[t].bootstrapNextV;
      const double boot = (std::isfinite(rawBoot) && rawBoot > 0.0) ? rawBoot : 0.0;
      gNext = r + (1.0 - lam) * boot + lam * gNext;
    }
    outReturns[t] = gNext;
  }
}

struct RlRolloutConfig {
  RngMode rngMode = RngMode::Nes;
  int numGames = 400;
  uint64_t baseSeed = 40000000ULL;
  double epsilon = 0.06;          // Exploration rate: 50% runner-up, 50% random among ranks 2..4
  double lambda = 0.9;            // TD(lambda) parameter in [0, 1]
  bool collectMoveTargets = true; // Populate 2-ply ExIt move Q-targets for every legal move
  int maxStepsPerGame = DEFAULT_MAX_PIECES_PER_GAME;
  int maxDecisions = 200000;
  int numThreads = 1;
};

struct RlRolloutStats {
  int games = 0;
  double meanLines = 0.0;
  double stdErrLines = 0.0;
  int stepCapHits = 0;
  size_t totalDecisionsCollected = 0;
  std::array<uint64_t, NUM_HOLE_BUCKETS> decisionsByHoles{};
};

// Collects multi-threaded, bit-for-bit 1T == MT deterministic self-play trajectories using a frozen
// 2-ply targetNet in deterministic game chunks (halting as soon as maxDecisions is reached so peak
// RAM and compute stay strictly bounded), computing exact backward TD(lambda) returns and 2-ply
// Expert-Iteration move Q-targets on every state visited (zero tablebase boundary).
inline std::vector<RlDecisionSample>
collectRlSelfPlayDecisions(const TinyNextBoxNet &targetNet, const RlRolloutConfig &cfg,
                           RlRolloutStats *statsOut = nullptr) {
  if (statsOut != nullptr) {
    *statsOut = RlRolloutStats{};
  }
  if (!targetNet.isLoaded()
      || cfg.numGames <= 0
      || cfg.maxStepsPerGame <= 0
      || cfg.maxDecisions <= 0) {
    return {};
  }

  const int numGames = cfg.numGames;
  const int threads = std::max(1, cfg.numThreads);
  const double eps = std::isfinite(cfg.epsilon) ? std::clamp(cfg.epsilon, 0.0, 1.0) : 0.0;
  const size_t cap = static_cast<size_t>(cfg.maxDecisions);
  const int chunkGames = std::max(32, threads * 4);

  std::vector<RlDecisionSample> merged;
  merged.reserve(cap);
  std::vector<int> allGameLines;
  allGameLines.reserve(static_cast<size_t>(numGames));
  std::array<uint64_t, NUM_HOLE_BUCKETS> byHoles{};
  int totalCapHits = 0;

  std::vector<std::vector<RlDecisionSample>> chunkPerGame(static_cast<size_t>(chunkGames));
  std::vector<int> chunkLines(static_cast<size_t>(chunkGames), 0);
  std::vector<uint8_t> chunkCapHit(static_cast<size_t>(chunkGames), 0);

  for (int baseG = 0; baseG < numGames && merged.size() < cap; baseG += chunkGames) {
    const int batchCount = std::min(chunkGames, numGames - baseG);
    for (int i = 0; i < batchCount; ++i) {
      chunkPerGame[static_cast<size_t>(i)].clear();
      chunkLines[static_cast<size_t>(i)] = 0;
      chunkCapHit[static_cast<size_t>(i)] = 0;
    }

    std::atomic<int> nextLocal{0};
    detail::runWorkerPool(
        threads, static_cast<size_t>(batchCount), /*minParallelWork=*/4, [&](int /*tid*/) {
          TransitionList tl;
          TransitionList tl2;
          Eval2PlyCache cache;
          cache.init();
          std::vector<RlTrajectoryStep> traj;
          std::vector<double> returns;
          std::vector<std::pair<double, int>> ranked;
          ranked.reserve(MAX_TRANSITIONS);

          while (true) {
            const int li = nextLocal.fetch_add(1, std::memory_order_relaxed);
            if (li >= batchCount) {
              break;
            }
            const int g = baseG + li;
            Rng rng(detail::deriveGameSeed(cfg.baseSeed, g));
            uint64_t board = 0ULL;
            int cur = rng.nextOpeningPiece(cfg.rngMode);
            int nxt = rng.nextPiece(cfg.rngMode, cur);
            int cleared = 0;
            bool hitCap = false;
            traj.clear();

            for (int step = 0; step < cfg.maxStepsPerGame; ++step) {
              generateTransitions(board, cur, tl);
              if (tl.empty()) {
                break;
              }

              ranked.clear();
              std::vector<RlMoveTarget> moveTargets;
              if (cfg.collectMoveTargets) {
                moveTargets.reserve(static_cast<size_t>(tl.size()));
              }

              for (int m = 0; m < tl.size(); ++m) {
                const double q2 =
                    static_cast<double>(tl[m].linesCleared)
                    + cache.bestReply(tl[m].afterBoard, nxt, targetNet, cfg.rngMode, tl2);
                ranked.emplace_back(q2, m);
                if (cfg.collectMoveTargets) {
                  moveTargets.push_back(RlMoveTarget{tl[m].afterBoard, tl[m].linesCleared, q2});
                }
              }

              std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
                return (a.first != b.first) ? (a.first > b.first) : (a.second < b.second);
              });

              int chosenIdx = ranked[0].second;
              if (eps > 0.0 && tl.size() >= 2 && rng.nextDouble() < eps) {
                const int topK = std::min<int>(4, tl.size());
                if (topK == 2 || rng.nextDouble() < 0.5) {
                  chosenIdx = ranked[1].second;
                } else {
                  const int pickRank =
                      1 + static_cast<int>(rng.nextU64() % static_cast<uint64_t>(topK - 1));
                  chosenIdx = ranked[static_cast<size_t>(pickRank)].second;
                }
              }

              const Transition &chosen = tl[chosenIdx];
              const double bootNext =
                  std::max(0.0, static_cast<double>(targetNet.evalPiece(chosen.afterBoard, nxt)));

              RlTrajectoryStep st{};
              st.board = board;
              st.curPiece = cur;
              st.nextPiece = nxt;
              st.linesCleared = chosen.linesCleared;
              st.bootstrapNextV = bootNext;
              st.moves = std::move(moveTargets);
              traj.push_back(std::move(st));

              cleared += chosen.linesCleared;
              board = chosen.afterBoard;
              cur = nxt;
              nxt = rng.nextPiece(cfg.rngMode, cur);
              if (step + 1 == cfg.maxStepsPerGame && hasAnyLegalTransition(board, cur)) {
                hitCap = true;
              }
            }

            const double termBoot =
                hitCap ? std::max(0.0, static_cast<double>(targetNet.evalPiece(board, cur))) : 0.0;
            computeTdLambdaReturns(traj, cfg.lambda, hitCap, termBoot, returns);

            auto &dst = chunkPerGame[static_cast<size_t>(li)];
            dst.reserve(traj.size());
            for (size_t t = 0; t < traj.size(); ++t) {
              RlDecisionSample sample{};
              sample.board = traj[t].board;
              sample.curPiece = traj[t].curPiece;
              sample.nextPiece = traj[t].nextPiece;
              sample.tdReturn = returns[t];
              sample.moves = std::move(traj[t].moves);
              dst.push_back(std::move(sample));
            }
            chunkLines[static_cast<size_t>(li)] = cleared;
            chunkCapHit[static_cast<size_t>(li)] = hitCap ? 1U : 0U;
          }
        });

    for (int li = 0; li < batchCount; ++li) {
      allGameLines.push_back(chunkLines[static_cast<size_t>(li)]);
      if (chunkCapHit[static_cast<size_t>(li)] != 0U) {
        ++totalCapHits;
      }
      auto &v = chunkPerGame[static_cast<size_t>(li)];
      for (auto &s : v) {
        if (merged.size() >= cap) {
          break;
        }
        ++byHoles[static_cast<size_t>(HoleStratifiedCounts::bucket(countHoles(s.board)))];
        merged.push_back(std::move(s));
      }
      std::vector<RlDecisionSample>().swap(v);
    }
  }

  if (statsOut != nullptr) {
    statsOut->games = static_cast<int>(allGameLines.size());
    statsOut->totalDecisionsCollected = merged.size();
    statsOut->decisionsByHoles = byHoles;
    statsOut->stepCapHits = totalCapHits;
    const auto dist = detail::computeDistributionStats(std::move(allGameLines));
    statsOut->meanLines = dist.mean;
    statsOut->stdErrLines = dist.stdErr;
  }
  return merged;
}

struct RlLossConfig {
  double huberDelta = 2.0;      // Huber threshold in lines
  double mcWeight = 1.0;        // Weight on single-head TD(lambda) Huber value loss
  double exitChildWeight = 1.0; // Weight on 2-ply ExIt candidate-child Huber value loss
  double exitRankWeight = 4.0;  // Weight on 2-ply ExIt Boltzmann ranking CE loss
  double temperature = 1.0;     // Boltzmann temperature in lines
  double weightDecay = 1e-5;    // L2 regularization on weight matrices
};

struct RlLossBreakdown {
  double totalLoss = 0.0;
  double mcLoss = 0.0;
  double exitChildLoss = 0.0;
  double exitRankLoss = 0.0;
};

// Computes exact self-play RL loss & parameter gradients over indices[start..end):
//   L = mcWeight        * Huber(V_hat(b_t, c_t) - G_t^(lambda))
//     + exitChildWeight * mean_m Huber(V_hat(b'_m, n_t) - (Q_2ply(m) - r_m))
//     + exitRankWeight  * CE(softmax(Q_2ply / T), softmax((r_m + V_hat(b'_m, n_t)) / T))
//     + 0.5 * weightDecay * ||W||^2
inline RlLossBreakdown
computeRlSliceLossAndGrad(const TinyNextBoxNet &net, const std::vector<RlDecisionSample> &decisions,
                          const std::vector<size_t> &indices, size_t start, size_t end,
                          const RlLossConfig &cfg, std::vector<float> &outGrad,
                          NnBatchScratch &scratch, bool includeWeightDecay = true) {
  assert(net.isLoaded());
  outGrad.assign(net.params.size(), 0.0f);
  RlLossBreakdown out{};
  if (!net.isLoaded()
      || start >= end
      || end > indices.size()
      || !std::isfinite(cfg.huberDelta)
      || cfg.huberDelta <= 0.0
      || !std::isfinite(cfg.temperature)
      || cfg.temperature <= 0.0
      || !std::isfinite(cfg.mcWeight)
      || cfg.mcWeight < 0.0
      || !std::isfinite(cfg.exitChildWeight)
      || cfg.exitChildWeight < 0.0
      || !std::isfinite(cfg.exitRankWeight)
      || cfg.exitRankWeight < 0.0
      || !std::isfinite(cfg.weightDecay)
      || cfg.weightDecay < 0.0) {
    return out;
  }

  const size_t count = end - start;
  const double invCount = 1.0 / static_cast<double>(count);
  const double invTemp = 1.0 / cfg.temperature;
  const double delta = cfg.huberDelta;

  float xParent[NN_INPUT_DIM];
  float a1Parent[NN_MAX_HIDDEN_DIM];
  float a2Parent[NN_MAX_HIDDEN_DIM];

  double mcLossSum = 0.0;
  double exitChildLossSum = 0.0;
  double exitRankLossSum = 0.0;

  for (size_t idxPos = start; idxPos < end; ++idxPos) {
    const size_t decIdx = indices[idxPos];
    if (decIdx >= decisions.size()) {
      continue;
    }
    const RlDecisionSample &d = decisions[decIdx];

    // 1. Single-head TD(lambda) / Monte Carlo Huber supervision on V_hat(b_t, c_t).
    if (cfg.mcWeight > 0.0
        && d.curPiece >= 0
        && d.curPiece < NUM_PIECES
        && std::isfinite(d.tdReturn)) {
      extractNnFeatures(d.board, xParent);
      net.forwardToH2(xParent, a1Parent, a2Parent);
      const float pred = net.dotOutputRow(d.curPiece, a2Parent);
      const double err = static_cast<double>(pred) - d.tdReturn;
      double deriv = 0.0;
      mcLossSum += detail::huberLossAndGrad(err, delta, deriv);
      const float gOut = static_cast<float>(cfg.mcWeight * invCount * deriv);
      net.accumulateBackwardSingleOutput(xParent, a1Parent, a2Parent, d.curPiece, gOut,
                                         outGrad.data());
    }

    // 2. 2-ply Expert-Iteration candidate-child value + Boltzmann ranking supervision.
    detail::accumulateCandidateMoveLossAndGrad(
        net, d.moves, d.nextPiece, invCount, invTemp, delta, cfg.exitChildWeight,
        cfg.exitRankWeight, scratch, outGrad, exitChildLossSum, exitRankLossSum,
        [](const RlMoveTarget &mv) noexcept { return mv.q2Ply; });
  }

  out.mcLoss = mcLossSum * invCount;
  out.exitChildLoss = exitChildLossSum * invCount;
  out.exitRankLoss = exitRankLossSum * invCount;
  out.totalLoss = cfg.mcWeight * out.mcLoss
                  + cfg.exitChildWeight * out.exitChildLoss
                  + cfg.exitRankWeight * out.exitRankLoss;

  if (includeWeightDecay) {
    out.totalLoss += detail::addL2WeightDecay(net, cfg.weightDecay, outGrad);
  }
  return out;
}

// Polyak EMA target-network update: target = (1 - tau) * target + tau * online.
// When tau >= 1.0, performs an exact copy. No-op on unloaded/mismatched networks or invalid tau.
inline void updateTargetNetEma(TinyNextBoxNet &targetNet, const TinyNextBoxNet &onlineNet,
                               double tau) {
  if (!targetNet.isLoaded()
      || !onlineNet.isLoaded()
      || targetNet.params.size() != onlineNet.params.size()
      || !std::isfinite(tau)
      || tau <= 0.0) {
    return;
  }
  if (tau >= 1.0) {
    targetNet.params = onlineNet.params;
    return;
  }
  const double keep = 1.0 - tau;
  for (size_t i = 0; i < targetNet.params.size(); ++i) {
    targetNet.params[i] = static_cast<float>(keep * static_cast<double>(targetNet.params[i])
                                             + tau * static_cast<double>(onlineNet.params[i]));
  }
}

} // namespace tb

#endif // TABLEBASE_RL_FINETUNE_HPP
