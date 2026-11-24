// solve_oracle.cpp — Exact bounded-overhang tablebase solver and Monte Carlo verifier.
//
// Enumerates all canonical 4x10 boards with 0 sealed holes and <= 1 open overhang cell
// (or --pure-surface for 0 overhangs), solves the exact closed-world Bellman equation via
// Modified Policy Iteration, writes the *.tb.bin file, and verifies V(emptyBoard) against
// empirical Monte Carlo game rollouts.
//
// Build:
//   g++ -O3 -std=c++17 -pthread -Wall -Wextra -Werror experimental/tablebase/solve_oracle.cpp -o
//   build/tablebase_oracle
//
// Usage:
//   ./build/tablebase_oracle [output.tb.bin] [--pure-surface] [--mc-games=50000]

#include "vi_solver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t MC_BASE_SEED = 700000ULL;
constexpr int MAX_LIVE_FALLBACK_GAMES = 5000;
constexpr double MAX_PASSING_Z_SCORE = 3.5;

bool parseFlagIntRange(const std::string &arg, const std::string &prefix, int minVal, int &out) {
  if (arg.rfind(prefix, 0) != 0)
    return false;
  const std::string sub = arg.substr(prefix.size());
  if (sub.empty())
    return false;
  long v = 0;
  for (char c : sub) {
    if (c < '0' || c > '9')
      return false;
    v = v * 10 + (c - '0');
    if (v > 100000000L)
      return false;
  }
  if (v < minVal)
    return false;
  out = static_cast<int>(v);
  return true;
}

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s [output.tb.bin] [--pure-surface] [--mc-games=50000]\n", prog);
}

} // namespace

int main(int argc, char **argv) {
  std::string outPath;
  bool pureSurface = false;
  int mcGames = 50000;

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
    } else if (arg == "--pure-surface") {
      if (!claimFlag("--pure-surface")) {
        printUsage(argv[0], stderr);
        return 1;
      }
      pureSurface = true;
    } else if (arg.rfind("--mc-games=", 0) == 0) {
      if (!claimFlag("--mc-games=") || !parseFlagIntRange(arg, "--mc-games=", 100, mcGames)) {
        std::fprintf(stderr, "Invalid --mc-games value (expected >= 100): %s\n", arg.c_str());
        printUsage(argv[0], stderr);
        return 1;
      }
    } else if (!arg.empty() && arg[0] != '-' && outPath.empty()) {
      outPath = arg;
    } else {
      std::fprintf(stderr, "Unknown or duplicate argument: %s\n", arg.c_str());
      printUsage(argv[0], stderr);
      return 1;
    }
  }

  if (outPath.empty()) {
    outPath = pureSurface ? "experimental/tablebase/data/oracle_0hole.tb.bin"
                          : "experimental/tablebase/data/oracle_1overhang.tb.bin";
  }

  const int hwThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  const int maxOverhangs = pureSurface ? 0 : 1;

  std::printf(
      "=== Exact Bounded-Overhang Tablebase Solver (sealedHoles == 0 && overhangs <= %d) ===\n",
      maxOverhangs);
  const auto enumT0 = std::chrono::steady_clock::now();
  const std::vector<uint64_t> boards = tb::enumerateZeroSealedHoleBoards(maxOverhangs, hwThreads);
  const double enumSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - enumT0).count();

  std::printf("Enumerated %zu canonical boards in %.2fs (%.1f MB tablebase on disk)\n",
              boards.size(), enumSec,
              static_cast<double>(boards.size() * sizeof(tb::PackedTablebaseRecord))
                  / (1024.0 * 1024.0));
  std::fflush(stdout);

  tb::ViSolveConfig cfg{};
  cfg.tolerance = 1e-7;
  cfg.maxOuterIters = 250;
  cfg.innerEvalSweeps = 25;
  cfg.numThreads = hwThreads;
  cfg.verbose = true;

  const tb::ViSolveResult solved = tb::solveValueIterationMPI(boards, cfg);
  std::printf(
      "Solved in %.2fs (%d outer MPI steps, %d inner sweeps) | V(empty) = %.6f | residual = %.3e\n",
      solved.elapsedSec, solved.outerIterations, solved.totalInnerSweeps, solved.emptyBoardValue,
      solved.finalBellmanResidual);
  if (solved.finalBellmanResidual > cfg.tolerance) {
    std::fprintf(stderr, "ERROR: Solver failed to reach tolerance %.2e (residual=%.3e)\n",
                 cfg.tolerance, solved.finalBellmanResidual);
    return 1;
  }

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
  std::string err;
  if (!tb::writeTablebaseFile(outPath, boards, solved.values, solved.finalBellmanResidual, &err)) {
    std::fprintf(stderr, "Error writing tablebase: %s\n", err.c_str());
    return 1;
  }
  std::printf("Saved tablebase to %s\n", outPath.c_str());

  tb::MappedTablebase mapped;
  if (!mapped.open(outPath, &err, /*verifySorted=*/false)) {
    std::fprintf(stderr, "Error mmap-reopening tablebase: %s\n", err.c_str());
    return 1;
  }

  std::printf("\nRunning Monte Carlo verification (%d games, %d threads)...\n", mcGames, hwThreads);
  const tb::TablebaseEvalStats cw =
      tb::evaluateTablebasePolicy(mapped, /*closedWorld=*/true, mcGames, MC_BASE_SEED, hwThreads,
                                  /*fallbackPly=*/2, tb::EvalWeights{}, tb::DEFAULT_EVAL_MAX_STEPS);
  const double absDiff = std::abs(cw.meanLines - solved.emptyBoardValue);
  const double zScore =
      tb::computeClosedWorldZScore(cw.meanLines, cw.stdErrLines, solved.emptyBoardValue);
  std::printf(
      "  Closed-world MC rollout : mean = %.4f +- %.4f (max=%d, capHits=%d, in-TB=%.2f%%) vs "
      "V(empty)=%.4f [|diff|=%.4f, %.2f sigma]\n",
      cw.meanLines, cw.stdErrLines, cw.maxLines, cw.stepCapHits, cw.inTbMovePct,
      solved.emptyBoardValue, absDiff, zScore);

  const int liveGames = std::min(mcGames, MAX_LIVE_FALLBACK_GAMES);
  const tb::TablebaseEvalStats live =
      tb::evaluateTablebasePolicy(mapped, /*closedWorld=*/false, liveGames, MC_BASE_SEED, hwThreads,
                                  /*fallbackPly=*/2, tb::EvalWeights{}, tb::DEFAULT_EVAL_MAX_STEPS);
  std::printf("  Live TB + 2-ply fallback: mean = %.4f +- %.4f (games=%d, max=%d, in-TB=%.2f%%)\n",
              live.meanLines, live.stdErrLines, liveGames, live.maxLines, live.inTbMovePct);

  if (cw.stepCapHits > 0) {
    std::fprintf(stderr, "ERROR: %d closed-world MC rollouts hit DEFAULT_EVAL_MAX_STEPS (%d)\n",
                 cw.stepCapHits, tb::DEFAULT_EVAL_MAX_STEPS);
    return 1;
  }
  if (zScore > MAX_PASSING_Z_SCORE) {
    std::fprintf(stderr, "ERROR: Closed-world MC rollout deviated from V(empty) by %.2f sigma!\n",
                 zScore);
    return 1;
  }
  return 0;
}
