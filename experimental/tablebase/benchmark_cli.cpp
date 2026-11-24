// benchmark_cli.cpp — throughput & survival benchmark + coordinate/random weight tuner
// for Very Low Lineout agents (NNB 1-ply/2-ply, Next-Box V1/V2 2-ply heuristics, TinyNextBoxNet
// 1-ply/2-ply neural evaluators, Hybrid NNB-tablebase + 2-ply search, and Next-Box V(b, c) TB).
//
// Build:
//   g++ -O3 -DNDEBUG -std=c++17 -pthread -Wall -Wextra -Werror \
//       experimental/tablebase/benchmark_cli.cpp -o build/tablebase_bench
//
// Usage:
//   ./build/tablebase_bench [games=500] [maxPiecesPerGame=50000]
//   ./build/tablebase_bench tune [trainGames=400] [evalGames=1000] [iters=80]
//   ./build/tablebase_bench --mode=nextbox-heur|nextbox-heur-v2 [--rng=uniform|nes] [--games=10000]
//   ./build/tablebase_bench --mode=nextbox-nn1|nextbox-nn2 --nn=PATH [--rng=uniform|nes]
//   ./build/tablebase_bench --mode=nextbox-hybrid --tb=PATH [--rng=uniform|nes] [--games=10000]
//   ./build/tablebase_bench --mode=nextbox-tb --tb=PATH [--rng=uniform|nes] [--games=10000]

#include "cli_utils.hpp"
#include "distill_linear.hpp"
#include "heuristic.hpp"
#include "nextbox_solver.hpp"
#include "tiny_nn.hpp"
#include "vi_solver.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

enum class BenchMode {
  Nnb1Ply,
  Nnb2Ply,
  NextBoxHeur,
  NextBoxHeurV2,
  NextBoxNn1,
  NextBoxNn2,
  NextBoxHybrid,
  NextBoxTb,
};

struct GameResult {
  int linesCleared = 0;
  int piecesPlaced = 0;
  uint64_t inTbMoves = 0;
  uint64_t fallbackMoves = 0;
  bool hitCap = false;
};

struct BenchmarkSummary {
  double meanLines = 0.0;
  double stdErrLines = 0.0;
  double medianLines = 0.0;
  int p90Lines = 0;
  int maxLines = 0;
  int cappedGames = 0;
  uint64_t totalMoves = 0;
  double inTbLeafPct = 0.0;
  double fallbackPct = 0.0;
  double elapsedSec = 0.0;
  double movesPerSec = 0.0;
};

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage:\n");
  std::fprintf(out, "  %s [games=500] [maxPiecesPerGame=50000]\n", prog);
  std::fprintf(out, "  %s tune [trainGames=400] [evalGames=1000] [iters=80]\n", prog);
  std::fprintf(
      out,
      "  %s --mode=nextbox-heur|nextbox-heur-v2|nextbox-nn1|nextbox-nn2|nextbox-hybrid|nextbox-tb\n"
      "     [--tb=PATH] [--nn=PATH] [--out-margin=F|inf (default: inf)] [--out-ply=1|2 (default: 2)]\n"
      "     [--rng=uniform|nes] [--games=10000] [--seed=700000] [--max-pieces=50000] [--threads=T]\n",
      prog);
}

