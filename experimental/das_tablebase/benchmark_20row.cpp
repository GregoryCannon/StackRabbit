// Fixed-seed multi-threaded benchmark CLI for 20-row NES Tetris (10 Hz DAS).
// Supports Stationary L19, Stationary L18, and Full NTSC 18 -> 29 games across:
//   - eval2ply         : fast 0-frame 2-ply eval.cpp (10 Hz, no playouts)
//   - precompute       : production StackRabbit precompute (fullMoveSearch + RT + DAS + playouts)
//   - tbnn             : fast 0-frame Hybrid TB + NNUE (or standalone NNUE)
//   - tbnn-precompute  : exact 18-frame RT + fullMoveSearch + DAS penalties + Hybrid TB + NNUE

#include "eval_adapter.hpp"
#include "nnue_20row.hpp"
#include "tablebase_io_20row.hpp"
#include "tbnn_precompute_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

enum class BenchMode {
  L19,    // Stationary Level 19 (default 100 lines; stationary eval lines=130)
  L18,    // Stationary Level 18 (default 130 lines; stationary eval lines=0)
  Full18, // Full NTSC 18 -> 29 (L=0..230 with exact level-ups & NES scoring)
};

enum class AgentKind {
  Eval2Ply,
  Precompute,
  TbNn,
  TbNnPrecompute,
};

struct BenchConfig {
  BenchMode mode = BenchMode::Full18;
  AgentKind agent = AgentKind::Eval2Ply;
  int games = 500;
  uint64_t seed = 30000000ULL;
  int threads = 0; // 0 = auto
  std::optional<int> maxLines;
  bool uncappedLines = false;
  int maxPieces = 2000;
  int reactionTime = 18;
  int useDas = 1;
  int playoutCount = 0;
  int playoutLength = 0;
  int pruningBreadth = 12;
  bool hasPrecomputeOnlyFlag = false;
  bool hasPlayoutFlag = false;
  AggressionLevel aggro = AGGRO;
  das::RngMode rngMode = das::RngMode::Nes;

  // TB + NNUE options
  std::string tbPath;
  std::string nnPath;
  double gamma = 0.995;
  double outMargin = 0.25;
  int outPly = 2;
  int outTopK = das::NN_DEFAULT_3PLY_TOPK;
  bool hasTbNnFlag = false;
  bool gammaSet = false;
};

struct GameOutcome {
  uint64_t score = 0;
  uint64_t scoreAt230 = 0;
  double normTetris = 0.0;
  int lines = 0;
  int pieces = 0;
  uint64_t tbHits = 0;
  uint64_t allOutMoves = 0;
  uint64_t sumMaxHeight = 0;
  int tetrisCount = 0;
  bool reachedTransition = false;
  bool reachedGoal = false;
  bool reachedKillscreen = false;
  bool reachedMaxout = false;
};

const char *modeName(BenchMode m) {
  switch (m) {
  case BenchMode::L19:
    return "l19";
  case BenchMode::L18:
    return "l18";
  case BenchMode::Full18:
    return "full18";
  }
  return "unknown";
}

const char *agentName(AgentKind a) {
  switch (a) {
  case AgentKind::Eval2Ply:
    return "eval2ply";
  case AgentKind::Precompute:
    return "precompute";
  case AgentKind::TbNn:
    return "tbnn";
  case AgentKind::TbNnPrecompute:
    return "tbnn-precompute";
  }
  return "unknown";
}

const char *aggroName(AggressionLevel a) {
  switch (a) {
  case SAFE:
    return "safe";
  case AGGRO:
    return "aggro";
  case HYPER:
    return "hyper";
  }
  return "unknown";
}

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

int resolvedLineGoal(const BenchConfig &cfg) {
  if (cfg.uncappedLines)
    return 0;
  if (cfg.maxLines.has_value())
    return *cfg.maxLines;
  if (cfg.mode == BenchMode::L19)
    return 100;
  if (cfg.mode == BenchMode::L18)
    return 130;
  return 230;
}

