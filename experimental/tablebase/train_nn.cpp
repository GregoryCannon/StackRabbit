// train_nn.cpp — Multi-threaded trainer for TinyNextBoxNet (SRNNV001), distilling a solved
// SRTBV002 Next-Box tablebase into a compact cache-resident 7-output MLP via joint Huber value
// regression (parent + candidate children) and Boltzmann move-ranking cross-entropy.
//
// Build:
//   g++ -O3 -DNDEBUG -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/train_nn.cpp -o build/tablebase_train_nn
//
// Usage:
//   ./build/tablebase_train_nn <nextbox.tb.bin> --out=<model.nn.bin> [options]

#include "cli_utils.hpp"
#include "distill_data.hpp"
#include "tiny_nn.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TrainOptions {
  std::string tbPath;
  std::string outPath;
  std::string oodTbPath; // Optional larger TB to measure out-of-TB generalization error
  int h1 = 64;
  int h2 = 32;
  int trainGames = 4000;
  int trainDecisions = 400000;
  int valGames = 400;
  int valDecisions = 40000;
  int epochs = 30;
  int batchSize = 512;
  double lr = 0.004;
  double valueWeight = 1.0;
  double childValueWeight = 1.0;
  double rankWeight = 4.0;
  double temperature = 1.0;
  double huberDelta = 2.0;
  double weightDecay = 1e-5;
  int evalGames = 10000;
  uint64_t evalSeed = 700000ULL;
  uint64_t trainSeed = 20261124ULL;
  int numThreads = 1;
  bool showHelp = false;
};

// Persistent barrier-style worker pool avoiding per-mini-batch pthread creation overhead.
class PersistentWorkerPool {
public:
  explicit PersistentWorkerPool(int numWorkers) : numWorkers_(std::max(1, numWorkers)) {
    if (numWorkers_ <= 1) {
      return;
    }
    threads_.reserve(static_cast<size_t>(numWorkers_ - 1));
    for (int tid = 1; tid < numWorkers_; ++tid) {
      threads_.emplace_back([this, tid]() { workerLoop(tid); });
    }
  }

  ~PersistentWorkerPool() {
    if (threads_.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
      ++generation_;
    }
    cvStart_.notify_all();
    for (auto &t : threads_) {
      t.join();
    }
  }

  PersistentWorkerPool(const PersistentWorkerPool &) = delete;
  PersistentWorkerPool &operator=(const PersistentWorkerPool &) = delete;

  template <typename Fn> void run(int activeWorkers, Fn &&fn) {
    const int n = std::clamp(activeWorkers, 1, numWorkers_);
    if (n <= 1) {
      fn(0);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      task_ = [&](int tid) { fn(tid); };
      active_ = n;
      remaining_ = n - 1;
      ++generation_;
    }
    cvStart_.notify_all();
    fn(0);
    std::unique_lock<std::mutex> lk(mu_);
    cvDone_.wait(lk, [&]() { return remaining_ == 0; });
    task_ = nullptr;
  }

private:
  void workerLoop(int tid) {
    uint64_t seenGen = 0;
    while (true) {
      std::function<void(int)> localTask;
      bool shouldRun = false;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cvStart_.wait(lk, [&]() { return generation_ != seenGen; });
        seenGen = generation_;
        if (stop_) {
          return;
        }
        if (tid < active_) {
          localTask = task_;
          shouldRun = true;
        }
      }
      if (shouldRun) {
        localTask(tid);
        std::lock_guard<std::mutex> lk(mu_);
        if (--remaining_ == 0) {
          cvDone_.notify_one();
        }
      }
    }
  }

  int numWorkers_ = 1;
  std::vector<std::thread> threads_;
  std::mutex mu_;
  std::condition_variable cvStart_;
  std::condition_variable cvDone_;
  std::function<void(int)> task_;
  uint64_t generation_ = 0;
  int active_ = 0;
  int remaining_ = 0;
  bool stop_ = false;
};

