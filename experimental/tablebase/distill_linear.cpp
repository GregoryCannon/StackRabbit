// distill_linear.cpp — Linear value regression, residual outlier mining, and regret-weighted
// move-ranking trainer against a solved SRTBV002 Next-Box tablebase (comparing V1 10-feature and
// residual-guided V2 26-feature models).
//
// Build:
//   g++ -O3 -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/distill_linear.cpp -o build/tablebase_distill_linear
//
// Usage:
//   ./build/tablebase_distill_linear <tablebase.tb.bin> [options]

#include "cli_utils.hpp"
#include "distill_data.hpp"
#include "distill_linear.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CliOptions {
  std::string tbPath;
  int trainGames = 1500;
  int trainDecisions = 200000;
  int valGames = 300;
  int valDecisions = 30000;
  double temperature = 1.0;
  double valueWeight = 0.02;
  int epochs = 25;
  int evalGames = 10000;
  uint64_t evalSeed = 700000ULL;
  int topOutliers = 10;
  int numThreads = 1;
  bool showHelp = false;
};

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s <nextbox.tb.bin> [options]\n", prog);
  std::fprintf(out, "  --train-games=N        Self-play games for train set (default: 1500)\n");
  std::fprintf(out, "  --train-decisions=N    Max train decisions (default: 200000)\n");
  std::fprintf(out, "  --val-games=N          Self-play games for val set (default: 300)\n");
  std::fprintf(out, "  --val-decisions=N      Max val decisions (default: 30000)\n");
  std::fprintf(out,
               "  --temp=F               Boltzmann ranking temperature in lines (default: 1.0)\n");
  std::fprintf(out, "  --val-weight=F         Joint value MSE weight (default: 0.02)\n");
  std::fprintf(out, "  --epochs=N             Ranking Adam epochs (default: 25)\n");
  std::fprintf(out, "  --eval-games=N         Held-out NES games to benchmark (default: 10000)\n");
  std::fprintf(out, "  --eval-seed=N          Held-out benchmark seed (default: 700000)\n");
  std::fprintf(out, "  --outliers=N           Residual outlier boards per tail (default: 10)\n");
  std::fprintf(out, "  --threads=T            Worker threads (default: hardware concurrency)\n");
}

bool parseArgs(int argc, char **argv, CliOptions &opts) {
  opts.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  std::set<std::string> seen;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      opts.showHelp = true;
      return true;
    }
    const size_t eq = arg.find('=');
    if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
      const std::string flag = arg.substr(0, eq);
      const std::string val = arg.substr(eq + 1);
      if (!seen.insert(flag).second) {
        std::fprintf(stderr, "Duplicate option: %s\n", flag.c_str());
        return false;
      }
      auto parseU64Flag = [&](uint64_t lo, uint64_t hi, int &dest) -> bool {
        uint64_t uVal = 0;
        if (!tb::cli::parseStrictU64(val, lo, hi, uVal)) {
          std::fprintf(stderr, "Invalid %s value '%s' (expected %llu..%llu)\n", flag.c_str(),
                       val.c_str(), static_cast<unsigned long long>(lo),
                       static_cast<unsigned long long>(hi));
          return false;
        }
        dest = static_cast<int>(uVal);
        return true;
      };
      auto parseDoubleFlag = [&](double lo, double hi, double &dest) -> bool {
        double dVal = 0.0;
        if (!tb::cli::parseStrictPositiveDouble(val, lo, hi, dVal)) {
          std::fprintf(stderr, "Invalid %s value '%s' (expected %.2f..%.2f)\n", flag.c_str(),
                       val.c_str(), lo, hi);
          return false;
        }
        dest = dVal;
        return true;
      };

      if (flag == "--train-games") {
        if (!parseU64Flag(1, 500000ULL, opts.trainGames))
          return false;
      } else if (flag == "--train-decisions") {
        if (!parseU64Flag(100, 10000000ULL, opts.trainDecisions))
          return false;
      } else if (flag == "--val-games") {
        if (!parseU64Flag(1, 100000ULL, opts.valGames))
          return false;
      } else if (flag == "--val-decisions") {
        if (!parseU64Flag(100, 2000000ULL, opts.valDecisions))
          return false;
      } else if (flag == "--temp") {
        if (!parseDoubleFlag(0.05, 50.0, opts.temperature))
          return false;
      } else if (flag == "--val-weight") {
        if (!parseDoubleFlag(0.0, 10.0, opts.valueWeight))
          return false;
      } else if (flag == "--epochs") {
        if (!parseU64Flag(1, 500ULL, opts.epochs))
          return false;
      } else if (flag == "--eval-games") {
        if (!parseU64Flag(0, 100000ULL, opts.evalGames))
          return false;
      } else if (flag == "--eval-seed") {
        uint64_t uVal = 0;
        if (!tb::cli::parseStrictU64(val, 0, UINT64_MAX, uVal)) {
          std::fprintf(stderr, "Invalid --eval-seed value '%s'\n", val.c_str());
          return false;
        }
        opts.evalSeed = uVal;
      } else if (flag == "--outliers") {
        if (!parseU64Flag(0, 100ULL, opts.topOutliers))
          return false;
      } else if (flag == "--threads") {
        if (!parseU64Flag(1, 256ULL, opts.numThreads))
          return false;
      } else {
        std::fprintf(stderr, "Unknown option: %s\n", flag.c_str());
        return false;
      }
    } else if (!arg.empty() && arg[0] != '-' && opts.tbPath.empty()) {
      opts.tbPath = arg;
    } else {
      std::fprintf(stderr, "Invalid argument: %s\n", arg.c_str());
      return false;
    }
  }
  if (opts.tbPath.empty()) {
    std::fprintf(stderr, "Missing required <nextbox.tb.bin> path.\n");
    return false;
  }
  return true;
}

