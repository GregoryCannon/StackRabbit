// rl_finetune_nn.cpp — Hybrid Expert-Iteration (2-ply search distillation) + TD(lambda) self-play
// RL fine-tuner for TinyNextBoxNet (SRNNV001), with optional <= 2.5M tablebase replay anchor and
// read-only out-of-distribution diagnostic auditing.
//
// Build:
//   g++ -O3 -DNDEBUG -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/rl_finetune_nn.cpp -o build/tablebase_rl_nn
//
// Usage:
//   ./build/tablebase_rl_nn --init-nn=<in.nn.bin> --out=<out.nn.bin> [options]

#include "cli_utils.hpp"
#include "rl_finetune.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int ANCHOR_SAMPLE_GAMES = 2000;
constexpr int ANCHOR_MAX_DECISIONS = 200000;
constexpr uint64_t VAL_BENCH_SEED = 30000000ULL;

struct RlCliOptions {
  std::string initNnPath;
  std::string outPath;
  std::string anchorTbPath; // Optional <= 2.5M TB replay anchor
  std::string oodTbPath;    // Optional read-only diagnostic oracle TB
  int iters = 6;
  int epochsPerIter = 3;
  int gamesPerIter = 400;
  int maxDecisions = 200000;
  double epsilon = 0.06;
  double lambda = 0.9;
  double mcWeight = 1.0;
  double exitRankWeight = 4.0;
  double exitChildWeight = 1.0;
  double anchorFrac = 0.25;
  bool anchorFracSet = false;
  double anchorValWeight = 1.0;
  double anchorRankWeight = 4.0;
  int batchSize = 512;
  double lr = 0.0002;
  double minLrFrac = 0.1;
  double huberDelta = 2.0;
  double temperature = 1.0;
  double weightDecay = 1e-5;
  double emaTau = 1.0;
  int valGames = 500;
  uint64_t seed = 40000000ULL;
  int numThreads = 1;
  bool showHelp = false;
};

struct alignas(128) RlWorkerState {
  std::vector<float> rlGrad;
  std::vector<float> anchorGrad;
  tb::NnBatchScratch scratch;
  tb::RlLossBreakdown rlLoss{};
  tb::NnLossBreakdown anchorLoss{};
  size_t rlCount = 0;
  size_t anchorCount = 0;
};

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s --init-nn=<in.nn.bin> --out=<out.nn.bin> [options]\n", prog);
  std::fprintf(out, "  --init-nn=PATH            Starting SRNNV001 weights (required)\n");
  std::fprintf(out, "  --out=PATH                Output SRNNV001 weights (required)\n");
  std::fprintf(out, "  --anchor-tb=PATH          Optional SRTBV002 anchor TB (enforces <= 2.5M "
                    "boards)\n");
  std::fprintf(out, "  --ood-tb=PATH             Optional read-only oracle TB for diagnostic MAE "
                    "& bias\n");
  std::fprintf(out, "  --iters=N                 Outer self-play RL iterations (default: 6)\n");
  std::fprintf(out, "  --epochs-per-iter=N       Training epochs per outer iter (default: 3)\n");
  std::fprintf(out, "  --games-per-iter=N        Self-play games per outer iter (default: 400)\n");
  std::fprintf(out,
               "  --max-decisions=N         Max RL decisions per outer iter (default: 200000)\n");
  std::fprintf(out, "  --epsilon=F               Exploration rate in [0,1] (default: 0.06)\n");
  std::fprintf(out, "  --lambda=F                TD(lambda) parameter in [0,1] (default: 0.9)\n");
  std::fprintf(out, "  --mc-weight=F             TD(lambda) Huber value weight (default: 1.0)\n");
  std::fprintf(out, "  --exit-rank-weight=F      2-ply ExIt ranking CE weight (default: 4.0)\n");
  std::fprintf(out, "  --exit-child-weight=F     2-ply ExIt child Huber weight (default: 1.0)\n");
  std::fprintf(out, "  --anchor-frac=F           TB anchor mini-batch fraction in [0,0.95] "
                    "(default: 0.25 with --anchor-tb, else 0)\n");
  std::fprintf(out, "  --anchor-val-weight=F     TB anchor value Huber weight (default: 1.0)\n");
  std::fprintf(out, "  --anchor-rank-weight=F    TB anchor ranking CE weight (default: 4.0)\n");
  std::fprintf(out, "  --batch-size=N            Mini-batch size in decisions (default: 512)\n");
  std::fprintf(out, "  --lr=F                    Initial Adam learning rate (default: 0.0002)\n");
  std::fprintf(out, "  --min-lr-frac=F           Final cosine LR fraction (default: 0.1)\n");
  std::fprintf(out, "  --huber-delta=F           Huber threshold in lines (default: 2.0)\n");
  std::fprintf(out, "  --temp=F                  Boltzmann temperature in lines (default: 1.0)\n");
  std::fprintf(out, "  --weight-decay=F          L2 weight decay (default: 1e-5)\n");
  std::fprintf(
      out, "  --ema-tau=F               Target-net EMA tau in (0,1] (default: 1.0 = refresh)\n");
  std::fprintf(out, "  --val-games=N             Validation 2-ply games per iter for best "
                    "checkpoint (default: 500)\n");
  std::fprintf(out, "  --seed=N                  Base rollout seed (default: 40000000)\n");
  std::fprintf(out, "  --threads=T               Worker threads (default: hardware concurrency)\n");
}

