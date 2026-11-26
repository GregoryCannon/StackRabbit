// solve_rt_20row.cpp — Reaction-time + DAS-bucket MPI solver CLI (SRTBV004).
// Solves V_RT(b, c, d) over a state set with optional warm start (SRTBV003 broadcast or
// SRTBV004), an optional frozen Soft-Boundary NNUE, and automatic RT Soft-MC verification.

#include "rt_nn_boundary_20row.hpp"
#include "solver_rt_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  const auto res = std::from_chars(s.data(), s.data() + s.size(), val);
  if (res.ec != std::errc() || res.ptr != s.data() + s.size())
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

bool parseBool01(std::string_view s, bool &out) {
  if (s == "0") {
    out = false;
    return true;
  }
  if (s == "1") {
    out = true;
    return true;
  }
  return false;
}

void printUsage(FILE *stream, const char *prog) {
  fprintf(stream,
          "Usage: %s --in=STATES_OR_TB --out=OUT_TB [options]\n"
          "  --init=PATH              Warm start from SRTBV003 (broadcast) or SRTBV004\n"
          "  --das-buckets=2|3        DAS buckets (default 3)\n"
          "  --rt=0..60               Reaction time in frames (default 18)\n"
          "  --quicktaps=0|1          Enable X.X quicktaps in the move search (default 0)\n"
          "  --level=19|18            Stationary level (default 19)\n"
          "  --rng=nes|uniform        RNG mode (default nes)\n"
          "  --gamma=F                Discount factor in (0, 1] (default 0.975)\n"
          "  --discount-mode=piece|line  (default line)\n"
          "  --boundary-nn=PATH       Frozen SRNNV001 NNUE (7 or 7*K outputs) soft boundary\n"
          "  --boundary-margin=F      Soft-boundary margin (default 0.15)\n"
          "  --tol=F                  Outer convergence tolerance (default 1e-5)\n"
          "  --inner-sweeps=N         Max policy-eval sweeps per outer iteration (default 300)\n"
          "  --inner-stop-ratio=F     Inner early-stop ratio (default 0.02)\n"
          "  --max-outer=N            Max outer iterations (default 40)\n"
          "  --exact-residual=0|1     Run an extra Bellman backup for the residual (default 1)\n"
          "  --policy-cache=auto|ram|disk  (default auto)\n"
          "  --limit=N                Solve only the first N sorted boards (timing runs)\n"
          "  --mc-games=N             RT Soft-MC verification games (default 4000, 0=skip)\n"
          "  --mc-seed=U64            Soft-MC seed (default 90000000)\n"
          "  --threads=N              Worker threads (default auto)\n",
          prog);
}

} // namespace