int boardMaxHeight(const das::Board20 &b) noexcept {
  return (b.topNonEmptyRow < das::BOARD_ROWS) ? (das::BOARD_ROWS - b.topNonEmptyRow) : 0;
}

GameOutcome runSingleGame(const BenchConfig &cfg, const das::BackendContexts &bc,
                          double calibratedMovesPerMs7T, const das::IndexedDasTablebase *tb,
                          const das::TinyDasNet *nn, int gameIdx, std::vector<LockPlacement> &m1,
                          std::vector<LockPlacement> &m2) {
  das::Rng rng(cfg.seed + static_cast<uint64_t>(gameIdx) * 1009ULL);
  const bool stationary = (cfg.mode != BenchMode::Full18);
  const int startLevel = (cfg.mode == BenchMode::L19) ? 19 : 18;
  const int startLines = (cfg.mode == BenchMode::L19) ? 130 : 0;
  const int lineGoal = resolvedLineGoal(cfg);

  GameState st =
      das::board20ToGameState(das::Board20::empty(), startLevel, startLines, bc, cfg.aggro);
  das::Board20 b20 = das::Board20::empty();
  int curLevel = startLevel;

  int curP = rng.sampleOpeningPiece(cfg.rngMode);
  int nextP = rng.sampleNextPiece(cfg.rngMode, curP);
  int dasCharge = cfg.useDas ? 0 : -1;

  const das::NnSearchConfig20 outCfg{cfg.outPly, cfg.outTopK};

  GameOutcome out = {};
  while (out.pieces < cfg.maxPieces) {
    if (lineGoal > 0 && out.lines >= lineGoal) {
      break;
    }

    int cleared = 0;
    if (cfg.agent == AgentKind::Eval2Ply) {
      out.sumMaxHeight += static_cast<uint64_t>(boardMaxHeight(das::gameStateToBoard20(st)));
      das::Eval2PlyResult er =
          das::chooseEval2PlyMove10Hz(st, curP, nextP, bc, cfg.aggro, stationary, m1, m2);
      if (!er.hasMove)
        break;
      cleared = er.linesCleared;
      st = er.afterState;
      curLevel = st.level;
    } else if (cfg.agent == AgentKind::Precompute) {
      out.sumMaxHeight += static_cast<uint64_t>(boardMaxHeight(das::gameStateToBoard20(st)));
      das::PrecomputeOptions opts;
      opts.dasCharge = dasCharge;
      opts.reactionTime = (out.pieces == 0) ? 0 : cfg.reactionTime;
      opts.playoutCount = cfg.playoutCount;
      opts.playoutLength = cfg.playoutLength;
      opts.pruningBreadth = cfg.pruningBreadth;
      opts.calibratedMovesPerMs7T = calibratedMovesPerMs7T;
      opts.aggro = cfg.aggro;
      opts.stationaryLevel = stationary;

      das::PrecomputeMoveResult pr = das::chooseProductionPrecomputeMove(st, curP, nextP, opts, bc);
      if (!pr.hasMove)
        break;
      cleared = pr.linesCleared;
      dasCharge = cfg.useDas ? pr.dasChargeAfter : -1;
      st = pr.afterState;
      curLevel = st.level;
    } else if (cfg.agent == AgentKind::TbNn) {
      out.sumMaxHeight += static_cast<uint64_t>(boardMaxHeight(b20));
      const int gravity = das::getGravity20(curLevel);
      das::HybridMoveChoice20 ch = das::chooseHybridMove20(b20, curP, nextP, gravity, cfg.gamma, tb,
                                                           nn, cfg.outMargin, cfg.rngMode, outCfg);
      if (!ch.found)
        break;
      cleared = ch.linesCleared;
      if (ch.chosenInTb)
        out.tbHits++;
      if (ch.usedAllOutFallback)
        out.allOutMoves++;
      b20 = das::Board20::fromRows(ch.afterRows);
      if (!stationary && cleared > 0) {
        curLevel = das::getLevelAfterLineClears20(curLevel, out.lines, cleared);
      }
    } else {
      out.sumMaxHeight += static_cast<uint64_t>(boardMaxHeight(das::gameStateToBoard20(st)));
      das::TbNnPrecomputeOptions opts;
      opts.dasCharge = dasCharge;
      opts.reactionTime = (out.pieces == 0) ? 0 : cfg.reactionTime;
      opts.gamma = cfg.gamma;
      opts.outMargin = cfg.outMargin;
      opts.outCfg = outCfg;
      opts.rngMode = cfg.rngMode;
      opts.aggro = cfg.aggro;
      opts.stationaryLevel = stationary;

      das::TbNnPrecomputeResult pr =
          das::chooseTbNnPrecomputeMove(st, curP, nextP, tb, nn, opts, bc);
      if (!pr.hasMove)
        break;
      cleared = pr.linesCleared;
      dasCharge = cfg.useDas ? pr.dasChargeAfter : -1;
      if (pr.chosenInTb)
        out.tbHits++;
      if (pr.usedAllOutFallback)
        out.allOutMoves++;
      st = pr.afterState;
      curLevel = st.level;
    }

    if (cleared > 0 && cleared <= 4) {
      const int prevLines = out.lines;
      out.lines += cleared;
      out.score += static_cast<uint64_t>(das::nesRawScore(cleared, curLevel));
      if (cfg.mode == BenchMode::Full18 && prevLines < 230 && out.lines >= 230) {
        out.scoreAt230 = out.score;
      }
      out.normTetris += das::REWARD_NORM_TETRIS[cleared];
      if (cleared == 4)
        out.tetrisCount++;
    }
    out.pieces++;
    curP = nextP;
    nextP = rng.sampleNextPiece(cfg.rngMode, curP);
  }

  if (cfg.mode == BenchMode::Full18 && out.lines < 230) {
    out.scoreAt230 = out.score;
  }
  out.reachedTransition = (cfg.mode != BenchMode::L19) && (out.lines >= 130);
  out.reachedKillscreen = (cfg.mode == BenchMode::Full18) && (out.lines >= 230);
  out.reachedGoal = (lineGoal > 0) && (out.lines >= lineGoal);
  out.reachedMaxout = (out.score >= 999999ULL);
  return out;
}

