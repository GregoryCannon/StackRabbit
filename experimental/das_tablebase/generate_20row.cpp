// generate_20row.cpp — State-set generator (SRSTV002) for 20-row NES Tetris.
// Supports:
//   - Round 0 seeding via production eval.cpp 2-ply (--seed-eval)
//   - Rounds 1+ via 0-frame TB+NNUE (SRTBV003) or live-RT V_RT+NNUE (SRTBV004 / multi-bucket NN)
//     50/50 exploration + core sibling expansion + nn-error eviction
//
// Example (Round 0):
//   ./build/generate_20row --seed-eval --level=19 --games=60000 --min-visits=2 \
//       --max-boards=5000000 --epsilon=0.10 --out=experimental/das_tablebase/data/l19/r0.states.bin
//
// Example (Round 2+):
//   ./build/generate_20row --level=19 --tb=.../r1.tb.bin --nn=.../r1_128x64.nn.bin \
//       --priority-mode=nn-error --protected-core=500000 --max-boards=5000000 \
//       --games=60000 --epsilon=0.14 --out=.../r2.states.bin

#include "eval_adapter.hpp"
#include "nnue_20row.hpp"
#include "state_gen_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <atomic>
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

struct GenCliConfig {
  bool seedEval = false;
  std::string tbPath;
  std::string nnPath;
  std::string mergePath;
  std::string outPath;
  AggressionLevel aggro = AGGRO;
  das::StateGenConfig20 gen{};
};

void printUsage(FILE *stream, const char *prog) {
  fprintf(
      stream,
      "Usage: %s --out=PATH [options]\n"
      "  --seed-eval               Seed states via 2-ply eval.cpp (Round 0 only)\n"
      "  --tb=PATH                 SRTBV003 or SRTBV004 tablebase for rollouts & nn-error scoring\n"
      "  --nn=PATH                 SRNNV001 NNUE (7 or 7*K outputs) for probes & nn-error scoring\n"
      "  --merge=PATH              Prior SRSTV002/SRTBV003/SRTBV004 board set to preserve\n"
      "  --level=19|18             Stationary level (default 19)\n"
      "  --rng=nes|uniform         RNG mode (default nes)\n"
      "  --gamma=F                 Discount factor (default 0.995)\n"
      "  --games=N                 Rollout games (default 30000)\n"
      "  --max-lines=N             Line cap per rollout game (default 100 for L19, 130 for L18, "
      "0=uncapped)\n"
      "  --max-pieces=N            Piece cap per rollout game (default 500)\n"
      "  --epsilon=F               Exploration rate (default 0.12)\n"
      "  --top-k=N                 Exploration runner-up beam (default 6)\n"
      "  --out-margin=F            Partial-OUT margin during rollouts (default 0.6)\n"
      "  --min-visits=N            Minimum visit filter (default 2)\n"
      "  --max-boards=N            Output board cap (default 5000000, 0=unlimited)\n"
      "  --sibling-min-visits=N    Expand core siblings on >=N-visit boards (default 4, 0=off)\n"
      "  --priority-mode=visits|nn-error (default visits)\n"
      "  --protected-core=N        Top-N highest-visit boards immune to nn-error eviction (default "
      "500000)\n"
      "  --nn-error-weight=F       Weight on |E[V_tb] - E[V_nn]| (default 2.0)\n"
      "  --err-cap=F               Cap on |E[V_tb] - E[V_nn]| (default 3.0)\n"
      "  --frontier-boost=F        Priority multiplier boost for out-of-TB frontier (default 2.0)\n"
      "  --aggro=safe|aggro|hyper  eval.cpp aggression when --seed-eval (default aggro)\n"
      "  --seed=U64                Base RNG seed (default 10000000)\n"
      "  --threads=N               Worker threads (default auto)\n",
      prog);
}