GameResult runSingleGame(uint64_t seed, BenchMode mode, tb::RngMode rngMode,
                         const tb::EvalWeights &w, const tb::IndexedTablebase *nnbTb,
                         const tb::IndexedNextBoxTablebase *nbTb, const tb::TinyNextBoxNet *nnPtr,
                         double outMargin, bool outUse2Ply, int maxPieces) {
  assert(mode != BenchMode::NextBoxHybrid || nnbTb != nullptr);
  assert(mode != BenchMode::NextBoxTb || nbTb != nullptr);
  assert((mode != BenchMode::NextBoxNn1 && mode != BenchMode::NextBoxNn2) || nnPtr != nullptr);
  tb::Rng rng(seed);
  uint64_t board = 0ULL;
  GameResult res{};

  if (mode == BenchMode::Nnb1Ply || mode == BenchMode::Nnb2Ply) {
    int piece = rng.nextOpeningPiece(rngMode);
    for (int step = 0; step < maxPieces; ++step) {
      const tb::ChosenTransition chosen = (mode == BenchMode::Nnb2Ply)
                                              ? tb::pickBestTransition2Ply(board, piece, w)
                                              : tb::pickBestTransition1Ply(board, piece, w);
      if (!chosen.found) {
        return res;
      }
      board = chosen.transition.afterBoard;
      res.linesCleared += chosen.transition.linesCleared;
      ++res.piecesPlaced;
      piece = rng.nextPiece(rngMode, piece);
    }
    res.hitCap = true;
    return res;
  }

  constexpr tb::LinearBoardEvalV2 v2Eval = tb::defaultV2LinearEval();
  int curPiece = rng.nextOpeningPiece(rngMode);
  int nextPiece = rng.nextPiece(rngMode, curPiece);
  for (int step = 0; step < maxPieces; ++step) {
    if (mode == BenchMode::NextBoxHybrid || mode == BenchMode::NextBoxTb) {
      const tb::TablebaseMoveChoice ch =
          (mode == BenchMode::NextBoxTb)
              ? tb::chooseNextBoxHybridFallbackMove(board, curPiece, nextPiece, *nbTb, nnPtr,
                                                    outMargin, rngMode, outUse2Ply, w)
              : tb::chooseNextBoxHybridMove(board, curPiece, nextPiece, *nnbTb, w);
      if (!ch.found) {
        return res;
      }
      if (ch.afterInTablebase) {
        ++res.inTbMoves;
      }
      if (ch.usedFallback) {
        ++res.fallbackMoves;
      }
      board = ch.transition.afterBoard;
      res.linesCleared += ch.transition.linesCleared;
    } else {
      tb::ChosenTransition ch{};
      if (mode == BenchMode::NextBoxHeurV2) {
        ch = v2Eval.pickBestNextBox2Ply(board, curPiece, nextPiece);
      } else if (mode == BenchMode::NextBoxNn1) {
        ch = tb::chooseNextBoxNnMove1Ply(board, curPiece, nextPiece, *nnPtr);
      } else if (mode == BenchMode::NextBoxNn2) {
        ch = tb::chooseNextBoxNnMove2Ply(board, curPiece, nextPiece, *nnPtr, rngMode);
      } else {
        ch = tb::pickBestNextBoxHeuristic(board, curPiece, nextPiece, w);
      }
      if (!ch.found) {
        return res;
      }
      board = ch.transition.afterBoard;
      res.linesCleared += ch.transition.linesCleared;
    }
    ++res.piecesPlaced;
    curPiece = nextPiece;
    nextPiece = rng.nextPiece(rngMode, curPiece);
  }
  res.hitCap = true;
  return res;
}