double percentileSorted(const std::vector<double> &sortedVals, double q) {
  if (sortedVals.empty())
    return 0.0;
  const double pos = q * static_cast<double>(sortedVals.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const size_t hi = std::min(sortedVals.size() - 1, lo + 1);
  const double frac = pos - static_cast<double>(lo);
  return sortedVals[lo] * (1.0 - frac) + sortedVals[hi] * frac;
}

} // namespace

int main(int argc, char **argv) {
  BenchConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "-h" || arg == "--help") {
      printf("Usage: %s --mode=l19|l18|full18 --agent=eval2ply|precompute|tbnn|tbnn-precompute "
             "[options]\n"
             "  --games=N --seed=U64 --threads=N --rng=nes|uniform --max-lines=N|uncapped\n"
             "  --rt=0..60 --das=0|1 --playouts=N --playout-len=N --breadth=-2..1156\n"
             "  --tb=PATH --nn=PATH --gamma=F --out-margin=F|inf --out-ply=1|2|3 --out-topk=N\n",
             argv[0]);
      return 0;
    }
    auto tryPrefix = [&](std::string_view prefix, std::string_view &val) -> bool {
      if (arg.size() >= prefix.size() && arg.substr(0, prefix.size()) == prefix) {
        val = arg.substr(prefix.size());
        return true;
      }
      return false;
    };

    std::string_view val;
    if (arg == "--mode=l19") {
      cfg.mode = BenchMode::L19;
      continue;
    }
    if (arg == "--mode=l18") {
      cfg.mode = BenchMode::L18;
      continue;
    }
    if (arg == "--mode=full18") {
      cfg.mode = BenchMode::Full18;
      continue;
    }
    if (arg == "--agent=eval2ply") {
      cfg.agent = AgentKind::Eval2Ply;
      continue;
    }
    if (arg == "--agent=precompute") {
      cfg.agent = AgentKind::Precompute;
      continue;
    }
    if (arg == "--agent=tbnn") {
      cfg.agent = AgentKind::TbNn;
      continue;
    }
    if (arg == "--agent=tbnn-precompute") {
      cfg.agent = AgentKind::TbNnPrecompute;
      continue;
    }
    if (arg == "--aggro=safe") {
      cfg.aggro = SAFE;
      continue;
    }
    if (arg == "--aggro=aggro") {
      cfg.aggro = AGGRO;
      continue;
    }
    if (arg == "--aggro=hyper") {
      cfg.aggro = HYPER;
      continue;
    }
    if (tryPrefix("--tb=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --tb path\n");
        return 1;
      }
      cfg.tbPath = std::string(val);
      cfg.hasTbNnFlag = true;
      continue;
    }
    if (tryPrefix("--nn=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --nn path\n");
        return 1;
      }
      cfg.nnPath = std::string(val);
      cfg.hasTbNnFlag = true;
      continue;
    }
    if (tryPrefix("--rng=", val)) {
      if (!das::parseRngMode(val, cfg.rngMode)) {
        fprintf(stderr, "Error: invalid --rng value '%.*s' (expected nes or uniform)\n",
                static_cast<int>(val.size()), val.data());
        return 1;
      }
      continue;
    }
    if (tryPrefix("--games=", val)) {
      if (!parseStrictInt(val, cfg.games) || cfg.games <= 0) {
        fprintf(stderr, "Error: invalid --games (expected integer > 0)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--seed=", val)) {
      if (!parseStrictInt(val, cfg.seed)) {
        fprintf(stderr, "Error: invalid --seed (expected unsigned 64-bit integer)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--threads=", val)) {
      if (!parseStrictInt(val, cfg.threads) || cfg.threads < 0) {
        fprintf(stderr, "Error: invalid --threads (expected integer >= 0)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--max-lines=", val)) {
      if (val == "uncapped") {
        cfg.uncappedLines = true;
        cfg.maxLines.reset();
      } else {
        int ml = 0;
        if (!parseStrictInt(val, ml) || ml <= 0) {
          fprintf(stderr, "Error: invalid --max-lines (expected integer > 0 or 'uncapped')\n");
          return 1;
        }
        cfg.uncappedLines = false;
        cfg.maxLines = ml;
      }
      continue;
    }
    if (tryPrefix("--max-pieces=", val)) {
      if (!parseStrictInt(val, cfg.maxPieces) || cfg.maxPieces <= 0) {
        fprintf(stderr, "Error: invalid --max-pieces (expected integer > 0)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--rt=", val)) {
      if (!parseStrictInt(val, cfg.reactionTime) || cfg.reactionTime < 0 || cfg.reactionTime > 60) {
        fprintf(stderr, "Error: invalid --rt (expected integer in 0..60)\n");
        return 1;
      }
      cfg.hasPrecomputeOnlyFlag = true;
      continue;
    }
    if (tryPrefix("--das=", val)) {
      if (!parseStrictInt(val, cfg.useDas) || (cfg.useDas != 0 && cfg.useDas != 1)) {
        fprintf(stderr, "Error: invalid --das (expected 0 or 1)\n");
        return 1;
      }
      cfg.hasPrecomputeOnlyFlag = true;
      continue;
    }
    if (tryPrefix("--playouts=", val)) {
      if (!parseStrictInt(val, cfg.playoutCount) || cfg.playoutCount < 0) {
        fprintf(stderr, "Error: invalid --playouts (expected integer >= 0)\n");
        return 1;
      }
      cfg.hasPrecomputeOnlyFlag = true;
      cfg.hasPlayoutFlag = true;
      continue;
    }
    if (tryPrefix("--playout-len=", val)) {
      if (!parseStrictInt(val, cfg.playoutLength)
          || cfg.playoutLength < 0
          || cfg.playoutLength > 20) {
        fprintf(stderr, "Error: invalid --playout-len (expected integer in 0..20)\n");
        return 1;
      }
      cfg.hasPrecomputeOnlyFlag = true;
      cfg.hasPlayoutFlag = true;
      continue;
    }
    if (tryPrefix("--breadth=", val)) {
      if (!parseStrictInt(val, cfg.pruningBreadth)
          || cfg.pruningBreadth < -2
          || cfg.pruningBreadth > 1156) {
        fprintf(stderr, "Error: invalid --breadth (expected integer in -2..1156)\n");
        return 1;
      }
      cfg.hasPrecomputeOnlyFlag = true;
      cfg.hasPlayoutFlag = true;
      continue;
    }
    if (tryPrefix("--gamma=", val)) {
      if (!parseStrictDouble(val, cfg.gamma) || cfg.gamma <= 0.0 || cfg.gamma > 1.0) {
        fprintf(stderr, "Error: invalid --gamma (expected in (0, 1])\n");
        return 1;
      }
      cfg.hasTbNnFlag = true;
      cfg.gammaSet = true;
      continue;
    }
    if (tryPrefix("--out-margin=", val)) {
      if (val == "inf") {
        cfg.outMargin = std::numeric_limits<double>::infinity();
      } else if (!parseStrictDouble(val, cfg.outMargin) || cfg.outMargin < 0.0) {
        fprintf(stderr, "Error: invalid --out-margin (expected >= 0 or 'inf')\n");
        return 1;
      }
      cfg.hasTbNnFlag = true;
      continue;
    }
    if (tryPrefix("--out-ply=", val)) {
      if (!parseStrictInt(val, cfg.outPly) || cfg.outPly < 1 || cfg.outPly > 3) {
        fprintf(stderr, "Error: invalid --out-ply (expected 1, 2, or 3)\n");
        return 1;
      }
      cfg.hasTbNnFlag = true;
      continue;
    }
    if (tryPrefix("--out-topk=", val)) {
      if (!parseStrictInt(val, cfg.outTopK) || cfg.outTopK < 1) {
        fprintf(stderr, "Error: invalid --out-topk (expected >= 1)\n");
        return 1;
      }
      cfg.hasTbNnFlag = true;
      continue;
    }
    fprintf(stderr, "Error: unknown arg '%.*s'\n", static_cast<int>(arg.size()), arg.data());
    return 1;
  }

  const bool isTbNnAgent = (cfg.agent == AgentKind::TbNn || cfg.agent == AgentKind::TbNnPrecompute);
  const bool isPrecomputeAgent =
      (cfg.agent == AgentKind::Precompute || cfg.agent == AgentKind::TbNnPrecompute);

  if (!isPrecomputeAgent && cfg.hasPrecomputeOnlyFlag) {
    fprintf(stderr,
            "Error: --rt/--das/--playouts/--playout-len/--breadth require precompute agent\n");
    return 1;
  }
  if (!isTbNnAgent && cfg.hasTbNnFlag) {
    fprintf(stderr, "Error: --tb/--nn/--gamma/--out-margin/--out-ply/--out-topk require tbnn or "
                    "tbnn-precompute\n");
    return 1;
  }
  if (cfg.agent == AgentKind::TbNnPrecompute && cfg.hasPlayoutFlag) {
    fprintf(stderr, "Error: --playouts/--playout-len/--breadth apply only to --agent=precompute\n");
    return 1;
  }
  if (cfg.agent == AgentKind::TbNnPrecompute && cfg.rngMode != das::RngMode::Nes) {
    fprintf(stderr, "Error: --agent=tbnn-precompute uses NES transition probabilities in collapse "
                    "(requires --rng=nes)\n");
    return 1;
  }
  if (isTbNnAgent && cfg.tbPath.empty() && cfg.nnPath.empty()) {
    fprintf(stderr, "Error: --agent=%s requires --tb and/or --nn\n", agentName(cfg.agent));
    return 1;
  }
  if (cfg.agent == AgentKind::Precompute) {
    const long long totalVol =
        (cfg.pruningBreadth < 0)
            ? static_cast<long long>(cfg.playoutCount)
            : static_cast<long long>(cfg.playoutCount) * static_cast<long long>(cfg.playoutLength);
    if (totalVol > MAX_CPP_PLAYOUT_MOVES) {
      fprintf(stderr, "Error: playout volume %lld exceeds MAX_CPP_PLAYOUT_MOVES\n", totalVol);
      return 1;
    }
  }

  das::IndexedDasTablebase tb;
  if (!cfg.tbPath.empty()) {
    std::string err;
    if (!tb.open(cfg.tbPath, &err)) {
      fprintf(stderr, "Error loading --tb: %s\n", err.c_str());
      return 1;
    }
    if (tb.rngMode() != cfg.rngMode) {
      fprintf(stderr, "Error: --tb rng (%s) does not match --rng=%s\n",
              das::rngModeName(tb.rngMode()), das::rngModeName(cfg.rngMode));
      return 1;
    }
    if ((cfg.mode == BenchMode::L19 && tb.level() != 19)
        || (cfg.mode == BenchMode::L18 && tb.level() != 18)) {
      fprintf(stderr, "Error: --tb level (%d) does not match --mode=%s\n", tb.level(),
              modeName(cfg.mode));
      return 1;
    }
    if (cfg.gammaSet && std::abs(tb.gamma() - cfg.gamma) > 1e-9) {
      fprintf(stderr, "Error: --gamma (%.5f) does not match --tb gamma (%.5f)\n", cfg.gamma,
              tb.gamma());
      return 1;
    }
    cfg.gamma = tb.gamma();
  }
  das::TinyDasNet nn;
  if (!cfg.nnPath.empty()) {
    std::string err;
    if (!nn.load(cfg.nnPath, &err)) {
      fprintf(stderr, "Error loading --nn: %s\n", err.c_str());
      return 1;
    }
    if (nn.rngMode != cfg.rngMode) {
      fprintf(stderr, "Error: --nn rng (%s) does not match --rng=%s\n",
              das::rngModeName(nn.rngMode), das::rngModeName(cfg.rngMode));
      return 1;
    }
  }

  const das::BackendContexts bc = das::BackendContexts::make10Hz();
  double calibratedMovesPerMs7T = 0.0;
  if (cfg.agent == AgentKind::Precompute && cfg.pruningBreadth == -2) {
    calibratedMovesPerMs7T = das::calibratePrecomputeSpeed7T(bc);
  }
  const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  int numThreads = cfg.threads;
  if (numThreads == 0) {
    if (cfg.agent == AgentKind::Precompute) {
      numThreads = (cfg.pruningBreadth == -2) ? 1 : std::min(2, hw);
    } else {
      numThreads = hw;
    }
  }
  numThreads = std::min(numThreads, cfg.games);
  if (cfg.agent == AgentKind::Precompute && cfg.pruningBreadth == -2 && numThreads > 1) {
    fprintf(stderr,
            "Warning: --breadth=-2 with --threads=%d oversubscribes wall-clock ms deadlines\n",
            numThreads);
  }

  std::vector<GameOutcome> outcomes(static_cast<size_t>(cfg.games));
  std::atomic<int> nextGame{0};
  std::atomic<int> doneGames{0};

  const auto t0 = std::chrono::steady_clock::now();
  {
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(numThreads));
    for (int tid = 0; tid < numThreads; ++tid) {
      workers.emplace_back([&]() {
        std::vector<LockPlacement> m1, m2;
        m1.reserve(64);
        m2.reserve(64);
        while (true) {
          const int g = nextGame.fetch_add(1, std::memory_order_relaxed);
          if (g >= cfg.games)
            break;
          outcomes[static_cast<size_t>(g)] =
              runSingleGame(cfg, bc, calibratedMovesPerMs7T, tb.isLoaded() ? &tb : nullptr,
                            nn.isLoaded() ? &nn : nullptr, g, m1, m2);
          const int done = doneGames.fetch_add(1, std::memory_order_relaxed) + 1;
          if (cfg.agent == AgentKind::Precompute
              && cfg.games >= 20
              && (done % 10 == 0 || done == cfg.games)) {
            const double sec =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "[precompute] %d/%d games done (%.1fs, %.1fs/game)\n", done, cfg.games,
                    sec, sec / done);
          }
        }
      });
    }
    for (auto &w : workers)
      w.join();
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  double sumScore = 0.0, sumScore230 = 0.0, sumNorm = 0.0, sumLines = 0.0, sumPieces = 0.0;
  uint64_t totalPieces = 0, totalTbHits = 0, totalAllOut = 0, totalHeightSum = 0;
  uint64_t totalTetrisLines = 0;
  int transCount = 0, ksCount = 0, goalCount = 0, maxoutCount = 0;
  std::vector<double> scores;
  scores.reserve(outcomes.size());

  for (const auto &o : outcomes) {
    const double sc = static_cast<double>(o.score);
    scores.push_back(sc);
    sumScore += sc;
    sumScore230 += static_cast<double>(o.scoreAt230);
    sumNorm += o.normTetris;
    sumLines += o.lines;
    sumPieces += o.pieces;
    totalPieces += static_cast<uint64_t>(o.pieces);
    totalTbHits += o.tbHits;
    totalAllOut += o.allOutMoves;
    totalHeightSum += o.sumMaxHeight;
    totalTetrisLines += static_cast<uint64_t>(o.tetrisCount) * 4ULL;
    if (o.reachedTransition)
      transCount++;
    if (o.reachedKillscreen)
      ksCount++;
    if (o.reachedGoal)
      goalCount++;
    if (o.reachedMaxout)
      maxoutCount++;
  }

  const double n = static_cast<double>(cfg.games);
  const double meanScore = sumScore / n;
  const double meanNorm = sumNorm / n;

  double varScoreAcc = 0.0, varNormAcc = 0.0;
  for (const auto &o : outcomes) {
    const double ds = static_cast<double>(o.score) - meanScore;
    const double dn = o.normTetris - meanNorm;
    varScoreAcc += ds * ds;
    varNormAcc += dn * dn;
  }
  const double seScore = (cfg.games > 1) ? std::sqrt((varScoreAcc / (n - 1.0)) / n) : 0.0;
  const double seNorm = (cfg.games > 1) ? std::sqrt((varNormAcc / (n - 1.0)) / n) : 0.0;

  std::sort(scores.begin(), scores.end());
  const double medScore = percentileSorted(scores, 0.50);
  const double p90Score = percentileSorted(scores, 0.90);
  const double maxScore = scores.empty() ? 0.0 : scores.back();
  const int lineGoal = resolvedLineGoal(cfg);
  const double trtPct =
      (sumLines > 0.0) ? (100.0 * static_cast<double>(totalTetrisLines) / sumLines) : 0.0;
  const double avgMaxH =
      totalPieces ? (static_cast<double>(totalHeightSum) / static_cast<double>(totalPieces)) : 0.0;

  if (cfg.agent == AgentKind::Eval2Ply) {
    printf("BENCH mode=%s agent=%s games=%d seed=%llu rng=%s aggro=%s goal=%d threads=%d\n",
           modeName(cfg.mode), agentName(cfg.agent), cfg.games,
           static_cast<unsigned long long>(cfg.seed), das::rngModeName(cfg.rngMode),
           aggroName(cfg.aggro), lineGoal, numThreads);
  } else if (cfg.agent == AgentKind::Precompute) {
    printf("BENCH mode=%s agent=%s games=%d seed=%llu rng=%s aggro=%s goal=%d rt=%d das=%d "
           "playouts=%dx%d b=%d threads=%d\n",
           modeName(cfg.mode), agentName(cfg.agent), cfg.games,
           static_cast<unsigned long long>(cfg.seed), das::rngModeName(cfg.rngMode),
           aggroName(cfg.aggro), lineGoal, cfg.reactionTime, cfg.useDas, cfg.playoutCount,
           cfg.playoutLength, cfg.pruningBreadth, numThreads);
  } else if (cfg.agent == AgentKind::TbNn) {
    printf("BENCH mode=%s agent=%s games=%d seed=%llu rng=%s goal=%d m=%.2f ply=%d gamma=%.4f "
           "tb=%s nn=%s threads=%d\n",
           modeName(cfg.mode), agentName(cfg.agent), cfg.games,
           static_cast<unsigned long long>(cfg.seed), das::rngModeName(cfg.rngMode), lineGoal,
           cfg.outMargin, cfg.outPly, cfg.gamma, tb.isLoaded() ? "on" : "off",
           nn.isLoaded() ? "on" : "off", numThreads);
  } else {
    printf("BENCH mode=%s agent=%s games=%d seed=%llu rng=%s goal=%d rt=%d das=%d m=%.2f ply=%d "
           "gamma=%.4f tb=%s nn=%s threads=%d\n",
           modeName(cfg.mode), agentName(cfg.agent), cfg.games,
           static_cast<unsigned long long>(cfg.seed), das::rngModeName(cfg.rngMode), lineGoal,
           cfg.reactionTime, cfg.useDas, cfg.outMargin, cfg.outPly, cfg.gamma,
           tb.isLoaded() ? "on" : "off", nn.isLoaded() ? "on" : "off", numThreads);
  }
  printf("  Score:      %.0f +/- %.0f | med=%.0f p90=%.0f max=%.0f\n", meanScore, seScore, medScore,
         p90Score, maxScore);
  printf("  NormTetris: %.3f +/- %.3f | lines=%.2f pieces=%.1f | TRT=%.1f%% | avgMaxH=%.2f\n",
         meanNorm, seNorm, sumLines / n, sumPieces / n, trtPct, avgMaxH);
  if (cfg.mode == BenchMode::Full18) {
    if (cfg.uncappedLines) {
      printf("  Rates:      trans(L>=130)=%.1f%% | ks(L>=230)=%.1f%% | maxout(>=1M)=%.1f%% | "
             "scoreAt230=%.0f\n",
             100.0 * transCount / n, 100.0 * ksCount / n, 100.0 * maxoutCount / n, sumScore230 / n);
    } else {
      printf("  Rates:      trans(L>=130)=%.1f%% | ks(L>=230)=%.1f%% | maxout(>=1M)=%.1f%%\n",
             100.0 * transCount / n, 100.0 * ksCount / n, 100.0 * maxoutCount / n);
    }
  } else {
    printf("  Rates:      goal(%dl)=%.1f%% | maxout(>=1M)=%.1f%%\n", lineGoal,
           100.0 * goalCount / n, 100.0 * maxoutCount / n);
  }
  if (isTbNnAgent && totalPieces > 0) {
    printf("  TB Stats:   hit=%.2f%% | allOut=%.2f%%\n",
           100.0 * static_cast<double>(totalTbHits) / static_cast<double>(totalPieces),
           100.0 * static_cast<double>(totalAllOut) / static_cast<double>(totalPieces));
  }
  printf("  Speed:      %.2fs total | %.1f mv/s | %.3f ms/mv (wall)\n", elapsed,
         static_cast<double>(totalPieces) / std::max(1e-6, elapsed),
         (elapsed * 1000.0) / std::max<uint64_t>(1, totalPieces));
  return 0;
}