int main(int argc, char **argv) {
  std::string inPath, outPath, initPath, boundaryNnPath;
  double boundaryMargin = 0.15;
  bool hasBoundaryMargin = false;
  int mcGames = 4000;
  uint64_t mcSeed = 90000000ULL;
  size_t limit = 0;

  das::SolveRtConfig20 cfg;
  cfg.base.verbose = true;
  cfg.base.numThreads = 0;
  cfg.base.gamma = 0.975;
  cfg.base.discountMode = das::DiscountMode::PerLine;
  cfg.base.innerEvalSweeps = 300;
  cfg.base.innerStopRatio = 0.02;
  cfg.base.maxOuterIterations = 40;
  cfg.numDasBuckets = 3;
  cfg.move.enableQuicktaps = false;
  cfg.move.reactionTime = 18;

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
    bool ok = true;
    if (tryPrefix("--in=", val)) {
      inPath = std::string(val);
      ok = !val.empty();
    } else if (tryPrefix("--out=", val)) {
      outPath = std::string(val);
      ok = !val.empty();
    } else if (tryPrefix("--init=", val)) {
      initPath = std::string(val);
      ok = !val.empty();
    } else if (tryPrefix("--boundary-nn=", val)) {
      boundaryNnPath = std::string(val);
      ok = !val.empty();
    } else if (tryPrefix("--das-buckets=", val)) {
      ok = parseStrictInt(val, cfg.numDasBuckets) && das::isValidDasBucketCount(cfg.numDasBuckets);
    } else if (tryPrefix("--rt=", val)) {
      ok = parseStrictInt(val, cfg.move.reactionTime)
           && cfg.move.reactionTime >= 0
           && cfg.move.reactionTime <= 60;
    } else if (tryPrefix("--quicktaps=", val)) {
      ok = parseBool01(val, cfg.move.enableQuicktaps);
    } else if (tryPrefix("--level=", val)) {
      ok = parseStrictInt(val, cfg.base.level) && (cfg.base.level == 18 || cfg.base.level == 19);
    } else if (tryPrefix("--rng=", val)) {
      ok = das::parseRngMode(val, cfg.base.rngMode);
    } else if (tryPrefix("--gamma=", val)) {
      ok = parseStrictDouble(val, cfg.base.gamma) && cfg.base.gamma > 0.0 && cfg.base.gamma <= 1.0;
    } else if (tryPrefix("--discount-mode=", val)) {
      ok = das::parseDiscountMode(val, cfg.base.discountMode);
    } else if (tryPrefix("--boundary-margin=", val)) {
      ok = parseStrictDouble(val, boundaryMargin) && boundaryMargin >= 0.0;
      hasBoundaryMargin = true;
    } else if (tryPrefix("--tol=", val)) {
      ok = parseStrictDouble(val, cfg.base.tolerance) && cfg.base.tolerance > 0.0;
    } else if (tryPrefix("--inner-sweeps=", val)) {
      ok = parseStrictInt(val, cfg.base.innerEvalSweeps) && cfg.base.innerEvalSweeps >= 0;
    } else if (tryPrefix("--inner-stop-ratio=", val)) {
      ok = parseStrictDouble(val, cfg.base.innerStopRatio)
           && cfg.base.innerStopRatio > 0.0
           && cfg.base.innerStopRatio < 1.0;
    } else if (tryPrefix("--max-outer=", val)) {
      ok = parseStrictInt(val, cfg.base.maxOuterIterations) && cfg.base.maxOuterIterations > 0;
    } else if (tryPrefix("--exact-residual=", val)) {
      ok = parseBool01(val, cfg.exactResidual);
    } else if (tryPrefix("--policy-cache=", val)) {
      ok = das::parsePolicyCacheMode(val, cfg.base.policyCacheMode);
    } else if (tryPrefix("--limit=", val)) {
      ok = parseStrictInt(val, limit) && limit > 0;
    } else if (tryPrefix("--mc-games=", val)) {
      ok = parseStrictInt(val, mcGames) && mcGames >= 0;
    } else if (tryPrefix("--mc-seed=", val)) {
      ok = parseStrictInt(val, mcSeed);
    } else if (tryPrefix("--threads=", val)) {
      ok = parseStrictInt(val, cfg.base.numThreads) && cfg.base.numThreads >= 0;
    } else {
      fprintf(stderr, "Error: unknown arg '%.*s'\n", static_cast<int>(arg.size()), arg.data());
      return 1;
    }
    if (!ok) {
      fprintf(stderr, "Error: invalid value in '%.*s'\n", static_cast<int>(arg.size()), arg.data());
      return 1;
    }
  }

  if (inPath.empty() || outPath.empty()) {
    fprintf(stderr, "Error: --in and --out are required\n");
    return 1;
  }
  if (hasBoundaryMargin && boundaryNnPath.empty()) {
    fprintf(stderr, "Error: --boundary-margin requires --boundary-nn\n");
    return 1;
  }
  if (das::detail::isSameFilePath(inPath, outPath)
      || das::detail::isSameFilePath(initPath, outPath)
      || das::detail::isSameFilePath(boundaryNnPath, outPath)) {
    fprintf(stderr, "Error: --out must not alias --in, --init, or --boundary-nn\n");
    return 1;
  }
  if (cfg.base.numThreads == 0) {
    cfg.base.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  }
  cfg.move.gravity = das::getGravity20(cfg.base.level);
  const int K = cfg.numDasBuckets;

  std::vector<das::BoardKey> keys;
  std::string err;
  if (!das::loadInputBoardKeysAnyFormat(inPath, keys, &err)) {
    fprintf(stderr, "Error loading --in: %s\n", err.c_str());
    return 1;
  }
  if (limit > 0 && limit < keys.size()) {
    keys.resize(limit);
    if (std::find(keys.begin(), keys.end(), das::EMPTY_BOARD_KEY) == keys.end()) {
      keys.back() = das::EMPTY_BOARD_KEY;
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    }
  }
  printf("Loaded %zu boards from %s\n", keys.size(), inPath.c_str());

  das::MappedDasTablebase initV3;
  das::MappedDasRtTablebase initV4;
  if (!initPath.empty()) {
    const std::string magic = das::peekFileMagic(initPath);
    const bool isV4 =
        magic.size() == 8 && std::memcmp(magic.data(), das::TABLEBASE_V4_MAGIC, 8) == 0;
    int initLevel = 0;
    das::RngMode initRng = das::RngMode::Nes;
    if (isV4) {
      if (!initV4.open(initPath, &err)) {
        fprintf(stderr, "Error loading --init: %s\n", err.c_str());
        return 1;
      }
      initLevel = initV4.level();
      initRng = initV4.rngMode();
    } else {
      if (!initV3.open(initPath, &err)) {
        fprintf(stderr, "Error loading --init: %s\n", err.c_str());
        return 1;
      }
      initLevel = initV3.level();
      initRng = initV3.rngMode();
    }
    if (initLevel != cfg.base.level || initRng != cfg.base.rngMode) {
      fprintf(stderr, "Error: --init level/rng does not match CLI\n");
      return 1;
    }
    printf("Warm-start TB: %s (%s)\n", initPath.c_str(), isV4 ? "SRTBV004" : "SRTBV003 broadcast");
  }

  das::TinyDasNet boundaryNn;
  if (!boundaryNnPath.empty()) {
    if (!boundaryNn.load(boundaryNnPath, &err)) {
      fprintf(stderr, "Error loading --boundary-nn: %s\n", err.c_str());
      return 1;
    }
    if (boundaryNn.rngMode != cfg.base.rngMode || !das::nnSupportsDasBuckets(boundaryNn, K)) {
      fprintf(stderr, "Error: --boundary-nn rng/outDim (%d) incompatible with K=%d\n",
              boundaryNn.outDim, K);
      return 1;
    }
    printf("Soft-Boundary NNUE: %s (%dx%d, outDim=%d, margin=%.3f)\n", boundaryNnPath.c_str(),
           boundaryNn.h1, boundaryNn.h2, boundaryNn.outDim, boundaryMargin);
  }

  printf("Solving V_RT L%d (K=%d, rt=%d, qt=%d, %s, gamma=%.5f [%s], tol=%.1e, threads=%d)...\n",
         cfg.base.level, K, cfg.move.reactionTime, cfg.move.enableQuicktaps ? 1 : 0,
         das::rngModeName(cfg.base.rngMode), cfg.base.gamma,
         das::discountModeName(cfg.base.discountMode), cfg.base.tolerance, cfg.base.numThreads);
  fflush(stdout);

  das::NnSoftBoundaryEvalRt20 softBound{};
  softBound.net = &boundaryNn;
  softBound.margin = boundaryMargin;
  const das::MappedDasTablebase *pInit3 = initV3.isLoaded() ? &initV3 : nullptr;
  const das::MappedDasRtTablebase *pInit4 = initV4.isLoaded() ? &initV4 : nullptr;
  das::SolveRtResult20 res = boundaryNn.isLoaded()
                                 ? das::solve20RtMPI(keys, cfg, pInit3, pInit4, softBound)
                                 : das::solve20RtMPI(keys, cfg, pInit3, pInit4);
  if (!res.ok) {
    fprintf(stderr, "Solver failed: %s\n", res.error.c_str());
    return 1;
  }
  initV3.close();
  initV4.close();

  das::TablebaseHeaderV4 hdr{};
  hdr.rngMode = static_cast<uint32_t>(cfg.base.rngMode);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = static_cast<uint32_t>(cfg.base.level);
  hdr.lineCount = 0U;
  hdr.flags =
      (cfg.base.discountMode == das::DiscountMode::PerLine) ? das::TB_FLAG_DISCOUNT_PER_LINE : 0U;
  hdr.numDasBuckets = static_cast<uint32_t>(K);
  das::fillHeaderV4MoveFields(hdr, cfg.move);
  hdr.gamma = cfg.base.gamma;
  hdr.emptyBoardValue = res.emptyBoardValue;
  hdr.bellmanResidual = res.bellmanResidual;
  if (!das::writeTablebaseV4(outPath, keys, res.values, hdr, &err)) {
    fprintf(stderr, "Error writing %s: %s\n", outPath.c_str(), err.c_str());
    return 1;
  }
  std::vector<double>().swap(res.values);
  std::vector<das::BoardKey>().swap(keys);

  printf("DONE solve_rt_20row -> %s\n"
         "  Converged: %s | outer=%d inner=%d | warmMatched=%zu | cache=%s\n"
         "  E[V(0,full)]: %.6f | maxV: %.4f | residual: %.3e | improve %.1fs | total %.1fs\n",
         outPath.c_str(), res.converged ? "YES" : "NO", res.outerIterations, res.totalInnerSweeps,
         res.warmStartMatched, res.usedDiskPolicyCache ? "disk" : "ram", res.emptyBoardValue,
         res.maxValue, res.bellmanResidual, res.improvementSeconds, res.elapsedSeconds);
  fflush(stdout);

  if (mcGames > 0) {
    das::MappedDasRtTablebase written;
    if (!written.open(outPath, &err)) {
      fprintf(stderr, "Error re-opening %s: %s\n", outPath.c_str(), err.c_str());
      return 1;
    }
    written.adviseRandom();
    const das::DasRtMoveTables tables(cfg.move);
    const das::SoftMcResult20 mc =
        boundaryNn.isLoaded() ? das::evaluateSoftBoundaryMcRt20(written, tables, mcGames, mcSeed,
                                                                cfg.base.numThreads, softBound)
                              : das::evaluateSoftBoundaryMcRt20(written, tables, mcGames, mcSeed,
                                                                cfg.base.numThreads);
    printf("  RT Soft-MC (%d games): mean=%.5f +/- %.5f | E[V(0,full)]=%.5f | z=%.2f | "
           "steps=%.1f | exit=%.1f%%\n",
           mc.numGames, mc.meanReturn, mc.stdError, written.emptyBoardValue(), mc.zScore,
           mc.meanSteps, mc.exitRate);
    if (mc.zScore > 3.5) {
      fprintf(stderr, "ERROR: RT Soft-MC verification exceeded 3.5 sigma (z=%.2f)!\n", mc.zScore);
      return 2;
    }
  }
  return 0;
}
