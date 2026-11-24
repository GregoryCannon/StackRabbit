// generate_states.cpp — Parallel self-play trajectory state-space sampler and coverage reporter.
//
// Build:
//   g++ -O3 -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/generate_states.cpp -o build/tablebase_gen
//
// Usage:
//   ./build/tablebase_gen <output.states.bin> [options]

#include "state_generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t COVERAGE_SEED_XOR = 0xA5A5A5A55A5A5A5AULL;

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s <output.states.bin> [options]\n", prog);
  std::fprintf(out, "  --games=N              Self-play games (default: 200000)\n");
  std::fprintf(out, "  --ply=1|2              Heuristic search ply (default: 2)\n");
  std::fprintf(
      out, "  --epsilon=F            Non-greedy top-K exploration rate in [0,1] (default: 0.08)\n");
  std::fprintf(out, "  --top-k=K              Exploration runner-up cap in [2,64] (default: 4)\n");
  std::fprintf(out, "  --min-visits=V         Min visits filter in [1,1000000] (default: 2)\n");
  std::fprintf(out, "  --max-boards=M         Max boards cap, 0=unlimited (default: 15000000)\n");
  std::fprintf(out,
               "  --sibling-min=S        1-ply sibling expansion threshold, 0=off (default: 5)\n");
  std::fprintf(out, "  --sibling-ply=1|2      Heuristic ply for sibling expansion (default: 1)\n");
  std::fprintf(out, "  --tb=PATH              Optional *.tb.bin policy\n");
  std::fprintf(out, "  --merge=PATH           Optional existing *.states.bin to merge\n");
  std::fprintf(out, "  --seed=N               Base RNG seed (default: 1000000)\n");
  std::fprintf(out, "  --coverage-games=N     Held-out greedy coverage games (default: 5000)\n");
  std::fprintf(out, "  --threads=T            Worker threads (default: hardware concurrency)\n");
}

bool parseStrictU64(const std::string &sub, uint64_t minVal, uint64_t maxVal, uint64_t &out) {
  if (sub.empty())
    return false;
  uint64_t v = 0;
  for (char c : sub) {
    if (c < '0' || c > '9')
      return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (digit > maxVal || v > (maxVal - digit) / 10ULL)
      return false;
    v = v * 10ULL + digit;
  }
  if (v < minVal || v > maxVal)
    return false;
  out = v;
  return true;
}

