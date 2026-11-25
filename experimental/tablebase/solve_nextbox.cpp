// solve_nextbox.cpp — Disk-streamed, warm-started parallel Modified Policy Iteration solver for
// 1-piece Next-Box tablebases (V(b, c), 36-byte SRTBV002 format).
//
// Build:
//   g++ -O3 -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/solve_nextbox.cpp -o build/tablebase_solve_nb
//
// Usage:
//   ./build/tablebase_solve_nb <input.states.bin|input.tb.bin> <output.tb.bin> [options]

#include "cli_utils.hpp"
#include "nextbox_solver.hpp"
#include "tiny_nn.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CliOptions {
  std::string inPath;
  std::string outPath;
  std::string initPath;
  std::string boundaryNnPath;
  double boundaryMargin = 2.0;
  int boundaryPly = 1;
  bool boundaryMarginSet = false;
  bool boundaryPlySet = false;
  tb::NextBoxSolveConfig cfg{};
  int mcGames = 20000;
  int evalGames = 10000;
  bool evalRngOverridden = false;
  tb::RngMode evalRngMode = tb::RngMode::Nes;
  uint64_t evalSeed = 700000ULL;
  bool showHelp = false;
};

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage: %s <input.states.bin|input.tb.bin> <output.tb.bin> [options]\n", prog);
  std::fprintf(out, "  --rng=uniform|nes           RNG model for Bellman solve (default: nes)\n");
  std::fprintf(out,
               "  --eval-rng=uniform|nes      Optional override RNG for live eval (default: same "
               "as --rng)\n");
  std::fprintf(out,
               "  --policy-cache=auto|ram|disk  MPI policy action storage (default: auto; disk "
               "above 6M boards)\n");
  std::fprintf(out,
               "  --init=PATH                 Optional prior SRTBV002 *.tb.bin for warm-start\n");
  std::fprintf(out, "  --boundary-nn=PATH          Optional frozen *.nn.bin for soft-boundary "
                    "Bellman bootstrapping\n");
  std::fprintf(out, "  --boundary-margin=F         Conservative subtraction on NN boundary values "
                    "in [0,1000] (default: 2.0, requires --boundary-nn)\n");
  std::fprintf(out, "  --boundary-ply=1|2          NN lookahead depth at TB boundary (default: 1, "
                    "requires --boundary-nn)\n");
  std::fprintf(out, "  --gamma=F                   Bellman discount factor in [1e-4, 1.0] "
                    "(default: 1.0)\n");
  std::fprintf(out, "  --max-v-guard=F             Abort solve if any V exceeds F in [1, 2047] "
                    "(default: 1000)\n");
  std::fprintf(out, "  --tol=F                     Bellman residual tolerance in [1e-12, 1e-1] "
                    "(default: 1e-7)\n");
  std::fprintf(out, "  --max-iters=N               Max outer MPI iterations in [1,10000] (default: "
                    "250)\n");
  std::fprintf(out,
               "  --inner-sweeps=N            Fixed-policy inner sweeps per outer step in [0,1000] "
               "(default: 50)\n");
  std::fprintf(out, "  --mc-games=N                Closed-world/soft-boundary MC games, 0=skip or "
                    "[100,1000000] (default: 20000)\n");
  std::fprintf(out, "  --eval-games=N              Live TB+2-ply heuristic fallback games in "
                    "[0,1000000] (default: 10000)\n");
  std::fprintf(out, "  --seed=N                    Base RNG seed for MC & live eval (default: "
                    "700000)\n");
  std::fprintf(out, "  --threads=T                 Worker threads in [1,256] (default: hardware "
                    "concurrency)\n");
  std::fprintf(out, "  --quiet                     Suppress per-iteration MPI progress lines\n");
}

