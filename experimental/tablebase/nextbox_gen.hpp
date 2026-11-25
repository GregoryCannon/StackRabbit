#ifndef TABLEBASE_NEXTBOX_GEN_HPP
#define TABLEBASE_NEXTBOX_GEN_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "nextbox_solver.hpp"
#include "state_generator.hpp"
#include "tablebase_io.hpp"
#include "vi_solver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace tb {

enum class BoardPriorityMode {
  Visits,
  NnError,
  HoleBoost,
};

inline constexpr double DEFAULT_PRIORITY_ERR_CAP = 10.0;
inline constexpr std::array<double, NUM_HOLE_BUCKETS> HOLE_BOOST_WEIGHTS = {0.0, 1.0, 2.0, 1.5};
static_assert(NUM_HOLE_BUCKETS == 4, "HOLE_BOOST_WEIGHTS expects 4 hole buckets (0, 1, 2, 3+)");

constexpr bool parseBoardPriorityMode(std::string_view s, BoardPriorityMode &out) noexcept {
  if (s == "visits") {
    out = BoardPriorityMode::Visits;
    return true;
  }
  if (s == "nn-error") {
    out = BoardPriorityMode::NnError;
    return true;
  }
  if (s == "hole-boost") {
    out = BoardPriorityMode::HoleBoost;
    return true;
  }
  return false;
}

constexpr const char *boardPriorityModeName(BoardPriorityMode mode) noexcept {
  switch (mode) {
  case BoardPriorityMode::Visits:
    return "visits";
  case BoardPriorityMode::NnError:
    return "nn-error";
  case BoardPriorityMode::HoleBoost:
    return "hole-boost";
  }
  return "visits";
}

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
  BoardPriorityMode priorityMode = BoardPriorityMode::Visits;
  double nnErrorWeight = 1.0;                       // Error weight (1/lines) in nn-error mode
  double priorityErrCap = DEFAULT_PRIORITY_ERR_CAP; // Max |E[V_tb] - E[V_nn]| counted in nn-error
  double frontierBoost = 2.0;    // Multiplier bonus for out-of-TB frontier boards
  double holeBoostScale = 1.0;   // Dimensionless hole-bucket boost scale
  size_t protectedCore = 500000; // Top-K most-visited boards always kept in prioritized modes
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

struct PrioritizedBoard {
  static constexpr uint8_t TIER_EMPTY = 0;
  static constexpr uint8_t TIER_PRIOR = 1;
  static constexpr uint8_t TIER_CORE = 2;
  static constexpr uint8_t TIER_SCORED = 3;

  uint64_t board;
  double score;
  uint32_t visits;
  uint8_t tier;
  bool siblingOnly;
};
static_assert(sizeof(PrioritizedBoard) == 24, "PrioritizedBoard must stay tightly packed at 24B");

// Promotes up to coreBudget of the highest-visit unprotected boards into TIER_CORE, returning how
// many boards were promoted. Uses (visits desc, board asc) for thread-independent determinism.
inline size_t markProtectedCore(std::vector<PrioritizedBoard> &items, size_t coreBudget) {
  if (coreBudget == 0) {
    return 0;
  }
  assert(items.size() <= static_cast<size_t>(UINT32_MAX));
  std::vector<uint32_t> eligible;
  eligible.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].tier == PrioritizedBoard::TIER_SCORED) {
      eligible.push_back(static_cast<uint32_t>(i));
    }
  }
  const size_t kCore = std::min(coreBudget, eligible.size());
  if (kCore == 0) {
    return 0;
  }
  auto byVisitsDesc = [&](uint32_t a, uint32_t b) noexcept {
    if (items[a].visits != items[b].visits) {
      return items[a].visits > items[b].visits;
    }
    return items[a].board < items[b].board;
  };
  if (kCore < eligible.size()) {
    std::nth_element(eligible.begin(), eligible.begin() + static_cast<std::ptrdiff_t>(kCore),
                     eligible.end(), byVisitsDesc);
  }
  for (size_t i = 0; i < kCore; ++i) {
    items[eligible[i]].tier = PrioritizedBoard::TIER_CORE;
  }
  return kCore;
}