bool parseArgs(int argc, char **argv, RlCliOptions &opts) {
  opts.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  std::set<std::string> seen;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      opts.showHelp = true;
      return true;
    }
    const size_t eq = arg.find('=');
    if (arg.rfind("--", 0) != 0 || eq == std::string::npos) {
      std::fprintf(stderr, "Invalid argument: %s\n", arg.c_str());
      return false;
    }
    const std::string flag = arg.substr(0, eq);
    const std::string val = arg.substr(eq + 1);
    if (!seen.insert(flag).second) {
      std::fprintf(stderr, "Duplicate option: %s\n", flag.c_str());
      return false;
    }
    auto parseU64 = [&](uint64_t lo, uint64_t hi, int &dest) -> bool {
      uint64_t u = 0;
      if (!tb::cli::parseStrictU64(val, lo, hi, u)) {
        std::fprintf(stderr, "Invalid %s value '%s' (expected %llu..%llu)\n", flag.c_str(),
                     val.c_str(), static_cast<unsigned long long>(lo),
                     static_cast<unsigned long long>(hi));
        return false;
      }
      dest = static_cast<int>(u);
      return true;
    };
    auto parseDbl = [&](double lo, double hi, double &dest) -> bool {
      double d = 0.0;
      if (!tb::cli::parseStrictPositiveDouble(val, lo, hi, d)) {
        std::fprintf(stderr, "Invalid %s value '%s' (expected %.4g..%.4g)\n", flag.c_str(),
                     val.c_str(), lo, hi);
        return false;
      }
      dest = d;
      return true;
    };

    if (flag == "--init-nn") {
      if (val.empty()) {
        std::fprintf(stderr, "Empty path for --init-nn=\n");
        return false;
      }
      opts.initNnPath = val;
    } else if (flag == "--out") {
      if (val.empty()) {
        std::fprintf(stderr, "Empty path for --out=\n");
        return false;
      }
      opts.outPath = val;
    } else if (flag == "--anchor-tb") {
      if (val.empty()) {
        std::fprintf(stderr, "Empty path for --anchor-tb=\n");
        return false;
      }
      opts.anchorTbPath = val;
    } else if (flag == "--ood-tb") {
      if (val.empty()) {
        std::fprintf(stderr, "Empty path for --ood-tb=\n");
        return false;
      }
      opts.oodTbPath = val;
    } else if (flag == "--iters") {
      if (!parseU64(1, 100ULL, opts.iters))
        return false;
    } else if (flag == "--epochs-per-iter") {
      if (!parseU64(1, 50ULL, opts.epochsPerIter))
        return false;
    } else if (flag == "--games-per-iter") {
      if (!parseU64(1, 50000ULL, opts.gamesPerIter))
        return false;
    } else if (flag == "--max-decisions") {
      if (!parseU64(100, 5000000ULL, opts.maxDecisions))
        return false;
    } else if (flag == "--epsilon") {
      if (!parseDbl(0.0, 1.0, opts.epsilon))
        return false;
    } else if (flag == "--lambda") {
      if (!parseDbl(0.0, 1.0, opts.lambda))
        return false;
    } else if (flag == "--mc-weight") {
      if (!parseDbl(0.0, 100.0, opts.mcWeight))
        return false;
    } else if (flag == "--exit-rank-weight") {
      if (!parseDbl(0.0, 100.0, opts.exitRankWeight))
        return false;
    } else if (flag == "--exit-child-weight") {
      if (!parseDbl(0.0, 100.0, opts.exitChildWeight))
        return false;
    } else if (flag == "--anchor-frac") {
      if (!parseDbl(0.0, 0.95, opts.anchorFrac))
        return false;
      opts.anchorFracSet = true;
    } else if (flag == "--anchor-val-weight") {
      if (!parseDbl(0.0, 100.0, opts.anchorValWeight))
        return false;
    } else if (flag == "--anchor-rank-weight") {
      if (!parseDbl(0.0, 100.0, opts.anchorRankWeight))
        return false;
    } else if (flag == "--batch-size") {
      if (!parseU64(1, 16384ULL, opts.batchSize))
        return false;
    } else if (flag == "--lr") {
      if (!parseDbl(1e-7, 0.1, opts.lr))
        return false;
    } else if (flag == "--min-lr-frac") {
      if (!parseDbl(0.01, 1.0, opts.minLrFrac))
        return false;
    } else if (flag == "--huber-delta") {
      if (!parseDbl(0.05, 100.0, opts.huberDelta))
        return false;
    } else if (flag == "--temp") {
      if (!parseDbl(0.05, 50.0, opts.temperature))
        return false;
    } else if (flag == "--weight-decay") {
      if (!parseDbl(0.0, 1.0, opts.weightDecay))
        return false;
    } else if (flag == "--ema-tau") {
      if (!parseDbl(1e-3, 1.0, opts.emaTau))
        return false;
    } else if (flag == "--val-games") {
      if (!parseU64(0, 10000ULL, opts.valGames))
        return false;
    } else if (flag == "--seed") {
      uint64_t u = 0;
      if (!tb::cli::parseStrictU64(val, 0, UINT64_MAX, u)) {
        std::fprintf(stderr, "Invalid --seed value '%s'\n", val.c_str());
        return false;
      }
      opts.seed = u;
    } else if (flag == "--threads") {
      if (!parseU64(1, 256ULL, opts.numThreads))
        return false;
    } else {
      std::fprintf(stderr, "Unknown option: %s\n", flag.c_str());
      return false;
    }
  }

  if (opts.initNnPath.empty() || opts.outPath.empty()) {
    std::fprintf(stderr, "Both --init-nn=PATH and --out=PATH are required.\n");
    return false;
  }
  if (opts.anchorTbPath.empty()) {
    if (opts.anchorFracSet && opts.anchorFrac > 0.0) {
      std::fprintf(stderr, "Error: --anchor-frac > 0 requires --anchor-tb=PATH\n");
      return false;
    }
    opts.anchorFrac = 0.0;
  } else if (opts.anchorFrac > 0.0 && opts.anchorValWeight == 0.0 && opts.anchorRankWeight == 0.0) {
    std::fprintf(stderr,
                 "Error: --anchor-val-weight and --anchor-rank-weight cannot both be 0 when "
                 "--anchor-tb is active.\n");
    return false;
  }
  if (opts.mcWeight == 0.0 && opts.exitRankWeight == 0.0 && opts.exitChildWeight == 0.0) {
    std::fprintf(
        stderr,
        "At least one of --mc-weight, --exit-rank-weight, or --exit-child-weight must be > 0.\n");
    return false;
  }
  return true;
}