// Cache-line-aligned per-worker training/validation state preventing 128 B Apple Silicon false
// sharing between threads.
struct alignas(128) WorkerState {
  std::vector<float> grad;
  tb::NnBatchScratch scratch;
  tb::NnLossBreakdown loss{};
  size_t count = 0;
};

struct alignas(128) ValWorkerResult {
  size_t valueSamples = 0;
  double sumAbs = 0.0;
  double sumSq = 0.0;
  std::array<double, tb::NUM_HOLE_BUCKETS> absByHoles{};
  std::array<uint64_t, tb::NUM_HOLE_BUCKETS> valCountByHoles{};

  size_t rankExactHits = 0;
  double rankSumRegret = 0.0;
  double rankMaxRegret = 0.0;
  std::array<double, tb::NUM_HOLE_BUCKETS> rankSumByHoles{};
  std::array<uint64_t, tb::NUM_HOLE_BUCKETS> rankCountByHoles{};
  std::vector<double> regrets;
};

void evaluateValidationParallel(const tb::TinyNextBoxNet &net,
                                const std::vector<tb::LabeledDecisionSample> &valDecisions,
                                PersistentWorkerPool &pool, int workers,
                                tb::NnValueEvalMetrics &outValV, tb::RankingEvalMetrics &outValR) {
  outValV = tb::NnValueEvalMetrics{};
  outValR = tb::RankingEvalMetrics{};
  if (!net.isLoaded() || valDecisions.empty()) {
    return;
  }
  const int active =
      std::min<int>(workers, static_cast<int>(std::max<size_t>(1, valDecisions.size() / 64)));
  std::vector<ValWorkerResult> results(static_cast<size_t>(active));

  pool.run(active, [&](int tid) {
    const size_t s = (valDecisions.size() * static_cast<size_t>(tid)) / static_cast<size_t>(active);
    const size_t e =
        (valDecisions.size() * static_cast<size_t>(tid + 1)) / static_cast<size_t>(active);
    ValWorkerResult &res = results[static_cast<size_t>(tid)];
    res.regrets.reserve(e - s);
    float pred[tb::NUM_PIECES];
    for (size_t i = s; i < e; ++i) {
      const auto &d = valDecisions[i];
      const int hb = tb::HoleStratifiedCounts::bucket(tb::countHoles(d.board));
      if (d.boardInTb) {
        net.evalAll(d.board, pred);
        double boardAbs = 0.0;
        for (int p = 0; p < tb::NUM_PIECES; ++p) {
          const double err =
              static_cast<double>(pred[p]) - static_cast<double>(d.vStar[static_cast<size_t>(p)]);
          boardAbs += std::abs(err);
          res.sumSq += err * err;
        }
        boardAbs /= static_cast<double>(tb::NUM_PIECES);
        res.sumAbs += boardAbs;
        ++res.valueSamples;
        res.absByHoles[static_cast<size_t>(hb)] += boardAbs;
        ++res.valCountByHoles[static_cast<size_t>(hb)];
      }
      if (!d.moves.empty()) {
        int pickIdx = 0;
        double bestSc = -std::numeric_limits<double>::infinity();
        for (size_t m = 0; m < d.moves.size(); ++m) {
          const double sc = tb::scoreNextBoxNn1Ply(d.moves[m].afterBoard, d.moves[m].linesCleared,
                                                   d.nextPiece, net);
          if (m == 0 || sc > bestSc) {
            bestSc = sc;
            pickIdx = static_cast<int>(m);
          }
        }
        const double regret =
            std::max(0.0, d.bestQStar - d.moves[static_cast<size_t>(pickIdx)].qStar);
        if (regret <= 1e-5) {
          ++res.rankExactHits;
        }
        res.rankSumRegret += regret;
        if (regret > res.rankMaxRegret) {
          res.rankMaxRegret = regret;
        }
        res.regrets.push_back(regret);
        res.rankSumByHoles[static_cast<size_t>(hb)] += regret;
        ++res.rankCountByHoles[static_cast<size_t>(hb)];
      }
    }
  });

  double sumAbs = 0.0;
  double sumSq = 0.0;
  std::array<double, tb::NUM_HOLE_BUCKETS> absByHoles{};
  size_t exactHits = 0;
  double sumRegret = 0.0;
  std::array<double, tb::NUM_HOLE_BUCKETS> rankSumByHoles{};
  std::vector<double> allRegrets;
  allRegrets.reserve(valDecisions.size());

  for (const auto &r : results) {
    outValV.numSamples += r.valueSamples;
    sumAbs += r.sumAbs;
    sumSq += r.sumSq;
    exactHits += r.rankExactHits;
    sumRegret += r.rankSumRegret;
    if (r.rankMaxRegret > outValR.maxRegret) {
      outValR.maxRegret = r.rankMaxRegret;
    }
    allRegrets.insert(allRegrets.end(), r.regrets.begin(), r.regrets.end());
    for (size_t b = 0; b < tb::NUM_HOLE_BUCKETS; ++b) {
      absByHoles[b] += r.absByHoles[b];
      outValV.countByHoles[b] += r.valCountByHoles[b];
      rankSumByHoles[b] += r.rankSumByHoles[b];
      outValR.countByHoles[b] += r.rankCountByHoles[b];
    }
  }

  if (outValV.numSamples > 0) {
    outValV.mae = sumAbs / static_cast<double>(outValV.numSamples);
    outValV.rmse = std::sqrt(sumSq / static_cast<double>(outValV.numSamples * tb::NUM_PIECES));
    for (size_t b = 0; b < tb::NUM_HOLE_BUCKETS; ++b) {
      outValV.maeByHoles[b] = (outValV.countByHoles[b] > 0)
                                  ? (absByHoles[b] / static_cast<double>(outValV.countByHoles[b]))
                                  : 0.0;
    }
  }
  outValR.numDecisions = allRegrets.size();
  if (!allRegrets.empty()) {
    outValR.top1ExactPct =
        100.0 * static_cast<double>(exactHits) / static_cast<double>(outValR.numDecisions);
    outValR.meanRegret = sumRegret / static_cast<double>(outValR.numDecisions);
    std::sort(allRegrets.begin(), allRegrets.end());
    const size_t p95Idx = std::min(allRegrets.size() - 1, (allRegrets.size() * 95) / 100);
    outValR.p95Regret = allRegrets[p95Idx];
    for (size_t b = 0; b < tb::NUM_HOLE_BUCKETS; ++b) {
      outValR.meanRegretByHoles[b] =
          (outValR.countByHoles[b] > 0)
              ? (rankSumByHoles[b] / static_cast<double>(outValR.countByHoles[b]))
              : 0.0;
    }
  }
}

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s <nextbox.tb.bin> --out=<model.nn.bin> [options]\n", prog);
  std::fprintf(out, "  --out=PATH             Output SRNNV001 weight file (required)\n");
  std::fprintf(out, "  --ood-tb=PATH          Optional larger TB to measure out-of-TB MAE\n");
  std::fprintf(out, "  --h1=N                 Hidden layer 1 width (default: 64)\n");
  std::fprintf(out, "  --h2=N                 Hidden layer 2 width (default: 32)\n");
  std::fprintf(out, "  --train-games=N        Self-play games for training (default: 4000)\n");
  std::fprintf(out, "  --train-decisions=N    Max training decisions (default: 400000)\n");
  std::fprintf(out, "  --val-games=N          Self-play games for validation (default: 400)\n");
  std::fprintf(out, "  --val-decisions=N      Max validation decisions (default: 40000)\n");
  std::fprintf(out, "  --epochs=N             Training epochs (default: 30)\n");
  std::fprintf(out, "  --batch-size=N         Mini-batch size in decisions (default: 512)\n");
  std::fprintf(out, "  --lr=F                 Initial Adam learning rate (default: 0.004)\n");
  std::fprintf(out, "  --val-weight=F         Huber parent value loss weight (default: 1.0)\n");
  std::fprintf(out,
               "  --child-val-weight=F   Huber candidate-child value loss weight (default: 1.0)\n");
  std::fprintf(out, "  --rank-weight=F        Boltzmann ranking loss weight (default: 4.0)\n");
  std::fprintf(out, "  --temp=F               Boltzmann temperature in lines (default: 1.0)\n");
  std::fprintf(out,
               "  --huber-delta=F        Huber transition threshold in lines (default: 2.0)\n");
  std::fprintf(out, "  --weight-decay=F       L2 weight decay (default: 1e-5)\n");
  std::fprintf(out, "  --eval-games=N         Held-out gameplay games (default: 10000)\n");
  std::fprintf(out, "  --eval-seed=N          Held-out benchmark seed (default: 700000)\n");
  std::fprintf(out, "  --seed=N               Training RNG seed (default: 20261124)\n");
  std::fprintf(out, "  --threads=T            Worker threads (default: hardware concurrency)\n");
}