// Round 0 state seeding via read-only 2-ply eval.cpp with epsilon runner-up exploration.
std::vector<das::BoardKey> runSeedEvalGeneration(const GenCliConfig &cli,
                                                 const std::vector<das::BoardKey> &priorBoards,
                                                 das::StateGenStats20 &statsOut) {
  const auto t0 = std::chrono::steady_clock::now();
  statsOut = {};

  const das::BackendContexts bc = das::BackendContexts::make10Hz();
  const int threads = std::max(1, cli.gen.numThreads);
  const int startLines = (cli.gen.level == 19) ? 130 : 0;

  das::ShardedVisitCounter20 counter;
  std::atomic<int> nextGame{0};
  std::atomic<uint64_t> totalMoves{0};
  std::atomic<uint64_t> inKeyMoves{0};

  das::detail::runWorkerPool(threads, static_cast<size_t>(cli.gen.numGames), 16, [&](int /*tid*/) {
    std::vector<LockPlacement> m1, m2;
    m1.reserve(64);
    m2.reserve(64);
    std::vector<das::BoardKey> batch;
    batch.reserve(16384);
    uint64_t locMoves = 0, locInKey = 0;

    struct EvalCand {
      float score;
      int lc;
      GameState after;
    };
    std::vector<EvalCand> cands;
    cands.reserve(64);

    while (true) {
      const int g = nextGame.fetch_add(1, std::memory_order_relaxed);
      if (g >= cli.gen.numGames)
        break;

      das::Rng rng(cli.gen.baseSeed + static_cast<uint64_t>(g) * 1009ULL);
      int curP = rng.sampleOpeningPiece(cli.gen.rngMode);
      int nextP = rng.sampleNextPiece(cli.gen.rngMode, curP);
      GameState st =
          das::board20ToGameState(das::Board20::empty(), cli.gen.level, startLines, bc, cli.aggro);
      int totalLines = 0;

      for (int step = 0; step < cli.gen.maxPiecesPerGame; ++step) {
        if (cli.gen.maxLinesPerGame > 0 && totalLines >= cli.gen.maxLinesPerGame)
          break;

        const das::Board20 b20 = das::gameStateToBoard20(st);
        das::BoardKey curKey;
        if (das::encodeBoardKey(b20.rows(), curKey, b20.topNonEmptyRow)) {
          batch.push_back(curKey);
          locInKey++;
          if (batch.size() >= 16384) {
            counter.addBatch(batch);
            batch.clear();
          }
        }
        locMoves++;

        const EvalContext ctx1 = getEvalContext(st, bc.lookup, false, cli.aggro);
        m1.clear();
        fastMoveSearch(st, &PIECE_LIST[curP], ctx1.pieceRangeContext.inputFrameTimeline, m1);
        if (m1.empty())
          break;

        cands.clear();
        for (const LockPlacement &p1 : m1) {
          GameState after1 = advanceGameState(st, p1, &ctx1);
          after1.level = st.level;
          const int lc1 = after1.lines - st.lines;
          const float r1 = getLineClearFactor(lc1, ctx1.weights, ctx1.shouldRewardLineClears,
                                              st.lines, ctx1.lineoutAt29);
          const EvalContext ctx2 = getEvalContext(after1, bc.lookup, false, cli.aggro);
          m2.clear();
          fastMoveSearch(after1, &PIECE_LIST[nextP], ctx2.pieceRangeContext.inputFrameTimeline, m2);

          float best2 = FLOAT_MIN;
          if (m2.empty()) {
            best2 = -1e6f + r1;
          } else {
            for (const LockPlacement &p2 : m2) {
              GameState after2 = advanceGameState(after1, p2, &ctx2);
              after2.level = st.level;
              const float total = r1 + fastEval(after1, after2, p2, &ctx2, nullptr);
              if (total > best2)
                best2 = total;
            }
          }
          cands.push_back({best2, lc1, after1});
        }

        std::stable_sort(cands.begin(), cands.end(),
                         [](const EvalCand &a, const EvalCand &b) { return a.score > b.score; });

        int pick = 0;
        if (cli.gen.epsilon > 0.0 && cands.size() > 1 && rng.nextUniform01() < cli.gen.epsilon) {
          const int hi =
              std::min<int>(static_cast<int>(cands.size()), std::max(2, cli.gen.exploreTopK));
          pick = 1 + rng.nextInt(hi - 1);
        }

        totalLines += cands[static_cast<size_t>(pick)].lc;
        st =
            das::board20ToGameState(das::gameStateToBoard20(cands[static_cast<size_t>(pick)].after),
                                    cli.gen.level, startLines, bc, cli.aggro);
        curP = nextP;
        nextP = rng.sampleNextPiece(cli.gen.rngMode, curP);
      }
    }

    if (!batch.empty()) {
      counter.addBatch(batch);
    }
    totalMoves.fetch_add(locMoves, std::memory_order_relaxed);
    inKeyMoves.fetch_add(locInKey, std::memory_order_relaxed);
  });

  std::vector<std::pair<das::BoardKey, uint32_t>> rawEntries = counter.drainAndRelease();
  statsOut.totalMoves = totalMoves.load();
  statsOut.inKeyMoves = inKeyMoves.load();
  statsOut.rawUniqueBoards = rawEntries.size();

  std::vector<das::BoardKey> emptySiblings;
  std::vector<das::BoardKey> result = das::detail::mergeFilterAndCap20(
      rawEntries, priorBoards, emptySiblings, cli.gen,
      static_cast<const das::IndexedDasTablebase *>(nullptr), nullptr);
  statsOut.outputBoards = result.size();
  statsOut.elapsedSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

} // namespace

