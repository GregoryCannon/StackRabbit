// benchmark_cli.cpp — throughput & survival benchmark + coordinate/random weight tuner
// for the Very Low Lineout baseline heuristic agent.
//
// Build:
//   g++ -O3 -std=c++17 -pthread -Wall -Wextra -Werror experimental/tablebase/benchmark_cli.cpp -o
//   build/tablebase_bench
//
// Usage:
//   ./build/tablebase_bench [games=500] [maxPiecesPerGame=50000]
//   ./build/tablebase_bench tune [trainGames=400] [evalGames=1000] [iters=80]

#include "heuristic.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

struct GameResult {
  int linesCleared = 0;
  int piecesPlaced = 0;
  bool hitCap = false;
};

struct BenchmarkSummary {
  double meanLines = 0.0;
  double stdErrLines = 0.0;
  int medianLines = 0;
  int p90Lines = 0;
  int maxLines = 0;
  int cappedGames = 0;
  uint64_t totalMoves = 0;
  double elapsedSec = 0.0;
  double movesPerSec = 0.0;
};

void printUsage(const char *prog, FILE *out) {
  std::fprintf(out, "Usage:\n");
  std::fprintf(out, "  %s [games=500] [maxPiecesPerGame=50000]\n", prog);
  std::fprintf(out, "  %s tune [trainGames=400] [evalGames=1000] [iters=80]\n", prog);
}

GameResult runSingleGame(uint64_t seed, int ply, const tb::EvalWeights &w, int maxPieces) {
  tb::Rng rng(seed);
  uint64_t board = 0ULL;
  GameResult res{};

  for (int step = 0; step < maxPieces; ++step) {
    const int piece = rng.nextPiece();
    const tb::ChosenTransition chosen = (ply == 2) ? tb::pickBestTransition2Ply(board, piece, w)
                                                   : tb::pickBestTransition1Ply(board, piece, w);
    if (!chosen.found) {
      return res;
    }
    board = chosen.transition.afterBoard;
    res.linesCleared += chosen.transition.linesCleared;
    ++res.piecesPlaced;
  }
  res.hitCap = true;
  return res;
}

BenchmarkSummary evaluateGames(int numGames, uint64_t baseSeed, int ply, const tb::EvalWeights &w,
                               int maxPieces, int numThreads) {
  std::vector<GameResult> results(static_cast<size_t>(numGames));
  std::atomic<int> nextIdx{0};

  const auto t0 = std::chrono::steady_clock::now();
  auto worker = [&]() {
    while (true) {
      const int i = nextIdx.fetch_add(1, std::memory_order_relaxed);
      if (i >= numGames)
        break;
      results[static_cast<size_t>(i)] = runSingleGame(
          baseSeed + static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ULL, ply, w, maxPieces);
    }
  };

  if (numThreads <= 1) {
    worker();
  } else {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(numThreads));
    for (int t = 0; t < numThreads; ++t) {
      threads.emplace_back(worker);
    }
    for (auto &th : threads) {
      th.join();
    }
  }
  const auto t1 = std::chrono::steady_clock::now();

  BenchmarkSummary s{};
  s.elapsedSec = std::chrono::duration<double>(t1 - t0).count();
  std::vector<int> lines(static_cast<size_t>(numGames));
  double sum = 0.0;
  for (int i = 0; i < numGames; ++i) {
    const GameResult &r = results[static_cast<size_t>(i)];
    lines[static_cast<size_t>(i)] = r.linesCleared;
    sum += static_cast<double>(r.linesCleared);
    // Count every pickBestTransition call, including the terminal piece that topped out.
    s.totalMoves += static_cast<uint64_t>(r.piecesPlaced + (r.hitCap ? 0 : 1));
    if (r.hitCap)
      ++s.cappedGames;
  }
  s.meanLines = sum / static_cast<double>(numGames);
  double varSum = 0.0;
  for (int x : lines) {
    const double d = static_cast<double>(x) - s.meanLines;
    varSum += d * d;
  }
  s.stdErrLines =
      (numGames > 1)
          ? std::sqrt(varSum / (static_cast<double>(numGames) * static_cast<double>(numGames - 1)))
          : 0.0;

  std::sort(lines.begin(), lines.end());
  s.medianLines = lines[(lines.size() - 1) / 2];
  s.p90Lines = lines[static_cast<size_t>(static_cast<double>(lines.size() - 1) * 0.90)];
  s.maxLines = lines.back();
  s.movesPerSec = (s.elapsedSec > 0.0) ? (static_cast<double>(s.totalMoves) / s.elapsedSec) : 0.0;
  return s;
}