BenchmarkSummary evaluateGames(int numGames, uint64_t baseSeed, BenchMode mode, tb::RngMode rngMode,
                               const tb::EvalWeights &w, const tb::IndexedTablebase *nnbTb,
                               const tb::IndexedNextBoxTablebase *nbTb,
                               const tb::TinyNextBoxNet *nnPtr, int maxPieces, int numThreads,
                               double outMargin = std::numeric_limits<double>::infinity(),
                               bool outUse2Ply = true) {
  std::vector<GameResult> results(static_cast<size_t>(numGames));
  std::atomic<int> nextIdx{0};

  const auto t0 = std::chrono::steady_clock::now();
  tb::detail::runWorkerPool(
      numThreads, static_cast<size_t>(numGames), /*minParallelWork=*/16, [&](int /*tid*/) {
        while (true) {
          const int i = nextIdx.fetch_add(1, std::memory_order_relaxed);
          if (i >= numGames)
            break;
          results[static_cast<size_t>(i)] =
              runSingleGame(tb::detail::deriveGameSeed(baseSeed, i), mode, rngMode, w, nnbTb, nbTb,
                            nnPtr, outMargin, outUse2Ply, maxPieces);
        }
      });
  const auto t1 = std::chrono::steady_clock::now();

  BenchmarkSummary s{};
  s.elapsedSec = std::chrono::duration<double>(t1 - t0).count();
  std::vector<int> lines(static_cast<size_t>(numGames));
  uint64_t totalPlaced = 0;
  uint64_t totalInTb = 0;
  uint64_t totalFallback = 0;
  for (int i = 0; i < numGames; ++i) {
    const GameResult &r = results[static_cast<size_t>(i)];
    lines[static_cast<size_t>(i)] = r.linesCleared;
    totalPlaced += static_cast<uint64_t>(r.piecesPlaced);
    totalInTb += r.inTbMoves;
    totalFallback += r.fallbackMoves;
    // Count every move selection call, including the terminal piece that topped out.
    s.totalMoves += static_cast<uint64_t>(r.piecesPlaced + (r.hitCap ? 0 : 1));
    if (r.hitCap)
      ++s.cappedGames;
  }

  const tb::detail::DistributionStats dist = tb::detail::computeDistributionStats(std::move(lines));
  s.meanLines = dist.mean;
  s.stdErrLines = dist.stdErr;
  s.medianLines = dist.median;
  s.p90Lines = dist.p90;
  s.maxLines = dist.maxVal;
  s.movesPerSec = (s.elapsedSec > 0.0) ? (static_cast<double>(s.totalMoves) / s.elapsedSec) : 0.0;
  if (totalPlaced > 0) {
    s.inTbLeafPct = 100.0 * static_cast<double>(totalInTb) / static_cast<double>(totalPlaced);
    s.fallbackPct = 100.0 * static_cast<double>(totalFallback) / static_cast<double>(totalPlaced);
  }
  return s;
}

void printSummary(const char *label, const BenchmarkSummary &s, int numGames,
                  const char *tbHitLabel = nullptr) {
  std::printf("%-34s | games=%d | mean=%.2f +- %.2f | med=%.1f | p90=%d | max=%d (cap=%d) | "
              "moves=%llu | %.1fk mv/s (%.2fs)",
              label, numGames, s.meanLines, s.stdErrLines, s.medianLines, s.p90Lines, s.maxLines,
              s.cappedGames, static_cast<unsigned long long>(s.totalMoves), s.movesPerSec / 1000.0,
              s.elapsedSec);
  if (tbHitLabel != nullptr) {
    std::printf(" | %s=%.2f%% | fb=%.2f%%", tbHitLabel, s.inTbLeafPct, s.fallbackPct);
  }
  std::printf("\n");
  std::fflush(stdout);
}

void printWeightsCode(const tb::EvalWeights &w) {
  std::printf("struct EvalWeights {\n");
  std::printf("  double lineClear = %.4f;\n", w.lineClear);
  std::printf("  double sealedHole = %.4f;\n", w.sealedHole);
  std::printf("  double overhang = %.4f;\n", w.overhang);
  std::printf("  double heightSq = %.4f;\n", w.heightSq);
  std::printf("  double height3Col = %.4f;\n", w.height3Col);
  std::printf("  double ceilingCol = %.4f;\n", w.ceilingCol);
  std::printf("  double bumpiness = %.4f;\n", w.bumpiness);
  std::printf("  double wellDepthSq = %.4f;\n", w.wellDepthSq);
  std::printf("  double parityImbalance = %.4f;\n", w.parityImbalance);
  std::printf("  double safeFlatPairBonus = %.4f;\n", w.safeFlatPairBonus);
  std::printf("  double rowFillSq = %.4f;\n", w.rowFillSq);
  std::printf("  double deathPenalty = %.4f;\n", w.deathPenalty);
  std::printf("};\n");
}