bool parseArgs(int argc, char **argv, TrainOptions &opts) {
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

      if (flag == "--out") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --out=\n");
          return false;
        }
        opts.outPath = val;
      } else if (flag == "--ood-tb") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --ood-tb=\n");
          return false;
        }
        opts.oodTbPath = val;
      } else if (flag == "--h1") {
        if (!parseU64(4, tb::NN_MAX_HIDDEN_DIM, opts.h1))
          return false;
      } else if (flag == "--h2") {
        if (!parseU64(4, tb::NN_MAX_HIDDEN_DIM, opts.h2))
          return false;
      } else if (flag == "--train-games") {
        if (!parseU64(1, 500000ULL, opts.trainGames))
          return false;
      } else if (flag == "--train-decisions") {
        if (!parseU64(100, 10000000ULL, opts.trainDecisions))
          return false;
      } else if (flag == "--val-games") {
        if (!parseU64(1, 100000ULL, opts.valGames))
          return false;
      } else if (flag == "--val-decisions") {
        if (!parseU64(100, 2000000ULL, opts.valDecisions))
          return false;
      } else if (flag == "--epochs") {
        if (!parseU64(1, 500ULL, opts.epochs))
          return false;
      } else if (flag == "--batch-size") {
        if (!parseU64(1, 16384ULL, opts.batchSize))
          return false;
      } else if (flag == "--lr") {
        if (!parseDbl(1e-6, 1.0, opts.lr))
          return false;
      } else if (flag == "--val-weight") {
        if (!parseDbl(0.0, 100.0, opts.valueWeight))
          return false;
      } else if (flag == "--child-val-weight") {
        if (!parseDbl(0.0, 100.0, opts.childValueWeight))
          return false;
      } else if (flag == "--rank-weight") {
        if (!parseDbl(0.0, 100.0, opts.rankWeight))
          return false;
      } else if (flag == "--temp") {
        if (!parseDbl(0.05, 50.0, opts.temperature))
          return false;
      } else if (flag == "--huber-delta") {
        if (!parseDbl(0.05, 100.0, opts.huberDelta))
          return false;
      } else if (flag == "--weight-decay") {
        if (!parseDbl(0.0, 1.0, opts.weightDecay))
          return false;
      } else if (flag == "--eval-games") {
        if (!parseU64(0, 100000ULL, opts.evalGames))
          return false;
      } else if (flag == "--eval-seed") {
        uint64_t u = 0;
        if (!tb::cli::parseStrictU64(val, 0, UINT64_MAX, u)) {
          std::fprintf(stderr, "Invalid --eval-seed value '%s'\n", val.c_str());
          return false;
        }
        opts.evalSeed = u;
      } else if (flag == "--seed") {
        uint64_t u = 0;
        if (!tb::cli::parseStrictU64(val, 0, UINT64_MAX, u)) {
          std::fprintf(stderr, "Invalid --seed value '%s'\n", val.c_str());
          return false;
        }
        opts.trainSeed = u;
      } else if (flag == "--threads") {
        if (!parseU64(1, 256ULL, opts.numThreads))
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
  if (opts.outPath.empty()) {
    std::fprintf(stderr, "Missing required --out=<model.nn.bin> path.\n");
    return false;
  }
  if (opts.valueWeight == 0.0 && opts.childValueWeight == 0.0 && opts.rankWeight == 0.0) {
    std::fprintf(
        stderr,
        "At least one of --val-weight, --child-val-weight, or --rank-weight must be > 0.\n");
    return false;
  }
  return true;
}

