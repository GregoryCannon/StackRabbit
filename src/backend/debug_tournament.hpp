#ifndef DEBUG_TOURNAMENT_HPP
#define DEBUG_TOURNAMENT_HPP

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Expects main.cpp, debug_ground_truth.hpp, and debug_precompute.hpp to be included first.

namespace debug_tmt {

inline int parseNextPieceArg(const std::string &arg) {
  if (arg.size() == 1) {
    char c = arg[0];
    if (c >= '0' && c <= '6') {
      return c - '0';
    }
    switch (c) {
    case 'I':
    case 'i':
      return 0;
    case 'O':
    case 'o':
      return 1;
    case 'L':
    case 'l':
      return 2;
    case 'J':
    case 'j':
      return 3;
    case 'T':
    case 't':
      return 4;
    case 'S':
    case 's':
      return 5;
    case 'Z':
    case 'z':
      return 6;
    default:
      return -1;
    }
  }
  return -1;
}

inline bool parseLockKey(const std::string &key, int &rot, int &x, int &y) {
  size_t p1 = key.find('|');
  if (p1 == std::string::npos) {
    return false;
  }
  size_t p2 = key.find('|', p1 + 1);
  if (p2 == std::string::npos) {
    return false;
  }
  std::string s0 = key.substr(0, p1);
  std::string s1 = key.substr(p1 + 1, p2 - p1 - 1);
  std::string s2 = key.substr(p2 + 1);
  if (!debug_gt::isPlainInt(s0) || !debug_gt::isPlainInt(s1) || !debug_gt::isPlainInt(s2)) {
    return false;
  }
  rot = std::atoi(s0.c_str());
  x = std::atoi(s1.c_str());
  y = std::atoi(s2.c_str());
  return true;
}

// Runs the tournament search for one (board, curPiece, nextPiece) with full per-round
// tracing and returns JSON. Requires tournament mode (pruningBreadth == -1 or -2).
inline std::string runDebugTournament(const std::string &enginePipe, int nextPieceIdx,
                                      const EngineTimingContext *timing = nullptr) {
  const auto started = std::chrono::steady_clock::now();

  if (nextPieceIdx < 0 || nextPieceIdx > 6) {
    return "Error: invalid next piece (expected 0-6 or I/O/L/J/T/S/Z).";
  }

  debug_pre::ParsedPrecomputePipe req;
  std::string parseError;
  if (!debug_pre::parsePrecomputeDebugPipe(enginePipe, req, parseError)) {
    return parseError;
  }
  if (req.pruningBreadth >= 0) {
    return "Error: /debug-tournament requires tournament mode (pruningBreadth == -1 or -2).";
  }

  const Piece *nextPiece = &PIECE_LIST[nextPieceIdx];
  const bool useDas = req.dasCharge >= 0;
  std::string eqTimelineStorage;
  char const *contextTimeline = req.inputFrameTimeline.c_str();
  if (useDas) {
    eqTimelineStorage = getDasEquivalentInputTimeline(req.inputFrameTimeline.c_str());
    contextTimeline = eqTimelineStorage.c_str();
  }
  const PieceRangeContext contexts[4] = {
      getPieceRangeContext(contextTimeline, 1, /* gravityDoubled= */ true),
      getPieceRangeContext(contextTimeline, 1, /* gravityDoubled= */ false),
      getPieceRangeContext(contextTimeline, 2, /* gravityDoubled= */ false),
      getPieceRangeContext(contextTimeline, 3, /* gravityDoubled= */ false),
  };
  const EvalContext evalContext = getEvalContext(req.gameState, contexts,
                                                 /* useTestWeights= */ false, req.aggressionLevel);
  auto holes2 = updateSurfaceAndHoles(req.gameState.surfaceArray, req.gameState.board,
                                      evalContext.countWellHoles ? -1 : evalContext.wellColumn,
                                      evalContext.aiMode == DIG);
  req.gameState.numTrueHoles = holes2.first;
  req.gameState.numPartialHoles = holes2.second;

  const FirstMoveSearchResult firstMoves = buildFirstMoveBranches(
      req.gameState, req.curPiece, useDas ? req.dasCharge : -1,
      useDas ? req.inputFrameTimeline.c_str() : contextTimeline, &evalContext);

  // Map first-piece keys to their branch metadata (dasAfter, post-ply-1 board).
  struct BranchMeta {
    int dasAfter = -1;
    std::string boardAfter;
  };
  std::unordered_map<std::string, BranchMeta> branchMeta;
  for (const FirstMoveBranch &b : firstMoves.branches) {
    std::string k = encodeLockPosition(
        {b.firstPlacement.x, b.firstPlacement.y, b.firstPlacement.rotationIndex});
    if (branchMeta.count(k) == 0) {
      branchMeta[k] = {b.dasChargeAfter, debug_gt::boardToBitString(b.afterFirstMove.board)};
    }
  }

  // Shallow DAS penalties when in DAS mode (eval-only, fast).
  DasLockValueMaps dasPenalties;
  if (useDas) {
    dasPenalties = getLockValueLookupDasMapsWithFirstMoves(
        firstMoves, nextPiece, /* pruningBreadth= */ 20, /* playoutCount= */ 0,
        /* playoutLength= */ 0, req.inputFrameTimeline.c_str(), &evalContext, contexts, nullptr);
  }

  std::vector<Possibility> possibilities;
  searchDepth2FromFirstMoves(firstMoves, nextPiece, /* secondPieceDasCharge= */ useDas ? 15 : -1,
                             useDas ? req.inputFrameTimeline.c_str() : contextTimeline,
                             &evalContext, possibilities);

  // Configure SearchBudget matching precompute.cpp's timed/fixed behavior.
  SearchBudget budget = SearchBudget::fromWire(req.pruningBreadth, req.playoutCount);
  if (budget.mode == BUDGET_TIME_MS) {
    const auto anchor =
        (timing != nullptr && timing->hasRequestReceived) ? timing->requestReceived : started;
    const double calRate = timing != nullptr ? timing->calibratedMovesPerMs7T : 0.0;
    budget = SearchBudget::fromDeadline(
        req.playoutCount, anchor + std::chrono::milliseconds(req.playoutCount), calRate);
    int reserveMs = std::min(15, std::max(0, req.playoutCount / 3));
    budget.deadline -= std::chrono::milliseconds(reserveMs);
  }

  TournamentTrace trace;
  getLockValueLookupTournamentFromPossibilities(possibilities, nextPiece, req.playoutLength, budget,
                                                &evalContext, contexts, &trace);

  const double elapsedMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  static const char *PIECE_NAMES[7] = {"I", "O", "L", "J", "T", "S", "Z"};
  std::ostringstream out;
  out << "{";
  out << "\"aiMode\":\"" << debug_gt::aiModeName(evalContext.aiMode) << "\",";
  out << "\"nextPiece\":\"" << PIECE_NAMES[nextPieceIdx] << "\",";
  out << "\"nextPieceIdx\":" << nextPieceIdx << ",";
  out << "\"useDas\":" << (useDas ? "true" : "false") << ",";
  out << "\"firstPieceDasCharge\":" << req.dasCharge << ",";
  out << "\"playoutLength\":" << req.playoutLength << ",";
  out << "\"pruningBreadth\":" << req.pruningBreadth << ",";
  out << "\"playoutCount\":" << req.playoutCount << ",";
  out << "\"elapsedMs\":" << debug_gt::fmtDouble(elapsedMs, 1) << ",";

  out << "\"usedEvalOnlyFallback\":" << (trace.usedEvalOnlyFallback ? "true" : "false") << ",";
  out << "\"isTimed\":" << (trace.isTimed ? "true" : "false") << ",";
  out << "\"useTieredR1\":" << (trace.useTieredR1 ? "true" : "false") << ",";
  out << "\"candidateCount\":" << trace.candidateCount << ",";
  out << "\"r1Entries\":" << trace.r1Entries << ",";
  out << "\"r2Entries\":" << trace.r2Entries << ",";
  out << "\"n1\":" << trace.n1 << ",";
  out << "\"n2\":" << trace.n2 << ",";
  out << "\"n3\":" << trace.n3 << ",";
  out << "\"survivorsCut1\":" << trace.survivorsCut1 << ",";
  out << "\"finalistsCut2\":" << trace.finalistsCut2 << ",";
  out << "\"samplesSoFar\":" << trace.samplesSoFar << ",";
  out << "\"r1Ms\":" << debug_gt::fmtDouble(trace.r1Ms, 2) << ",";
  out << "\"r2Ms\":" << debug_gt::fmtDouble(trace.r2Ms, 2) << ",";
  out << "\"r3Ms\":" << debug_gt::fmtDouble(trace.r3Ms, 2) << ",";
  out << "\"observedMovesPerMs\":" << debug_gt::fmtDouble(trace.observedMovesPerMs, 1) << ",";

  out << "\"candidates\":[";
  for (size_t i = 0; i < trace.candidates.size(); i++) {
    if (i > 0) {
      out << ",";
    }
    const TournamentCandidateTrace &c = trace.candidates[i];
    int rot = 0, x = 0, y = 0;
    parseLockKey(c.key, rot, x, y);
    const auto bmIt = branchMeta.find(c.key);
    const int dasAfter = bmIt != branchMeta.end() ? bmIt->second.dasAfter : -1;
    const std::string &boardAfter = bmIt != branchMeta.end() ? bmIt->second.boardAfter : "";
    const double partialPen = (useDas && dasPenalties.partialPenalty.count(c.key))
                                  ? dasPenalties.partialPenalty.at(c.key)
                                  : 0.0;
    const double minPen =
        (useDas && dasPenalties.minPenalty.count(c.key)) ? dasPenalties.minPenalty.at(c.key) : 0.0;

    out
        << "{\"key\":\""
        << debug_gt::jsonEscape(c.key)
        << "\""
        << ",\"rot\":"
        << rot
        << ",\"x\":"
        << x
        << ",\"y\":"
        << y
        << ",\"relRot\":"
        << rot
        << ",\"relX\":"
        << (x - INITIAL_X)
        << ",\"relY\":"
        << (y - req.curPiece->initialY)
        << ",\"dasAfter\":"
        << dasAfter
        << ",\"partialPenalty\":"
        << debug_gt::fmtDouble(partialPen, 2)
        << ",\"minPenalty\":"
        << debug_gt::fmtDouble(minPen, 2)
        << ",\"boardAfter\":\""
        << boardAfter
        << "\""
        << ",\"evalRank\":"
        << c.evalRank
        << ",\"r1Conts\":"
        << c.r1Conts
        << ",\"survivedCut1\":"
        << (c.survivedCut1 ? "true" : "false")
        << ",\"cut1Rank\":"
        << c.cut1Rank
        << ",\"cut1Score\":"
        << debug_gt::fmtDouble(c.cut1Score, 2)
        << ",\"survivedCut2\":"
        << (c.survivedCut2 ? "true" : "false")
        << ",\"cut2Rank\":"
        << c.cut2Rank
        << ",\"cut2Score\":"
        << debug_gt::fmtDouble(c.cut2Score, 2)
        << ",\"finalSamples\":"
        << c.finalSamples
        << ",\"publishedScore\":"
        << debug_gt::fmtDouble(c.publishedScore, 2)
        << ",\"bestRawMean\":"
        << debug_gt::fmtDouble(c.bestRawMean, 2)
        << ",\"bestSe\":"
        << debug_gt::fmtDouble(c.bestSe, 2)
        << ",\"bestContIdx\":"
        << c.bestContIdx
        << ",\"conts\":[";
    for (size_t j = 0; j < c.conts.size(); j++) {
      if (j > 0) {
        out << ",";
      }
      const TournamentContTrace &ct = c.conts[j];
      int cRot = 0, cX = 0, cY = 0;
      parseLockKey(ct.contKey, cRot, cX, cY);
      out
          << "{\"evalRank\":"
          << ct.evalRank
          << ",\"contKey\":\""
          << debug_gt::jsonEscape(ct.contKey)
          << "\""
          << ",\"rot\":"
          << cRot
          << ",\"x\":"
          << cX
          << ",\"y\":"
          << cY
          << ",\"relRot\":"
          << cRot
          << ",\"relX\":"
          << (cX - INITIAL_X)
          << ",\"relY\":"
          << (cY - nextPiece->initialY)
          << ",\"evalScore\":"
          << debug_gt::fmtDouble(ct.evalScoreInclReward, 2)
          << ",\"immediateReward\":"
          << debug_gt::fmtDouble(ct.immediateReward, 2)
          << ",\"samples\":"
          << ct.samples
          << ",\"rawMean\":"
          << debug_gt::fmtDouble(ct.rawMean, 2)
          << ",\"se\":"
          << debug_gt::fmtDouble(ct.se, 2)
          << ",\"lcbScore\":"
          << debug_gt::fmtDouble(ct.lcbScore, 2)
          << ",\"deathPct\":"
          << debug_gt::fmtDouble(ct.deathPct, 2)
          << ",\"isR3Cont\":"
          << (ct.isR3Cont ? "true" : "false")
          << "}";
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

} // namespace debug_tmt

#endif
