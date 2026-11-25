#ifndef TABLEBASE_DISTILL_DATA_HPP
#define TABLEBASE_DISTILL_DATA_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "nextbox_gen.hpp"
#include "nextbox_solver.hpp"
#include "vi_solver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace tb {

struct LabeledMoveCandidate {
  uint64_t afterBoard = 0;
  uint8_t linesCleared = 0;
  double qStar = 0.0;         // Exact Next-Box Q*(b, c, n, m) = r + V*(b', n)
  double expectedQStar = 0.0; // Piece-averaged r + E_rng[V*(b')] (noise-free target for V(b))
};

struct LabeledDecisionSample {
  uint64_t board = 0;
  int curPiece = 0;
  int nextPiece = 0;
  bool boardInTb = false;
  std::array<float, NUM_PIECES> vStar{}; // Exact V*(board, 0..6) for piece-conditioned models
  double expectedVStar = 0.0;
  double bestQStar = 0.0;
  double bestExpectedQStar = 0.0;
  std::vector<LabeledMoveCandidate> moves;
};

struct DistillSampleConfig {
  int numGames = 2000;
  int maxDecisions = 200000;
  int maxPiecesPerGame = DEFAULT_MAX_PIECES_PER_GAME;
  double recoverFrac = 0.30;    // Fraction of games using epsilon-perturbed recovery rollouts
  double recoverEpsilon = 0.08; // Exploration rate during recovery games
  int exploreTopK = 4;
  uint64_t baseSeed = 20000000ULL;
  int numThreads = 1;
  bool requireAllMovesInTb = false; // If false, keeps all in-TB moves when >= 2 in-TB moves exist
  // Include out-of-TB moves where nextPiece tops out (exact qStar = linesCleared; expectedQStar
  // is approximate if other pieces still fit, so keep false for board-only linear training).
  bool includeTerminalOutMoves = false;
  EvalWeights weights{};
};

struct DistillDataset {
  std::vector<LabeledDecisionSample> decisions;
  uint64_t totalStepsSimulated = 0;
  uint64_t skippedIncomplete = 0;
  std::array<uint64_t, NUM_HOLE_BUCKETS> decisionsByHoles{};
};

struct RankingEvalMetrics {
  size_t numDecisions = 0;
  double top1ExactPct = 0.0; // % picking a move within 1e-5 of bestQStar
  double meanRegret = 0.0;   // Mean (bestQStar - pickedQStar) in lines/move
  double p95Regret = 0.0;
  double maxRegret = 0.0;
  std::array<double, NUM_HOLE_BUCKETS> meanRegretByHoles{};
  std::array<uint64_t, NUM_HOLE_BUCKETS> countByHoles{};
};

// Fast early-exit check equivalent to !generateTransitions(board, pieceIdx, tmp).empty().
// Checks straight drops first (returning immediately on the first ceiling-fitting lock), and
// falls back to full generateTransitions only on the rare tuck-only / top-out boards.
inline bool hasAnyLegalTransition(uint64_t board, int pieceIdx) noexcept {
  if (pieceIdx < 0 || pieceIdx >= NUM_PIECES) {
    return false;
  }
  const int numRots = PIECES[pieceIdx].numRots;
  for (int rot = 0; rot < numRots; ++rot) {
    for (int xIdx = 0; xIdx < NUM_X_SLOTS; ++xIdx) {
      const auto &colGeom = detail::GEOMETRY[pieceIdx][rot][xIdx];
      if (!colGeom[0].inBounds) {
        continue;
      }
      int y = 0;
      while (!collides(board, colGeom[y + 1])) {
        ++y;
      }
      if (colGeom[y].fitsCeiling) {
        return true;
      }
    }
  }
  TransitionList tmp;
  generateTransitions(board, pieceIdx, tmp);
  return !tmp.empty();
}