void printSummary(const char *label, const BenchmarkSummary &s, int numGames) {
  std::printf("%-28s | games=%d | mean=%.2f +- %.2f | med=%d | p90=%d | max=%d (cap=%d) | "
              "moves=%llu | %.1fk mv/s (%.2fs)\n",
              label, numGames, s.meanLines, s.stdErrLines, s.medianLines, s.p90Lines, s.maxLines,
              s.cappedGames, static_cast<unsigned long long>(s.totalMoves), s.movesPerSec / 1000.0,
              s.elapsedSec);
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
  if (s == nullptr || *s == '\0')
    return false;
  long v = 0;
  for (const char *c = s; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9')
      return false;
    v = v * 10 + (*c - '0');
    if (v > 100000000L)
      return false;
  }
  if (v < 1)
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
      evaluateGames(trainGames, TRAIN_SEED, 1, bestW, MAX_PIECES, hwThreads);
  const BenchmarkSummary initEval1 =
      evaluateGames(evalGames, HELD_OUT_SEED, 1, bestW, MAX_PIECES, hwThreads);
  printSummary("Initial 1-ply (train)", initTrain, trainGames);
  printSummary("Initial 1-ply (held-out)", initEval1, evalGames);

  double bestScore = initTrain.meanLines;
  tb::Rng tunerRng(20261123ULL);
  const int switchIter = (iters * 3) / 4;

  for (int iter = 0; iter < iters; ++iter) {
    const bool use2PlyStage = (iter >= switchIter);
    if (iter == switchIter) {
      bestScore =
          evaluateGames(std::min(trainGames, 500), TRAIN_SEED, 2, bestW, MAX_PIECES, hwThreads)
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

    const int stageGames = use2PlyStage ? std::min(trainGames, 500) : trainGames;
    const int stagePly = use2PlyStage ? 2 : 1;
    const BenchmarkSummary s =
        evaluateGames(stageGames, TRAIN_SEED, stagePly, cand, MAX_PIECES, hwThreads);
    if (s.meanLines > bestScore) {
      bestScore = s.meanLines;
      bestW = cand;
      std::printf("  [iter %3d | %d-ply] new best train mean = %.2f +- %.2f\n", iter, stagePly,
                  s.meanLines, s.stdErrLines);
      std::fflush(stdout);
    }
  }

  const BenchmarkSummary finalEval1 =
      evaluateGames(evalGames, HELD_OUT_SEED, 1, bestW, MAX_PIECES, hwThreads);
  const BenchmarkSummary finalEval2 =
      evaluateGames(evalGames, HELD_OUT_SEED, 2, bestW, MAX_PIECES, hwThreads);
  printSummary("Tuned 1-ply (held-out)", finalEval1, evalGames);
  printSummary("Tuned 2-ply (held-out)", finalEval2, evalGames);
  printWeightsCode(bestW);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc >= 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
    printUsage(argv[0], stdout);
    return 0;
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
  int maxPieces = 50000;
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
  const BenchmarkSummary st1 = evaluateGames(std::min(games, 200), SEED, 1, w, maxPieces, 1);
  printSummary("1-ply NNB (1 thread)", st1, std::min(games, 200));
  const BenchmarkSummary st2 = evaluateGames(std::min(games, 50), SEED, 2, w, maxPieces, 1);
  printSummary("2-ply Expectimax (1 thread)", st2, std::min(games, 50));

  std::printf("\n=== Multi-Core Lineout Survival Benchmark (%d threads, identical seeds) ===\n",
              hwThreads);
  const BenchmarkSummary mt1 = evaluateGames(games, SEED, 1, w, maxPieces, hwThreads);
  printSummary("1-ply NNB", mt1, games);
  const BenchmarkSummary mt2 = evaluateGames(games, SEED, 2, w, maxPieces, hwThreads);
  printSummary("2-ply Expectimax", mt2, games);
  return 0;
}