double evalEmptyExpectedValue(const tb::TinyNextBoxNet &net, tb::RngMode rngMode) noexcept {
  float row[tb::NUM_PIECES];
  net.evalAll(0ULL, row);
  return tb::expectedValueFromRow(row, rngMode);
}

tb::detail::DistributionStats evaluateStandalone2Ply(const tb::TinyNextBoxNet &net,
                                                     tb::RngMode rngMode, int numGames,
                                                     uint64_t baseSeed, int numThreads) {
  if (numGames <= 0 || !net.isLoaded()) {
    return {};
  }
  std::vector<int> lines(static_cast<size_t>(numGames), 0);
  std::atomic<int> nextIdx{0};
  tb::detail::runWorkerPool(
      numThreads, static_cast<size_t>(numGames), /*minParallelWork=*/8, [&](int /*tid*/) {
        tb::TransitionList tl;
        tb::TransitionList tl2;
        tb::Eval2PlyCache cache;
        cache.init();
        while (true) {
          const int g = nextIdx.fetch_add(1, std::memory_order_relaxed);
          if (g >= numGames) {
            break;
          }
          tb::Rng rng(tb::detail::deriveGameSeed(baseSeed, g));
          uint64_t board = 0ULL;
          int cur = rng.nextOpeningPiece(rngMode);
          int nxt = rng.nextPiece(rngMode, cur);
          int cleared = 0;
          for (int step = 0; step < tb::DEFAULT_MAX_PIECES_PER_GAME; ++step) {
            tb::generateTransitions(board, cur, tl);
            if (tl.empty()) {
              break;
            }
            int bestIdx = 0;
            double bestSc = -1e300;
            for (int m = 0; m < tl.size(); ++m) {
              const double sc = static_cast<double>(tl[m].linesCleared)
                                + cache.bestReply(tl[m].afterBoard, nxt, net, rngMode, tl2);
              if (m == 0 || sc > bestSc) {
                bestSc = sc;
                bestIdx = m;
              }
            }
            cleared += tl[bestIdx].linesCleared;
            board = tl[bestIdx].afterBoard;
            cur = nxt;
            nxt = rng.nextPiece(rngMode, cur);
          }
          lines[static_cast<size_t>(g)] = cleared;
        }
      });
  return tb::detail::computeDistributionStats(std::move(lines));
}