bool parsePositiveInt(const char *s, int &out) {
  if (s == nullptr)
    return false;
  uint64_t v = 0;
  if (!tb::cli::parseStrictU64(s, 1, 100000000ULL, v))
    return false;
  out = static_cast<int>(v);
  return true;
}

int runTuner(int trainGames, int evalGames, int iters) {
  const int hwThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  constexpr uint64_t TRAIN_SEED = 10000ULL;
  constexpr uint64_t HELD_OUT_SEED = 900000ULL;
  constexpr int MAX_PIECES = 25000;

  tb::EvalWeights bestW{};
  std::printf("Tuning heuristic weights (%d games, %d threads, %d total iters: 75%% 1-ply + 25%% "
              "2-ply)...\n",
              trainGames, hwThreads, iters);

  const BenchmarkSummary initTrain =
      evaluateGames(trainGames, TRAIN_SEED, BenchMode::Nnb1Ply, tb::RngMode::Uniform, bestW,
                    nullptr, nullptr, nullptr, MAX_PIECES, hwThreads);
  const BenchmarkSummary initEval1 =
      evaluateGames(evalGames, HELD_OUT_SEED, BenchMode::Nnb1Ply, tb::RngMode::Uniform, bestW,
                    nullptr, nullptr, nullptr, MAX_PIECES, hwThreads);
  printSummary("Initial 1-ply (train)", initTrain, trainGames);
  printSummary("Initial 1-ply (held-out)", initEval1, evalGames);

  double bestScore = initTrain.meanLines;
  tb::Rng tunerRng(20261123ULL);
  const int switchIter = (iters * 3) / 4;

  const int twoPlyTrainGames = std::min(trainGames, 500);
  for (int iter = 0; iter < iters; ++iter) {
    const bool use2PlyStage = (iter >= switchIter);
    if (iter == switchIter) {
      bestScore =
          evaluateGames(twoPlyTrainGames, TRAIN_SEED, BenchMode::Nnb2Ply, tb::RngMode::Uniform,
                        bestW, nullptr, nullptr, nullptr, MAX_PIECES, hwThreads)
              .meanLines;
      std::printf("  --- Switching to 2-ply fine-tuning (baseline 2-ply train mean = %.2f) ---\n",
                  bestScore);
    }

    const double scale = (iter < iters / 2) ? 0.30 : 0.12;
    tb::EvalWeights cand = bestW;
    // Keep deathPenalty last: 1-ply never observes next-piece top-outs, so only 2-ply tunes it.
    double *candFields[] = {
        &cand.lineClear,       &cand.sealedHole,        &cand.overhang,  &cand.heightSq,
        &cand.height3Col,      &cand.ceilingCol,        &cand.bumpiness, &cand.wellDepthSq,
        &cand.parityImbalance, &cand.safeFlatPairBonus, &cand.rowFillSq, &cand.deathPenalty,
    };
    constexpr int TOTAL_FIELDS = static_cast<int>(std::size(candFields));
    const int numFields = use2PlyStage ? TOTAL_FIELDS : (TOTAL_FIELDS - 1);

    const int numPerturb = 1 + static_cast<int>(tunerRng.nextU64() % 3ULL);
    for (int k = 0; k < numPerturb; ++k) {
      const int f = static_cast<int>(tunerRng.nextU64() % static_cast<uint64_t>(numFields));
      const double mult = 1.0 + (tunerRng.nextDouble() * 2.0 - 1.0) * scale;
      const double add = (tunerRng.nextDouble() * 2.0 - 1.0) * 0.25 * scale;
      *candFields[f] = (*candFields[f]) * mult + add;
    }

    const int stageGames = use2PlyStage ? twoPlyTrainGames : trainGames;
    const BenchMode stageMode = use2PlyStage ? BenchMode::Nnb2Ply : BenchMode::Nnb1Ply;
    const BenchmarkSummary s =
        evaluateGames(stageGames, TRAIN_SEED, stageMode, tb::RngMode::Uniform, cand, nullptr,
                      nullptr, nullptr, MAX_PIECES, hwThreads);
    if (s.meanLines > bestScore) {
      bestScore = s.meanLines;
      bestW = cand;
      std::printf("  [iter %3d | %d-ply] new best train mean = %.2f +- %.2f\n", iter,
                  use2PlyStage ? 2 : 1, s.meanLines, s.stdErrLines);
      std::fflush(stdout);
    }
  }

  const BenchmarkSummary finalEval1 =
      evaluateGames(evalGames, HELD_OUT_SEED, BenchMode::Nnb1Ply, tb::RngMode::Uniform, bestW,
                    nullptr, nullptr, nullptr, MAX_PIECES, hwThreads);
  const BenchmarkSummary finalEval2 =
      evaluateGames(evalGames, HELD_OUT_SEED, BenchMode::Nnb2Ply, tb::RngMode::Uniform, bestW,
                    nullptr, nullptr, nullptr, MAX_PIECES, hwThreads);
  printSummary("Tuned 1-ply (held-out)", finalEval1, evalGames);
  printSummary("Tuned 2-ply (held-out)", finalEval2, evalGames);
  printWeightsCode(bestW);
  return 0;
}