// Samples a deterministic (thread-count invariant) dataset of labeled Next-Box decision points
// directly from self-play rollouts of a solved IndexedNextBoxTablebase.
// Mixes on-policy greedy games (1 - recoverFrac) with epsilon-perturbed games (recoverFrac) so
// training reflects true strong-play visitation while still covering 1..3+-hole recoveries.
// Out-of-TB moves are excluded so ranking labels have zero V=0 boundary bias (except optional
// exact terminal moves where nextPiece tops out when includeTerminalOutMoves is enabled).
template <typename NextBoxTbType>
inline DistillDataset sampleDistillDataset(const NextBoxTbType &tb,
                                           const DistillSampleConfig &cfg) {
  assert(tb.isLoaded());
  assert(cfg.numGames >= 0);
  assert(cfg.maxDecisions >= 0);

  DistillDataset out{};
  if (!tb.isLoaded() || cfg.numGames <= 0 || cfg.maxDecisions <= 0) {
    return out;
  }

  const RngMode rngMode = tb.rngMode();
  const int threads = std::max(1, cfg.numThreads);
  const double clampedRecoverFrac =
      std::isfinite(cfg.recoverFrac) ? std::clamp(cfg.recoverFrac, 0.0, 1.0) : 0.0;
  const int recoverMod =
      std::clamp(static_cast<int>(std::lround(clampedRecoverFrac * 100.0)), 0, 100);
  // Process in deterministic game-index chunks so memory and work stop once maxDecisions is met.
  const int chunkGames = std::max(64, threads * 8);

  out.decisions.reserve(static_cast<size_t>(cfg.maxDecisions));
  std::vector<std::vector<LabeledDecisionSample>> perGame(static_cast<size_t>(chunkGames));
  std::vector<uint64_t> perGameSteps(static_cast<size_t>(chunkGames), 0);
  std::vector<uint64_t> perGameSkipped(static_cast<size_t>(chunkGames), 0);

  for (int baseG = 0;
       baseG < cfg.numGames && static_cast<int>(out.decisions.size()) < cfg.maxDecisions;
       baseG += chunkGames) {
    const int batchCount = std::min(chunkGames, cfg.numGames - baseG);
    for (int i = 0; i < batchCount; ++i) {
      perGame[static_cast<size_t>(i)].clear();
      perGameSteps[static_cast<size_t>(i)] = 0;
      perGameSkipped[static_cast<size_t>(i)] = 0;
    }

    std::atomic<int> nextLocal{0};
    detail::runWorkerPool(
        threads, static_cast<size_t>(batchCount), /*minParallelWork=*/16, [&](int /*tid*/) {
          TransitionList tl;
          float row[NUM_PIECES];
          float childRow[NUM_PIECES];
          while (true) {
            const int local = nextLocal.fetch_add(1, std::memory_order_relaxed);
            if (local >= batchCount) {
              break;
            }
            const int g = baseG + local;
            const bool isRecover = ((g % 100) < recoverMod);
            const double eps = isRecover ? cfg.recoverEpsilon : 0.0;
            Rng rng(detail::deriveGameSeed(cfg.baseSeed, g));

            int curPiece = rng.nextOpeningPiece(rngMode);
            int nextPiece = rng.nextPiece(rngMode, curPiece);
            uint64_t board = 0ULL;
            auto &gameVec = perGame[static_cast<size_t>(local)];
            uint64_t steps = 0;
            uint64_t skipped = 0;

            for (int step = 0; step < cfg.maxPiecesPerGame; ++step) {
              generateTransitions(board, curPiece, tl);
              if (tl.empty()) {
                break;
              }

              bool allInTb = true;
              double bestQ = -std::numeric_limits<double>::infinity();
              double minQ = std::numeric_limits<double>::infinity();
              double bestExpQ = -std::numeric_limits<double>::infinity();
              std::vector<LabeledMoveCandidate> cands;
              cands.reserve(static_cast<size_t>(tl.size()));

              auto pushCand = [&](uint64_t afterB, uint8_t lc, double q, double expQ) {
                cands.push_back({afterB, lc, q, expQ});
                if (q > bestQ) {
                  bestQ = q;
                }
                if (q < minQ) {
                  minQ = q;
                }
                if (expQ > bestExpQ) {
                  bestExpQ = expQ;
                }
              };

              for (int m = 0; m < tl.size(); ++m) {
                const bool inTb = tb.lookup(tl[m].afterBoard, childRow);
                if (!inTb) {
                  allInTb = false;
                  if (cfg.includeTerminalOutMoves
                      && !hasAnyLegalTransition(tl[m].afterBoard, nextPiece)) {
                    const double rew = static_cast<double>(tl[m].linesCleared);
                    pushCand(tl[m].afterBoard, tl[m].linesCleared, rew, rew);
                  }
                  continue;
                }
                const double rew = static_cast<double>(tl[m].linesCleared);
                const double q = rew + static_cast<double>(childRow[nextPiece]);
                const double expQ = rew + expectedValueFromRow(childRow, rngMode);
                pushCand(tl[m].afterBoard, tl[m].linesCleared, q, expQ);
              }

              if ((cfg.requireAllMovesInTb && !allInTb) || cands.size() < 2) {
                ++skipped;
              } else if (bestQ > minQ + 1e-9) {
                // Omit zero-spread all-tied decisions (uninformative for ranking; not incomplete).
                LabeledDecisionSample sample{};
                sample.board = board;
                sample.curPiece = curPiece;
                sample.nextPiece = nextPiece;
                sample.boardInTb = tb.lookup(board, row);
                if (sample.boardInTb) {
                  for (int p = 0; p < NUM_PIECES; ++p) {
                    sample.vStar[static_cast<size_t>(p)] = row[p];
                  }
                  sample.expectedVStar = expectedValueFromRow(row, rngMode);
                }
                sample.bestQStar = bestQ;
                sample.bestExpectedQStar = bestExpQ;
                sample.moves = std::move(cands);
                gameVec.push_back(std::move(sample));
              }

              const ChosenTransition ch =
                  (eps > 0.0) ? selectNextBoxRolloutTransition(board, curPiece, nextPiece, rng, &tb,
                                                               eps, cfg.exploreTopK, cfg.weights)
                              : detail::chooseNextBoxGreedyPolicyMove(board, curPiece, nextPiece,
                                                                      &tb, cfg.weights);
              if (!ch.found) {
                break;
              }
              ++steps;
              board = ch.transition.afterBoard;
              curPiece = nextPiece;
              nextPiece = rng.nextPiece(rngMode, curPiece);
            }

            perGameSteps[static_cast<size_t>(local)] = steps;
            perGameSkipped[static_cast<size_t>(local)] = skipped;
          }
        });

    for (int local = 0; local < batchCount; ++local) {
      out.totalStepsSimulated += perGameSteps[static_cast<size_t>(local)];
      out.skippedIncomplete += perGameSkipped[static_cast<size_t>(local)];
      for (auto &s : perGame[static_cast<size_t>(local)]) {
        if (static_cast<int>(out.decisions.size()) >= cfg.maxDecisions) {
          break;
        }
        const int hb = HoleStratifiedCounts::bucket(countHoles(s.board));
        ++out.decisionsByHoles[static_cast<size_t>(hb)];
        out.decisions.push_back(std::move(s));
      }
      if (static_cast<int>(out.decisions.size()) >= cfg.maxDecisions) {
        break;
      }
    }
  }
  return out;
}