bool parseArgs(int argc, char **argv, CliOptions &opts) {
  opts.cfg.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  opts.cfg.verbose = true;

  if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
    opts.showHelp = true;
    return true;
  }

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
    if (arg == "--quiet") {
      if (!claimFlag("--quiet"))
        return false;
      opts.cfg.verbose = false;
      continue;
    }

    const size_t eq = arg.find('=');
    if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
      const std::string flag = arg.substr(0, eq);
      const std::string val = arg.substr(eq + 1);
      if (!claimFlag(flag))
        return false;

      uint64_t uVal = 0;
      double dVal = 0.0;
      if (flag == "--rng") {
        if (!tb::parseRngMode(val, opts.cfg.rngMode)) {
          std::fprintf(stderr, "Invalid --rng '%s' (expected uniform or nes)\n", val.c_str());
          return false;
        }
      } else if (flag == "--eval-rng") {
        if (!tb::parseRngMode(val, opts.evalRngMode)) {
          std::fprintf(stderr, "Invalid --eval-rng '%s' (expected uniform or nes)\n", val.c_str());
          return false;
        }
        opts.evalRngOverridden = true;
      } else if (flag == "--policy-cache") {
        if (!tb::parsePolicyCacheMode(val, opts.cfg.policyCacheMode)) {
          std::fprintf(stderr, "Invalid --policy-cache '%s' (expected auto, ram, or disk)\n",
                       val.c_str());
          return false;
        }
      } else if (flag == "--init") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --init=\n");
          return false;
        }
        opts.initPath = val;
      } else if (flag == "--boundary-nn") {
        if (val.empty()) {
          std::fprintf(stderr, "Empty path for --boundary-nn=\n");
          return false;
        }
        opts.boundaryNnPath = val;
      } else if (flag == "--boundary-margin") {
        if (!tb::cli::parseStrictPositiveDouble(val, 0.0, 1000.0, dVal)) {
          std::fprintf(stderr, "Invalid --boundary-margin '%s' (expected [0, 1000])\n",
                       val.c_str());
          return false;
        }
        opts.boundaryMargin = dVal;
        opts.boundaryMarginSet = true;
      } else if (flag == "--boundary-ply") {
        if (!tb::cli::parseStrictU64(val, 1, 2, uVal)) {
          std::fprintf(stderr, "Invalid --boundary-ply '%s' (expected 1 or 2)\n", val.c_str());
          return false;
        }
        opts.boundaryPly = static_cast<int>(uVal);
        opts.boundaryPlySet = true;
      } else if (flag == "--gamma") {
        if (!tb::cli::parseStrictPositiveDouble(val, 1e-4, 1.0, dVal)) {
          std::fprintf(stderr, "Invalid --gamma '%s' (expected [1e-4, 1.0])\n", val.c_str());
          return false;
        }
        opts.cfg.gamma = dVal;
      } else if (flag == "--max-v-guard") {
        if (!tb::cli::parseStrictPositiveDouble(val, 1.0, tb::NB_TERMINAL_MAX_PAYOFF, dVal)) {
          std::fprintf(stderr, "Invalid --max-v-guard '%s' (expected [1, %.0f])\n", val.c_str(),
                       tb::NB_TERMINAL_MAX_PAYOFF);
          return false;
        }
        opts.cfg.maxValueDivergenceGuard = dVal;
      } else if (flag == "--tol") {
        if (!tb::cli::parseStrictPositiveDouble(val, 1e-12, 1e-1, dVal)) {
          std::fprintf(stderr, "Invalid --tol '%s' (expected [1e-12, 1e-1])\n", val.c_str());
          return false;
        }
        opts.cfg.tolerance = dVal;
      } else if (flag == "--max-iters") {
        if (!tb::cli::parseStrictU64(val, 1, 10000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --max-iters '%s' (expected 1..10000)\n", val.c_str());
          return false;
        }
        opts.cfg.maxOuterIters = static_cast<int>(uVal);
      } else if (flag == "--inner-sweeps") {
        if (!tb::cli::parseStrictU64(val, 0, 1000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --inner-sweeps '%s' (expected 0..1000)\n", val.c_str());
          return false;
        }
        opts.cfg.innerEvalSweeps = static_cast<int>(uVal);
      } else if (flag == "--mc-games") {
        if (!tb::cli::parseStrictU64(val, 0, 1000000ULL, uVal) || (uVal > 0 && uVal < 100)) {
          std::fprintf(stderr, "Invalid --mc-games '%s' (expected 0 or 100..1000000)\n",
                       val.c_str());
          return false;
        }
        opts.mcGames = static_cast<int>(uVal);
      } else if (flag == "--eval-games") {
        if (!tb::cli::parseStrictU64(val, 0, 1000000ULL, uVal)) {
          std::fprintf(stderr, "Invalid --eval-games '%s' (expected 0..1000000)\n", val.c_str());
          return false;
        }
        opts.evalGames = static_cast<int>(uVal);
      } else if (flag == "--seed") {
        if (!tb::cli::parseStrictU64(val, 0, UINT64_MAX, uVal)) {
          std::fprintf(stderr, "Invalid --seed '%s'\n", val.c_str());
          return false;
        }
        opts.evalSeed = uVal;
      } else if (flag == "--threads") {
        if (!tb::cli::parseStrictU64(val, 1, 256, uVal)) {
          std::fprintf(stderr, "Invalid --threads '%s' (expected 1..256)\n", val.c_str());
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
    std::fprintf(stderr,
                 "Missing required <input.states.bin|input.tb.bin> and <output.tb.bin> paths.\n");
    return false;
  }
  if (opts.boundaryNnPath.empty() && (opts.boundaryMarginSet || opts.boundaryPlySet)) {
    std::fprintf(stderr,
                 "Error: --boundary-margin and --boundary-ply require --boundary-nn=PATH\n");
    return false;
  }
  if (!opts.evalRngOverridden) {
    opts.evalRngMode = opts.cfg.rngMode;
  }
  return true;
}

tb::NnSoftBoundaryEval makeBoundaryEval(const tb::TinyNextBoxNet *boundaryNn,
                                        const CliOptions &opts) noexcept {
  return tb::NnSoftBoundaryEval{boundaryNn, opts.boundaryMargin, opts.cfg.rngMode,
                                opts.boundaryPly == 2};
}

bool runBellmanMcVerification(const tb::IndexedNextBoxTablebase &indexedTb, const CliOptions &opts,
                              const tb::TinyNextBoxNet *boundaryNn, double expectedEmptyValue) {
  if (expectedEmptyValue < 0.0) {
    std::printf("\nSkipping Bellman MC verification (empty board 0ULL absent from set).\n");
    return true;
  }

  const bool useSoftMc = (boundaryNn != nullptr) || (opts.cfg.gamma < 1.0);
  const char *label = (boundaryNn != nullptr)  ? "Soft-boundary MC"
                      : (opts.cfg.gamma < 1.0) ? "Discounted closed-world MC"
                                               : "Closed-world MC";
  double meanVal = 0.0;
  double stdErrVal = 0.0;
  int capHits = 0;

  if (useSoftMc) {
    std::printf("\nRunning %s verification (%d games, rng=%s, gamma=%.6f, seed=%llu, %d "
                "threads)...\n",
                label, opts.mcGames, tb::rngModeName(opts.cfg.rngMode), opts.cfg.gamma,
                static_cast<unsigned long long>(opts.evalSeed), opts.cfg.numThreads);
    const tb::SoftBoundaryMcStats mc =
        (boundaryNn != nullptr)
            ? tb::evaluateNextBoxSoftBoundaryMC(
                  indexedTb, opts.cfg.rngMode, opts.cfg.gamma, makeBoundaryEval(boundaryNn, opts),
                  opts.mcGames, opts.evalSeed, opts.cfg.numThreads, tb::DEFAULT_EVAL_MAX_STEPS)
            : tb::evaluateNextBoxSoftBoundaryMC(indexedTb, opts.cfg.rngMode, opts.cfg.gamma,
                                                tb::NoOpBoundaryEval{}, opts.mcGames, opts.evalSeed,
                                                opts.cfg.numThreads, tb::DEFAULT_EVAL_MAX_STEPS);
    meanVal = mc.meanReturn;
    stdErrVal = mc.stdErr;
    capHits = mc.stepCapHits;
    const double absDiff = std::abs(meanVal - expectedEmptyValue);
    const double zScore = tb::computeClosedWorldZScore(meanVal, stdErrVal, expectedEmptyValue);
    std::printf("  %s: mean = %.4f +- %.4f (in-TB=%.2f%%, capHits=%d) vs "
                "E[V(0)]=%.4f [|diff|=%.4f, %.2f sigma]\n",
                label, meanVal, stdErrVal, mc.inTbMovePct, capHits, expectedEmptyValue, absDiff,
                zScore);
  } else {
    std::printf("\nRunning closed-world MC verification (%d games, rng=%s, seed=%llu, %d "
                "threads)...\n",
                opts.mcGames, tb::rngModeName(opts.cfg.rngMode),
                static_cast<unsigned long long>(opts.evalSeed), opts.cfg.numThreads);
    const tb::TablebaseEvalStats cw = tb::evaluateNextBoxPolicy(
        indexedTb, opts.cfg.rngMode, /*closedWorld=*/true, opts.mcGames, opts.evalSeed,
        opts.cfg.numThreads, tb::EvalWeights{}, tb::DEFAULT_EVAL_MAX_STEPS);
    meanVal = cw.meanLines;
    stdErrVal = cw.stdErrLines;
    capHits = cw.stepCapHits;
    const double absDiff = std::abs(meanVal - expectedEmptyValue);
    const double zScore = tb::computeClosedWorldZScore(meanVal, stdErrVal, expectedEmptyValue);
    std::printf(
        "  Closed-world MC : mean = %.4f +- %.4f (median=%.1f, p90=%d, max=%d, in-TB=%.2f%%, "
        "capHits=%d) vs E[V(0)]=%.4f [|diff|=%.4f, %.2f sigma]\n",
        meanVal, stdErrVal, cw.medianLines, cw.p90Lines, cw.maxLines, cw.inTbMovePct, capHits,
        expectedEmptyValue, absDiff, zScore);
  }

  if (capHits > 0) {
    std::fprintf(stderr, "ERROR: %d %s rollouts hit DEFAULT_EVAL_MAX_STEPS (%d)\n", capHits, label,
                 tb::DEFAULT_EVAL_MAX_STEPS);
    return false;
  }
  const double zScore = tb::computeClosedWorldZScore(meanVal, stdErrVal, expectedEmptyValue);
  if (zScore > tb::cli::MAX_PASSING_Z_SCORE) {
    std::fprintf(stderr, "ERROR: %s deviated from E[V(0)] by %.2f sigma!\n", label, zScore);
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

  size_t numBoards = 0;
  double emptyExpectedValue = tb::MISSING_BOARD_VALUE;
  double solveElapsedSec = 0.0;
  std::string err;

  tb::TinyNextBoxNet boundaryNn;
  const tb::TinyNextBoxNet *boundaryNnPtr = nullptr;
  if (!opts.boundaryNnPath.empty()) {
    if (!boundaryNn.load(opts.boundaryNnPath, &err)) {
      std::fprintf(stderr, "Error loading --boundary-nn '%s': %s\n", opts.boundaryNnPath.c_str(),
                   err.c_str());
      return 1;
    }
    if (boundaryNn.rngMode != opts.cfg.rngMode) {
      std::fprintf(stderr, "Error: --boundary-nn rngMode (%s) does not match --rng=%s\n",
                   tb::rngModeName(boundaryNn.rngMode), tb::rngModeName(opts.cfg.rngMode));
      return 1;
    }
    boundaryNnPtr = &boundaryNn;
  }

  // Scope solver buffers (boards, FlatBoardIndex, solved.values) so all solver memory is released
  // before building the IndexedNextBoxTablebase for post-solve MC & live evaluation.
  {
    const auto loadT0 = std::chrono::steady_clock::now();
    std::vector<uint64_t> boards;
    tb::MappedNextBoxTablebase initTb;
    const tb::MappedNextBoxTablebase *initTbPtr = nullptr;

    // When refining the exact same SRTBV002 file (--init == input), open and verify once.
    const bool sameInitInput = !opts.initPath.empty() && (opts.initPath == opts.inPath);
    if (sameInitInput) {
      if (!initTb.open(opts.initPath, &err, /*verifySorted=*/true)) {
        std::fprintf(stderr, "Error opening input/warm-start tablebase '%s': %s\n",
                     opts.initPath.c_str(), err.c_str());
        return 1;
      }
      if (initTb.rngMode() != opts.cfg.rngMode) {
        std::fprintf(stderr, "Error: --init tablebase rngMode (%s) does not match --rng=%s\n",
                     tb::rngModeName(initTb.rngMode()), tb::rngModeName(opts.cfg.rngMode));
        return 1;
      }
      const uint64_t n = initTb.size();
      boards.resize(static_cast<size_t>(n));
      for (uint64_t i = 0; i < n; ++i) {
        boards[static_cast<size_t>(i)] = initTb.boardAt(i);
      }
      initTbPtr = &initTb;
    } else {
      if (!tb::loadNextBoxInputBoards(opts.inPath, boards, &err)) {
        std::fprintf(stderr, "Error reading input boards '%s': %s\n", opts.inPath.c_str(),
                     err.c_str());
        return 1;
      }
      if (!opts.initPath.empty()) {
        if (!initTb.open(opts.initPath, &err, /*verifySorted=*/true)) {
          std::fprintf(stderr, "Error opening warm-start tablebase '%s': %s\n",
                       opts.initPath.c_str(), err.c_str());
          return 1;
        }
        if (initTb.rngMode() != opts.cfg.rngMode) {
          std::fprintf(stderr, "Error: --init tablebase rngMode (%s) does not match --rng=%s\n",
                       tb::rngModeName(initTb.rngMode()), tb::rngModeName(opts.cfg.rngMode));
          return 1;
        }
        initTbPtr = &initTb;
      }
    }

    if (boards.empty()) {
      std::fprintf(stderr, "Error: input '%s' contains 0 boards.\n", opts.inPath.c_str());
      return 1;
    }
    if (boards.size() >= static_cast<size_t>(tb::NB_ACTION_OUT_CHILD)) {
      std::fprintf(stderr, "Error: input has too many boards (%zu >= %u).\n", boards.size(),
                   tb::NB_ACTION_OUT_CHILD);
      return 1;
    }
    numBoards = boards.size();

    std::array<uint64_t, tb::NUM_HOLE_BUCKETS> holeDist{};
    for (uint64_t b : boards) {
      ++holeDist[static_cast<size_t>(tb::HoleStratifiedCounts::bucket(tb::countHoles(b)))];
    }
    const double loadSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - loadT0).count();

    const auto idxT0 = std::chrono::steady_clock::now();
    tb::FlatBoardIndex index(boards);
    const double idxSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - idxT0).count();

    std::printf("=== Next-Box V(b,c) MPI Solver (%zu boards, rng=%s, gamma=%.6f, maxV-guard=%.2f, "
                "cache=%s, %d threads, tol=%.1e, inner=%d) ===\n",
                boards.size(), tb::rngModeName(opts.cfg.rngMode), opts.cfg.gamma,
                opts.cfg.maxValueDivergenceGuard, tb::policyCacheModeName(opts.cfg.policyCacheMode),
                opts.cfg.numThreads, opts.cfg.tolerance, opts.cfg.innerEvalSweeps);
    std::printf(
        "  Loaded %s in %.2fs | Built FlatBoardIndex in %.2fs | Output %.1f MB (SRTBV002)\n",
        opts.inPath.c_str(), loadSec, idxSec,
        static_cast<double>(boards.size() * sizeof(tb::PackedNextBoxRecord)) / (1024.0 * 1024.0));
    std::printf(
        "  State-set holes: 0h=%llu | 1h=%llu | 2h=%llu | 3+h=%llu\n",
        static_cast<unsigned long long>(holeDist[0]), static_cast<unsigned long long>(holeDist[1]),
        static_cast<unsigned long long>(holeDist[2]), static_cast<unsigned long long>(holeDist[3]));
    if (initTbPtr != nullptr) {
      std::printf("  Warm-starting from %s (%llu boards)\n", opts.initPath.c_str(),
                  static_cast<unsigned long long>(initTb.size()));
    }
    if (boundaryNnPtr != nullptr) {
      std::printf("  Soft boundary   : %s (%dx%d, margin=%.2f, ply=%d)\n",
                  opts.boundaryNnPath.c_str(), boundaryNn.h1, boundaryNn.h2, opts.boundaryMargin,
                  opts.boundaryPly);
    }
    std::fflush(stdout);

    tb::NextBoxSolveResult solved{};
    if (boundaryNnPtr != nullptr) {
      solved = tb::solveNextBoxMPI(boards, index, opts.cfg, initTbPtr,
                                   makeBoundaryEval(boundaryNnPtr, opts));
    } else {
      solved = tb::solveNextBoxMPI(boards, index, opts.cfg, initTbPtr);
    }
    // Close --init mapping immediately so in-place refinement (--init == outPath) is safe.
    initTb.close();
    index = tb::FlatBoardIndex{};

    if (!solved.ok) {
      std::fprintf(stderr, "ERROR: Next-Box MPI solve failed: %s\n", solved.errorMessage.c_str());
      return 1;
    }

    emptyExpectedValue = solved.emptyBoardExpectedValue;
    solveElapsedSec = solved.elapsedSec;
    std::printf("Solved in %.2fs (%d outer MPI steps, %d inner sweeps, policy=%s, warm=%zu) | "
                "E[V(0)] = %.6f | maxV = %.2f | residual = %.3e\n",
                solved.elapsedSec, solved.outerIterations, solved.totalInnerSweeps,
                solved.usedDiskPolicyCache ? "disk" : "ram", solved.warmStartBoardsMatched,
                solved.emptyBoardExpectedValue, solved.maxBoardValue, solved.finalBellmanResidual);
    if (solved.emptyBoardExpectedValue >= 0.0) {
      std::printf("  V(0, c): ");
      for (int c = 0; c < tb::NUM_PIECES; ++c) {
        std::printf("%s%c=%.3f", (c > 0 ? " | " : ""), tb::PIECE_CHARS[c],
                    solved.emptyBoardValues[static_cast<size_t>(c)]);
      }
      std::printf("\n");
    }
    if (solved.finalBellmanResidual > opts.cfg.tolerance) {
      std::fprintf(stderr, "ERROR: Solver failed to reach tolerance %.2e (residual=%.3e)\n",
                   opts.cfg.tolerance, solved.finalBellmanResidual);
      return 1;
    }

    if (!tb::cli::ensureParentDirectory(opts.outPath)) {
      return 1;
    }
    if (!tb::writeNextBoxTablebaseFile(opts.outPath, boards, solved.values, opts.cfg.rngMode,
                                       solved.finalBellmanResidual, &err)) {
      std::fprintf(stderr, "Error writing Next-Box tablebase '%s': %s\n", opts.outPath.c_str(),
                   err.c_str());
      return 1;
    }
    std::printf("Saved Next-Box tablebase to %s\n", opts.outPath.c_str());
  }

  tb::IndexedNextBoxTablebase indexedTb;
  if (opts.mcGames > 0 || opts.evalGames > 0) {
    tb::MappedNextBoxTablebase mapped;
    if (!mapped.open(opts.outPath, &err, /*verifySorted=*/false)) {
      std::fprintf(stderr, "Error mmap-reopening tablebase '%s': %s\n", opts.outPath.c_str(),
                   err.c_str());
      return 1;
    }
    indexedTb.buildFromMapped(mapped);
    mapped.close();
  }

  if (opts.mcGames > 0
      && !runBellmanMcVerification(indexedTb, opts, boundaryNnPtr, emptyExpectedValue)) {
    return 1;
  }

  tb::TablebaseEvalStats live{};
  if (opts.evalGames > 0) {
    std::printf("\nRunning live Next-Box TB + 2-ply heuristic fallback (%d games, eval-rng=%s, "
                "seed=%llu, %d threads)...\n",
                opts.evalGames, tb::rngModeName(opts.evalRngMode),
                static_cast<unsigned long long>(opts.evalSeed), opts.cfg.numThreads);
    live = tb::evaluateNextBoxPolicy(indexedTb, opts.evalRngMode, /*closedWorld=*/false,
                                     opts.evalGames, opts.evalSeed, opts.cfg.numThreads,
                                     tb::EvalWeights{}, tb::DEFAULT_EVAL_MAX_STEPS);
    std::printf(
        "  Live NB TB + 2p : mean = %.4f +- %.4f (median=%.1f, p90=%d, max=%d, capHits=%d)\n",
        live.meanLines, live.stdErrLines, live.medianLines, live.p90Lines, live.maxLines,
        live.stepCapHits);
    std::printf(
        "  Live coverage   : in-TB moves = %.2f%% | all-OUT fallback = %.2f%% | 100%%-covered "
        "games = %.2f%%\n",
        live.inTbMovePct, live.fallbackMovePct, live.fullyCoveredGamePct);
    tb::printHoleStratifiedCoverage(live.byHoles);
  }

  if (opts.evalGames > 0) {
    std::printf("\nSUMMARY | boards=%zu | rng=%s | eval_rng=%s | E[V(0)]=%.6f | solve=%.2fs | "
                "live_mean=%.4f+-%.4f | in_tb=%.2f%% | full_games=%.2f%%\n",
                numBoards, tb::rngModeName(opts.cfg.rngMode), tb::rngModeName(opts.evalRngMode),
                emptyExpectedValue, solveElapsedSec, live.meanLines, live.stdErrLines,
                live.inTbMovePct, live.fullyCoveredGamePct);
  } else {
    std::printf("\nSUMMARY | boards=%zu | rng=%s | E[V(0)]=%.6f | solve=%.2fs | live=skipped\n",
                numBoards, tb::rngModeName(opts.cfg.rngMode), emptyExpectedValue, solveElapsedSec);
  }
  return 0;
}
