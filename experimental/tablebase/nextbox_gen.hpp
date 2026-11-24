#ifndef TABLEBASE_NEXTBOX_GEN_HPP
#define TABLEBASE_NEXTBOX_GEN_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "nextbox_solver.hpp"
#include "state_generator.hpp"
#include "tablebase_io.hpp"
#include "vi_solver.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace tb {

struct NextBoxGenConfig {
  RngMode rngMode = RngMode::Nes;
  int numGames = 200000;
  int maxPiecesPerGame = DEFAULT_MAX_PIECES_PER_GAME;
  uint64_t baseSeed = 1000000ULL;
  double epsilon = 0.04;         // Probability of picking a non-greedy top-K exploration move
  int exploreTopK = 4;           // Exploration chooses uniformly among ranks 2..K (if available)
  uint32_t minVisits = 2;        // Prune rare one-off tail states below this visit count
  size_t maxBoards = 20000000;   // Maximum output state-set size (0 = unlimited)
  uint32_t siblingMinVisits = 5; // Expand greedy replies on >= M-visit core boards (0 = off)
  int numThreads = 1;
  int progressInterval = 25000;
  bool verbose = false;
  EvalWeights weights{};
};

namespace detail {

template <typename NextBoxTablebase>
inline ChosenTransition chooseNextBoxGreedyPolicyMove(uint64_t board, int curPiece, int nextPiece,
                                                      const NextBoxTablebase *tb,
                                                      const EvalWeights &w) {
  assert(nextPiece >= 0 && nextPiece < NUM_PIECES);
  if (tb != nullptr && tb->isLoaded()) {
    const TablebaseMoveChoice ch =
        chooseNextBoxTablebaseMove(board, curPiece, nextPiece, *tb, /*closedWorld=*/false, w);
    return {ch.transition, ch.tablebaseQ, ch.found};
  }
  return pickBestNextBoxHeuristic(board, curPiece, nextPiece, w);
}

// Expands greedy Next-Box sibling replies on core boards (visits >= cfg.siblingMinVisits).
// For each (board, curPiece), transitions are generated once:
//   - Always adds the 1-ply heuristic reply so promising out-of-TB frontier neighbors enter S.
//   - When a Next-Box tablebase is active, hashes each child once to read all 7 V(b', n) entries
//     and emits every distinct best Next-Box TB reply across n = 0..6 (equal-Q TB ties break in
//     first-move order for speed, while the 1-ply heuristic winner is emitted separately).
template <typename NextBoxTablebase>
inline std::vector<uint64_t>
expandNextBoxCoreSiblings(const std::vector<std::pair<uint64_t, uint32_t>> &rawEntries,
                          const NextBoxGenConfig &cfg, const NextBoxTablebase *tb) {
  const bool tbActive = (tb != nullptr && tb->isLoaded());
  return collectCoreSiblings(
      rawEntries, cfg.siblingMinVisits, cfg.numThreads,
      [&](uint64_t b, std::vector<uint64_t> &out) {
        TransitionList tl;
        for (int c = 0; c < NUM_PIECES; ++c) {
          generateTransitions(b, c, tl);
          if (tl.empty()) {
            continue;
          }

          int bestHeurIdx = 0;
          double bestHeur = -std::numeric_limits<double>::infinity();
          bool anyInTb = false;
          double bestTbQ[NUM_PIECES] = {};
          int bestTbIdx[NUM_PIECES] = {};
          float childRow[NUM_PIECES] = {};

          for (int m = 0; m < tl.size(); ++m) {
            const Transition &tr = tl[m];
            const double h = scoreTransition1Ply(tr.afterBoard, tr.linesCleared, cfg.weights);
            if (m == 0 || h > bestHeur) {
              bestHeur = h;
              bestHeurIdx = m;
            }
            if (tbActive) {
              const bool inTb = tb->lookup(tr.afterBoard, childRow);
              anyInTb = anyInTb || inTb;
              const double rew = static_cast<double>(tr.linesCleared);
              for (int next = 0; next < NUM_PIECES; ++next) {
                const double q = rew + (inTb ? static_cast<double>(childRow[next]) : 0.0);
                if (m == 0 || q > bestTbQ[next]) {
                  bestTbQ[next] = q;
                  bestTbIdx[next] = m;
                }
              }
            }
          }

          const uint64_t heurBoard = tl[bestHeurIdx].afterBoard;
          out.push_back(heurBoard);
          if (tbActive && anyInTb) {
            uint64_t emitted[NUM_PIECES + 1];
            int emittedCount = 1;
            emitted[0] = heurBoard;
            for (int next = 0; next < NUM_PIECES; ++next) {
              const uint64_t tbBoard = tl[bestTbIdx[next]].afterBoard;
              if (std::find(emitted, emitted + emittedCount, tbBoard) == emitted + emittedCount) {
                emitted[emittedCount++] = tbBoard;
                out.push_back(tbBoard);
              }
            }
          }
        }
      });
}

} // namespace detail