int main(int argc, char **argv) {
  GenCliConfig cli;
  bool maxLinesSet = false;
  bool gammaSet = false;
  cli.gen.numThreads = 0;

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
    if (arg == "--seed-eval") {
      cli.seedEval = true;
      continue;
    }
    if (arg == "--aggro=safe") {
      cli.aggro = SAFE;
      continue;
    }
    if (arg == "--aggro=aggro") {
      cli.aggro = AGGRO;
      continue;
    }
    if (arg == "--aggro=hyper") {
      cli.aggro = HYPER;
      continue;
    }
    if (tryPrefix("--tb=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --tb path\n");
        return 1;
      }
      cli.tbPath = std::string(val);
      continue;
    }
    if (tryPrefix("--nn=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --nn path\n");
        return 1;
      }
      cli.nnPath = std::string(val);
      continue;
    }
    if (tryPrefix("--merge=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --merge path\n");
        return 1;
      }
      cli.mergePath = std::string(val);
      continue;
    }
    if (tryPrefix("--out=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --out path\n");
        return 1;
      }
      cli.outPath = std::string(val);
      continue;
    }
    if (tryPrefix("--rng=", val)) {
      if (!das::parseRngMode(val, cli.gen.rngMode)) {
        fprintf(stderr, "Error: invalid --rng\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--priority-mode=", val)) {
      if (!das::parseBoardPriorityMode(val, cli.gen.priorityMode)) {
        fprintf(stderr, "Error: invalid --priority-mode (expected visits or nn-error)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--level=", val)) {
      if (!parseStrictInt(val, cli.gen.level) || (cli.gen.level != 18 && cli.gen.level != 19)) {
        fprintf(stderr, "Error: invalid --level (expected 18 or 19)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--gamma=", val)) {
      if (!parseStrictDouble(val, cli.gen.gamma) || cli.gen.gamma <= 0.0 || cli.gen.gamma > 1.0) {
        fprintf(stderr, "Error: invalid --gamma (expected in (0, 1])\n");
        return 1;
      }
      gammaSet = true;
      continue;
    }
    if (tryPrefix("--games=", val)) {
      if (!parseStrictInt(val, cli.gen.numGames) || cli.gen.numGames <= 0) {
        fprintf(stderr, "Error: invalid --games\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--max-lines=", val)) {
      if (!parseStrictInt(val, cli.gen.maxLinesPerGame) || cli.gen.maxLinesPerGame < 0) {
        fprintf(stderr, "Error: invalid --max-lines\n");
        return 1;
      }
      maxLinesSet = true;
      continue;
    }
    if (tryPrefix("--max-pieces=", val)) {
      if (!parseStrictInt(val, cli.gen.maxPiecesPerGame) || cli.gen.maxPiecesPerGame <= 0) {
        fprintf(stderr, "Error: invalid --max-pieces\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--epsilon=", val)) {
      if (!parseStrictDouble(val, cli.gen.epsilon)
          || cli.gen.epsilon < 0.0
          || cli.gen.epsilon > 1.0) {
        fprintf(stderr, "Error: invalid --epsilon (expected in [0, 1])\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--top-k=", val)) {
      if (!parseStrictInt(val, cli.gen.exploreTopK) || cli.gen.exploreTopK < 1) {
        fprintf(stderr, "Error: invalid --top-k\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--out-margin=", val)) {
      if (!parseStrictDouble(val, cli.gen.outMargin) || cli.gen.outMargin < 0.0) {
        fprintf(stderr, "Error: invalid --out-margin\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--min-visits=", val)) {
      if (!parseStrictInt(val, cli.gen.minVisits)) {
        fprintf(stderr, "Error: invalid --min-visits\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--max-boards=", val)) {
      if (!parseStrictInt(val, cli.gen.maxBoards)) {
        fprintf(stderr, "Error: invalid --max-boards\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--sibling-min-visits=", val)) {
      if (!parseStrictInt(val, cli.gen.siblingMinVisits)) {
        fprintf(stderr, "Error: invalid --sibling-min-visits\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--protected-core=", val)) {
      if (!parseStrictInt(val, cli.gen.protectedCore)) {
        fprintf(stderr, "Error: invalid --protected-core\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--nn-error-weight=", val)) {
      if (!parseStrictDouble(val, cli.gen.nnErrorWeight) || cli.gen.nnErrorWeight < 0.0) {
        fprintf(stderr, "Error: invalid --nn-error-weight\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--err-cap=", val)) {
      if (!parseStrictDouble(val, cli.gen.priorityErrCap) || cli.gen.priorityErrCap < 0.0) {
        fprintf(stderr, "Error: invalid --err-cap\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--frontier-boost=", val)) {
      if (!parseStrictDouble(val, cli.gen.frontierBoost) || cli.gen.frontierBoost < 0.0) {
        fprintf(stderr, "Error: invalid --frontier-boost\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--seed=", val)) {
      if (!parseStrictInt(val, cli.gen.baseSeed)) {
        fprintf(stderr, "Error: invalid --seed\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--threads=", val)) {
      if (!parseStrictInt(val, cli.gen.numThreads) || cli.gen.numThreads < 0) {
        fprintf(stderr, "Error: invalid --threads\n");
        return 1;
      }
      continue;
    }
    fprintf(stderr, "Error: unknown arg '%.*s'\n", static_cast<int>(arg.size()), arg.data());
    return 1;
  }

  if (cli.outPath.empty()) {
    fprintf(stderr, "Error: --out=PATH is required\n");
    return 1;
  }
  if (cli.seedEval && (!cli.tbPath.empty() || !cli.nnPath.empty())) {
    fprintf(stderr, "Error: --seed-eval is mutually exclusive with --tb and --nn\n");
    return 1;
  }
  if (!cli.seedEval && cli.tbPath.empty() && cli.nnPath.empty()) {
    fprintf(stderr, "Error: specify --seed-eval OR at least one of --tb / --nn\n");
    return 1;
  }
  if (cli.gen.priorityMode == das::BoardPriorityMode::NnError && cli.nnPath.empty()) {
    fprintf(stderr, "Error: --priority-mode=nn-error requires --nn=PATH\n");
    return 1;
  }
  if (das::detail::isSameFilePath(cli.tbPath, cli.outPath)
      || das::detail::isSameFilePath(cli.nnPath, cli.outPath)
      || das::detail::isSameFilePath(cli.mergePath, cli.outPath)) {
    fprintf(stderr, "Error: --out must not alias --tb, --nn, or --merge\n");
    return 1;
  }

  if (!maxLinesSet) {
    cli.gen.maxLinesPerGame = (cli.gen.level == 19) ? 100 : 130;
  }
  if (cli.gen.numThreads == 0) {
    cli.gen.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  }

  std::vector<das::BoardKey> priorBoards;
  if (!cli.mergePath.empty()) {
    std::string err;
    if (!das::loadInputBoardKeysAnyFormat(cli.mergePath, priorBoards, &err)) {
      fprintf(stderr, "Error loading --merge: %s\n", err.c_str());
      return 1;
    }
    printf("Loaded %zu prior boards from %s\n", priorBoards.size(), cli.mergePath.c_str());
  }

  das::StateGenStats20 stats{};
  std::vector<das::BoardKey> boards;

  if (cli.seedEval) {
    printf("Generating Round 0 states via eval.cpp (L%d, games=%d, eps=%.2f, maxBoards=%zu, "
           "threads=%d)...\n",
           cli.gen.level, cli.gen.numGames, cli.gen.epsilon, cli.gen.maxBoards, cli.gen.numThreads);
    fflush(stdout);
    boards = runSeedEvalGeneration(cli, priorBoards, stats);
  } else {
    const bool isRtTb =
        !cli.tbPath.empty()
        && (das::peekFileMagic(cli.tbPath) == std::string(das::TABLEBASE_V4_MAGIC, 8));
    das::IndexedDasTablebase fastTb;
    das::IndexedDasRtTablebase rtTb;
    if (isRtTb) {
      std::string err;
      if (!rtTb.open(cli.tbPath, &err)) {
        fprintf(stderr, "Error loading SRTBV004 --tb: %s\n", err.c_str());
        return 1;
      }
      if (rtTb.level() != cli.gen.level || rtTb.rngMode() != cli.gen.rngMode) {
        fprintf(stderr, "Error: --tb level/rng (%d/%s) does not match CLI (%d/%s)\n", rtTb.level(),
                das::rngModeName(rtTb.rngMode()), cli.gen.level, das::rngModeName(cli.gen.rngMode));
        return 1;
      }
      if (gammaSet && std::abs(rtTb.gamma() - cli.gen.gamma) > 1e-9) {
        fprintf(stderr, "Error: --gamma (%.5f) does not match --tb gamma (%.5f)\n", cli.gen.gamma,
                rtTb.gamma());
        return 1;
      }
      cli.gen.gamma = rtTb.gamma();
      printf("Indexed V_RT TB %s (%llu boards, K=%d, rt=%uf, E[V(0,full)]=%.4f)\n",
             cli.tbPath.c_str(), static_cast<unsigned long long>(rtTb.size()), rtTb.numDasBuckets(),
             rtTb.header().reactionTimeFrames, rtTb.emptyBoardValue());
    } else if (!cli.tbPath.empty()) {
      std::string err;
      if (!fastTb.open(cli.tbPath, &err)) {
        fprintf(stderr, "Error loading --tb: %s\n", err.c_str());
        return 1;
      }
      if (fastTb.level() != cli.gen.level || fastTb.rngMode() != cli.gen.rngMode) {
        fprintf(stderr, "Error: --tb level/rng (%d/%s) does not match CLI (%d/%s)\n",
                fastTb.level(), das::rngModeName(fastTb.rngMode()), cli.gen.level,
                das::rngModeName(cli.gen.rngMode));
        return 1;
      }
      if (gammaSet && std::abs(fastTb.gamma() - cli.gen.gamma) > 1e-9) {
        fprintf(stderr, "Error: --gamma (%.5f) does not match --tb gamma (%.5f)\n", cli.gen.gamma,
                fastTb.gamma());
        return 1;
      }
      cli.gen.gamma = fastTb.gamma();
      printf("Indexed TB %s (%llu boards, E[V(0)]=%.4f)\n", cli.tbPath.c_str(),
             static_cast<unsigned long long>(fastTb.size()), fastTb.emptyBoardValue());
    }
    das::TinyDasNet nn;
    if (!cli.nnPath.empty()) {
      std::string err;
      if (!nn.load(cli.nnPath, &err)) {
        fprintf(stderr, "Error loading --nn: %s\n", err.c_str());
        return 1;
      }
      if (nn.rngMode != cli.gen.rngMode) {
        fprintf(stderr, "Error: --nn rng (%s) does not match CLI (%s)\n",
                das::rngModeName(nn.rngMode), das::rngModeName(cli.gen.rngMode));
        return 1;
      }
      if (isRtTb && !das::nnSupportsDasBuckets(nn, rtTb.numDasBuckets())) {
        fprintf(stderr, "Error: --nn outDim (%d) is incompatible with SRTBV004 K=%d\n", nn.outDim,
                rtTb.numDasBuckets());
        return 1;
      }
      if (fastTb.isLoaded() && nn.outDim > das::NUM_PIECES) {
        fprintf(stderr, "Error: --nn outDim (%d) must be %d when paired with SRTBV003 --tb\n",
                nn.outDim, das::NUM_PIECES);
        return 1;
      }
      printf("Loaded NNUE %s (%dx%d, out=%d, rng=%s)\n", cli.nnPath.c_str(), nn.h1, nn.h2,
             nn.outDim, das::rngModeName(nn.rngMode));
    }

    const bool useRtGen = isRtTb || (nn.isLoaded() && nn.outDim > das::NUM_PIECES);
    if (useRtGen) {
      das::MoveSearchConfig mcfg =
          isRtTb ? das::moveConfigFromHeaderV4(rtTb.header()) : das::MoveSearchConfig{};
      mcfg.gravity = das::getGravity20(cli.gen.level);
      const das::DasRtMoveTables tables(mcfg);
      printf("Generating live-RT states via V_RT+NN (L%d, rt=%d, games=%d, eps=%.2f, mode=%s, "
             "core=%zu, maxBoards=%zu, threads=%d)...\n",
             cli.gen.level, mcfg.reactionTime, cli.gen.numGames, cli.gen.epsilon,
             das::boardPriorityModeName(cli.gen.priorityMode), cli.gen.protectedCore,
             cli.gen.maxBoards, cli.gen.numThreads);
      fflush(stdout);
      boards = das::generateStateSetRt20(cli.gen, rtTb.isLoaded() ? &rtTb : nullptr, tables,
                                         nn.isLoaded() ? &nn : nullptr, priorBoards, stats);
    } else {
      printf("Generating states via TB+NN (L%d, games=%d, eps=%.2f, mode=%s, core=%zu, "
             "maxBoards=%zu, threads=%d)...\n",
             cli.gen.level, cli.gen.numGames, cli.gen.epsilon,
             das::boardPriorityModeName(cli.gen.priorityMode), cli.gen.protectedCore,
             cli.gen.maxBoards, cli.gen.numThreads);
      fflush(stdout);
      boards = das::generateStateSetTbNn20(cli.gen, fastTb.isLoaded() ? &fastTb : nullptr,
                                           nn.isLoaded() ? &nn : nullptr, priorBoards, stats);
    }
  }

  std::string writeErr;
  if (!das::writeStatesFileV2(cli.outPath, boards, &writeErr)) {
    fprintf(stderr, "Error writing %s: %s\n", cli.outPath.c_str(), writeErr.c_str());
    return 1;
  }

  const double inKeyPct =
      stats.totalMoves ? (100.0 * static_cast<double>(stats.inKeyMoves) / stats.totalMoves) : 0.0;
  printf("DONE generate_20row -> %s\n"
         "  Moves: %llu (in-K16: %.2f%%) | rawUnique: %zu | siblings: %zu | output: %zu (%.1f MB) "
         "| %.2fs\n",
         cli.outPath.c_str(), static_cast<unsigned long long>(stats.totalMoves), inKeyPct,
         stats.rawUniqueBoards, stats.siblingBoardsAdded, stats.outputBoards,
         static_cast<double>(stats.outputBoards * sizeof(das::BoardKey)) / (1024.0 * 1024.0),
         stats.elapsedSeconds);
  return 0;
}