template <typename EvalNnExpectedFn>
inline double scorePrioritizedBoard(const PrioritizedBoard &pb, const NextBoxGenConfig &cfg,
                                    const IndexedNextBoxTablebase *fastTb, bool tbActive,
                                    double errWeight, double errCap, double fBoost,
                                    double holeScale, EvalNnExpectedFn &&evalNnExpected) {
  const double vCount = static_cast<double>(std::max<uint32_t>(1U, pb.visits));
  if (cfg.priorityMode == BoardPriorityMode::NnError) {
    float tbRow[NUM_PIECES];
    if (tbActive && fastTb->lookup(pb.board, tbRow)) {
      const double tbExp = expectedValueFromRow(tbRow, cfg.rngMode);
      const double nnExp = evalNnExpected(pb.board);
      const double err = (std::isfinite(tbExp) && std::isfinite(nnExp))
                             ? std::min(errCap, std::abs(tbExp - nnExp))
                             : 0.0;
      return vCount * (1.0 + errWeight * err);
    }
    return vCount * (1.0 + fBoost);
  }
  // HoleBoost: emphasize salvageable 1-2 hole tactical recovery boards over easy 0-hole boards
  // that the neural fallback already evaluates accurately.
  const int hb = HoleStratifiedCounts::bucket(countHoles(pb.board));
  return vCount * (1.0 + holeScale * HOLE_BOOST_WEIGHTS[static_cast<size_t>(hb)]);
}