struct OodBoardSample {
  uint64_t board = 0ULL;
  std::array<float, tb::NUM_PIECES> vStar{};
};

void printOodDiagnostic(const char *tag, const tb::TinyNextBoxNet &net,
                        const std::vector<OodBoardSample> &oodSamples) {
  if (oodSamples.empty()) {
    return;
  }
  double sumAbs = 0.0;
  double sumSigned = 0.0;
  double sumSq = 0.0;
  size_t count = 0;
  float pred[tb::NUM_PIECES];
  for (const auto &s : oodSamples) {
    net.evalAll(s.board, pred);
    for (int p = 0; p < tb::NUM_PIECES; ++p) {
      const double diff =
          static_cast<double>(pred[p]) - static_cast<double>(s.vStar[static_cast<size_t>(p)]);
      sumAbs += std::abs(diff);
      sumSigned += diff;
      sumSq += diff * diff;
      ++count;
    }
  }
  if (count == 0) {
    return;
  }
  const double mae = sumAbs / static_cast<double>(count);
  const double bias = sumSigned / static_cast<double>(count);
  const double rmse = std::sqrt(sumSq / static_cast<double>(count));
  std::printf("  [Oracle OOD %s] MAE=%.3f | Bias(pred-oracle)=%+.3f | RMSE=%.3f (%zu boards)\n",
              tag, mae, bias, rmse, oodSamples.size());
}

} // namespace

