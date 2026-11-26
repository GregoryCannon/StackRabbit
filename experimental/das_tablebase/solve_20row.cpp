// solve_20row.cpp — Stationary 20-row Next-Box MPI solver CLI (SRTBV003).
// Supports Hard-Boundary and Frozen-NN Soft-Boundary MPI with warm-start (--init),
// auto/ram/disk policy caching, and automatic Soft-Boundary Monte Carlo verification.

#include "nnue_20row.hpp"
#include "solver_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <charconv>
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

void printUsage(FILE *stream, const char *prog) {
  fprintf(stream,
          "Usage: %s --in=STATES_OR_TB --out=OUT_TB [options]\n"
          "  --init=PATH              Warm-start SRTBV003 tablebase\n"
          "  --level=19|18            Stationary level (default 19)\n"
          "  --rng=nes|uniform        RNG mode (default nes)\n"
          "  --gamma=F                Discount factor in (0, 1] (default 0.995)\n"
          "  --discount-mode=piece|line  Per-piece or per-line discount (default piece)\n"
          "  --boundary-nn=PATH       Frozen SRNNV001 NNUE for Soft-Boundary MPI\n"
          "  --boundary-margin=F      Conservative soft-boundary subtraction (default 0.6)\n"
          "  --boundary-ply=1|2       Soft-boundary eval depth (default 1)\n"
          "  --tol=F                  Outer convergence tolerance (default 1e-5)\n"
          "  --inner-sweeps=N         Max policy-eval sweeps per outer iter (default 200)\n"
          "  --max-outer=N            Max outer MPI iterations (default 80)\n"
          "  --max-v-guard=F          Divergence abort ceiling (default 500.0)\n"
          "  --policy-cache=auto|ram|disk  Policy cache mode (default auto; disk above 6M)\n"
          "  --mc-games=N             Soft-MC verification games (default 10000, 0=skip)\n"
          "  --mc-seed=U64            Soft-MC verification seed (default 90000000)\n"
          "  --threads=N              Worker threads (default auto)\n",
          prog);
}

} // namespace