int runFlaggedBenchmark(int argc, char **argv) {
  bool modeSet = false;
  bool rngSet = false;
  BenchMode mode = BenchMode::NextBoxHeur;
  tb::RngMode rngMode = tb::RngMode::Uniform;
  std::string tbPath;
  std::string nnPath;
  double outMargin = std::numeric_limits<double>::infinity();
  bool outMarginSet = false;
  bool outUse2Ply = true;
  bool outPlySet = false;
  int games = 10000;
  int maxPieces = tb::DEFAULT_MAX_PIECES_PER_GAME;
  uint64_t seed = 700000ULL;
  int numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

  std::set<std::string> seenFlags;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0], stdout);
      return 0;
    }
    const size_t eq = arg.find('=');
    if (arg.rfind("--", 0) != 0 || eq == std::string::npos) {
      std::fprintf(stderr, "Invalid option: %s\n", arg.c_str());
      printUsage(argv[0], stderr);
      return 1;
    }
    const std::string flag = arg.substr(0, eq);
    const std::string val = arg.substr(eq + 1);
    if (!seenFlags.insert(flag).second) {
      std::fprintf(stderr, "Duplicate option: %s\n", flag.c_str());
      printUsage(argv[0], stderr);
      return 1;
    }

    uint64_t uVal = 0;
    if (flag == "--mode") {
      if (val == "nextbox-heur") {
        mode = BenchMode::NextBoxHeur;
      } else if (val == "nextbox-heur-v2") {
        mode = BenchMode::NextBoxHeurV2;
      } else if (val == "nextbox-nn1") {
        mode = BenchMode::NextBoxNn1;
      } else if (val == "nextbox-nn2") {
        mode = BenchMode::NextBoxNn2;
      } else if (val == "nextbox-hybrid") {
        mode = BenchMode::NextBoxHybrid;
      } else if (val == "nextbox-tb") {
        mode = BenchMode::NextBoxTb;
      } else {
        std::fprintf(stderr,
                     "Invalid --mode '%s' (expected nextbox-heur, nextbox-heur-v2, nextbox-nn1, "
                     "nextbox-nn2, nextbox-hybrid, or nextbox-tb)\n",
                     val.c_str());
        return 1;
      }
      modeSet = true;
    } else if (flag == "--rng") {
      if (!tb::parseRngMode(val, rngMode)) {
        std::fprintf(stderr, "Invalid --rng '%s' (expected uniform or nes)\n", val.c_str());
        return 1;
      }
      rngSet = true;
    } else if (flag == "--tb") {
      if (val.empty()) {
        std::fprintf(stderr, "Empty path for --tb=\n");
        return 1;
      }
      tbPath = val;
    } else if (flag == "--nn") {
      if (val.empty()) {
        std::fprintf(stderr, "Empty path for --nn=\n");
        return 1;
      }
      nnPath = val;
    } else if (flag == "--out-margin") {
      if (val == "inf") {
        outMargin = std::numeric_limits<double>::infinity();
      } else {
        double dVal = 0.0;
        if (!tb::cli::parseStrictPositiveDouble(val, 0.0, 1000.0, dVal)) {
          std::fprintf(stderr, "Invalid --out-margin '%s' (expected 0..1000 or inf)\n",
                       val.c_str());
          return 1;
        }
        outMargin = dVal;
      }
      outMarginSet = true;
    } else if (flag == "--out-ply") {
      if (val == "1") {
        outUse2Ply = false;
      } else if (val == "2") {
        outUse2Ply = true;
      } else {
        std::fprintf(stderr, "Invalid --out-ply '%s' (expected 1 or 2)\n", val.c_str());
        return 1;
      }
      outPlySet = true;
    } else if (flag == "--games") {
      if (!tb::cli::parseStrictU64(val, 1, 1000000ULL, uVal)) {
        std::fprintf(stderr, "Invalid --games '%s' (expected 1..1000000)\n", val.c_str());
        return 1;
      }
      games = static_cast<int>(uVal);
    } else if (flag == "--max-pieces") {
      if (!tb::cli::parseStrictU64(val, 1, 1000000ULL, uVal)) {
        std::fprintf(stderr, "Invalid --max-pieces '%s' (expected 1..1000000)\n", val.c_str());
        return 1;
      }
      maxPieces = static_cast<int>(uVal);
    } else if (flag == "--seed") {
      if (!tb::cli::parseStrictU64(val, 0, UINT64_MAX, uVal)) {
        std::fprintf(stderr, "Invalid --seed '%s'\n", val.c_str());
        return 1;
      }
      seed = uVal;
    } else if (flag == "--threads") {
      if (!tb::cli::parseStrictU64(val, 1, 256, uVal)) {
        std::fprintf(stderr, "Invalid --threads '%s' (expected 1..256)\n", val.c_str());
        return 1;
      }
      numThreads = static_cast<int>(uVal);
    } else {
      std::fprintf(stderr, "Unknown option: %s\n", flag.c_str());
      printUsage(argv[0], stderr);
      return 1;
    }
  }

  if (!modeSet) {
    std::fprintf(stderr, "Missing required --mode=nextbox-heur|nextbox-heur-v2|nextbox-nn1|"
                         "nextbox-nn2|nextbox-hybrid|nextbox-tb.\n");
    printUsage(argv[0], stderr);
    return 1;
  }
  const bool needsTb = (mode == BenchMode::NextBoxHybrid || mode == BenchMode::NextBoxTb);
  const bool needsNn = (mode == BenchMode::NextBoxNn1 || mode == BenchMode::NextBoxNn2);
  const bool allowsNn = needsNn || (mode == BenchMode::NextBoxTb);
  if (needsTb && tbPath.empty()) {
    std::fprintf(stderr, "--mode=%s requires --tb=<path.tb.bin>.\n",
                 mode == BenchMode::NextBoxTb ? "nextbox-tb" : "nextbox-hybrid");
    return 1;
  }
  if (!needsTb && !tbPath.empty()) {
    std::fprintf(stderr, "--tb= is only valid with --mode=nextbox-hybrid or nextbox-tb.\n");
    return 1;
  }
  if (needsNn && nnPath.empty()) {
    std::fprintf(stderr, "--mode=%s requires --nn=<model.nn.bin>.\n",
                 mode == BenchMode::NextBoxNn1 ? "nextbox-nn1" : "nextbox-nn2");
    return 1;
  }
  if (!allowsNn && !nnPath.empty()) {
    std::fprintf(stderr, "--nn= is only valid with nextbox-nn1, nextbox-nn2, or nextbox-tb.\n");
    return 1;
  }
  if ((outMarginSet || outPlySet) && (mode != BenchMode::NextBoxTb || nnPath.empty())) {
    std::fprintf(stderr, "--out-margin= and --out-ply= require --mode=nextbox-tb with --nn=.\n");
    return 1;
  }

  tb::IndexedTablebase indexedNnbTb;
  const tb::IndexedTablebase *nnbTbPtr = nullptr;
  tb::IndexedNextBoxTablebase indexedNbTb;
  const tb::IndexedNextBoxTablebase *nbTbPtr = nullptr;
  tb::TinyNextBoxNet loadedNn;
  const tb::TinyNextBoxNet *nnPtr = nullptr;
  if (mode == BenchMode::NextBoxHybrid) {
    tb::MappedTablebase mappedTb;
    std::string err;
    if (!mappedTb.open(tbPath, &err, /*verifySorted=*/true)) {
      std::fprintf(stderr, "Error opening tablebase '%s': %s\n", tbPath.c_str(), err.c_str());
      return 1;
    }
    indexedNnbTb.buildFromMapped(mappedTb);
    nnbTbPtr = &indexedNnbTb;
  } else if (mode == BenchMode::NextBoxTb) {
    tb::MappedNextBoxTablebase mappedNbTb;
    std::string err;
    if (!mappedNbTb.open(tbPath, &err, /*verifySorted=*/true)) {
      std::fprintf(stderr, "Error opening Next-Box tablebase '%s': %s\n", tbPath.c_str(),
                   err.c_str());
      return 1;
    }
    if (!rngSet) {
      rngMode = mappedNbTb.rngMode();
    }
    indexedNbTb.buildFromMapped(mappedNbTb);
    nbTbPtr = &indexedNbTb;
  }
  if (!nnPath.empty()) {
    std::string err;
    if (!loadedNn.load(nnPath, &err)) {
      std::fprintf(stderr, "Error loading NN '%s': %s\n", nnPath.c_str(), err.c_str());
      return 1;
    }
    if (needsNn && !rngSet) {
      rngMode = loadedNn.rngMode;
    }
    nnPtr = &loadedNn;
  }

  const tb::EvalWeights w{};
  std::string label;
  const char *tbHitLabel = nullptr;
  if (mode == BenchMode::NextBoxTb) {
    tbHitLabel = "in-TB";
    if (nnPtr != nullptr) {
      char mBuf[64];
      if (std::isfinite(outMargin)) {
        std::snprintf(mBuf, sizeof(mBuf), "m=%.1f,p%d,%s", outMargin, outUse2Ply ? 2 : 1,
                      tb::rngModeName(rngMode));
      } else {
        std::snprintf(mBuf, sizeof(mBuf), "allOUT,p%d,%s", outUse2Ply ? 2 : 1,
                      tb::rngModeName(rngMode));
      }
      label = std::string("Next-Box TB+NN [") + mBuf;
      if (nbTbPtr->rngMode() != rngMode || nnPtr->rngMode != rngMode) {
        label += std::string(",tb=") + tb::rngModeName(nbTbPtr->rngMode()) + ",nn="
                 + tb::rngModeName(nnPtr->rngMode);
      }
      label += "]";
    } else if (nbTbPtr->rngMode() != rngMode) {
      label = std::string("Next-Box TB [tb=")
              + tb::rngModeName(nbTbPtr->rngMode())
              + ", eval="
              + tb::rngModeName(rngMode)
              + "]";
    } else {
      label = std::string("Next-Box TB (") + tb::rngModeName(rngMode) + ")";
    }
  } else if (mode == BenchMode::NextBoxHybrid) {
    tbHitLabel = "in-TB-leaf";
    label = std::string("Next-Box Hybrid 2-ply (") + tb::rngModeName(rngMode) + ")";
  } else if (mode == BenchMode::NextBoxNn1 || mode == BenchMode::NextBoxNn2) {
    const std::string prefix =
        (mode == BenchMode::NextBoxNn1) ? "Next-Box NN 1-ply" : "Next-Box NN 2-ply";
    if (nnPtr->rngMode != rngMode) {
      label = prefix
              + " [nn="
              + tb::rngModeName(nnPtr->rngMode)
              + ", eval="
              + tb::rngModeName(rngMode)
              + "]";
    } else {
      label = prefix + " (" + tb::rngModeName(rngMode) + ")";
    }
  } else if (mode == BenchMode::NextBoxHeurV2) {
    label = std::string("Next-Box Heur-V2 2-ply (") + tb::rngModeName(rngMode) + ")";
  } else {
    label = std::string("Next-Box Heur 2-ply (") + tb::rngModeName(rngMode) + ")";
  }
  const BenchmarkSummary s = evaluateGames(games, seed, mode, rngMode, w, nnbTbPtr, nbTbPtr, nnPtr,
                                           maxPieces, numThreads, outMargin, outUse2Ply);
  printSummary(label.c_str(), s, games, tbHitLabel);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
    printUsage(argv[0], stdout);
    return 0;
  }

  if (argc >= 2 && std::string(argv[1]).rfind("--", 0) == 0) {
    return runFlaggedBenchmark(argc, argv);
  }

  if (argc >= 2 && std::string(argv[1]) == "tune") {
    int trainGames = 400;
    int evalGames = 1000;
    int iters = 80;
    if (argc > 5
        || (argc >= 3 && !parsePositiveInt(argv[2], trainGames))
        || (argc >= 4 && !parsePositiveInt(argv[3], evalGames))
        || (argc >= 5 && !parsePositiveInt(argv[4], iters))) {
      std::fprintf(stderr, "Invalid tune arguments. Expected positive integers.\n");
      printUsage(argv[0], stderr);
      return 1;
    }
    return runTuner(trainGames, evalGames, iters);
  }

  int games = 500;
  int maxPieces = tb::DEFAULT_MAX_PIECES_PER_GAME;
  if (argc > 3
      || (argc >= 2 && !parsePositiveInt(argv[1], games))
      || (argc >= 3 && !parsePositiveInt(argv[2], maxPieces))) {
    std::fprintf(stderr, "Invalid benchmark arguments. Expected positive integers.\n");
    printUsage(argv[0], stderr);
    return 1;
  }

  const tb::EvalWeights w{};
  const int hwThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  constexpr uint64_t SEED = 42ULL;

  std::printf("=== Single-Core Throughput Check (1 thread) ===\n");
  const BenchmarkSummary st1 =
      evaluateGames(std::min(games, 200), SEED, BenchMode::Nnb1Ply, tb::RngMode::Uniform, w,
                    nullptr, nullptr, nullptr, maxPieces, 1);
  printSummary("1-ply NNB (1 thread)", st1, std::min(games, 200));
  const BenchmarkSummary st2 =
      evaluateGames(std::min(games, 50), SEED, BenchMode::Nnb2Ply, tb::RngMode::Uniform, w, nullptr,
                    nullptr, nullptr, maxPieces, 1);
  printSummary("2-ply Expectimax (1 thread)", st2, std::min(games, 50));

  std::printf("\n=== Multi-Core Lineout Survival Benchmark (%d threads, identical seeds) ===\n",
              hwThreads);
  const BenchmarkSummary mt1 = evaluateGames(games, SEED, BenchMode::Nnb1Ply, tb::RngMode::Uniform,
                                             w, nullptr, nullptr, nullptr, maxPieces, hwThreads);
  printSummary("1-ply NNB", mt1, games);
  const BenchmarkSummary mt2 = evaluateGames(games, SEED, BenchMode::Nnb2Ply, tb::RngMode::Uniform,
                                             w, nullptr, nullptr, nullptr, maxPieces, hwThreads);
  printSummary("2-ply Expectimax", mt2, games);
  return 0;
}
