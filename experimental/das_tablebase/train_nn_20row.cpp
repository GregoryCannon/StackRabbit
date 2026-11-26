// train_nn_20row.cpp — Pure-supervised on-policy multi-task NNUE distillation CLI (SRNNV001 v2).
// Samples greedy + recovery + danger-zone trajectories from an SRTBV003 tablebase (optionally
// guided on out-of-TB steps by a prior NNUE) and trains TinyDasNet with 4-part multi-task Adam
// loss:
//   parent 7-output Huber + child Huber + ply-2 top-out target (0.0) + Boltzmann move-ranking CE.

#include "nnue_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

template <typename IntT> bool parseStrictInt(std::string_view s, IntT &out) {
  if (s.empty())
    return false;
  if constexpr (std::is_unsigned_v<IntT>) {
    if (s[0] == '-')
      return false;
  }
  IntT val = 0;
  const char *begin = s.data();
  const char *end = begin + s.size();
  const auto res = std::from_chars(begin, end, val);
  if (res.ec != std::errc() || res.ptr != end)
    return false;
  out = val;
  return true;
}

bool parseStrictDouble(std::string_view s, double &out) {
  if (s.empty())
    return false;
  std::string tmp(s);
  char *endPtr = nullptr;
  const double v = std::strtod(tmp.c_str(), &endPtr);
  if (endPtr != tmp.c_str() + tmp.size() || !std::isfinite(v))
    return false;
  out = v;
  return true;
}

void printUsage(FILE *stream, const char *prog) {
  fprintf(
      stream,
      "Usage: %s --tb=TB_PATH --out=NN_PATH [options]\n"
      "  --guide-nn=PATH          Optional prior SRNNV001 NNUE for out-of-TB rollout guidance\n"
      "  --init-nn=PATH           Optional warm-start SRNNV001 NNUE (matching h1/h2)\n"
      "  --h1=N                   Hidden layer 1 width (default 128)\n"
      "  --h2=N                   Hidden layer 2 width (default 64)\n"
      "  --games=N                Rollout games for training dataset (default 4000)\n"
      "  --max-decisions=N        Max training decisions (default 300000)\n"
      "  --val-decisions=N        Max validation decisions (default 15000)\n"
      "  --recover-frac=F         Fraction of epsilon-recovery rollout games (default 0.25)\n"
      "  --recover-eps=F          Epsilon during recovery games (default 0.08)\n"
      "  --danger-frac=F          Fraction of uniform danger-zone rollout games (default 0.12)\n"
      "  --danger-eps=F           Epsilon during danger-zone games (default 0.35)\n"
      "  --rollout-margin=F       Partial-OUT margin when --guide-nn is set (default 0.6)\n"
      "  --epochs=N               Training epochs (default 25)\n"
      "  --batch-size=N           Adam mini-batch size (default 256)\n"
      "  --lr=F                   Initial learning rate (default 2e-3)\n"
      "  --min-lr=F               Cosine floor learning rate (default 1e-4)\n"
      "  --weight-decay=F         L2 weight decay (default 1e-5)\n"
      "  --huber-delta=F          Huber transition delta (default 0.5)\n"
      "  --temp=F                 Boltzmann ranking temperature (default 0.35)\n"
      "  --val-weight=F           Parent 7-output Huber weight (default 1.0)\n"
      "  --child-val-weight=F     Candidate-child Huber weight (default 1.0)\n"
      "  --rank-weight=F          Boltzmann move-ranking CE weight (default 4.0)\n"
      "  --seed=U64               Base RNG seed (default 20000000)\n"
      "  --threads=N              Worker threads (default auto)\n",
      prog);
}

} // namespace

