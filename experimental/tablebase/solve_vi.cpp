// solve_vi.cpp — Parallel Modified Policy Iteration solver for arbitrary *.states.bin state sets.
//
// Loads a strictly sorted 4x10 state set (*.states.bin), builds a flat open-addressing hash index,
// solves the undiscounted afterstate Bellman equation in IEEE-754 double precision via parallel
// double-buffered MPI, writes a compact mmap-ready *.tb.bin file, and evaluates both closed-world
// Bellman consistency and live TB + heuristic fallback gameplay.
//
// Build:
//   g++ -O3 -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/solve_vi.cpp -o build/tablebase_solve
//
// Usage:
//   ./build/tablebase_solve <input.states.bin> <output.tb.bin> [options]

#include "vi_solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

constexpr double MAX_PASSING_Z_SCORE = 3.5;

struct CliOptions {
  std::string inPath;
  std::string outPath;
  tb::ViSolveConfig cfg{};
  int mcGames = 20000;
  int evalGames = 10000;
  int fallbackPly = 2;
  uint64_t evalSeed = 700000ULL;
  bool showHelp = false;
};

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s <input.states.bin> <output.tb.bin> [options]\n", prog);
  std::fprintf(out,
               "  --tol=F                Bellman residual tolerance in [1e-12, 1e-1] (default: "
               "1e-7)\n");
  std::fprintf(out, "  --max-iters=N          Max outer MPI iterations in [1,10000] (default: "
                    "250)\n");
  std::fprintf(out, "  --inner-sweeps=N       Fixed-policy inner sweeps per outer step in [0,1000] "
                    "(default: 25)\n");
  std::fprintf(out, "  --mc-games=N           Closed-world MC verification games, 0=skip or "
                    "[100,1000000] (default: 20000)\n");
  std::fprintf(out, "  --eval-games=N         Live TB+fallback eval games in [0,1000000] "
                    "(default: 10000)\n");
  std::fprintf(out, "  --fallback-ply=1|2     Heuristic fallback ply for live play (default: 2)\n");
  std::fprintf(out,
               "  --seed=N               Base RNG seed for MC & live eval (default: 700000)\n");
  std::fprintf(out, "  --threads=T            Worker threads in [1,256] (default: hardware "
                    "concurrency)\n");
  std::fprintf(out, "  --quiet                Suppress per-iteration MPI progress lines\n");
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

// Strict positive decimal/scientific float parser (e.g. "1e-7", "0.00001"); rejects whitespace,
// hex floats, NaN, Inf, or junk suffixes.
bool parseStrictPositiveDouble(const std::string &sub, double minVal, double maxVal, double &out) {
  if (sub.empty())
    return false;
  size_t i = 0;
  bool seenMantissaDigit = false;
  bool seenDot = false;
  while (i < sub.size()) {
    const char c = sub[i];
    if (c >= '0' && c <= '9') {
      seenMantissaDigit = true;
      ++i;
    } else if (c == '.' && !seenDot) {
      seenDot = true;
      ++i;
    } else {
      break;
    }
  }
  if (!seenMantissaDigit)
    return false;

  if (i < sub.size()) {
    if (sub[i] != 'e' && sub[i] != 'E')
      return false;
    ++i;
    if (i < sub.size() && (sub[i] == '+' || sub[i] == '-'))
      ++i;
    bool seenExpDigit = false;
    while (i < sub.size() && sub[i] >= '0' && sub[i] <= '9') {
      seenExpDigit = true;
      ++i;
    }
    if (!seenExpDigit || i != sub.size())
      return false;
  }

  char *endPtr = nullptr;
  const double v = std::strtod(sub.c_str(), &endPtr);
  if (endPtr == sub.c_str() || *endPtr != '\0' || !std::isfinite(v) || v < minVal || v > maxVal) {
    return false;
  }
  out = v;
  return true;
}