int main(int argc, char **argv) {
  std::string inPath;
  std::string outPath;
  std::string initPath;
  std::string boundaryNnPath;
  double boundaryMargin = 0.6;
  int boundaryPly = 1;
  bool hasBoundaryMargin = false;
  bool hasBoundaryPly = false;
  int mcGames = 10000;
  uint64_t mcSeed = 90000000ULL;

  das::SolveConfig20 cfg;
  cfg.verbose = true;
  cfg.numThreads = 0;

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
    if (tryPrefix("--in=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --in path\n");
        return 1;
      }
      inPath = std::string(val);
      continue;
    }
    if (tryPrefix("--out=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --out path\n");
        return 1;
      }
      outPath = std::string(val);
      continue;
    }
    if (tryPrefix("--init=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --init path\n");
        return 1;
      }
      initPath = std::string(val);
      continue;
    }
    if (tryPrefix("--boundary-nn=", val)) {
      if (val.empty()) {
        fprintf(stderr, "Error: empty --boundary-nn path\n");
        return 1;
      }
      boundaryNnPath = std::string(val);
      continue;
    }
    if (tryPrefix("--rng=", val)) {
      if (!das::parseRngMode(val, cfg.rngMode)) {
        fprintf(stderr, "Error: invalid --rng\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--policy-cache=", val)) {
      if (!das::parsePolicyCacheMode(val, cfg.policyCacheMode)) {
        fprintf(stderr, "Error: invalid --policy-cache (expected auto, ram, or disk)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--level=", val)) {
      if (!parseStrictInt(val, cfg.level) || (cfg.level != 18 && cfg.level != 19)) {
        fprintf(stderr, "Error: invalid --level (expected 18 or 19)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--gamma=", val)) {
      if (!parseStrictDouble(val, cfg.gamma) || cfg.gamma <= 0.0 || cfg.gamma > 1.0) {
        fprintf(stderr, "Error: invalid --gamma\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--discount-mode=", val)) {
      if (!das::parseDiscountMode(val, cfg.discountMode)) {
        fprintf(stderr, "Error: invalid --discount-mode (expected piece or line)\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--boundary-margin=", val)) {
      if (!parseStrictDouble(val, boundaryMargin) || boundaryMargin < 0.0) {
        fprintf(stderr, "Error: invalid --boundary-margin\n");
        return 1;
      }
      hasBoundaryMargin = true;
      continue;
    }
    if (tryPrefix("--boundary-ply=", val)) {
      if (!parseStrictInt(val, boundaryPly) || (boundaryPly != 1 && boundaryPly != 2)) {
        fprintf(stderr, "Error: invalid --boundary-ply (expected 1 or 2)\n");
        return 1;
      }
      hasBoundaryPly = true;
      continue;
    }
    if (tryPrefix("--tol=", val)) {
      if (!parseStrictDouble(val, cfg.tolerance) || cfg.tolerance <= 0.0) {
        fprintf(stderr, "Error: invalid --tol\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--inner-sweeps=", val)) {
      if (!parseStrictInt(val, cfg.innerEvalSweeps) || cfg.innerEvalSweeps < 0) {
        fprintf(stderr, "Error: invalid --inner-sweeps\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--max-outer=", val)) {
      if (!parseStrictInt(val, cfg.maxOuterIterations) || cfg.maxOuterIterations <= 0) {
        fprintf(stderr, "Error: invalid --max-outer\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--max-v-guard=", val)) {
      if (!parseStrictDouble(val, cfg.maxValueDivergenceGuard)
          || cfg.maxValueDivergenceGuard <= 0.0) {
        fprintf(stderr, "Error: invalid --max-v-guard\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--mc-games=", val)) {
      if (!parseStrictInt(val, mcGames) || mcGames < 0) {
        fprintf(stderr, "Error: invalid --mc-games\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--mc-seed=", val)) {
      if (!parseStrictInt(val, mcSeed)) {
        fprintf(stderr, "Error: invalid --mc-seed\n");
        return 1;
      }
      continue;
    }
    if (tryPrefix("--threads=", val)) {
      if (!parseStrictInt(val, cfg.numThreads) || cfg.numThreads < 0) {
        fprintf(stderr, "Error: invalid --threads\n");
        return 1;
      }
      continue;
    }
    fprintf(stderr, "Error: unknown arg '%.*s'\n", static_cast<int>(arg.size()), arg.data());
    return 1;
  }

  if (inPath.empty() || outPath.empty()) {
    fprintf(stderr, "Error: --in and --out are required\n");
    return 1;
  }
  if ((hasBoundaryMargin || hasBoundaryPly) && boundaryNnPath.empty()) {
    fprintf(stderr, "Error: --boundary-margin/--boundary-ply require --boundary-nn\n");
    return 1;
  }
  if (das::detail::isSameFilePath(inPath, outPath)
      || das::detail::isSameFilePath(initPath, outPath)
      || das::detail::isSameFilePath(boundaryNnPath, outPath)) {
    fprintf(stderr, "Error: --out must not alias --in, --init, or --boundary-nn\n");
    return 1;
  }
  if (cfg.numThreads == 0) {
    cfg.numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  }

  std::vector<das::BoardKey> keys;
  std::string err;
  if (!das::loadInputBoardKeys(inPath, keys, &err)) {
    fprintf(stderr, "Error loading --in: %s\n", err.c_str());
    return 1;
  }
  printf("Loaded %zu boards from %s\n", keys.size(), inPath.c_str());

  das::MappedDasTablebase initTb;
  if (!initPath.empty()) {
    if (!initTb.open(initPath, &err)) {
      fprintf(stderr, "Error loading --init: %s\n", err.c_str());
      return 1;
    }
    if (initTb.level() != cfg.level || initTb.rngMode() != cfg.rngMode) {
      fprintf(stderr, "Error: --init level/rng (%d/%s) does not match CLI (%d/%s)\n",
              initTb.level(), das::rngModeName(initTb.rngMode()), cfg.level,
              das::rngModeName(cfg.rngMode));
      return 1;
    }
    printf("Warm-start TB: %s (%llu boards, %s, gamma=%.5f, E[V(0)]=%.5f)\n", initPath.c_str(),
           static_cast<unsigned long long>(initTb.size()),
           das::discountModeName(initTb.discountMode()), initTb.gamma(), initTb.emptyBoardValue());
    if (initTb.discountMode() != cfg.discountMode) {
      fprintf(stderr, "Note: warm-start TB discount mode (%s) differs from CLI (%s)\n",
              das::discountModeName(initTb.discountMode()),
              das::discountModeName(cfg.discountMode));
    }
  }

  das::TinyDasNet boundaryNn;
  if (!boundaryNnPath.empty()) {
    if (!boundaryNn.load(boundaryNnPath, &err)) {
      fprintf(stderr, "Error loading --boundary-nn: %s\n", err.c_str());
      return 1;
    }
    if (boundaryNn.rngMode != cfg.rngMode) {
      fprintf(stderr, "Error: --boundary-nn rng (%s) does not match CLI (%s)\n",
              das::rngModeName(boundaryNn.rngMode), das::rngModeName(cfg.rngMode));
      return 1;
    }
    printf("Soft-Boundary NNUE: %s (%dx%d, margin=%.3f, ply=%d)\n", boundaryNnPath.c_str(),
           boundaryNn.h1, boundaryNn.h2, boundaryMargin, boundaryPly);
  }

  printf("Solving L%d (%s, gamma=%.5f [%s], tol=%.1e, cache=%s, threads=%d)...\n", cfg.level,
         das::rngModeName(cfg.rngMode), cfg.gamma, das::discountModeName(cfg.discountMode),
         cfg.tolerance, das::policyCacheModeName(cfg.policyCacheMode), cfg.numThreads);
  fflush(stdout);

  das::SolveResult20 res;
  das::NnSoftBoundaryEval20 softBound{};
  if (boundaryNn.isLoaded()) {
    softBound.net = &boundaryNn;
    softBound.margin = boundaryMargin;
    softBound.gravity = das::getGravity20(cfg.level);
    softBound.gamma = cfg.gamma;
    softBound.discountMode = cfg.discountMode;
    softBound.rngMode = cfg.rngMode;
    softBound.use2Ply = (boundaryPly == 2);
    res = das::solve20MPI(keys, cfg, initTb.isLoaded() ? &initTb : nullptr, softBound);
  } else {
    res = das::solve20MPI(keys, cfg, initTb.isLoaded() ? &initTb : nullptr);
  }

  if (!res.ok) {
    fprintf(stderr, "Solver failed: %s\n", res.error.c_str());
    return 1;
  }

  // Release initTb before writing output.
  initTb.close();

  das::TablebaseHeaderV3 hdr{};
  hdr.rngMode = static_cast<uint32_t>(cfg.rngMode);
  hdr.rewardMode = das::REWARD_MODE_NORM_TETRIS;
  hdr.level = static_cast<uint32_t>(cfg.level);
  hdr.lineCount = 0U;
  hdr.reactionTimeFrames = 0U;
  hdr.flags =
      (cfg.discountMode == das::DiscountMode::PerLine) ? das::TB_FLAG_DISCOUNT_PER_LINE : 0U;
  hdr.gamma = cfg.gamma;
  hdr.emptyBoardValue = res.emptyBoardValue;
  hdr.bellmanResidual = res.bellmanResidual;

  if (!das::writeTablebaseV3(outPath, keys, res.values, hdr, &err)) {
    fprintf(stderr, "Error writing %s: %s\n", outPath.c_str(), err.c_str());
    return 1;
  }

  // Free large solver buffers before optional Soft-MC verification so RAM never overlaps.
  std::vector<double>().swap(res.values);
  std::vector<das::BoardKey>().swap(keys);

  printf("DONE solve_20row -> %s\n"
         "  Converged: %s | outer=%d inner=%d | warmMatched=%zu | cache=%s\n"
         "  E[V(0)]: %.6f | maxV: %.4f | residual: %.3e | %.2fs\n",
         outPath.c_str(), res.converged ? "YES" : "NO", res.outerIterations, res.totalInnerSweeps,
         res.warmStartMatched, res.usedDiskPolicyCache ? "disk" : "ram", res.emptyBoardValue,
         res.maxValue, res.bellmanResidual, res.elapsedSeconds);
  fflush(stdout);

  if (mcGames > 0) {
    // Use MappedDasTablebase directly: zero anonymous RAM and zero index rebuild wait at 20M-50M.
    das::MappedDasTablebase writtenTb;
    if (!writtenTb.open(outPath, &err)) {
      fprintf(stderr, "Error re-opening %s for Soft-MC: %s\n", outPath.c_str(), err.c_str());
      return 1;
    }
    writtenTb.adviseRandom();
    const das::SoftMcResult20 mc =
        boundaryNn.isLoaded()
            ? das::evaluateSoftBoundaryMC20(writtenTb, mcGames, mcSeed, cfg.numThreads, softBound)
            : das::evaluateSoftBoundaryMC20(writtenTb, mcGames, mcSeed, cfg.numThreads);
    printf("  Soft-MC (%d games): mean=%.5f +/- %.5f | E[V(0)]=%.5f | z=%.2f | steps=%.1f | "
           "exit=%.1f%%\n",
           mc.numGames, mc.meanReturn, mc.stdError, writtenTb.emptyBoardValue(), mc.zScore,
           mc.meanSteps, mc.exitRate);
    if (mc.zScore > 3.5) {
      fprintf(stderr, "ERROR: Soft-MC verification exceeded 3.5 sigma (z=%.2f)!\n", mc.zScore);
      return 2;
    }
  }
  return 0;
}