void printOutliers(const char *heading, const std::vector<tb::ResidualOutlier> &outliers) {
  std::printf("\n--- %s (%zu boards) ---\n", heading, outliers.size());
  for (size_t i = 0; i < outliers.size(); ++i) {
    const auto &o = outliers[i];
    std::printf("  #%zu hex=%s | V*=%.2f | V_hat=%.2f | err=%+.2f | holes=%d\n", i + 1,
                tb::formatBoardHex(o.board).c_str(), o.vStar, o.vHat, o.error,
                tb::countHoles(o.board));
    for (int r = tb::BOARD_ROWS - 1; r >= 0; --r) {
      const uint64_t row = tb::getRow(o.board, r);
      std::printf("      |");
      for (int c = 0; c < tb::BOARD_WIDTH; ++c) {
        std::printf("%c", (row & (1ULL << (tb::BOARD_WIDTH - 1 - c))) ? 'X' : '.');
      }
      std::printf("|\n");
    }
  }
}

void printRankingMetrics(const char *label, const tb::RankingEvalMetrics &m) {
  std::printf("  %-34s | top1=%5.2f%% | meanRegret=%6.4f | p95=%6.3f | max=%5.2f | "
              "0h=%.3f 1h=%.3f 2h=%.3f 3+h=%.3f\n",
              label, m.top1ExactPct, m.meanRegret, m.p95Regret, m.maxRegret, m.meanRegretByHoles[0],
              m.meanRegretByHoles[1], m.meanRegretByHoles[2], m.meanRegretByHoles[3]);
}