int main(int argc, char **argv) {
  RlCliOptions opts{};
  if (!parseArgs(argc, argv, opts)) {
    printUsage(argv[0], stderr);
    return 1;
  }
  if (opts.showHelp) {
    printUsage(argv[0], stdout);
    return 0;
  }

  std::string err;
  if (!tb::cli::ensureParentDirectory(opts.outPath)) {
    return 1;
  }

  tb::TinyNextBoxNet net;
  if (!net.load(opts.initNnPath, &err)) {
    std::fprintf(stderr, "Error loading --init-nn '%s': %s\n", opts.initNnPath.c_str(),
                 err.c_str());
    return 1;
  }
  const tb::RngMode rngMode = net.rngMode;

  // Optional <= 2.5M TB replay anchor (always validate <= 2.5M cap whenever --anchor-tb is passed).
  tb::DistillDataset anchorDs;
  if (!opts.anchorTbPath.empty()) {
    tb::MappedNextBoxTablebase anchorMapped;
    if (!anchorMapped.open(opts.anchorTbPath, &err, /*verifySorted=*/true)) {
      std::fprintf(stderr, "Error opening --anchor-tb '%s': %s\n", opts.anchorTbPath.c_str(),
                   err.c_str());
      return 1;
    }
    if (anchorMapped.size() > tb::RL_MAX_ANCHOR_TB_BOARDS) {
      std::fprintf(stderr,
                   "Error: --anchor-tb '%s' has %llu boards, exceeding hard <= %llu board cap!\n",
                   opts.anchorTbPath.c_str(), static_cast<unsigned long long>(anchorMapped.size()),
                   static_cast<unsigned long long>(tb::RL_MAX_ANCHOR_TB_BOARDS));
      return 1;
    }
    if (anchorMapped.rngMode() != rngMode) {
      std::fprintf(stderr, "Error: --anchor-tb rngMode (%s) does not match --init-nn (%s)\n",
                   tb::rngModeName(anchorMapped.rngMode()), tb::rngModeName(rngMode));
      return 1;
    }
    if (opts.anchorFrac > 0.0) {
      tb::IndexedNextBoxTablebase anchorIdx(anchorMapped);
      tb::DistillSampleConfig aCfg{};
      aCfg.numGames = ANCHOR_SAMPLE_GAMES;
      aCfg.maxDecisions = ANCHOR_MAX_DECISIONS;
      aCfg.baseSeed = 25000000ULL;
      aCfg.includeTerminalOutMoves = true;
      aCfg.numThreads = opts.numThreads;
      anchorDs = tb::sampleDistillDataset(anchorIdx, aCfg);
      if (anchorDs.decisions.empty()) {
        std::fprintf(stderr, "Error: sampled 0 anchor decisions from '%s'\n",
                     opts.anchorTbPath.c_str());
        return 1;
      }
    }
    anchorMapped.close();
  }

  // Optional read-only oracle OOD diagnostic sample (uses MappedNextBoxTablebase directly to avoid
  // allocating a ~1.35 GB IndexedNextBoxTablebase for 10,000 diagnostic samples).
  std::vector<OodBoardSample> oodSamples;
  if (!opts.oodTbPath.empty()) {
    tb::MappedNextBoxTablebase oodMapped;
    if (!oodMapped.open(opts.oodTbPath, &err, /*verifySorted=*/true)) {
      std::fprintf(stderr, "Error opening --ood-tb '%s': %s\n", opts.oodTbPath.c_str(),
                   err.c_str());
      return 1;
    }
    if (oodMapped.rngMode() != rngMode) {
      std::fprintf(stderr, "Error: --ood-tb rngMode (%s) does not match --init-nn (%s)\n",
                   tb::rngModeName(oodMapped.rngMode()), tb::rngModeName(rngMode));
      return 1;
    }
    tb::DistillSampleConfig oodCfg{};
    oodCfg.numGames = 300;
    oodCfg.maxDecisions = 10000;
    oodCfg.baseSeed = 50000000ULL;
    oodCfg.numThreads = opts.numThreads;
    const tb::DistillDataset rawOod = tb::sampleDistillDataset(oodMapped, oodCfg);
    oodMapped.close();
    oodSamples.reserve(rawOod.decisions.size());
    for (const auto &s : rawOod.decisions) {
      if (s.boardInTb) {
        oodSamples.push_back(OodBoardSample{s.board, s.vStar});
      }
    }
  }

  std::printf("=== Self-Play RL + Expert-Iteration Fine-Tuner (%dx%d | rng=%s | %d threads) ===\n",
              net.h1, net.h2, tb::rngModeName(rngMode), opts.numThreads);
  std::printf("  Init NN : %s (%.1f KB) | E[V_hat(0)] = %.2f\n", opts.initNnPath.c_str(),
              static_cast<double>(net.byteSize()) / 1024.0, evalEmptyExpectedValue(net, rngMode));
  std::printf("  RL cfg  : iters=%d | epochs/iter=%d | games/iter=%d | eps=%.3f | lambda=%.2f | "
              "mcW=%.2f exitChildW=%.2f exitRankW=%.2f | lr=%.2e | tau=%.2f\n",
              opts.iters, opts.epochsPerIter, opts.gamesPerIter, opts.epsilon, opts.lambda,
              opts.mcWeight, opts.exitChildWeight, opts.exitRankWeight, opts.lr, opts.emaTau);
  if (!anchorDs.decisions.empty()) {
    std::printf("  Anchor  : %s (%zu decisions, frac=%.2f, valW=%.2f, rankW=%.2f)\n",
                opts.anchorTbPath.c_str(), anchorDs.decisions.size(), opts.anchorFrac,
                opts.anchorValWeight, opts.anchorRankWeight);
  }
  printOodDiagnostic("init", net, oodSamples);

  double bestValMean = -1.0;
  tb::TinyNextBoxNet bestNet = net;
  if (opts.valGames > 0) {
    const auto initVal =
        evaluateStandalone2Ply(net, rngMode, opts.valGames, VAL_BENCH_SEED, opts.numThreads);
    bestValMean = initVal.mean;
    std::printf("  [iter 0/%d | init] val 2-ply = %.2f +- %.2f (med=%.1f, p90=%d)\n", opts.iters,
                initVal.mean, initVal.stdErr, initVal.median, initVal.p90);
  }
  std::fflush(stdout);

  tb::RlLossConfig rlLossCfg{};
  rlLossCfg.huberDelta = opts.huberDelta;
  rlLossCfg.mcWeight = opts.mcWeight;
  rlLossCfg.exitChildWeight = opts.exitChildWeight;
  rlLossCfg.exitRankWeight = opts.exitRankWeight;
  rlLossCfg.temperature = opts.temperature;
  rlLossCfg.weightDecay = 0.0; // Applied once to the combined batch gradient

  tb::NnLossConfig anchorLossCfg{};
  anchorLossCfg.huberDelta = opts.huberDelta;
  anchorLossCfg.valueWeight = opts.anchorValWeight;
  anchorLossCfg.childValueWeight = opts.anchorValWeight;
  anchorLossCfg.rankWeight = opts.anchorRankWeight;
  anchorLossCfg.temperature = opts.temperature;
  anchorLossCfg.weightDecay = 0.0;

  const size_t nParams = net.paramCount();
  std::vector<double> adamM(nParams, 0.0);
  std::vector<double> adamV(nParams, 0.0);
  std::vector<double> gradAccum(nParams, 0.0);

  const int workers = std::max(1, opts.numThreads);
  tb::PersistentWorkerPool pool(workers);
  std::vector<RlWorkerState> workerStates(static_cast<size_t>(workers));
  for (auto &ws : workerStates) {
    ws.scratch.ensureCapacity(tb::MAX_TRANSITIONS, net.h1, net.h2);
  }

  std::vector<size_t> anchorOrder(anchorDs.decisions.size());
  std::iota(anchorOrder.begin(), anchorOrder.end(), 0);
  size_t anchorCursor = 0;
  tb::Rng shufRng(opts.seed ^ 0x9E3779B97F4A7C15ULL);

  auto reshuffle = [&](std::vector<size_t> &ord) {
    for (size_t i = ord.size(); i > 1; --i) {
      const size_t j = static_cast<size_t>(shufRng.nextU64() % static_cast<uint64_t>(i));
      std::swap(ord[i - 1], ord[j]);
    }
  };
  if (!anchorOrder.empty()) {
    reshuffle(anchorOrder);
  }

  tb::TinyNextBoxNet targetNet = net;
  const bool needMoveTargets = (opts.exitRankWeight > 0.0 || opts.exitChildWeight > 0.0);
  constexpr double BETA1 = 0.9;
  constexpr double BETA2 = 0.999;
  constexpr double ADAM_EPS = 1e-8;
  constexpr double PI = 3.14159265358979323846;

  double beta1Pow = 1.0;
  double beta2Pow = 1.0;

  const auto tStart = std::chrono::steady_clock::now();
  for (int iter = 1; iter <= opts.iters; ++iter) {
    const auto tRoll0 = std::chrono::steady_clock::now();
    tb::RlRolloutConfig rCfg{};
    rCfg.rngMode = rngMode;
    rCfg.numGames = opts.gamesPerIter;
    rCfg.baseSeed = opts.seed + static_cast<uint64_t>(iter - 1) * 1000000ULL;
    rCfg.epsilon = opts.epsilon;
    rCfg.lambda = opts.lambda;
    rCfg.collectMoveTargets = needMoveTargets;
    rCfg.maxDecisions = opts.maxDecisions;
    rCfg.numThreads = workers;

    tb::RlRolloutStats rStats{};
    std::vector<tb::RlDecisionSample> rlDecisions =
        tb::collectRlSelfPlayDecisions(targetNet, rCfg, &rStats);
    const double rollSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tRoll0).count();
    if (rlDecisions.empty()) {
      std::fprintf(stderr, "Error: collected 0 self-play decisions on iter %d\n", iter);
      return 1;
    }

    std::vector<size_t> rlOrder(rlDecisions.size());
    std::iota(rlOrder.begin(), rlOrder.end(), 0);

    const bool useAnchor = !anchorDs.decisions.empty() && opts.anchorFrac > 0.0;
    const size_t rlBatchTarget =
        useAnchor ? std::max<size_t>(
                        1, static_cast<size_t>(std::llround(static_cast<double>(opts.batchSize)
                                                            * (1.0 - opts.anchorFrac))))
                  : static_cast<size_t>(opts.batchSize);
    const size_t batchesThisIter =
        std::max<size_t>(1, static_cast<size_t>(opts.epochsPerIter)
                                * ((rlOrder.size() + rlBatchTarget - 1) / rlBatchTarget));

    double iterRlLoss = 0.0;
    double iterMcLoss = 0.0;
    double iterExChildLoss = 0.0;
    double iterExRankLoss = 0.0;
    size_t iterBatches = 0;

    for (int ep = 0; ep < opts.epochsPerIter; ++ep) {
      reshuffle(rlOrder);
      for (size_t bStart = 0; bStart < rlOrder.size(); bStart += rlBatchTarget) {
        const size_t bEnd = std::min(rlOrder.size(), bStart + rlBatchTarget);
        const size_t rlCount = bEnd - bStart;

        // Scale anchor batch size proportionally to rlCount so trailing partial batches preserve
        // the exact configured anchorFrac ratio.
        size_t aStart = 0;
        size_t aEnd = 0;
        if (useAnchor) {
          const size_t scaledAnchor = std::max<size_t>(
              1, static_cast<size_t>(std::llround(
                     static_cast<double>(rlCount) * opts.anchorFrac / (1.0 - opts.anchorFrac))));
          if (anchorCursor + scaledAnchor > anchorOrder.size()) {
            reshuffle(anchorOrder);
            anchorCursor = 0;
          }
          aStart = anchorCursor;
          aEnd = std::min(anchorOrder.size(), aStart + scaledAnchor);
          anchorCursor = aEnd;
        }
        const size_t aCount = aEnd - aStart;
        const size_t totalBatch = rlCount + aCount;
        const int activeWorkers =
            std::min<int>(workers, static_cast<int>(std::max<size_t>(1, totalBatch / 8)));

        pool.run(activeWorkers, [&](int tid) {
          RlWorkerState &ws = workerStates[static_cast<size_t>(tid)];
          const size_t rs =
              bStart + (rlCount * static_cast<size_t>(tid)) / static_cast<size_t>(activeWorkers);
          const size_t re =
              bStart
              + (rlCount * static_cast<size_t>(tid + 1)) / static_cast<size_t>(activeWorkers);
          ws.rlCount = (re > rs) ? (re - rs) : 0;
          if (ws.rlCount > 0) {
            ws.rlLoss = tb::computeRlSliceLossAndGrad(net, rlDecisions, rlOrder, rs, re, rlLossCfg,
                                                      ws.rlGrad, ws.scratch,
                                                      /*includeWeightDecay=*/false);
          } else {
            ws.rlLoss = tb::RlLossBreakdown{};
          }

          if (aCount > 0) {
            const size_t as =
                aStart + (aCount * static_cast<size_t>(tid)) / static_cast<size_t>(activeWorkers);
            const size_t ae =
                aStart
                + (aCount * static_cast<size_t>(tid + 1)) / static_cast<size_t>(activeWorkers);
            ws.anchorCount = (ae > as) ? (ae - as) : 0;
            if (ws.anchorCount > 0) {
              ws.anchorLoss =
                  tb::computeNnSliceLossAndGrad(net, anchorDs.decisions, anchorOrder, as, ae,
                                                anchorLossCfg, ws.anchorGrad, ws.scratch,
                                                /*includeWeightDecay=*/false);
            } else {
              ws.anchorLoss = tb::NnLossBreakdown{};
            }
          } else {
            ws.anchorCount = 0;
          }
        });

        std::fill(gradAccum.begin(), gradAccum.end(), 0.0);
        const double invTotal = 1.0 / static_cast<double>(totalBatch);
        const double invRl = 1.0 / static_cast<double>(rlCount);
        double bMc = 0.0;
        double bExChild = 0.0;
        double bExRank = 0.0;

        for (int tid = 0; tid < activeWorkers; ++tid) {
          const RlWorkerState &ws = workerStates[static_cast<size_t>(tid)];
          if (ws.rlCount > 0) {
            const double wTot = static_cast<double>(ws.rlCount) * invTotal;
            const double wRl = static_cast<double>(ws.rlCount) * invRl;
            bMc += wRl * ws.rlLoss.mcLoss;
            bExChild += wRl * ws.rlLoss.exitChildLoss;
            bExRank += wRl * ws.rlLoss.exitRankLoss;
            for (size_t p = 0; p < nParams; ++p) {
              gradAccum[p] += wTot * static_cast<double>(ws.rlGrad[p]);
            }
          }
          if (ws.anchorCount > 0) {
            const double wTot = static_cast<double>(ws.anchorCount) * invTotal;
            for (size_t p = 0; p < nParams; ++p) {
              gradAccum[p] += wTot * static_cast<double>(ws.anchorGrad[p]);
            }
          }
        }

        if (opts.weightDecay > 0.0) {
          net.forEachWeightMatrix([&](size_t off, size_t len) {
            for (size_t i = 0; i < len; ++i) {
              gradAccum[off + i] += opts.weightDecay * static_cast<double>(net.params[off + i]);
            }
          });
        }

        ++iterBatches;
        beta1Pow *= BETA1;
        beta2Pow *= BETA2;
        const double iterFrac =
            static_cast<double>(iterBatches) / static_cast<double>(batchesThisIter);
        const double progress = std::clamp(
            (static_cast<double>(iter - 1) + iterFrac) / static_cast<double>(opts.iters), 0.0, 1.0);
        const double lr =
            opts.lr
            * (opts.minLrFrac + (1.0 - opts.minLrFrac) * 0.5 * (1.0 + std::cos(PI * progress)));
        const double invBc1 = 1.0 / (1.0 - beta1Pow);
        const double invBc2 = 1.0 / (1.0 - beta2Pow);

        for (size_t p = 0; p < nParams; ++p) {
          const double g = gradAccum[p];
          adamM[p] = BETA1 * adamM[p] + (1.0 - BETA1) * g;
          adamV[p] = BETA2 * adamV[p] + (1.0 - BETA2) * g * g;
          const double mHat = adamM[p] * invBc1;
          const double vHat = adamV[p] * invBc2;
          net.params[p] = static_cast<float>(static_cast<double>(net.params[p])
                                             - lr * mHat / (std::sqrt(vHat) + ADAM_EPS));
        }

        iterMcLoss += bMc;
        iterExChildLoss += bExChild;
        iterExRankLoss += bExRank;
        iterRlLoss +=
            opts.mcWeight * bMc + opts.exitChildWeight * bExChild + opts.exitRankWeight * bExRank;
      }
    }

    tb::updateTargetNetEma(targetNet, net, opts.emaTau);

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
    const double invBatches = 1.0 / static_cast<double>(iterBatches);
    const double meanPred0 = evalEmptyExpectedValue(net, rngMode);
    std::printf("  [iter %2d/%d | %5.1fs] sp=%.2f (%dg, %zu dec, %.1fs) | loss=%.3f (mc=%.2f "
                "ec=%.2f er=%.3f) | V(0)=%.2f",
                iter, opts.iters, elapsed, rStats.meanLines, rStats.games, rlDecisions.size(),
                rollSec, iterRlLoss * invBatches, iterMcLoss * invBatches,
                iterExChildLoss * invBatches, iterExRankLoss * invBatches, meanPred0);
    if (opts.valGames > 0) {
      const auto valDist =
          evaluateStandalone2Ply(net, rngMode, opts.valGames, VAL_BENCH_SEED, opts.numThreads);
      const bool isBest = valDist.mean > bestValMean;
      if (isBest) {
        bestValMean = valDist.mean;
        bestNet = net;
      }
      std::printf(" | val2p=%.2f+-%.2f%s\n", valDist.mean, valDist.stdErr, isBest ? " *" : "");
    } else {
      bestNet = net;
      std::printf("\n");
    }
    std::fflush(stdout);
  }

  if (!bestNet.save(opts.outPath, &err)) {
    std::fprintf(stderr, "Error saving RL fine-tuned model '%s': %s\n", opts.outPath.c_str(),
                 err.c_str());
    return 1;
  }
  if (opts.valGames > 0) {
    std::printf("\nSaved RL fine-tuned model -> %s (%.1f KB, best val 2-ply=%.2f)\n",
                opts.outPath.c_str(), static_cast<double>(bestNet.byteSize()) / 1024.0,
                bestValMean);
  } else {
    std::printf("\nSaved RL fine-tuned model -> %s (%.1f KB)\n", opts.outPath.c_str(),
                static_cast<double>(bestNet.byteSize()) / 1024.0);
  }
  printOodDiagnostic("final", bestNet, oodSamples);
  return 0;
}