template <typename PickFn>
void benchmarkNnPolicy(const char *label, tb::RngMode rngMode, int numGames, uint64_t baseSeed,
                       int numThreads, PickFn &&pickMove) {
  if (numGames <= 0) {
    return;
  }
  std::vector<int> lines(static_cast<size_t>(numGames), 0);
  std::atomic<uint64_t> totalMoves{0};
  std::atomic<int> nextIdx{0};
  const auto t0 = std::chrono::steady_clock::now();

  tb::detail::runWorkerPool(numThreads, static_cast<size_t>(numGames), /*minParallelWork=*/16,
                            [&](int /*tid*/) {
                              uint64_t localMoves = 0;
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
                                  ++localMoves;
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
                              totalMoves.fetch_add(localMoves, std::memory_order_relaxed);
                            });
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const auto dist = tb::detail::computeDistributionStats(std::move(lines));
  const double mvPerSec = (sec > 0.0) ? (static_cast<double>(totalMoves.load()) / sec) : 0.0;
  std::printf("  %-32s | games=%d | mean=%6.2f +- %.2f | med=%5.1f | p90=%3d | max=%3d | "
              "%.1fk mv/s (%.2fs)\n",
              label, numGames, dist.mean, dist.stdErr, dist.median, dist.p90, dist.maxVal,
              mvPerSec / 1000.0, sec);
  std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
  TrainOptions opts{};
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
    std::fprintf(stderr, "Error preparing output directory for '%s'.\n", opts.outPath.c_str());
    return 1;
  }

  tb::MappedNextBoxTablebase mapped;
  if (!mapped.open(opts.tbPath, &err, /*verifySorted=*/true)) {
    std::fprintf(stderr, "Error opening tablebase '%s': %s\n", opts.tbPath.c_str(), err.c_str());
    return 1;
  }
  const tb::RngMode rngMode = mapped.rngMode();
  const uint64_t tbBoardCount = mapped.size();

  std::printf("=== TinyNextBoxNet Training (%d -> %d -> %d -> %d | TB: %llu boards, rng=%s, %d "
              "threads) ===\n",
              tb::NN_INPUT_DIM, opts.h1, opts.h2, tb::NUM_PIECES,
              static_cast<unsigned long long>(tbBoardCount), tb::rngModeName(rngMode),
              opts.numThreads);

  const auto tSample0 = std::chrono::steady_clock::now();
  tb::DistillDataset trainDs;
  tb::DistillDataset valDs;
  {
    // Scope IndexedNextBoxTablebase so its ~1 GB hash index is freed immediately after sampling.
    tb::IndexedNextBoxTablebase idxTb(mapped);
    tb::DistillSampleConfig trainCfg{};
    trainCfg.numGames = opts.trainGames;
    trainCfg.maxDecisions = opts.trainDecisions;
    trainCfg.baseSeed = 20000000ULL;
    trainCfg.includeTerminalOutMoves = true;
    trainCfg.numThreads = opts.numThreads;
    trainDs = tb::sampleDistillDataset(idxTb, trainCfg);

    tb::DistillSampleConfig valCfg = trainCfg;
    valCfg.numGames = opts.valGames;
    valCfg.maxDecisions = opts.valDecisions;
    valCfg.baseSeed = 30000000ULL;
    valDs = tb::sampleDistillDataset(idxTb, valCfg);
  }
  const double sampleSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - tSample0).count();

  std::printf("  Sampled in %.2fs | Train: %zu (0h=%llu 1h=%llu 2h=%llu 3+h=%llu) | Val: %zu\n",
              sampleSec, trainDs.decisions.size(),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[0]),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[1]),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[2]),
              static_cast<unsigned long long>(trainDs.decisionsByHoles[3]), valDs.decisions.size());
  if (trainDs.decisions.empty() || valDs.decisions.empty()) {
    std::fprintf(stderr, "Error: sampled empty train (%zu) or val (%zu) decision set.\n",
                 trainDs.decisions.size(), valDs.decisions.size());
    return 1;
  }

  // Initialize output bias to empirical mean V* so initial predictions start calibrated.
  double meanV = 0.0;
  size_t meanCount = 0;
  for (const auto &d : trainDs.decisions) {
    if (d.boardInTb) {
      meanV += d.expectedVStar;
      ++meanCount;
    }
  }
  const float initBias =
      (meanCount > 0) ? static_cast<float>(meanV / static_cast<double>(meanCount)) : 0.0f;

  tb::TinyNextBoxNet net;
  if (!net.initRandom(opts.h1, opts.h2, rngMode, opts.trainSeed, initBias)) {
    std::fprintf(stderr, "Error initializing TinyNextBoxNet.\n");
    return 1;
  }
  std::printf("  Model: %zu params (%.1f KB) | initBias=%.2f | lr=%.4g | valW=%.2f childValW=%.2f "
              "rankW=%.2f T=%.2f\n",
              net.paramCount(), static_cast<double>(net.byteSize()) / 1024.0,
              static_cast<double>(initBias), opts.lr, opts.valueWeight, opts.childValueWeight,
              opts.rankWeight, opts.temperature);
  std::fflush(stdout);

  tb::NnLossConfig lossCfg{};
  lossCfg.huberDelta = opts.huberDelta;
  lossCfg.valueWeight = opts.valueWeight;
  lossCfg.childValueWeight = opts.childValueWeight;
  lossCfg.rankWeight = opts.rankWeight;
  lossCfg.temperature = opts.temperature;
  lossCfg.weightDecay = opts.weightDecay;

  const size_t nParams = net.paramCount();
  std::vector<double> adamM(nParams, 0.0);
  std::vector<double> adamV(nParams, 0.0);
  std::vector<double> gradAccum(nParams, 0.0);

  const int workers = std::max(1, opts.numThreads);
  PersistentWorkerPool pool(workers);
  std::vector<WorkerState> workerStates(static_cast<size_t>(workers));
  for (auto &ws : workerStates) {
    ws.scratch.ensureCapacity(tb::MAX_TRANSITIONS, net.h1, net.h2);
  }

  std::vector<size_t> trainOrder(trainDs.decisions.size());
  std::iota(trainOrder.begin(), trainOrder.end(), 0);
  tb::Rng shufRng(opts.trainSeed ^ 0x9E3779B97F4A7C15ULL);

  const size_t batchSize = static_cast<size_t>(opts.batchSize);
  const size_t batchesPerEpoch = (trainOrder.size() + batchSize - 1) / batchSize;
  const size_t totalSteps = std::max<size_t>(1, static_cast<size_t>(opts.epochs) * batchesPerEpoch);

  constexpr double BETA1 = 0.9;
  constexpr double BETA2 = 0.999;
  constexpr double ADAM_EPS = 1e-8;
  constexpr double MIN_LR_FRAC = 0.02;
  constexpr double PI = 3.14159265358979323846;

  uint64_t stepNum = 0;
  double beta1Pow = 1.0;
  double beta2Pow = 1.0;
  double bestValRegret = std::numeric_limits<double>::infinity();
  tb::TinyNextBoxNet bestNet = net;

  const auto tTrain0 = std::chrono::steady_clock::now();
  for (int ep = 1; ep <= opts.epochs; ++ep) {
    for (size_t i = trainOrder.size(); i > 1; --i) {
      const size_t j = static_cast<size_t>(shufRng.nextU64() % static_cast<uint64_t>(i));
      std::swap(trainOrder[i - 1], trainOrder[j]);
    }

    double epTotalLoss = 0.0;
    double epValLoss = 0.0;
    double epChildValLoss = 0.0;
    double epRankLoss = 0.0;
    size_t epBatches = 0;

    for (size_t bStart = 0; bStart < trainOrder.size(); bStart += batchSize) {
      const size_t bEnd = std::min(trainOrder.size(), bStart + batchSize);
      const size_t bCount = bEnd - bStart;
      const int activeWorkers =
          std::min<int>(workers, static_cast<int>(std::max<size_t>(1, bCount / 8)));

      pool.run(activeWorkers, [&](int tid) {
        const size_t s =
            bStart + (bCount * static_cast<size_t>(tid)) / static_cast<size_t>(activeWorkers);
        const size_t e =
            bStart + (bCount * static_cast<size_t>(tid + 1)) / static_cast<size_t>(activeWorkers);
        WorkerState &ws = workerStates[static_cast<size_t>(tid)];
        ws.count = (e > s) ? (e - s) : 0;
        ws.loss = tb::computeNnSliceLossAndGrad(net, trainDs.decisions, trainOrder, s, e, lossCfg,
                                                ws.grad, ws.scratch, /*includeWeightDecay=*/false);
      });

      std::fill(gradAccum.begin(), gradAccum.end(), 0.0);
      double bValLoss = 0.0;
      double bChildValLoss = 0.0;
      double bRankLoss = 0.0;
      const double invBCount = 1.0 / static_cast<double>(bCount);
      for (int tid = 0; tid < activeWorkers; ++tid) {
        const WorkerState &ws = workerStates[static_cast<size_t>(tid)];
        if (ws.count == 0) {
          continue;
        }
        const double wFrac = static_cast<double>(ws.count) * invBCount;
        bValLoss += wFrac * ws.loss.valueLoss;
        bChildValLoss += wFrac * ws.loss.childValueLoss;
        bRankLoss += wFrac * ws.loss.rankLoss;
        for (size_t p = 0; p < nParams; ++p) {
          gradAccum[p] += wFrac * static_cast<double>(ws.grad[p]);
        }
      }

      if (lossCfg.weightDecay > 0.0) {
        net.forEachWeightMatrix([&](size_t off, size_t len) {
          for (size_t i = 0; i < len; ++i) {
            gradAccum[off + i] += lossCfg.weightDecay * static_cast<double>(net.params[off + i]);
          }
        });
      }

      ++stepNum;
      beta1Pow *= BETA1;
      beta2Pow *= BETA2;
      const double progress = static_cast<double>(stepNum) / static_cast<double>(totalSteps);
      const double lr =
          opts.lr * (MIN_LR_FRAC + (1.0 - MIN_LR_FRAC) * 0.5 * (1.0 + std::cos(PI * progress)));
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

      epValLoss += bValLoss;
      epChildValLoss += bChildValLoss;
      epRankLoss += bRankLoss;
      epTotalLoss += lossCfg.valueWeight * bValLoss
                     + lossCfg.childValueWeight * bChildValLoss
                     + lossCfg.rankWeight * bRankLoss;
      ++epBatches;
    }

    tb::NnValueEvalMetrics valV{};
    tb::RankingEvalMetrics valR{};
    evaluateValidationParallel(net, valDs.decisions, pool, workers, valV, valR);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tTrain0).count();
    const bool isBest = valR.meanRegret < bestValRegret;
    if (isBest) {
      bestValRegret = valR.meanRegret;
      bestNet = net;
    }
    if (ep == 1 || ep == opts.epochs || ep % 5 == 0 || isBest) {
      std::printf("  [ep %2d/%d | %5.1fs] loss=%.4f (v=%.3f cv=%.3f r=%.3f) | valMAE=%.3f (0h=%.2f "
                  "1h=%.2f 2h=%.2f 3+h=%.2f) | top1=%5.2f%% regret=%.4f%s\n",
                  ep, opts.epochs, elapsed, epTotalLoss / static_cast<double>(epBatches),
                  epValLoss / static_cast<double>(epBatches),
                  epChildValLoss / static_cast<double>(epBatches),
                  epRankLoss / static_cast<double>(epBatches), valV.mae, valV.maeByHoles[0],
                  valV.maeByHoles[1], valV.maeByHoles[2], valV.maeByHoles[3], valR.top1ExactPct,
                  valR.meanRegret, isBest ? " *" : "");
      std::fflush(stdout);
    }
  }

  // Free training/validation datasets before optional OOD/gameplay evaluation.
  trainDs = tb::DistillDataset{};
  valDs = tb::DistillDataset{};

  if (!bestNet.save(opts.outPath, &err)) {
    std::fprintf(stderr, "Error saving model '%s': %s\n", opts.outPath.c_str(), err.c_str());
    return 1;
  }
  std::printf("\n  Saved best checkpoint -> %s (%.1f KB, best val 1-ply regret=%.4f)\n",
              opts.outPath.c_str(), static_cast<double>(bestNet.byteSize()) / 1024.0,
              bestValRegret);

  // Optional out-of-TB generalization check on boards present in oodTbPath but absent from tbPath.
  if (!opts.oodTbPath.empty()) {
    tb::MappedNextBoxTablebase oodMapped;
    if (!oodMapped.open(opts.oodTbPath, &err, /*verifySorted=*/true)) {
      std::fprintf(stderr, "Error opening --ood-tb '%s': %s\n", opts.oodTbPath.c_str(),
                   err.c_str());
      return 1;
    }
    if (oodMapped.rngMode() != rngMode) {
      std::fprintf(stderr,
                   "Error: --ood-tb rngMode (%s) does not match training TB rngMode (%s).\n",
                   tb::rngModeName(oodMapped.rngMode()), tb::rngModeName(rngMode));
      return 1;
    }
    tb::IndexedNextBoxTablebase oodIdx(oodMapped);
    tb::DistillSampleConfig oodCfg{};
    oodCfg.numGames = opts.valGames;
    oodCfg.maxDecisions = opts.valDecisions;
    oodCfg.baseSeed = 40000000ULL;
    oodCfg.numThreads = opts.numThreads;
    const tb::DistillDataset oodDs = tb::sampleDistillDataset(oodIdx, oodCfg);
    std::vector<tb::LabeledDecisionSample> unseenOnly;
    float dummyRow[tb::NUM_PIECES];
    for (const auto &s : oodDs.decisions) {
      if (s.boardInTb && !mapped.lookup(s.board, dummyRow)) {
        unseenOnly.push_back(s);
      }
    }
    const tb::NnValueEvalMetrics oodM = tb::evaluateNnValueAccuracy(bestNet, unseenOnly);
    std::printf("\n--- Out-of-TB Generalization (%zu unseen samples from %s) ---\n",
                oodM.numSamples, opts.oodTbPath.c_str());
    std::printf("  OOD MAE = %.3f | RMSE = %.3f | byHoles: 0h=%.2f 1h=%.2f 2h=%.2f 3+h=%.2f\n",
                oodM.mae, oodM.rmse, oodM.maeByHoles[0], oodM.maeByHoles[1], oodM.maeByHoles[2],
                oodM.maeByHoles[3]);
  }
  mapped.close();

  if (opts.evalGames > 0) {
    std::printf("\n--- Held-Out Gameplay Benchmark (%d %s games, seed=%llu) ---\n", opts.evalGames,
                tb::rngModeName(rngMode), static_cast<unsigned long long>(opts.evalSeed));
    benchmarkNnPolicy(
        "Tiny NN 1-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed, opts.numThreads,
        [&](uint64_t b, int c, int n) { return tb::chooseNextBoxNnMove1Ply(b, c, n, bestNet); });
    benchmarkNnPolicy("Tiny NN 2-ply Next-Box", rngMode, opts.evalGames, opts.evalSeed,
                      opts.numThreads, [&](uint64_t b, int c, int n) {
                        return tb::chooseNextBoxNnMove2Ply(b, c, n, bestNet, rngMode);
                      });
  }

  return 0;
}