// Evaluates any move-scoring function scoreMove(afterBoard, linesCleared, nextPiece) against a
// labeled decision set, reporting top-1 exact agreement and mean/p95/max regret (lines/move).
template <typename ScoreMoveFn>
inline RankingEvalMetrics evaluateMoveRanker(const std::vector<LabeledDecisionSample> &decisions,
                                             ScoreMoveFn &&scoreMove) {
  RankingEvalMetrics m{};
  if (decisions.empty()) {
    return m;
  }
  std::vector<double> regrets;
  regrets.reserve(decisions.size());

  size_t exactHits = 0;
  double sumRegret = 0.0;
  std::array<double, NUM_HOLE_BUCKETS> sumByHoles{};

  for (const auto &d : decisions) {
    if (d.moves.empty()) {
      continue;
    }
    int pickIdx = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < d.moves.size(); ++i) {
      const double sc = scoreMove(d.moves[i].afterBoard, d.moves[i].linesCleared, d.nextPiece);
      if (i == 0 || sc > bestScore) {
        bestScore = sc;
        pickIdx = static_cast<int>(i);
      }
    }
    const double regret = std::max(0.0, d.bestQStar - d.moves[static_cast<size_t>(pickIdx)].qStar);
    if (regret <= 1e-5) {
      ++exactHits;
    }
    sumRegret += regret;
    if (regret > m.maxRegret) {
      m.maxRegret = regret;
    }
    regrets.push_back(regret);

    const int hb = HoleStratifiedCounts::bucket(countHoles(d.board));
    sumByHoles[static_cast<size_t>(hb)] += regret;
    ++m.countByHoles[static_cast<size_t>(hb)];
  }

  m.numDecisions = regrets.size();
  if (regrets.empty()) {
    return m;
  }
  m.top1ExactPct = 100.0 * static_cast<double>(exactHits) / static_cast<double>(m.numDecisions);
  m.meanRegret = sumRegret / static_cast<double>(m.numDecisions);
  std::sort(regrets.begin(), regrets.end());
  const size_t p95Idx = std::min(regrets.size() - 1, (regrets.size() * 95) / 100);
  m.p95Regret = regrets[p95Idx];
  for (size_t b = 0; b < NUM_HOLE_BUCKETS; ++b) {
    m.meanRegretByHoles[b] =
        (m.countByHoles[b] > 0) ? (sumByHoles[b] / static_cast<double>(m.countByHoles[b])) : 0.0;
  }
  return m;
}

} // namespace tb

#endif // TABLEBASE_DISTILL_DATA_HPP