int main(int argc, char **argv) {
  std::string tbPath;
  std::string guideNnPath;
  std::string initNnPath;
  std::string outPath;
  int valDecisions = 15000;
  bool hasRolloutMargin = false;

  das::DistillSampleConfig20 sampleCfg{};
  das::NnTrainConfig20 trainCfg{};
  trainCfg.seed = sampleCfg.baseSeed ^ 0x517CC1B727220A95ULL;
  trainCfg.numThreads = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "-h" || arg == "--help") {
      printUsage(stdout, argv[0]);
      return 0;
    }
    auto tryPrefix = [&](std::string_view p, std::string_view &v) -> bool {
      if (arg.size() >= p.size() && arg.substr(0, p.size()) == p) {
        v = arg.substr(p.size());
        return true;
      }
      return false;
    };

    std::string_view val;
    if (tryPrefix("--tb=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --tb path\n");
        return 1;
      }
      tbPath = std::string(val);
      continue;
    }
    if (tryPrefix("--guide-nn=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --guide-nn path\n");
        return 1;
      }
      guideNnPath = std::string(val);
      continue;
    }
    if (tryPrefix("--init-nn=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --init-nn path\n");
        return 1;
      }
      initNnPath = std::string(val);
      continue;
    }
    if (tryPrefix("--out=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --out path\n");
        return 1;
      }
      outPath = std::string(val);
      continue;
    }
    if (tryPrefix("--h1=", val)) {
      if (!parseStrictInt(val, trainCfg.h1)
          || trainCfg.h1 <= 0
          || trainCfg.h1 > das::NN_MAX_HIDDEN_DIM) {
        fprintf(stderr, "Error: invalid --h1\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--h2=", val)) {
      if (!parseStrictInt(val, trainCfg.h2)
          || trainCfg.h2 <= 0
          || trainCfg.h2 > das::NN_MAX_HIDDEN_DIM) {
        fprintf(stderr, "Error: invalid --h2\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--games=", val)) {
      if (!parseStrictInt(val, sampleCfg.numGames) || sampleCfg.numGames <= 0) {
        fprintf(stderr, "Error: invalid --games\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--max-decisions=", val)) {
      if (!parseStrictInt(val, sampleCfg.maxDecisions) || sampleCfg.maxDecisions <= 0) {
        fprintf(stderr, "Error: invalid --max-decisions\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--val-decisions=", val)) {
      if (!parseStrictInt(val, valDecisions) || valDecisions < 0) {
        fprintf(stderr, "Error: invalid --val-decisions\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--recover-frac=", val)) {
      if (!parseStrictDouble(val, sampleCfg.recoverFrac)
          || sampleCfg.recoverFrac < 0.0
          || sampleCfg.recoverFrac > 1.0) {
        fprintf(stderr, "Error: invalid --recover-frac\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--recover-eps=", val)) {
      if (!parseStrictDouble(val, sampleCfg.recoverEpsilon)
          || sampleCfg.recoverEpsilon < 0.0
          || sampleCfg.recoverEpsilon > 1.0) {
        fprintf(stderr, "Error: invalid --recover-eps\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--danger-frac=", val)) {
      if (!parseStrictDouble(val, sampleCfg.dangerFrac)
          || sampleCfg.dangerFrac < 0.0
          || sampleCfg.dangerFrac > 1.0) {
        fprintf(stderr, "Error: invalid --danger-frac\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--danger-eps=", val)) {
      if (!parseStrictDouble(val, sampleCfg.dangerEpsilon)
          || sampleCfg.dangerEpsilon < 0.0
          || sampleCfg.dangerEpsilon > 1.0) {
        fprintf(stderr, "Error: invalid --danger-eps\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--rollout-margin=", val)) {
      if (!parseStrictDouble(val, sampleCfg.rolloutMargin) || sampleCfg.rolloutMargin < 0.0) {
        fprintf(stderr, "Error: invalid --rollout-margin\n");
        return 1;
      }
      hasRolloutMargin = true;
      continue;
    }
    if (tryPrefix("--epochs=", val)) {
      if (!parseStrictInt(val, trainCfg.epochs) || trainCfg.epochs <= 0) {
        fprintf(stderr, "Error: invalid --epochs\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--batch-size=", val)) {
      if (!parseStrictInt(val, trainCfg.batchSize) || trainCfg.batchSize <= 0) {
        fprintf(stderr, "Error: invalid --batch-size\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--lr=", val)) {
      if (!parseStrictDouble(val, trainCfg.lr) || trainCfg.lr <= 0.0) {
        fprintf(stderr, "Error: invalid --lr\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--min-lr=", val)) {
      if (!parseStrictDouble(val, trainCfg.minLr) || trainCfg.minLr < 0.0) {
        fprintf(stderr, "Error: invalid --min-lr\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--weight-decay=", val)) {
      if (!parseStrictDouble(val, trainCfg.weightDecay) || trainCfg.weightDecay < 0.0) {
        fprintf(stderr, "Error: invalid --weight-decay\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--huber-delta=", val)) {
      if (!parseStrictDouble(val, trainCfg.huberDelta) || trainCfg.huberDelta <= 0.0) {
        fprintf(stderr, "Error: invalid --huber-delta\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--temp=", val)) {
      if (!parseStrictDouble(val, trainCfg.temperature) || trainCfg.temperature <= 0.0) {
        fprintf(stderr, "Error: invalid --temp\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--val-weight=", val)) {
      if (!parseStrictDouble(val, trainCfg.valWeight) || trainCfg.valWeight < 0.0) {
        fprintf(stderr, "Error: invalid --val-weight\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--child-val-weight=", val)) {
      if (!parseStrictDouble(val, trainCfg.childValWeight) || trainCfg.childValWeight < 0.0) {
        fprintf(stderr, "Error: invalid --child-val-weight\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--rank-weight=", val)) {
      if (!parseStrictDouble(val, trainCfg.rankWeight) || trainCfg.rankWeight < 0.0) {
        fprintf(stderr, "Error: invalid --rank-weight\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--seed=", val)) {
      if (!parseStrictInt(val, sampleCfg.baseSeed)) {
        fprintf(stderr, "Error: invalid --seed\n");
        return 1;
      }
      trainCfg.seed = sampleCfg.baseSeed ^ 0x517CC1B727220A95ULL;
      continue;
    }
    if (tryPrefix("--threads=", val)) {
      if (!parseStrictInt(val, trainCfg.numThreads) || trainCfg.numThreads < 0) {
        fprintf(stderr, "Error: invalid --threads\n");
        return 1;
      }
      continue;
    }
    fprintf(stderr, "Error: unknown arg '%.*s'\n", static_cast<int>(arg.size()), arg.data());
    return 1;
  }

  if (tbPath.empty() || outPath.empty()) {
    fprintf(stderr, "Error: --tb and --out are required\n");
    return 1;
  }
  if (hasRolloutMargin && guideNnPath.empty()) {
    fprintf(stderr, "Error: --rollout-margin requires --guide-nn\n");
    return 1;
  }
  if (trainCfg.minLr > trainCfg.lr) {
    fprintf(stderr, "Error: --min-lr (%.3e) must be <= --lr (%.3e)\n", trainCfg.minLr, trainCfg.lr);
    return 1;
  }
  if (das::detail::isSameFilePath(tbPath, outPath)
      || das::detail::isSameFilePath(guideNnPath, outPath)
      || das::detail::isSameFilePath(initNnPath, outPath)) {
    fprintf(stderr, "Error: --out must not alias --tb, --guide-nn, or --init-nn\n");
    return 1;
  }
  if (sampleCfg.recoverFrac + sampleCfg.dangerFrac > 1.0 + 1e-9) {
    fprintf(stderr, "Error: --recover-frac + --danger-frac (%.3f) must be <= 1.0\n",
            sampleCfg.recoverFrac + sampleCfg.dangerFrac);
    return 1;
  }
  if (trainCfg.numThreads == 0) {
    trainCfg.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  }
  sampleCfg.numThreads = trainCfg.numThreads;

  const auto tStart = std::chrono::steady_clock::now();
  das::IndexedDasTablebase tb;
  std::string err;
  if (!tb.open(tbPath, &err)) {
    fprintf(stderr, "Error loading --tb: %s\n", err.c_str());
    return 1;
  }
  printf("Indexed TB %s (%llu boards, L%d, rng=%s, gamma=%.5f [%s], E[V(0)]=%.5f)\n",
         tbPath.c_str(), static_cast<unsigned long long>(tb.size()), tb.level(),
         das::rngModeName(tb.rngMode()), tb.gamma(), das::discountModeName(tb.discountMode()),
         tb.emptyBoardValue());

  das::TinyDasNet guideNn;
  if (!guideNnPath.empty()) {
    if (!guideNn.load(guideNnPath, &err)) {
      fprintf(stderr, "Error loading --guide-nn: %s\n", err.c_str());
      return 1;
    }
    if (guideNn.rngMode != tb.rngMode()) {
      fprintf(stderr, "Error: --guide-nn rng (%s) does not match --tb (%s)\n",
              das::rngModeName(guideNn.rngMode), das::rngModeName(tb.rngMode()));
      return 1;
    }
    printf("Loaded guide NNUE %s (%dx%d)\n", guideNnPath.c_str(), guideNn.h1, guideNn.h2);
  }
  das::TinyDasNet initNn;
  if (!initNnPath.empty()) {
    if (!initNn.load(initNnPath, &err)) {
      fprintf(stderr, "Error loading --init-nn: %s\n", err.c_str());
      return 1;
    }
    if (initNn.h1 != trainCfg.h1 || initNn.h2 != trainCfg.h2) {
      fprintf(stderr, "Error: --init-nn shape (%dx%d) does not match --h1=%d --h2=%d\n", initNn.h1,
              initNn.h2, trainCfg.h1, trainCfg.h2);
      return 1;
    }
    if (initNn.rngMode != tb.rngMode()) {
      fprintf(stderr, "Error: --init-nn rng (%s) does not match --tb (%s)\n",
              das::rngModeName(initNn.rngMode), das::rngModeName(tb.rngMode()));
      return 1;
    }
    printf("Loaded warm-start NNUE %s (%dx%d)\n", initNnPath.c_str(), initNn.h1, initNn.h2);
  }

  printf(
      "Sampling on-policy distillation dataset (games=%d, maxDec=%d, valDec=%d, threads=%d)...\n",
      sampleCfg.numGames, sampleCfg.maxDecisions, valDecisions, sampleCfg.numThreads);
  fflush(stdout);

  const auto tSamp = std::chrono::steady_clock::now();
  das::DistillDataset20 trainDs =
      das::sampleDistillDataset20(tb, guideNn.isLoaded() ? &guideNn : nullptr, sampleCfg);

  das::DistillDataset20 valDs;
  if (valDecisions > 0) {
    das::DistillSampleConfig20 valCfg = sampleCfg;
    valCfg.numGames = std::max(200, sampleCfg.numGames / 8);
    valCfg.maxDecisions = valDecisions;
    valCfg.baseSeed = sampleCfg.baseSeed + 5000000ULL;
    valDs = das::sampleDistillDataset20(tb, guideNn.isLoaded() ? &guideNn : nullptr, valCfg);
  }
  const double sampSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - tSamp).count();
  printf("Sampled train=%zu (steps=%llu skipped=%llu) | val=%zu in %.2fs\n",
         trainDs.decisions.size(), static_cast<unsigned long long>(trainDs.totalSteps),
         static_cast<unsigned long long>(trainDs.skippedIncomplete), valDs.decisions.size(),
         sampSec);
  fflush(stdout);

  // Release indexed TB RAM before training.
  const das::RngMode rngMode = tb.rngMode();
  trainCfg.gamma = tb.gamma();
  trainCfg.discountMode = tb.discountMode();
  tb.clear();

  printf(
      "Training TinyDasNet (%dx%d, %zu params, %.1f KB, epochs=%d, bs=%d, lr=%.1e, T=%.2f, "
      "delta=%.2f)...\n",
      trainCfg.h1, trainCfg.h2,
      das::TinyDasNet::totalParams(das::NN_INPUT_DIM, trainCfg.h1, trainCfg.h2),
      static_cast<double>(das::TinyDasNet::totalParams(das::NN_INPUT_DIM, trainCfg.h1, trainCfg.h2)
                          * sizeof(float))
          / 1024.0,
      trainCfg.epochs, trainCfg.batchSize, trainCfg.lr, trainCfg.temperature, trainCfg.huberDelta);
  fflush(stdout);

  das::TinyDasNet trained = das::trainTinyDasNet(trainDs.decisions, valDs.decisions, rngMode,
                                                 trainCfg, initNn.isLoaded() ? &initNn : nullptr);

  if (!trained.save(outPath, &err)) {
    fprintf(stderr, "Error saving %s: %s\n", outPath.c_str(), err.c_str());
    return 1;
  }

  const das::NnEvalMetrics20 finalVal = das::evaluateNnOnDecisions20(
      trained, valDs.decisions, trainCfg.gamma, nullptr, trainCfg.discountMode);
  const double totalSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
  printf("DONE train_nn_20row -> %s\n"
         "  Final val: MAE=%.4f | top1=%.2f%% | regret=%.4f | total=%.2fs\n",
         outPath.c_str(), finalVal.valMae, finalVal.top1ExactPct, finalVal.meanRegret, totalSec);
  return 0;
}