template <typename PickFn>
void benchmarkNextBoxPolicy(const char *label, tb::RngMode rngMode, int numGames, uint64_t baseSeed,
                            int numThreads, PickFn &&pickMove) {
  if (numGames <= 0)
    return;
  std::vector<int> lines(static_cast<size_t>(numGames), 0);
  std::atomic<int> nextIdx{0};
  const auto t0 = std::chrono::steady_clock::now();

  tb::detail::runWorkerPool(numThreads, static_cast<size_t>(numGames), /*minParallelWork=*/16,
                            [&](int /*tid*/) {
                              while (true) {
                                const int g = nextIdx.fetch_add(1, std::memory_order_relaxed);
                                if (g >= numGames)
                                  break;
                                tb::Rng rng(tb::detail::deriveGameSeed(baseSeed, g));
                                uint64_t board = 0ULL;
                                int cur = rng.nextOpeningPiece(rngMode);
                                int nxt = rng.nextPiece(rngMode, cur);
                                int cleared = 0;
                                for (int step = 0; step < tb::DEFAULT_MAX_PIECES_PER_GAME; ++step) {
                                  const tb::ChosenTransition ch = pickMove(board, cur, nxt);
                                  if (!ch.found)
                                    break;
                                  cleared += ch.transition.linesCleared;
                                  board = ch.transition.afterBoard;
                                  cur = nxt;
                                  nxt = rng.nextPiece(rngMode, cur);
                                }
                                lines[static_cast<size_t>(g)] = cleared;
                              }
                            });
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const auto dist = tb::detail::computeDistributionStats(std::move(lines));
  std::printf("  %-34s | games=%d | mean=%6.2f +- %.2f | med=%5.1f | p90=%3d | max=%3d (%.2fs)\n",
              label, numGames, dist.mean, dist.stdErr, dist.median, dist.p90, dist.maxVal, sec);
  std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
  CliOptions opts{};
  if (!parseArgs(argc, argv, opts)) {
    printUsage(argv[0], stderr);
    return 1;
  }
  if (opts.showHelp) {
    printUsage(argv[0], stdout);
    return 0;
  }

  tb::MappedNextBoxTablebase mapped;
  std::string err;
  if (!mapped.open(opts.tbPath, &err, /*verifySorted=*/true)) {
    std::fprintf(stderr, "Error opening tablebase '%s': %s\n", opts.tbPath.c_str(), err.c_str());
    return 1;
  }
  tb::IndexedNextBoxTablebase idxTb(mapped);
  const tb::RngMode rngMode = idxTb.rngMode();
  std::printf(
      "=== Linear Distillation & Feature Discovery (TB: %llu boards, rng=%s, %d threads) ===\n",
      static_cast<unsigned long long>(idxTb.size()), tb::rngModeName(rngMode), opts.numThreads);

  tb::DistillSampleConfig trainCfg{};
  trainCfg.numGames = opts.trainGames;
  trainCfg.maxDecisions = opts.trainDecisions;
  trainCfg.baseSeed = 20000000ULL;
  trainCfg.numThreads = opts.numThreads;
  const tb::DistillDataset trainDs = tb::sampleDistillDataset(idxTb, trainCfg);

  tb::DistillSampleConfig valCfg = trainCfg;
  valCfg.numGames = opts.valGames;
  valCfg.maxDecisions = opts.valDecisions;
  valCfg.baseSeed = 30000000ULL;
  const tb::DistillDataset valDs = tb::sampleDistillDataset(idxTb, valCfg);

  std::printf("  Train: %zu decisions (0h=%llu 1h=%llu 2h=%llu 3+h=%llu) | Val: %zu decisions\n",
              trainDs.decisions.size(),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[0]),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[1]),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[2]),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[3]), valDs.decisions.size());

  if (trainDs.decisions.empty()) {
    std::fprintf(stderr, "Error: sampled 0 training decisions from '%s'.\n", opts.tbPath.c_str());
    return 1;
  }

  // 1. V1 (10-feature) Ridge & Regret-Ranked models.
  const tb::RidgeRegressionResult v1Ridge = tb::fitRidgeValueModel(
      trainDs.decisions, tb::V1_NUM_BOARD_FEATURES, 1.0, tb::extractV1BoardFeatures);
  if (!v1Ridge.ok) {
    std::fprintf(stderr,
                 "Error: V1 Ridge value regression failed (singular or empty in-TB set).\n");
    return 1;
  }
  tb::RidgeRegressionResult v1RidgeVal{};
  tb::evaluateRidgeFit(valDs.decisions, tb::V1_NUM_BOARD_FEATURES, v1Ridge.intercept,
                       v1Ridge.weights, tb::extractV1BoardFeatures, v1RidgeVal);

  std::printf("\n--- V1 (10-feature) Value Regression ---\n");
  std::printf(
      "  Val R^2 = %.4f | RMSE = %.3f | MAE = %.3f | byHoles: 0h=%.2f 1h=%.2f 2h=%.2f 3+h=%.2f\n",
      v1RidgeVal.r2, v1RidgeVal.rmse, v1RidgeVal.mae, v1RidgeVal.maeByHoles[0],
      v1RidgeVal.maeByHoles[1], v1RidgeVal.maeByHoles[2], v1RidgeVal.maeByHoles[3]);

  tb::LinearRankingConfig rCfg{};
  rCfg.temperature = opts.temperature;
  rCfg.valueLossWeight = opts.valueWeight;
  rCfg.epochs = opts.epochs;
  rCfg.useExpectedTarget = true;
  rCfg.fixLineClearToOne = true;
  rCfg.verbose = false;

  const tb::LinearRankingResult v1Rank = tb::trainLinearRegretRanking(
      trainDs.decisions, valDs.decisions, tb::V1_NUM_BOARD_FEATURES, v1Ridge.intercept,
      v1Ridge.weights, 1.0, rCfg, tb::extractV1BoardFeatures);

  // 2. V2 (26-feature interpretable) Ridge & Regret-Ranked models.
  const tb::RidgeRegressionResult v2Ridge = tb::fitRidgeValueModel(
      trainDs.decisions, tb::V2_NUM_BOARD_FEATURES, 1.0, tb::extractV2BoardFeatures);
  if (!v2Ridge.ok) {
    std::fprintf(stderr,
                 "Error: V2 Ridge value regression failed (singular or empty in-TB set).\n");
    return 1;
  }
  tb::RidgeRegressionResult v2RidgeVal{};
  tb::evaluateRidgeFit(valDs.decisions, tb::V2_NUM_BOARD_FEATURES, v2Ridge.intercept,
                       v2Ridge.weights, tb::extractV2BoardFeatures, v2RidgeVal);

  std::printf("\n--- V2 (26-feature interpretable) Value Regression ---\n");
  std::printf(
      "  Val R^2 = %.4f | RMSE = %.3f | MAE = %.3f | byHoles: 0h=%.2f 1h=%.2f 2h=%.2f 3+h=%.2f\n",
      v2RidgeVal.r2, v2RidgeVal.rmse, v2RidgeVal.mae, v2RidgeVal.maeByHoles[0],
      v2RidgeVal.maeByHoles[1], v2RidgeVal.maeByHoles[2], v2RidgeVal.maeByHoles[3]);

  if (opts.topOutliers > 0) {
    std::vector<tb::ResidualOutlier> overV2, underV2;
    tb::collectResidualOutliers(valDs.decisions, tb::V2_NUM_BOARD_FEATURES, v2Ridge.intercept,
                                v2Ridge.weights, opts.topOutliers, tb::extractV2BoardFeatures,
                                overV2, underV2);
    printOutliers("V2 Ridge Overestimated Traps (V_hat >> V*)", overV2);
    printOutliers("V2 Ridge Underestimated Strong Boards (V_hat << V*)", underV2);
  }

  rCfg.verbose = true;
  std::printf("\n--- V2 Regret-Weighted Ranking (temp=%.2f, valW=%.3f, epochs=%d) ---\n",
              opts.temperature, opts.valueWeight, opts.epochs);
  const tb::LinearRankingResult v2Rank = tb::trainLinearRegretRanking(
      trainDs.decisions, valDs.decisions, tb::V2_NUM_BOARD_FEATURES, v2Ridge.intercept,
      v2Ridge.weights, 1.0, rCfg, tb::extractV2BoardFeatures);

  tb::LinearRankingConfig rPureCfg = rCfg;
  rPureCfg.valueLossWeight = 0.0;
  rPureCfg.fixLineClearToOne = true;
  rPureCfg.verbose = false;
  const tb::LinearRankingResult v2PureRank = tb::trainLinearRegretRanking(
      trainDs.decisions, valDs.decisions, tb::V2_NUM_BOARD_FEATURES, v2Ridge.intercept,
      v2Ridge.weights, 1.0, rPureCfg, tb::extractV2BoardFeatures);

  std::printf("\n  V2 Weights (Ridge vs Joint-Ranked vs Pure-Ranked):\n");
  std::printf("    %-18s : Ridge=%+9.4f | Joint=%+9.4f | PureRank=%+9.4f\n", "intercept",
              v2Ridge.intercept, v2Rank.intercept, v2PureRank.intercept);
  for (int i = 0; i < tb::V2_NUM_BOARD_FEATURES; ++i) {
    std::printf("    %-18s : Ridge=%+9.4f | Joint=%+9.4f | PureRank=%+9.4f\n",
                tb::V2_FEATURE_NAMES[i], v2Ridge.weights[static_cast<size_t>(i)],
                v2Rank.boardWeights[static_cast<size_t>(i)],
                v2PureRank.boardWeights[static_cast<size_t>(i)]);
  }

  const tb::EvalWeights baseW{};
  const auto v1RidgeEv = tb::LinearBoardEvalV1::fromVector(v1Ridge.intercept, 1.0, v1Ridge.weights);
  const auto v1RankEv = tb::LinearBoardEvalV1::fromVector(v1Rank.intercept, v1Rank.lineClearWeight,
                                                          v1Rank.boardWeights);
  const auto v2RidgeEv = tb::LinearBoardEvalV2::fromVector(v2Ridge.intercept, 1.0, v2Ridge.weights);
  const auto v2RankEv = tb::LinearBoardEvalV2::fromVector(v2Rank.intercept, v2Rank.lineClearWeight,
                                                          v2Rank.boardWeights);
  const auto v2PureEv = tb::LinearBoardEvalV2::fromVector(
      v2PureRank.intercept, v2PureRank.lineClearWeight, v2PureRank.boardWeights);

  std::printf("\n--- Validation Ranking Metrics (%zu decisions) ---\n", valDs.decisions.size());
  printRankingMetrics("Original 2-ply Next-Box",
                      tb::evaluateMoveRanker(valDs.decisions, [&](uint64_t b, int r, int n) {
                        return tb::scoreNextBoxHeuristicTransition(b, r, n, baseW);
                      }));
  printRankingMetrics("V1 Ridge 2-ply Next-Box",
                      tb::evaluateMoveRanker(valDs.decisions, [&](uint64_t b, int r, int n) {
                        return v1RidgeEv.scoreNextBox2Ply(b, r, n);
                      }));
  printRankingMetrics("V1 Joint-Rank 2-ply Next-Box",
                      tb::evaluateMoveRanker(valDs.decisions, [&](uint64_t b, int r, int n) {
                        return v1RankEv.scoreNextBox2Ply(b, r, n);
                      }));
  printRankingMetrics("V2 Ridge 2-ply Next-Box",
                      tb::evaluateMoveRanker(valDs.decisions, [&](uint64_t b, int r, int n) {
                        return v2RidgeEv.scoreNextBox2Ply(b, r, n);
                      }));
  printRankingMetrics("V2 Joint-Rank 2-ply Next-Box",
                      tb::evaluateMoveRanker(valDs.decisions, [&](uint64_t b, int r, int n) {
                        return v2RankEv.scoreNextBox2Ply(b, r, n);
                      }));
  printRankingMetrics("V2 Pure-Rank 2-ply Next-Box",
                      tb::evaluateMoveRanker(valDs.decisions, [&](uint64_t b, int r, int n) {
                        return v2PureEv.scoreNextBox2Ply(b, r, n);
                      }));

  if (opts.evalGames > 0) {
    std::printf("\n--- Held-Out Gameplay Benchmark (%d %s games, seed=%llu) ---\n", opts.evalGames,
                tb::rngModeName(rngMode), static_cast<unsigned long long>(opts.evalSeed));
    benchmarkNextBoxPolicy(
        "Original 2-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed, opts.numThreads,
        [&](uint64_t b, int c, int n) { return tb::pickBestNextBoxHeuristic(b, c, n, baseW); });
    benchmarkNextBoxPolicy(
        "V1 Ridge 2-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed, opts.numThreads,
        [&](uint64_t b, int c, int n) { return v1RidgeEv.pickBestNextBox2Ply(b, c, n); });
    benchmarkNextBoxPolicy(
        "V1 Joint-Rank 2-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed, opts.numThreads,
        [&](uint64_t b, int c, int n) { return v1RankEv.pickBestNextBox2Ply(b, c, n); });
    benchmarkNextBoxPolicy(
        "V2 Ridge 2-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed, opts.numThreads,
        [&](uint64_t b, int c, int n) { return v2RidgeEv.pickBestNextBox2Ply(b, c, n); });
    benchmarkNextBoxPolicy(
        "V2 Joint-Rank 2-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed, opts.numThreads,
        [&](uint64_t b, int c, int n) { return v2RankEv.pickBestNextBox2Ply(b, c, n); });
    benchmarkNextBoxPolicy(
        "V2 Pure-Rank 2-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed, opts.numThreads,
        [&](uint64_t b, int c, int n) { return v2PureEv.pickBestNextBox2Ply(b, c, n); });
  }
  return 0;
}