// Picks a Next-Box rollout move using caller-supplied greedy and exploration-score callbacks.
template <typename NextBoxTablebase, typename GreedyFn, typename ExploreScoreFn>
inline ChosenTransition
selectNextBoxRolloutWithPolicy(uint64_t board, int curPiece, int nextPiece, Rng &rng,
                               const NextBoxTablebase *tb, double epsilon, int exploreTopK,
                               GreedyFn &&pickGreedy, ExploreScoreFn &&scoreExplore) {
  assert(nextPiece >= 0 && nextPiece < NUM_PIECES);
  const bool tbActive = (tb != nullptr && tb->isLoaded());
  const bool doExplore = (epsilon > 0.0 && exploreTopK >= 2 && rng.nextDouble() < epsilon);

  if (!doExplore) {
    return pickGreedy(board, curPiece, nextPiece, tb);
  }

  TransitionList tl;
  generateTransitions(board, curPiece, tl);
  return detail::selectExploreTransition(
      tl, rng, exploreTopK, tbActive,
      [&](uint64_t b, float &outVal) { return tb->lookupPiece(b, nextPiece, outVal); },
      scoreExplore);
}

// Picks a Next-Box rollout move under the mixed (1 - epsilon) greedy + epsilon top-K exploration
// policy, splitting exploration 50/50 between TB runner-up moves and 2-ply Next-Box heuristic
// frontier probes.
template <typename NextBoxTablebase>
inline ChosenTransition selectNextBoxRolloutTransition(uint64_t board, int curPiece, int nextPiece,
                                                       Rng &rng, const NextBoxTablebase *tb,
                                                       double epsilon, int exploreTopK,
                                                       const EvalWeights &w) {
  return selectNextBoxRolloutWithPolicy(
      board, curPiece, nextPiece, rng, tb, epsilon, exploreTopK,
      [&](uint64_t b, int c, int n, const NextBoxTablebase *t) {
        return detail::chooseNextBoxGreedyPolicyMove(b, c, n, t, w);
      },
      [&](const Transition &t) {
        return scoreNextBoxHeuristicTransition(t.afterBoard, t.linesCleared, nextPiece, w);
      });
}

inline ChosenTransition selectNextBoxRolloutTransition(uint64_t board, int curPiece, int nextPiece,
                                                       Rng &rng, std::nullptr_t, double epsilon,
                                                       int exploreTopK, const EvalWeights &w) {
  return selectNextBoxRolloutTransition<MappedNextBoxTablebase>(board, curPiece, nextPiece, rng,
                                                                nullptr, epsilon, exploreTopK, w);
}

