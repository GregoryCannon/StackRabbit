// generate_nextbox.cpp — Parallel 1-piece Next-Box self-play state-space sampler.
//
// Build:
//   g++ -O3 -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/generate_nextbox.cpp -o build/tablebase_gen_nb
//
// Usage:
//   ./build/tablebase_gen_nb <output.states.bin> [options]

#include "cli_utils.hpp"
#include "nextbox_gen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t COVERAGE_SEED_XOR = 0x5A5A5A5AA5A5A5A5ULL;

struct CliOptions {
  std::string outPath;
  std::string tbPath;
  std::string mergePath;
  tb::NextBoxGenConfig cfg{};
  int coverageGames = 5000;
  bool showHelp = false;
};

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s <output.states.bin> [options]\n", prog);
  std::fprintf(out, "  --rng=uniform|nes      Piece RNG mode (default: nes)\n");
  std::fprintf(out, "  --games=N              Self-play games (default: 200000)\n");
  std::fprintf(
      out, "  --epsilon=F            Non-greedy top-K exploration rate in [0,1] (default: 0.04)\n");
  std::fprintf(out, "  --top-k=K              Exploration runner-up cap in [2,64] (default: 4)\n");
  std::fprintf(out, "  --min-visits=V         Min visits filter in [1,1000000] (default: 2)\n");
  std::fprintf(out, "  --max-boards=M         Max boards cap, 0=unlimited (default: 20000000)\n");
  std::fprintf(out,
               "  --sibling-min=S        Core sibling expansion threshold, 0=off (default: 5)\n");
  std::fprintf(out, "  --tb=PATH              Optional SRTBV002 Next-Box *.tb.bin policy\n");
  std::fprintf(out, "  --merge=PATH           Optional existing *.states.bin to merge\n");
  std::fprintf(out, "  --seed=N               Base RNG seed (default: 1000000)\n");
  std::fprintf(out, "  --coverage-games=N     Held-out greedy coverage games (default: 5000)\n");
  std::fprintf(out, "  --threads=T            Worker threads (default: hardware concurrency)\n");
}