// Strict decimal float parser (digits + optional single '.'); rejects whitespace, signs, hex, NaN.
bool parseStrictDecimalDouble(const std::string &sub, double minVal, double maxVal, double &out) {
  if (sub.empty())
    return false;
  bool seenDot = false;
  bool seenDigit = false;
  for (char c : sub) {
    if (c >= '0' && c <= '9') {
      seenDigit = true;
    } else if (c == '.' && !seenDot) {
      seenDot = true;
    } else {
      return false;
    }
  }
  if (!seenDigit)
    return false;
  char *endPtr = nullptr;
  const double v = std::strtod(sub.c_str(), &endPtr);
  if (endPtr == sub.c_str() || *endPtr != '\0' || !std::isfinite(v) || v < minVal || v > maxVal) {
    return false;
  }
  out = v;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::string outPath;
  std::string tbPath;
  std::string mergePath;

  tb::StateGenConfig cfg{};
  cfg.numGames = 200000;
  cfg.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  cfg.verbose = true;

  int coverageGames = 5000;
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
      printUsage(argv[0], stdout);
      return 0;
    }

    const size_t eq = arg.find('=');
    if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
      const std::string flag = arg.substr(0, eq);
      const std::string val = arg.substr(eq + 1);
      if (!claimFlag(flag)) {
        printUsage(argv[0], stderr);
        return 1;
      }

      uint64_t uVal = 0;
      double dVal = 0.0;
      if (flag == "--games") {
        if (!parseStrictU64(val, 1, 100000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --games value '%s' (expected 1..100000000)\n", val.c_str());
          return 1;
        }
        cfg.numGames = static_cast<int>(uVal);
      } else if (flag == "--ply") {
        if (!parseStrictU64(val, 1, 2, uVal)) {
          std::fprintf(stderr, "Invalid --ply value '%s' (expected 1 or 2)\n", val.c_str());
          return 1;
        }
        cfg.heurPly = static_cast<int>(uVal);
      } else if (flag == "--epsilon") {
        if (!parseStrictDecimalDouble(val, 0.0, 1.0, dVal)) {
          std::fprintf(stderr, "Invalid --epsilon value '%s' (expected decimal in [0.0, 1.0])\n",
                       val.c_str());
          return 1;
        }
        cfg.epsilon = dVal;
      } else if (flag == "--top-k") {
        if (!parseStrictU64(val, 2, 64, uVal)) {
          std::fprintf(stderr, "Invalid --top-k value '%s' (expected 2..64)\n", val.c_str());
          return 1;
        }
        cfg.exploreTopK = static_cast<int>(uVal);
      } else if (flag == "--min-visits") {
        if (!parseStrictU64(val, 1, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --min-visits value '%s' (expected 1..1000000)\n",
                       val.c_str());
          return 1;
        }
        cfg.minVisits = static_cast<uint32_t>(uVal);
      } else if (flag == "--max-boards") {
        if (!parseStrictU64(val, 0, 500000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --max-boards value '%s' (expected 0..500000000)\n",
                       val.c_str());
          return 1;
        }
        cfg.maxBoards = static_cast<size_t>(uVal);
      } else if (flag == "--sibling-min") {
        if (!parseStrictU64(val, 0, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --sibling-min value '%s' (expected 0..1000000)\n",
                       val.c_str());
          return 1;
        }
        cfg.siblingMinVisits = static_cast<uint32_t>(uVal);
      } else if (flag == "--sibling-ply") {
        if (!parseStrictU64(val, 1, 2, uVal)) {
          std::fprintf(stderr, "Invalid --sibling-ply value '%s' (expected 1 or 2)\n", val.c_str());
          return 1;
        }
        cfg.siblingPly = static_cast<int>(uVal);
      } else if (flag == "--tb") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --tb=\n");
          return 1;
        }
        tbPath = val;
      } else if (flag == "--merge") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --merge=\n");
          return 1;
        }
        mergePath = val;
      } else if (flag == "--seed") {
        if (!parseStrictU64(val, 0, UINT64_MAX, uVal)) {
          std::fprintf(stderr, "Invalid --seed value '%s'\n", val.c_str());
          return 1;
        }
        cfg.baseSeed = uVal;
      } else if (flag == "--coverage-games") {
        if (!parseStrictU64(val, 0, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --coverage-games value '%s' (expected 0..1000000)\n",
                       val.c_str());
          return 1;
        }
        coverageGames = static_cast<int>(uVal);
      } else if (flag == "--threads") {
        if (!parseStrictU64(val, 1, 256, uVal)) {
          std::fprintf(stderr, "Invalid --threads value '%s' (expected 1..256)\n", val.c_str());
          return 1;
        }
        cfg.numThreads = static_cast<int>(uVal);
      } else {
        std::fprintf(stderr, "Unknown option: %s\n", flag.c_str());
        printUsage(argv[0], stderr);
        return 1;
      }
    } else if (!arg.empty() && arg[0] != '-' && outPath.empty()) {
      outPath = arg;
    } else {
      std::fprintf(stderr, "Invalid or duplicate argument: %s\n", arg.c_str());
      printUsage(argv[0], stderr);
      return 1;
    }
  }

  if (outPath.empty()) {
    std::fprintf(stderr, "Missing required <output.states.bin> path.\n");
    printUsage(argv[0], stderr);
    return 1;
  }

  tb::MappedTablebase mappedTb;
  const tb::MappedTablebase *tbPtr = nullptr;
  std::string err;
  if (!tbPath.empty()) {
    if (!mappedTb.open(tbPath, &err, /*verifySorted=*/false)) {
      std::fprintf(stderr, "Error opening tablebase '%s': %s\n", tbPath.c_str(), err.c_str());
      return 1;
    }
    tbPtr = &mappedTb;
  }

  std::vector<uint64_t> priorBoards;
  const std::vector<uint64_t> *priorPtr = nullptr;
  if (!mergePath.empty()) {
    if (!tb::readStatesFile(mergePath, priorBoards, &err)) {
      std::fprintf(stderr, "Error reading merge states '%s': %s\n", mergePath.c_str(), err.c_str());
      return 1;
    }
    if (cfg.maxBoards > 0 && priorBoards.size() > cfg.maxBoards) {
      std::fprintf(stderr, "Error: --merge file has %zu boards which exceeds --max-boards=%zu\n",
                   priorBoards.size(), cfg.maxBoards);
      return 1;
    }
    priorPtr = &priorBoards;
  }

  cfg.progressInterval = std::max(1, cfg.numGames / 8);

  std::printf(
      "=== State-Space Trajectory Generator (%d games, %d threads, seed=%llu, ply=%d, eps=%.3f "
      "topK=%d, minVisits=%u, maxBoards=%zu, siblingMin=%u siblingPly=%d) ===\n",
      cfg.numGames, cfg.numThreads, static_cast<unsigned long long>(cfg.baseSeed), cfg.heurPly,
      cfg.epsilon, cfg.exploreTopK, cfg.minVisits, cfg.maxBoards, cfg.siblingMinVisits,
      cfg.siblingPly);
  if (tbPtr) {
    std::printf("  Rollout policy: Tablebase (%zu boards from %s) + %d-ply heuristic fallback\n",
                static_cast<size_t>(mappedTb.size()), tbPath.c_str(), cfg.heurPly);
  } else {
    std::printf("  Rollout policy: Pure %d-ply heuristic (independent of any tablebase)\n",
                cfg.heurPly);
  }
  if (priorPtr) {
    std::printf("  Merging prior state set: %zu boards from %s\n", priorBoards.size(),
                mergePath.c_str());
  }
  std::fflush(stdout);

  const tb::StateGenResult gen = tb::generateStateSpace(cfg, tbPtr, priorPtr);
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

  const std::filesystem::path p(outPath);
  if (p.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
      std::fprintf(stderr, "Error creating parent directory '%s': %s\n",
                   p.parent_path().string().c_str(), ec.message().c_str());
      return 1;
    }
  }
  if (!tb::writeStatesFile(outPath, gen.sortedBoards, &err)) {
    std::fprintf(stderr, "Error writing states file: %s\n", err.c_str());
    return 1;
  }
  std::printf("Saved %zu boards (%.1f MB) to %s\n", gen.sortedBoards.size(),
              static_cast<double>(gen.sortedBoards.size() * sizeof(uint64_t)) / (1024.0 * 1024.0),
              outPath.c_str());

  if (coverageGames > 0) {
    const uint64_t covSeed = cfg.baseSeed ^ COVERAGE_SEED_XOR;
    std::printf("\nEvaluating held-out greedy coverage (%d fresh games, seed=%llu)...\n",
                coverageGames, static_cast<unsigned long long>(covSeed));
    const tb::CoverageReport cov = tb::measureCoverage(
        gen.sortedBoards, coverageGames, covSeed, tbPtr, cfg.heurPly, cfg.numThreads, cfg.weights);
    std::printf(
        "  Held-out greedy mean : %.2f +- %.2f lines | Move hit-rate: %.2f%% | 100%%-covered "
        "games: %.2f%%\n",
        cov.meanLines, cov.stdErrLines, cov.overallHitPct, cov.fullyCoveredGamePct);
    tb::printHoleStratifiedCoverage(cov.byHoles);
  }

  return 0;
}