bool parseArgs(int argc, char **argv, CliOptions &opts) {
  opts.cfg.tolerance = 1e-7;
  opts.cfg.maxOuterIters = 250;
  opts.cfg.innerEvalSweeps = 25;
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
    if (arg == "--quiet") {
      if (!claimFlag("--quiet")) {
        return false;
      }
      opts.cfg.verbose = false;
      continue;
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
      if (flag == "--tol") {
        if (!parseStrictPositiveDouble(val, 1e-12, 1e-1, dVal)) {
          std::fprintf(stderr, "Invalid --tol value '%s' (expected [1e-12, 1e-1])\n", val.c_str());
          return false;
        }
        opts.cfg.tolerance = dVal;
      } else if (flag == "--max-iters") {
        if (!parseStrictU64(val, 1, 10000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --max-iters value '%s' (expected 1..10000)\n", val.c_str());
          return false;
        }
        opts.cfg.maxOuterIters = static_cast<int>(uVal);
      } else if (flag == "--inner-sweeps") {
        if (!parseStrictU64(val, 0, 1000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --inner-sweeps value '%s' (expected 0..1000)\n",
                       val.c_str());
          return false;
        }
        opts.cfg.innerEvalSweeps = static_cast<int>(uVal);
      } else if (flag == "--mc-games") {
        if (!parseStrictU64(val, 0, 1000000ULL, uVal) || (uVal > 0 && uVal < 100)) {
          std::fprintf(stderr, "Invalid --mc-games value '%s' (expected 0 or 100..1000000)\n",
                       val.c_str());
          return false;
        }
        opts.mcGames = static_cast<int>(uVal);
      } else if (flag == "--eval-games") {
        if (!parseStrictU64(val, 0, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --eval-games value '%s' (expected 0..1000000)\n",
                       val.c_str());
          return false;
        }
        opts.evalGames = static_cast<int>(uVal);
      } else if (flag == "--fallback-ply") {
        if (!parseStrictU64(val, 1, 2, uVal)) {
          std::fprintf(stderr, "Invalid --fallback-ply value '%s' (expected 1 or 2)\n",
                       val.c_str());
          return false;
        }
        opts.fallbackPly = static_cast<int>(uVal);
      } else if (flag == "--seed") {
        if (!parseStrictU64(val, 0, UINT64_MAX, uVal)) {
          std::fprintf(stderr, "Invalid --seed value '%s'\n", val.c_str());
          return false;
        }
        opts.evalSeed = uVal;
      } else if (flag == "--threads") {
        if (!parseStrictU64(val, 1, 256, uVal)) {
          std::fprintf(stderr, "Invalid --threads value '%s' (expected 1..256)\n", val.c_str());
          return false;
        }
        opts.cfg.numThreads = static_cast<int>(uVal);
      } else {
        std::fprintf(stderr, "Unknown option: %s\n", flag.c_str());
        return false;
      }
    } else if (!arg.empty() && arg[0] != '-') {
      if (opts.inPath.empty()) {
        opts.inPath = arg;
      } else if (opts.outPath.empty()) {
        opts.outPath = arg;
      } else {
        std::fprintf(stderr, "Unexpected positional argument: %s\n", arg.c_str());
        return false;
      }
    } else {
      std::fprintf(stderr, "Invalid argument: %s\n", arg.c_str());
      return false;
    }
  }

  if (opts.inPath.empty() || opts.outPath.empty()) {
    std::fprintf(stderr, "Missing required <input.states.bin> and <output.tb.bin> paths.\n");
    return false;
  }
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

  const auto loadT0 = std::chrono::steady_clock::now();
  std::vector<uint64_t> boards;
  std::string err;
  if (!tb::readStatesFile(opts.inPath, boards, &err)) {
    std::fprintf(stderr, "Error reading states file '%s': %s\n", opts.inPath.c_str(), err.c_str());
    return 1;
  }
  if (boards.empty()) {
    std::fprintf(stderr, "Error: states file '%s' contains 0 boards.\n", opts.inPath.c_str());
    return 1;
  }
  if (boards.size() >= static_cast<size_t>(tb::INVALID_BOARD_ID)) {
    std::fprintf(stderr, "Error: states file has too many boards (%zu >= %u).\n", boards.size(),
                 tb::INVALID_BOARD_ID);
    return 1;
  }

  std::array<uint64_t, tb::NUM_HOLE_BUCKETS> holeDist{};
  for (uint64_t b : boards) {
    ++holeDist[static_cast<size_t>(tb::HoleStratifiedCounts::bucket(tb::countHoles(b)))];
  }
  const double loadSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - loadT0).count();

  const auto idxT0 = std::chrono::steady_clock::now();
  const tb::FlatBoardIndex index(boards);
  const double idxSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - idxT0).count();

  std::printf("=== Value Iteration / MPI Solver (%zu boards, %d threads, tol=%.1e, inner=%d) ===\n",
              boards.size(), opts.cfg.numThreads, opts.cfg.tolerance, opts.cfg.innerEvalSweeps);
  std::printf("  Loaded %s in %.2fs | Built FlatBoardIndex in %.2fs | Output %.1f MB\n",
              opts.inPath.c_str(), loadSec, idxSec,
              static_cast<double>(boards.size() * sizeof(tb::PackedTablebaseRecord))
                  / (1024.0 * 1024.0));
  std::printf(
      "  State-set holes: 0h=%llu | 1h=%llu | 2h=%llu | 3+h=%llu\n",
      static_cast<unsigned long long>(holeDist[0]), static_cast<unsigned long long>(holeDist[1]),
      static_cast<unsigned long long>(holeDist[2]), static_cast<unsigned long long>(holeDist[3]));
  std::fflush(stdout);

  const tb::ViSolveResult solved = tb::solveValueIterationMPI(boards, index, opts.cfg);
  std::printf(
      "Solved in %.2fs (%d outer MPI steps, %d inner sweeps) | V(empty) = %.6f | residual = %.3e\n",
      solved.elapsedSec, solved.outerIterations, solved.totalInnerSweeps, solved.emptyBoardValue,
      solved.finalBellmanResidual);
  if (solved.finalBellmanResidual > opts.cfg.tolerance) {
    std::fprintf(stderr, "ERROR: Solver failed to reach tolerance %.2e (residual=%.3e)\n",
                 opts.cfg.tolerance, solved.finalBellmanResidual);
    return 1;
  }

  const std::filesystem::path p(opts.outPath);
  if (p.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
      std::fprintf(stderr, "Error creating parent directory '%s': %s\n",
                   p.parent_path().string().c_str(), ec.message().c_str());
      return 1;
    }
  }
  if (!tb::writeTablebaseFile(opts.outPath, boards, solved.values, solved.finalBellmanResidual,
                              &err)) {
    std::fprintf(stderr, "Error writing tablebase '%s': %s\n", opts.outPath.c_str(), err.c_str());
    return 1;
  }
  std::printf("Saved tablebase to %s\n", opts.outPath.c_str());

  tb::MappedTablebase mapped;
  if (!mapped.open(opts.outPath, &err, /*verifySorted=*/false)) {
    std::fprintf(stderr, "Error mmap-reopening tablebase '%s': %s\n", opts.outPath.c_str(),
                 err.c_str());
    return 1;
  }

  if (opts.mcGames > 0) {
    if (solved.emptyBoardValue < 0.0) {
      std::printf("\nSkipping closed-world MC verification (empty board 0ULL absent from set).\n");
    } else {
      std::printf("\nRunning closed-world MC verification (%d games, seed=%llu, %d threads)...\n",
                  opts.mcGames, static_cast<unsigned long long>(opts.evalSeed),
                  opts.cfg.numThreads);
      const tb::TablebaseEvalStats cw = tb::evaluateTablebasePolicy(
          mapped, /*closedWorld=*/true, opts.mcGames, opts.evalSeed, opts.cfg.numThreads,
          opts.fallbackPly, tb::EvalWeights{}, tb::DEFAULT_EVAL_MAX_STEPS);
      const double absDiff = std::abs(cw.meanLines - solved.emptyBoardValue);
      const double zScore =
          tb::computeClosedWorldZScore(cw.meanLines, cw.stdErrLines, solved.emptyBoardValue);
      std::printf(
          "  Closed-world MC : mean = %.4f +- %.4f (median=%.1f, p90=%d, max=%d, in-TB=%.2f%%, "
          "capHits=%d) vs V(empty)=%.4f [|diff|=%.4f, %.2f sigma]\n",
          cw.meanLines, cw.stdErrLines, cw.medianLines, cw.p90Lines, cw.maxLines, cw.inTbMovePct,
          cw.stepCapHits, solved.emptyBoardValue, absDiff, zScore);
      if (cw.stepCapHits > 0) {
        std::fprintf(stderr, "ERROR: %d closed-world MC rollouts hit DEFAULT_EVAL_MAX_STEPS (%d)\n",
                     cw.stepCapHits, tb::DEFAULT_EVAL_MAX_STEPS);
        return 1;
      }
      if (zScore > MAX_PASSING_Z_SCORE) {
        std::fprintf(stderr, "ERROR: Closed-world MC deviated from V(empty) by %.2f sigma!\n",
                     zScore);
        return 1;
      }
    }
  }

  tb::TablebaseEvalStats live{};
  if (opts.evalGames > 0) {
    std::printf("\nRunning live TB + %d-ply fallback evaluation (%d games, seed=%llu, %d "
                "threads)...\n",
                opts.fallbackPly, opts.evalGames, static_cast<unsigned long long>(opts.evalSeed),
                opts.cfg.numThreads);
    live = tb::evaluateTablebasePolicy(mapped, /*closedWorld=*/false, opts.evalGames, opts.evalSeed,
                                       opts.cfg.numThreads, opts.fallbackPly, tb::EvalWeights{},
                                       tb::DEFAULT_EVAL_MAX_STEPS);
    std::printf(
        "  Live TB + %d-ply : mean = %.4f +- %.4f (median=%.1f, p90=%d, max=%d, capHits=%d)\n",
        opts.fallbackPly, live.meanLines, live.stdErrLines, live.medianLines, live.p90Lines,
        live.maxLines, live.stepCapHits);
    std::printf(
        "  Live coverage   : in-TB moves = %.2f%% | all-OUT fallback = %.2f%% | 100%%-covered "
        "games = %.2f%%\n",
        live.inTbMovePct, live.fallbackMovePct, live.fullyCoveredGamePct);
    tb::printHoleStratifiedCoverage(live.byHoles);
  }

  if (opts.evalGames > 0) {
    std::printf("\nSUMMARY | boards=%zu | V(empty)=%.6f | solve=%.2fs | live_mean=%.4f+-%.4f | "
                "in_tb=%.2f%% | full_games=%.2f%%\n",
                boards.size(), solved.emptyBoardValue, solved.elapsedSec, live.meanLines,
                live.stdErrLines, live.inTbMovePct, live.fullyCoveredGamePct);
  } else {
    std::printf("\nSUMMARY | boards=%zu | V(empty)=%.6f | solve=%.2fs | live=skipped\n",
                boards.size(), solved.emptyBoardValue, solved.elapsedSec);
  }
  return 0;
}