// Deterministic prioritized merge, filter, and cap under maxBoards (consumes rawEntries in-place):
//   - PriorityMode::Visits delegates directly to mergeFilterAndCapBoards (bit-for-bit identical).
//   - PriorityMode::NnError and PriorityMode::HoleBoost preserve:
//       Tier 0: Empty board 0ULL,
//       Tier 1: Protected priorBoards (when merged),
//       Tier 2: Top protectedCore most-visited remaining boards (by visits desc, board asc),
//       Tier 3: Remaining slots by priorityScore desc, visits desc, board asc.
//   - Produces bit-for-bit identical sortedBoards across thread counts.
template <typename EvalNnExpectedFn>
inline void mergeFilterAndCapBoardsPrioritized(
    std::vector<std::pair<uint64_t, uint32_t>> &rawEntries,
    const std::vector<uint64_t> &siblingCandidates, const std::vector<uint64_t> *priorBoards,
    const NextBoxGenConfig &cfg, const IndexedNextBoxTablebase *fastTb,
    EvalNnExpectedFn &&evalNnExpected, StateGenResult &result) {
  if (cfg.priorityMode == BoardPriorityMode::Visits) {
    mergeFilterAndCapBoards(rawEntries, siblingCandidates, priorBoards, cfg.minVisits,
                            cfg.maxBoards, result);
    return;
  }

  std::vector<MergedBoardCandidate> merged =
      collectMergedBoardCandidates(rawEntries, siblingCandidates, priorBoards, cfg.minVisits);

  // If no capping is needed, finalize directly from compact 16B MergedBoardCandidate records.
  if (cfg.maxBoards == 0 || merged.size() <= cfg.maxBoards) {
    finalizeMergedBoards(
        merged, /*needsResort=*/false,
        [](const MergedBoardCandidate &e) noexcept { return e.protectedPrior; }, result);
    return;
  }

  std::vector<PrioritizedBoard> items;
  items.reserve(merged.size());
  size_t fixedProtected = 0;
  for (const auto &c : merged) {
    uint8_t tier = PrioritizedBoard::TIER_SCORED;
    if (c.board == 0ULL) {
      tier = PrioritizedBoard::TIER_EMPTY;
      ++fixedProtected;
    } else if (c.protectedPrior) {
      tier = PrioritizedBoard::TIER_PRIOR;
      ++fixedProtected;
    }
    items.push_back({c.board, 0.0, c.visits, tier, c.siblingOnly});
  }
  std::vector<MergedBoardCandidate>().swap(merged);

  size_t kCore = 0;
  if (fixedProtected < cfg.maxBoards && cfg.protectedCore > 0) {
    const size_t coreBudget = std::min(cfg.protectedCore, cfg.maxBoards - fixedProtected);
    kCore = markProtectedCore(items, coreBudget);
  }

  // Only score unprotected candidates when at least one prioritized slot remains after fixed &
  // protected-core reservations.
  if (fixedProtected + kCore < cfg.maxBoards) {
    const double errWeight =
        std::isfinite(cfg.nnErrorWeight) ? std::max(0.0, cfg.nnErrorWeight) : 0.0;
    const double errCap = std::isfinite(cfg.priorityErrCap) ? std::max(0.0, cfg.priorityErrCap)
                                                            : DEFAULT_PRIORITY_ERR_CAP;
    const double fBoost = std::isfinite(cfg.frontierBoost) ? std::max(0.0, cfg.frontierBoost) : 0.0;
    const double holeScale =
        std::isfinite(cfg.holeBoostScale) ? std::max(0.0, cfg.holeBoostScale) : 0.0;
    const bool tbActive = (fastTb != nullptr && fastTb->isLoaded());

    const size_t total = items.size();
    constexpr size_t SCORE_CHUNK = 4096;
    const size_t numChunks = (total + SCORE_CHUNK - 1) / SCORE_CHUNK;
    const int threads = static_cast<int>(std::min<size_t>(
        static_cast<size_t>(std::max(1, cfg.numThreads)), std::max<size_t>(1, numChunks)));
    std::atomic<size_t> nextIdx{0};
    runWorkerPool(threads, total, /*minParallelWork=*/SCORE_CHUNK + 1, [&](int /*tid*/) {
      while (true) {
        const size_t s = nextIdx.fetch_add(SCORE_CHUNK, std::memory_order_relaxed);
        if (s >= total) {
          break;
        }
        const size_t e = std::min(total, s + SCORE_CHUNK);
        for (size_t i = s; i < e; ++i) {
          PrioritizedBoard &pb = items[i];
          if (pb.tier != PrioritizedBoard::TIER_SCORED) {
            continue;
          }
          pb.score = scorePrioritizedBoard(pb, cfg, fastTb, tbActive, errWeight, errCap, fBoost,
                                           holeScale, evalNnExpected);
        }
      }
    });
  }

  std::nth_element(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(cfg.maxBoards),
                   items.end(), [](const PrioritizedBoard &a, const PrioritizedBoard &b) noexcept {
                     if (a.tier != b.tier) {
                       return a.tier < b.tier;
                     }
                     if (a.tier == PrioritizedBoard::TIER_SCORED && a.score != b.score) {
                       return a.score > b.score;
                     }
                     if (a.visits != b.visits) {
                       return a.visits > b.visits;
                     }
                     return a.board < b.board;
                   });
  items.resize(cfg.maxBoards);

  finalizeMergedBoards(
      items, /*needsResort=*/true,
      [](const PrioritizedBoard &e) noexcept { return e.tier == PrioritizedBoard::TIER_PRIOR; },
      result);
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
template <typename RolloutMoveFn, typename EvalNnExpectedFn>
inline StateGenResult generateNextBoxStateSpaceWithPolicy(const NextBoxGenConfig &cfg,
                                                          const MappedNextBoxTablebase *tb,
                                                          const std::vector<uint64_t> *priorBoards,
                                                          RolloutMoveFn &&rolloutMoveFn,
                                                          EvalNnExpectedFn &&evalNnExpected) {
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
  if (cfg.priorityMode != BoardPriorityMode::NnError) {
    indexedTb.clear();
    fastTb = nullptr;
  }
  detail::mergeFilterAndCapBoardsPrioritized(rawEntries, siblingCandidates, priorBoards, cfg,
                                             fastTb, evalNnExpected, result);
  indexedTb.clear();

  result.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

template <typename RolloutMoveFn>
inline StateGenResult generateNextBoxStateSpaceWithPolicy(const NextBoxGenConfig &cfg,
                                                          const MappedNextBoxTablebase *tb,
                                                          const std::vector<uint64_t> *priorBoards,
                                                          RolloutMoveFn &&rolloutMoveFn) {
  assert(cfg.priorityMode != BoardPriorityMode::NnError
         && "NnError priority mode requires the 5-arg overload with an NN expectation callback");
  return generateNextBoxStateSpaceWithPolicy(cfg, tb, priorBoards,
                                             std::forward<RolloutMoveFn>(rolloutMoveFn),
                                             [](uint64_t) noexcept { return 0.0; });
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
inline CoverageReport measureNextBoxCoverageWithPolicy(
    const std::vector<uint64_t> &sortedBoards, int numGames, uint64_t baseSeed, RngMode rngMode,
    const MappedNextBoxTablebase *tb, int numThreads, GreedyMoveFn &&greedyMoveFn,
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