// Runs parallel Next-Box trajectory sampling using a thread-safe rollout callback
// rolloutMoveFn(board, curPiece, nextPiece, rng, fastTb), merges optional priorBoards, applies
// optional core sibling expansion, filters by minVisits and maxBoards, and returns strictly sorted
// 40-bit boards. Produces identical sortedBoards across thread counts.
template <typename RolloutMoveFn>
inline StateGenResult generateNextBoxStateSpaceWithPolicy(const NextBoxGenConfig &cfg,
                                                          const MappedNextBoxTablebase *tb,
                                                          const std::vector<uint64_t> *priorBoards,
                                                          RolloutMoveFn &&rolloutMoveFn) {
  assert(cfg.numGames >= 0);
  const auto t0 = std::chrono::steady_clock::now();

  IndexedNextBoxTablebase indexedTb;
  const IndexedNextBoxTablebase *fastTb = nullptr;
  if (tb != nullptr && tb->isLoaded()) {
    indexedTb.buildFromMapped(*tb);
    fastTb = &indexedTb;
  }

  StateGenResult result{};
  std::vector<std::pair<uint64_t, uint32_t>> rawEntries = detail::collectRolloutVisits(
      cfg.numGames, cfg.numThreads, cfg.baseSeed, cfg.verbose, cfg.progressInterval, "nb-gen", t0,
      result.totalMovesSimulated, result.meanTrainLines,
      [&](Rng &rng, FlatVisitMap &localMap, uint64_t &localMoves, uint64_t &localLines) {
        int curPiece = rng.nextOpeningPiece(cfg.rngMode);
        int nextPiece = rng.nextPiece(cfg.rngMode, curPiece);
        uint64_t board = 0ULL;
        localMap.add(board, 1);

        for (int step = 0; step < cfg.maxPiecesPerGame; ++step) {
          const ChosenTransition ch = rolloutMoveFn(board, curPiece, nextPiece, rng, fastTb);
          if (!ch.found) {
            break;
          }
          ++localMoves;
          localLines += ch.transition.linesCleared;
          board = ch.transition.afterBoard;
          localMap.add(board, 1);
          curPiece = nextPiece;
          nextPiece = rng.nextPiece(cfg.rngMode, curPiece);
        }
      });
  result.rawUniqueDiscovered = rawEntries.size();

  const std::vector<uint64_t> siblingCandidates =
      detail::expandNextBoxCoreSiblings(rawEntries, cfg, fastTb);
  indexedTb.clear();
  detail::mergeFilterAndCapBoards(rawEntries, siblingCandidates, priorBoards, cfg.minVisits,
                                  cfg.maxBoards, result);

  result.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

inline StateGenResult
generateNextBoxStateSpace(const NextBoxGenConfig &cfg, const MappedNextBoxTablebase *tb = nullptr,
                          const std::vector<uint64_t> *priorBoards = nullptr) {
  return generateNextBoxStateSpaceWithPolicy(cfg, tb, priorBoards,
                                             [&](uint64_t board, int curPiece, int nextPiece,
                                                 Rng &rng, const IndexedNextBoxTablebase *fastTb) {
                                               return selectNextBoxRolloutTransition(
                                                   board, curPiece, nextPiece, rng, fastTb,
                                                   cfg.epsilon, cfg.exploreTopK, cfg.weights);
                                             });
}

// Measures held-out greedy Next-Box rollout coverage against sortedBoards using a thread-safe
// policy callback greedyMoveFn(board, curPiece, nextPiece, fastTb), stratified by hole count.
template <typename GreedyMoveFn>
inline CoverageReport
measureNextBoxCoverageWithPolicy(const std::vector<uint64_t> &sortedBoards, int numGames,
                                 uint64_t baseSeed, RngMode rngMode,
                                 const MappedNextBoxTablebase *tb, int numThreads,
                                 GreedyMoveFn &&greedyMoveFn,
                                 int maxPiecesPerGame = DEFAULT_MAX_PIECES_PER_GAME) {
  IndexedNextBoxTablebase indexedTb;
  const IndexedNextBoxTablebase *fastTb = nullptr;
  if (tb != nullptr && tb->isLoaded()) {
    indexedTb.buildFromMapped(*tb);
    fastTb = &indexedTb;
  }

  return detail::runCoverageEval(
      sortedBoards, numGames, baseSeed, numThreads,
      [&](Rng &rng, const FlatBoardIndex &index, HoleStratifiedCounts &local, bool &allIn) {
        int curPiece = rng.nextOpeningPiece(rngMode);
        int nextPiece = rng.nextPiece(rngMode, curPiece);
        uint64_t board = 0ULL;
        int lines = 0;

        for (int step = 0; step < maxPiecesPerGame; ++step) {
          const ChosenTransition ch = greedyMoveFn(board, curPiece, nextPiece, fastTb);
          if (!ch.found) {
            break;
          }
          lines += ch.transition.linesCleared;
          board = ch.transition.afterBoard;

          const int hb = HoleStratifiedCounts::bucket(countHoles(board));
          ++local.total[static_cast<size_t>(hb)];
          if (index.find(board) != INVALID_BOARD_ID) {
            ++local.inSet[static_cast<size_t>(hb)];
          } else {
            allIn = false;
          }

          curPiece = nextPiece;
          nextPiece = rng.nextPiece(rngMode, curPiece);
        }
        return lines;
      });
}

// Measures held-out greedy Next-Box rollout coverage against sortedBoards (overall and stratified
// by afterBoard hole count 0, 1, 2, 3+).
inline CoverageReport measureNextBoxCoverage(const std::vector<uint64_t> &sortedBoards,
                                             int numGames, uint64_t baseSeed, RngMode rngMode,
                                             const MappedNextBoxTablebase *tb, int numThreads,
                                             const EvalWeights &w = EvalWeights{},
                                             int maxPiecesPerGame = DEFAULT_MAX_PIECES_PER_GAME) {
  return measureNextBoxCoverageWithPolicy(
      sortedBoards, numGames, baseSeed, rngMode, tb, numThreads,
      [&](uint64_t board, int curPiece, int nextPiece, const IndexedNextBoxTablebase *fastTb) {
        return detail::chooseNextBoxGreedyPolicyMove(board, curPiece, nextPiece, fastTb, w);
      },
      maxPiecesPerGame);
}

} // namespace tb

#endif // TABLEBASE_NEXTBOX_GEN_HPP