bool parseArgs(int argc, char **argv, CliOptions &opts) {
  opts.cfg.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  opts.cfg.verbose = true;

  std::set<std::string> seenFlags;
  auto claimFlag = [&](const std::string &name) -> bool {
    if (!seenFlags.insert(name).second) {
      std::fprintf(stderr, "Duplicate option: %s\n", name.c_str());
      return false;
    }
    return true;
  };

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
      if (!claimFlag(flag)) {
        return false;
      }

      uint64_t uVal = 0;
      double dVal = 0.0;
      if (flag == "--rng") {
        if (!tb::parseRngMode(val, opts.cfg.rngMode)) {
          std::fprintf(stderr, "Invalid --rng value '%s' (expected uniform or nes)\n", val.c_str());
          return false;
        }
      } else if (flag == "--games") {
        if (!tb::cli::parseStrictU64(val, 1, 100000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --games value '%s' (expected 1..100000000)\n", val.c_str());
          return false;
        }
        opts.cfg.numGames = static_cast<int>(uVal);
      } else if (flag == "--epsilon") {
        if (!tb::cli::parseStrictPositiveDouble(val, 0.0, 1.0, dVal)) {
          std::fprintf(stderr, "Invalid --epsilon value '%s' (expected decimal in [0.0, 1.0])\n",
                       val.c_str());
          return false;
        }
        opts.cfg.epsilon = dVal;
      } else if (flag == "--top-k") {
        if (!tb::cli::parseStrictU64(val, 2, 64, uVal)) {
          std::fprintf(stderr, "Invalid --top-k value '%s' (expected 2..64)\n", val.c_str());
          return false;
        }
        opts.cfg.exploreTopK = static_cast<int>(uVal);
      } else if (flag == "--min-visits") {
        if (!tb::cli::parseStrictU64(val, 1, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --min-visits value '%s' (expected 1..1000000)\n",
                       val.c_str());
          return false;
        }
        opts.cfg.minVisits = static_cast<uint32_t>(uVal);
      } else if (flag == "--max-boards") {
        if (!tb::cli::parseStrictU64(val, 0, 500000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --max-boards value '%s' (expected 0..500000000)\n",
                       val.c_str());
          return false;
        }
        opts.cfg.maxBoards = static_cast<size_t>(uVal);
      } else if (flag == "--sibling-min") {
        if (!tb::cli::parseStrictU64(val, 0, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --sibling-min value '%s' (expected 0..1000000)\n",
                       val.c_str());
          return false;
        }
        opts.cfg.siblingMinVisits = static_cast<uint32_t>(uVal);
      } else if (flag == "--tb") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --tb=\n");
          return false;
        }
        opts.tbPath = val;
      } else if (flag == "--merge") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --merge=\n");
          return false;
        }
        opts.mergePath = val;
      } else if (flag == "--seed") {
        if (!tb::cli::parseStrictU64(val, 0, UINT64_MAX, uVal)) {
          std::fprintf(stderr, "Invalid --seed value '%s'\n", val.c_str());
          return false;
        }
        opts.cfg.baseSeed = uVal;
      } else if (flag == "--coverage-games") {
        if (!tb::cli::parseStrictU64(val, 0, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --coverage-games value '%s' (expected 0..1000000)\n",
                       val.c_str());
          return false;
        }
        opts.coverageGames = static_cast<int>(uVal);
      } else if (flag == "--threads") {
        if (!tb::cli::parseStrictU64(val, 1, 256, uVal)) {
          std::fprintf(stderr, "Invalid --threads value '%s' (expected 1..256)\n", val.c_str());
          return false;
        }
        opts.cfg.numThreads = static_cast<int>(uVal);
      } else {
        std::fprintf(stderr, "Unknown option: %s\n", flag.c_str());
        return false;
      }
    } else if (!arg.empty() && arg[0] != '-' && opts.outPath.empty()) {
      opts.outPath = arg;
    } else {
      std::fprintf(stderr, "Invalid or duplicate argument: %s\n", arg.c_str());
      return false;
    }
  }

  if (opts.outPath.empty()) {
    std::fprintf(stderr, "Missing required <output.states.bin> path.\n");
    return false;
  }
  opts.cfg.progressInterval = std::max(1, opts.cfg.numGames / 8);
  return true;
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

  tb::MappedNextBoxTablebase mappedTb;
  const tb::MappedNextBoxTablebase *tbPtr = nullptr;
  std::string err;
  if (!opts.tbPath.empty()) {
    if (!mappedTb.open(opts.tbPath, &err, /*verifySorted=*/true)) {
      std::fprintf(stderr, "Error opening Next-Box tablebase '%s': %s\n", opts.tbPath.c_str(),
                   err.c_str());
      return 1;
    }
    if (mappedTb.rngMode() != opts.cfg.rngMode) {
      std::fprintf(stderr, "Error: --tb tablebase '%s' has rng=%s, which does not match --rng=%s\n",
                   opts.tbPath.c_str(), tb::rngModeName(mappedTb.rngMode()),
                   tb::rngModeName(opts.cfg.rngMode));
      return 1;
    }
    tbPtr = &mappedTb;
  }

  std::vector<uint64_t> priorBoards;
  const std::vector<uint64_t> *priorPtr = nullptr;
  if (!opts.mergePath.empty()) {
    if (!tb::readStatesFile(opts.mergePath, priorBoards, &err)) {
      std::fprintf(stderr, "Error reading merge states '%s': %s\n", opts.mergePath.c_str(),
                   err.c_str());
      return 1;
    }
    const size_t minNeeded =
        priorBoards.size() + ((!priorBoards.empty() && priorBoards.front() == 0ULL) ? 0U : 1U);
    if (opts.cfg.maxBoards > 0 && minNeeded > opts.cfg.maxBoards) {
      std::fprintf(stderr,
                   "Error: --merge file needs %zu slots (including empty board 0) which exceeds "
                   "--max-boards=%zu\n",
                   minNeeded, opts.cfg.maxBoards);
      return 1;
    }
    priorPtr = &priorBoards;
  }

  std::printf(
      "=== Next-Box State-Space Generator (rng=%s, %d games, %d threads, seed=%llu, eps=%.3f "
      "topK=%d, minVisits=%u, maxBoards=%zu, siblingMin=%u) ===\n",
      tb::rngModeName(opts.cfg.rngMode), opts.cfg.numGames, opts.cfg.numThreads,
      static_cast<unsigned long long>(opts.cfg.baseSeed), opts.cfg.epsilon, opts.cfg.exploreTopK,
      opts.cfg.minVisits, opts.cfg.maxBoards, opts.cfg.siblingMinVisits);
  if (tbPtr) {
    std::printf("  Rollout policy: Next-Box TB (%zu boards from %s) + 2-ply heuristic fallback\n",
                static_cast<size_t>(mappedTb.size()), opts.tbPath.c_str());
  } else {
    std::printf("  Rollout policy: Pure 2-ply Next-Box heuristic\n");
  }
  if (priorPtr) {
    std::printf("  Merging prior state set: %zu boards from %s\n", priorBoards.size(),
                opts.mergePath.c_str());
  }
  std::fflush(stdout);

  const tb::StateGenResult gen = tb::generateNextBoxStateSpace(opts.cfg, tbPtr, priorPtr);
  std::vector<uint64_t>().swap(priorBoards);

  const double mvPerSec = (gen.elapsedSec > 0.0)
                              ? (static_cast<double>(gen.totalMovesSimulated) / gen.elapsedSec)
                              : 0.0;
  std::printf("\nGenerated %zu final boards in %.2fs (%.1fk mv/s, %llu total moves, mean %.2f "
              "lines/game)\n",
              gen.sortedBoards.size(), gen.elapsedSec, mvPerSec / 1000.0,
              static_cast<unsigned long long>(gen.totalMovesSimulated), gen.meanTrainLines);
  std::printf("  Raw unique discovered: %llu | Sibling-rescued: %llu | Prior merged: %llu\n",
              static_cast<unsigned long long>(gen.rawUniqueDiscovered),
              static_cast<unsigned long long>(gen.siblingBoardsAdded),
              static_cast<unsigned long long>(gen.priorBoardsMerged));
  std::printf("  Final set by holes   : 0h=%llu | 1h=%llu | 2h=%llu | 3+h=%llu\n",
              static_cast<unsigned long long>(gen.boardHoleDistribution[0]),
              static_cast<unsigned long long>(gen.boardHoleDistribution[1]),
              static_cast<unsigned long long>(gen.boardHoleDistribution[2]),
              static_cast<unsigned long long>(gen.boardHoleDistribution[3]));

  if (!tb::cli::ensureParentDirectory(opts.outPath)) {
    return 1;
  }
  if (!tb::writeStatesFile(opts.outPath, gen.sortedBoards, &err)) {
    std::fprintf(stderr, "Error writing states file: %s\n", err.c_str());
    return 1;
  }
  std::printf("Saved %zu boards (%.1f MB) to %s\n", gen.sortedBoards.size(),
              static_cast<double>(gen.sortedBoards.size() * sizeof(uint64_t)) / (1024.0 * 1024.0),
              opts.outPath.c_str());

  if (opts.coverageGames > 0) {
    const uint64_t covSeed = opts.cfg.baseSeed ^ COVERAGE_SEED_XOR;
    std::printf("\nEvaluating held-out greedy Next-Box coverage (%d fresh games, rng=%s, "
                "seed=%llu)...\n",
                opts.coverageGames, tb::rngModeName(opts.cfg.rngMode),
                static_cast<unsigned long long>(covSeed));
    const tb::CoverageReport cov =
        tb::measureNextBoxCoverage(gen.sortedBoards, opts.coverageGames, covSeed, opts.cfg.rngMode,
                                   tbPtr, opts.cfg.numThreads, opts.cfg.weights);
    std::printf(
        "  Held-out greedy mean : %.2f +- %.2f lines | Move hit-rate: %.2f%% | 100%%-covered "
        "games: %.2f%%\n",
        cov.meanLines, cov.stdErrLines, cov.overallHitPct, cov.fullyCoveredGamePct);
    tb::printHoleStratifiedCoverage(cov.byHoles);
  }

  return 0;
}
