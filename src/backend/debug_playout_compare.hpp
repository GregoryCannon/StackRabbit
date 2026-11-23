#ifndef DEBUG_PLAYOUT_COMPARE_HPP
#define DEBUG_PLAYOUT_COMPARE_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Expects main.cpp, debug_ground_truth.hpp, and debug_tournament.hpp to be included first.

namespace debug_pc {

constexpr int DEFAULT_COMPARE_PLAYOUTS = 500;
constexpr int DEFAULT_COMPARE_PLAYOUT_LENGTH = 8;
constexpr float PARTIAL_HOLE_EPS = 0.01f;
constexpr double WIN_TIE_EPS = 1e-4;
constexpr int DEPTH_CURVE_POINTS[] = {0, 2, 4, 8, 12};

struct PlayoutStepTrace {
  int ply = 0;
  char pieceId = '?';
  const char *aiMode = "STD";
  bool died = false;
  int rot = 0;
  int x = 0;
  int y = 0;
  int relRot = 0;
  int relX = 0;
  int relY = 0;
  char tuckInput = '.';
  int cleared = 0;
  double stepReward = 0.0;
  int trueHoles = 0;
  double partialHoles = 0.0;
  double evalScore = 0.0;
  bool isLeaf = false;
  std::string expl;
  std::string boardBefore;
  std::string boardAfter;
};

struct DetailedPlayoutRun {
  int seqIdx = 0;
  double playoutScore = 0.0; // exact match to playSequence()
  double interReward = 0.0;
  double leafEval = 0.0;
  bool died = false;
  double deathValue = 0.0;
  int burns = 0;
  int tetrises = 0;
  int totalLines = 0;
  int digPlies = 0;
  int tuckPlies = 0;
  bool everCoveredWell = false;
  bool everInaccLeft = false;
  bool gotTetris = false;
  bool startedWithHole = false;
  bool holeClearedByTuck = false;
  bool holeClearedByLine = false;
  bool stillHasHoleAtEnd = false;
  std::string leafExpl;
  std::map<std::string, double> leafTerms;
  std::vector<PlayoutStepTrace> steps;
};

struct CompareSideInput {
  std::string firstKey;
  std::string secondKey;
  int firstRel[3] = {0, 0, 0};
  bool hasSecond = false;
  int secondRel[3] = {0, 0, 0};
  float immReward = 0.0f;
  float depth0Eval = 0.0f;
  std::string depth0Expl;
  GameState resultingState{};
};

struct SideCounters {
  int deaths = 0;
  int burns = 0;
  int tetrises = 0;
  int lines = 0;
  int tuckPlies = 0;
  int digPlies = 0;
  int everCoveredWell = 0;
  int everInaccLeft = 0;
  int gotTetris = 0;
  bool startedWithHole = false;
  int holeClearedByTuck = 0;
  int holeClearedByLine = 0;
  int stillHasHoleAtEnd = 0;
};

struct TermDelta {
  std::string name;
  double aMean = 0.0;
  double bMean = 0.0;
  double delta = 0.0;
};

struct DepthPoint {
  int depth = 0;
  double scoreA = 0.0;
  double scoreB = 0.0;
};

struct PairedAggregate {
  double meanA = 0.0;
  double meanB = 0.0;
  double meanDiff = 0.0;
  double pairedSe = 0.0;
  double sumInterA = 0.0;
  double sumInterB = 0.0;
  double sumLeafA = 0.0;
  double sumLeafB = 0.0;
  double sumDeathA = 0.0;
  double sumDeathB = 0.0;
  int bothAlive = 0;
  int onlyAAlive = 0;
  int onlyBAlive = 0;
  int bothDead = 0;
  int winsA = 0;
  int winsB = 0;
  int ties = 0;
  SideCounters cntA{};
  SideCounters cntB{};
  std::vector<std::pair<double, int>> sortedDiffs; // (scoreA - scoreB, seqIdx)
  std::vector<TermDelta> termDeltas;
};

inline bool hasHole(const GameState &gs) {
  return gs.numTrueHoles > 0 || gs.numPartialHoles > PARTIAL_HOLE_EPS;
}

inline bool hasInaccessibleLeft(const GameState &prevState, const GameState &nextState,
                                const EvalContext *evalContext) {
  const bool isKillscreenLineout = prevState.level >= 29 && evalContext->aiMode == LINEOUT;
  if (isKillscreenLineout
      || std::fabs(evalContext->weights.inaccessibleLeftCoef) <= FLOAT_EPSILON) {
    return false;
  }
  const float factor = getInaccessibleLeftFactor(
      nextState.board, nextState.surfaceArray,
      evalContext->pieceRangeContext.maxAccessibleLeft5Surface,
      evalContext->pieceRangeContext.maxAccessibleLeftLPieceSurface, evalContext->wellColumn,
      evalContext->aiMode, nextState.numTrueHoles > 0);
  return std::fabs(factor) > FLOAT_EPSILON;
}

inline std::map<std::string, double> parseFastEvalExpl(const std::string &expl) {
  std::map<std::string, double> terms;
  size_t pos = 0;
  while (pos < expl.size()) {
    size_t semi = expl.find(';', pos);
    std::string tok = expl.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
    size_t eq = tok.find('=');
    if (eq != std::string::npos) {
      std::string key = tok.substr(0, eq);
      try {
        terms[key] += std::stod(tok.substr(eq + 1));
      } catch (...) {
        // Skip non-numeric diagnostic annotations in fastEval explanations (e.g. labels).
      }
    }
    if (semi == std::string::npos) {
      break;
    }
    pos = semi + 1;
  }
  return terms;
}

inline const char *shortAiMode(AiMode mode) {
  switch (mode) {
  case STANDARD:
    return "STD";
  case DIG:
    return "DIG";
  case LINEOUT:
    return "LO";
  default:
    return "?";
  }
}

inline PlayoutStepTrace makeStepTrace(int ply, const Piece *piece, AiMode aiMode,
                                      const LockPlacement &move, int cleared, double stepReward,
                                      const GameState &beforeState, const GameState &afterState,
                                      double evalScore, bool isLeaf, std::string expl) {
  PlayoutStepTrace st;
  st.ply = ply;
  st.pieceId = piece->id;
  st.aiMode = shortAiMode(aiMode);
  st.rot = move.rotationIndex;
  st.x = move.x;
  st.y = move.y;
  st.relRot = move.rotationIndex;
  st.relX = move.x - INITIAL_X;
  st.relY = move.y - piece->initialY;
  st.tuckInput = move.tuckInput;
  st.cleared = cleared;
  st.stepReward = stepReward;
  st.trueHoles = afterState.numTrueHoles;
  st.partialHoles = afterState.numPartialHoles;
  st.evalScore = evalScore;
  st.isLeaf = isLeaf;
  st.expl = std::move(expl);
  st.boardBefore = debug_gt::boardToBitString(beforeState.board);
  st.boardAfter = debug_gt::boardToBitString(afterState.board);
  return st;
}

// Mirrors playSequence() in src/backend/playout.cpp line-for-line so total scores match
// getPlayoutScore() exactly while collecting per-factor and behavioral diagnostics.
inline DetailedPlayoutRun playSequenceDetailed(GameState gameState,
                                               const PieceRangeContext pieceRangeContextLookup[3],
                                               const int pieceSequence[SEQUENCE_LENGTH], int seqIdx,
                                               int playoutLength, AggressionLevel aggressionLevel,
                                               AiMode originalAiMode, bool recordSteps) {
  DetailedPlayoutRun out;
  out.seqIdx = seqIdx;
  out.startedWithHole = hasHole(gameState);
  bool holeCleared = !out.startedWithHole;

  if (originalAiMode != LINEOUT && getAiMode(gameState, pieceRangeContextLookup) == LINEOUT) {
    GameState preKillscreenState = gameState;
    preKillscreenState.level = 28;
    preKillscreenState.lines = std::min(gameState.lines, 226);
    EvalContext contextRaw = getEvalContext(preKillscreenState, pieceRangeContextLookup,
                                            /* useTestWeights= */ false, aggressionLevel);
    contextRaw.aiMode = originalAiMode;
    contextRaw.weights = getWeights(originalAiMode, /* useTestWeights= */ false, aggressionLevel);
    contextRaw.shouldRewardLineClears = false;
    std::string expl;
    float evalScore = fastEval(gameState, gameState, NO_PLACEMENT, &contextRaw, &expl);
    out.leafEval = evalScore;
    out.playoutScore = evalScore;
    out.leafExpl = expl;
    out.leafTerms = parseFastEvalExpl(expl);
    out.stillHasHoleAtEnd = hasHole(gameState);
    return out;
  }

  float totalReward = 0.0f;
  for (int i = 0; i < playoutLength; i++) {
    const EvalContext evalContextRaw = getEvalContext(gameState, pieceRangeContextLookup,
                                                      /* useTestWeights= */ false, aggressionLevel);
    const EvalContext *evalContext = &evalContextRaw;
    FastEvalWeights weights =
        getWeights(evalContext->aiMode, /* useTestWeights= */ false, aggressionLevel);
    if (evalContext->aiMode == DIG) {
      out.digPlies++;
    }

    std::vector<LockPlacement> lockPlacements;
    const Piece *piece = &PIECE_LIST[pieceSequence[i]];
    fastMoveSearch(gameState, piece, evalContext->pieceRangeContext.inputFrameTimeline,
                   lockPlacements);

    if (lockPlacements.empty()) {
      out.died = true;
      out.deathValue = weights.deathCoef;
      out.interReward = 0.0; // playout.cpp returns weights.deathCoef directly on topout
      out.playoutScore = weights.deathCoef;
      out.stillHasHoleAtEnd = hasHole(gameState);
      if (recordSteps) {
        PlayoutStepTrace st;
        st.ply = i;
        st.pieceId = piece->id;
        st.aiMode = shortAiMode(evalContext->aiMode);
        st.died = true;
        st.evalScore = weights.deathCoef;
        st.boardBefore = debug_gt::boardToBitString(gameState.board);
        st.boardAfter = st.boardBefore;
        out.steps.push_back(std::move(st));
      }
      return out;
    }

    LockPlacement bestMove = pickLockPlacement(gameState, evalContext, lockPlacements);
    GameState nextState = advanceGameState(gameState, bestMove, evalContext);
    const int cleared = nextState.lines - gameState.lines;

    out.totalLines += cleared;
    if (cleared == 4) {
      out.tetrises++;
      out.gotTetris = true;
    } else if (cleared > 0) {
      out.burns += cleared;
    }
    if (bestMove.tuckInput != '.') {
      out.tuckPlies++;
    }
    if (!holeCleared && !hasHole(nextState)) {
      holeCleared = true;
      if (bestMove.tuckInput != '.') {
        out.holeClearedByTuck = true;
      } else if (cleared > 0) {
        out.holeClearedByLine = true;
      }
    }
    if (evalContext->wellColumn >= 0 && nextState.surfaceArray[evalContext->wellColumn] > 0) {
      out.everCoveredWell = true;
    }

    const bool reachedKillscreenLineout =
        originalAiMode != LINEOUT && getAiMode(nextState, pieceRangeContextLookup) == LINEOUT;
    const bool isLeaf = (i == playoutLength - 1 || reachedKillscreenLineout);

    if (isLeaf) {
      EvalContext contextRaw = *evalContext;
      if (contextRaw.aiMode != originalAiMode) {
        contextRaw.aiMode = originalAiMode;
        contextRaw.weights =
            getWeights(originalAiMode, /* useTestWeights= */ false, aggressionLevel);
        contextRaw.shouldRewardLineClears = (originalAiMode == LINEOUT);
        nextState = advanceGameState(gameState, bestMove, &contextRaw);
      }
      std::string expl;
      float evalScore = fastEval(gameState, nextState, bestMove, &contextRaw, &expl);
      if (!out.everInaccLeft && hasInaccessibleLeft(gameState, nextState, &contextRaw)) {
        out.everInaccLeft = true;
      }
      out.interReward = totalReward;
      out.leafEval = evalScore;
      out.playoutScore = totalReward + evalScore;
      out.leafExpl = expl;
      out.leafTerms = parseFastEvalExpl(expl);
      out.stillHasHoleAtEnd = hasHole(nextState);
      if (recordSteps) {
        // Record the mode used by pickLockPlacement for this ply; leaf eval uses originalAiMode.
        out.steps.push_back(makeStepTrace(i, piece, evalContext->aiMode, bestMove, cleared,
                                          /* stepReward= */ 0.0, gameState, nextState, evalScore,
                                          /* isLeaf= */ true, std::move(expl)));
      }
      return out;
    }

    if (!out.everInaccLeft && hasInaccessibleLeft(gameState, nextState, evalContext)) {
      out.everInaccLeft = true;
    }

    int oldLines = gameState.lines;
    FastEvalWeights rewardWeights =
        getWeights(originalAiMode, /* useTestWeights= */ false, aggressionLevel);
    int shouldRewardClears = (originalAiMode == LINEOUT);
    float stepReward = getLineClearFactor(nextState.lines - oldLines, rewardWeights,
                                          shouldRewardClears, oldLines, evalContext->lineoutAt29);
    totalReward += stepReward;

    if (recordSteps) {
      std::string expl;
      float stepEv = fastEval(gameState, nextState, bestMove, evalContext, &expl);
      out.steps.push_back(makeStepTrace(i, piece, evalContext->aiMode, bestMove, cleared,
                                        stepReward, gameState, nextState, stepEv,
                                        /* isLeaf= */ false, std::move(expl)));
    }
    gameState = nextState;
  }
  return out;
}

inline bool parsePairSpec(const std::string &spec, std::string &firstKey, std::string &secondKey) {
  size_t semi = spec.find(';');
  if (semi == std::string::npos) {
    return false;
  }
  firstKey = spec.substr(0, semi);
  secondKey = spec.substr(semi + 1);
  int r = 0, x = 0, y = 0;
  return debug_tmt::parseLockKey(firstKey, r, x, y) && debug_tmt::parseLockKey(secondKey, r, x, y);
}

inline bool resolveCompareSidesNnb(const debug_gt::ParsedDebugPipe &req,
                                   const EvalContext &evalContext, bool useDas,
                                   char const *contextTimeline, const std::string &aFirst,
                                   const std::string &bFirst, CompareSideInput &sideA,
                                   CompareSideInput &sideB, std::string &errorOut) {
  const FirstMoveSearchResult allFirst = buildFirstMoveBranches(
      req.gameState, req.curPiece, useDas ? req.dasCharge : -1,
      useDas ? req.inputFrameTimeline.c_str() : contextTimeline, &evalContext);
  const FirstMoveBranch *bA = nullptr;
  const FirstMoveBranch *bB = nullptr;
  for (const FirstMoveBranch &b : allFirst.branches) {
    std::string fk = encodeLockPosition(
        {b.firstPlacement.x, b.firstPlacement.y, b.firstPlacement.rotationIndex});
    if (fk == aFirst && (bA == nullptr || b.firstMoveEval > bA->firstMoveEval)) {
      bA = &b;
    }
    if (fk == bFirst && (bB == nullptr || b.firstMoveEval > bB->firstMoveEval)) {
      bB = &b;
    }
  }
  if (bA == nullptr) {
    errorOut = "Error: move A (" + aFirst + ") is not reachable for this board.";
    return false;
  }
  if (bB == nullptr) {
    errorOut = "Error: move B (" + bFirst + ") is not reachable for this board.";
    return false;
  }
  auto fillSide = [&](CompareSideInput &s, const FirstMoveBranch *b, const std::string &fk) {
    s.firstKey = fk;
    s.firstRel[0] = b->firstPlacement.rotationIndex;
    s.firstRel[1] = b->firstPlacement.x - INITIAL_X;
    s.firstRel[2] = b->firstPlacement.y - req.curPiece->initialY;
    s.hasSecond = false;
    s.immReward = b->firstMoveReward;
    s.resultingState = b->afterFirstMove;
    std::string expl;
    // fastEval already includes lineClearFactor (matching buildFirstMoveBranches).
    s.depth0Eval =
        fastEval(req.gameState, b->afterFirstMove, b->firstPlacement, &evalContext, &expl);
    s.depth0Expl = std::move(expl);
  };
  fillSide(sideA, bA, aFirst);
  fillSide(sideB, bB, bFirst);
  return true;
}

inline bool resolveCompareSides2Ply(const debug_gt::ParsedDebugPipe &req,
                                    const EvalContext &evalContext, const Piece *nextPiece,
                                    bool useDas, char const *contextTimeline,
                                    const std::string &specA, const std::string &specB,
                                    const std::string &aFirst, const std::string &aSecond,
                                    const std::string &bFirst, const std::string &bSecond,
                                    CompareSideInput &sideA, CompareSideInput &sideB,
                                    std::string &errorOut) {
  std::vector<Possibility> poss;
  searchDepth2(req.gameState, req.curPiece, nextPiece, useDas ? req.dasCharge : -1,
               useDas ? 15 : -1, useDas ? req.inputFrameTimeline.c_str() : contextTimeline,
               &evalContext, poss, /* generateExplanation= */ true);

  const Possibility *pA = nullptr;
  const Possibility *pB = nullptr;
  for (const Possibility &p : poss) {
    std::string fk = encodeLockPosition(p.firstPlacement);
    std::string sk = encodeLockPosition(p.secondPlacement);
    if (fk == aFirst
        && sk == aSecond
        && (pA == nullptr || p.evalScoreInclReward > pA->evalScoreInclReward)) {
      pA = &p;
    }
    if (fk == bFirst
        && sk == bSecond
        && (pB == nullptr || p.evalScoreInclReward > pB->evalScoreInclReward)) {
      pB = &p;
    }
  }
  if (pA == nullptr) {
    errorOut = "Error: pair A (" + specA + ") is not reachable for this board and next piece.";
    return false;
  }
  if (pB == nullptr) {
    errorOut = "Error: pair B (" + specB + ") is not reachable for this board and next piece.";
    return false;
  }
  auto fillSide = [&](CompareSideInput &s, const Possibility *p, const std::string &fk,
                      const std::string &sk) {
    s.firstKey = fk;
    s.secondKey = sk;
    s.firstRel[0] = p->firstPlacement.rotationIndex;
    s.firstRel[1] = p->firstPlacement.x - INITIAL_X;
    s.firstRel[2] = p->firstPlacement.y - req.curPiece->initialY;
    s.hasSecond = true;
    s.secondRel[0] = p->secondPlacement.rotationIndex;
    s.secondRel[1] = p->secondPlacement.x - INITIAL_X;
    s.secondRel[2] = p->secondPlacement.y - nextPiece->initialY;
    s.immReward = p->immediateReward;
    s.depth0Eval = p->evalScoreInclReward;
    s.depth0Expl = p->evalExplanation;
    s.resultingState = p->resultingState;
  };
  fillSide(sideA, pA, aFirst, aSecond);
  fillSide(sideB, pB, bFirst, bSecond);
  return true;
}

inline std::vector<int> selectCanonicalSequenceIndices(const int *seqBase, int playoutCount,
                                                       int filterFirstPieceIdx) {
  std::vector<int> indices;
  indices.reserve(playoutCount);
  if (filterFirstPieceIdx < 0) {
    for (int i = 0; i < playoutCount; i++) {
      indices.push_back(i);
    }
    return indices;
  }
  for (int i = 0;
       i < CANONICAL_SEQUENCE_BATCH_SIZE && static_cast<int>(indices.size()) < playoutCount; i++) {
    if (seqBase[i * SEQUENCE_LENGTH] == filterFirstPieceIdx) {
      indices.push_back(i);
    }
  }
  return indices;
}

inline PairedAggregate
aggregatePairedPlayouts(const CompareSideInput &sideA, const CompareSideInput &sideB,
                        const PieceRangeContext contexts[4], const EvalContext &evalContext,
                        const int *seqBase, const std::vector<int> &seqIndices, int playoutLength) {
  PairedAggregate agg;
  const int effectiveN = static_cast<int>(seqIndices.size());
  agg.sortedDiffs.reserve(effectiveN);
  agg.cntA.startedWithHole = hasHole(sideA.resultingState);
  agg.cntB.startedWithHole = hasHole(sideB.resultingState);

  std::set<std::string> allTermNames;
  std::map<std::string, double> termSumA;
  std::map<std::string, double> termSumB;
  double sumTotalA = 0.0;
  double sumTotalB = 0.0;

  auto accumSide = [](const DetailedPlayoutRun &r, SideCounters &cnt) {
    if (r.died) {
      cnt.deaths++;
    }
    cnt.burns += r.burns;
    cnt.tetrises += r.tetrises;
    cnt.lines += r.totalLines;
    cnt.tuckPlies += r.tuckPlies;
    cnt.digPlies += r.digPlies;
    if (r.everCoveredWell) {
      cnt.everCoveredWell++;
    }
    if (r.everInaccLeft) {
      cnt.everInaccLeft++;
    }
    if (r.gotTetris) {
      cnt.gotTetris++;
    }
    if (r.holeClearedByTuck) {
      cnt.holeClearedByTuck++;
    }
    if (r.holeClearedByLine) {
      cnt.holeClearedByLine++;
    }
    if (r.stillHasHoleAtEnd) {
      cnt.stillHasHoleAtEnd++;
    }
  };

  for (int seqIdx : seqIndices) {
    const int *seq = seqBase + seqIdx * SEQUENCE_LENGTH;
    DetailedPlayoutRun rA = playSequenceDetailed(sideA.resultingState, contexts, seq, seqIdx,
                                                 playoutLength, evalContext.aggressionLevel,
                                                 evalContext.aiMode, /* recordSteps= */ false);
    DetailedPlayoutRun rB = playSequenceDetailed(sideB.resultingState, contexts, seq, seqIdx,
                                                 playoutLength, evalContext.aggressionLevel,
                                                 evalContext.aiMode, /* recordSteps= */ false);

    const double sA = sideA.immReward + rA.playoutScore;
    const double sB = sideB.immReward + rB.playoutScore;
    sumTotalA += sA;
    sumTotalB += sB;
    agg.sumInterA += rA.interReward;
    agg.sumInterB += rB.interReward;
    agg.sumLeafA += rA.leafEval;
    agg.sumLeafB += rB.leafEval;
    agg.sumDeathA += rA.deathValue;
    agg.sumDeathB += rB.deathValue;

    const double diff = sA - sB;
    agg.sortedDiffs.push_back({diff, seqIdx});
    if (diff > WIN_TIE_EPS) {
      agg.winsA++;
    } else if (diff < -WIN_TIE_EPS) {
      agg.winsB++;
    } else {
      agg.ties++;
    }

    if (!rA.died && !rB.died) {
      agg.bothAlive++;
    } else if (!rA.died && rB.died) {
      agg.onlyAAlive++;
    } else if (rA.died && !rB.died) {
      agg.onlyBAlive++;
    } else {
      agg.bothDead++;
    }

    accumSide(rA, agg.cntA);
    accumSide(rB, agg.cntB);

    for (const auto &kv : rA.leafTerms) {
      termSumA[kv.first] += kv.second;
      allTermNames.insert(kv.first);
    }
    for (const auto &kv : rB.leafTerms) {
      termSumB[kv.first] += kv.second;
      allTermNames.insert(kv.first);
    }
  }

  agg.meanA = sumTotalA / effectiveN;
  agg.meanB = sumTotalB / effectiveN;
  agg.meanDiff = agg.meanA - agg.meanB;
  double sumDiffSq = 0.0;
  for (const auto &d : agg.sortedDiffs) {
    const double err = d.first - agg.meanDiff;
    sumDiffSq += err * err;
  }
  agg.pairedSe = effectiveN > 1 ? std::sqrt(sumDiffSq / (effectiveN - 1) / effectiveN) : 0.0;
  std::sort(agg.sortedDiffs.begin(), agg.sortedDiffs.end());

  for (const std::string &name : allTermNames) {
    const double aMean = termSumA[name] / effectiveN;
    const double bMean = termSumB[name] / effectiveN;
    agg.termDeltas.push_back({name, aMean, bMean, aMean - bMean});
  }
  std::sort(agg.termDeltas.begin(), agg.termDeltas.end(),
            [](const TermDelta &x, const TermDelta &y) {
              return std::fabs(x.delta) > std::fabs(y.delta);
            });
  return agg;
}

inline std::vector<DepthPoint> computeDepthCurve(const CompareSideInput &sideA,
                                                 const CompareSideInput &sideB,
                                                 const PieceRangeContext contexts[4],
                                                 const EvalContext &evalContext, const int *seqBase,
                                                 const std::vector<int> &seqIndices,
                                                 int playoutLength, double meanA, double meanB) {
  std::set<int> depthSet(std::begin(DEPTH_CURVE_POINTS), std::end(DEPTH_CURVE_POINTS));
  depthSet.insert(playoutLength);

  const int effectiveN = static_cast<int>(seqIndices.size());
  std::vector<DepthPoint> curve;
  curve.reserve(depthSet.size());
  for (int depth : depthSet) {
    if (depth == 0) {
      curve.push_back({0, sideA.depth0Eval, sideB.depth0Eval});
    } else if (depth == playoutLength) {
      curve.push_back({depth, meanA, meanB});
    } else {
      double sumA = 0.0;
      double sumB = 0.0;
      for (int seqIdx : seqIndices) {
        const int *seq = seqBase + seqIdx * SEQUENCE_LENGTH;
        sumA += playSequence(sideA.resultingState, contexts, seq, depth,
                             evalContext.aggressionLevel, evalContext.aiMode, nullptr);
        sumB += playSequence(sideB.resultingState, contexts, seq, depth,
                             evalContext.aggressionLevel, evalContext.aiMode, nullptr);
      }
      curve.push_back(
          {depth, sideA.immReward + sumA / effectiveN, sideB.immReward + sumB / effectiveN});
    }
  }
  return curve;
}

inline void writeSideInfoJson(std::ostringstream &os, const CompareSideInput &s, double mean,
                              const SideCounters &cnt, int effectiveN) {
  const double denom = static_cast<double>(effectiveN);
  os
      << "{\"firstKey\":\""
      << debug_gt::jsonEscape(s.firstKey)
      << "\""
      << ",\"secondKey\":\""
      << debug_gt::jsonEscape(s.secondKey)
      << "\""
      << ",\"firstRel\":["
      << s.firstRel[0]
      << ","
      << s.firstRel[1]
      << ","
      << s.firstRel[2]
      << "]";
  if (s.hasSecond) {
    os
        << ",\"secondRel\":["
        << s.secondRel[0]
        << ","
        << s.secondRel[1]
        << ","
        << s.secondRel[2]
        << "]";
  } else {
    os << ",\"secondRel\":null";
  }
  os
      << ",\"immReward\":"
      << debug_gt::fmtDouble(s.immReward, 2)
      << ",\"depth0Eval\":"
      << debug_gt::fmtDouble(s.depth0Eval, 2)
      << ",\"depth0Expl\":\""
      << debug_gt::jsonEscape(s.depth0Expl)
      << "\""
      << ",\"boardAfter\":\""
      << debug_gt::boardToBitString(s.resultingState.board)
      << "\""
      << ",\"trueHoles\":"
      << s.resultingState.numTrueHoles
      << ",\"partialHoles\":"
      << debug_gt::fmtDouble(s.resultingState.numPartialHoles, 2)
      << ",\"mean\":"
      << debug_gt::fmtDouble(mean, 2)
      << ",\"deathPct\":"
      << debug_gt::fmtDouble(100.0 * static_cast<double>(cnt.deaths) / denom, 2)
      << ",\"avgBurns\":"
      << debug_gt::fmtDouble(static_cast<double>(cnt.burns) / denom, 2)
      << ",\"avgTetrises\":"
      << debug_gt::fmtDouble(static_cast<double>(cnt.tetrises) / denom, 2)
      << ",\"avgLines\":"
      << debug_gt::fmtDouble(static_cast<double>(cnt.lines) / denom, 2)
      << ",\"avgTuckPlies\":"
      << debug_gt::fmtDouble(static_cast<double>(cnt.tuckPlies) / denom, 2)
      << ",\"avgDigPlies\":"
      << debug_gt::fmtDouble(static_cast<double>(cnt.digPlies) / denom, 2)
      << ",\"everCoveredWellPct\":"
      << debug_gt::fmtDouble(100.0 * static_cast<double>(cnt.everCoveredWell) / denom, 1)
      << ",\"everInaccLeftPct\":"
      << debug_gt::fmtDouble(100.0 * static_cast<double>(cnt.everInaccLeft) / denom, 1)
      << ",\"gotTetrisPct\":"
      << debug_gt::fmtDouble(100.0 * static_cast<double>(cnt.gotTetris) / denom, 1)
      << ",\"startedWithHole\":"
      << (cnt.startedWithHole ? "true" : "false")
      << ",\"holeClearedByTuckPct\":"
      << debug_gt::fmtDouble(100.0 * static_cast<double>(cnt.holeClearedByTuck) / denom, 1)
      << ",\"holeClearedByLinePct\":"
      << debug_gt::fmtDouble(100.0 * static_cast<double>(cnt.holeClearedByLine) / denom, 1)
      << ",\"stillHasHoleAtEndPct\":"
      << debug_gt::fmtDouble(100.0 * static_cast<double>(cnt.stillHasHoleAtEnd) / denom, 1)
      << "}";
}

inline void writeStepsJson(std::ostringstream &os, const std::vector<PlayoutStepTrace> &steps) {
  os << "[";
  for (size_t k = 0; k < steps.size(); k++) {
    if (k > 0) {
      os << ",";
    }
    const PlayoutStepTrace &st = steps[k];
    os
        << "{\"ply\":"
        << st.ply
        << ",\"piece\":\""
        << st.pieceId
        << "\""
        << ",\"aiMode\":\""
        << st.aiMode
        << "\""
        << ",\"died\":"
        << (st.died ? "true" : "false")
        << ",\"rot\":"
        << st.rot
        << ",\"x\":"
        << st.x
        << ",\"y\":"
        << st.y
        << ",\"rel\":["
        << st.relRot
        << ","
        << st.relX
        << ","
        << st.relY
        << "]"
        << ",\"tuckInput\":\""
        << st.tuckInput
        << "\""
        << ",\"cleared\":"
        << st.cleared
        << ",\"stepReward\":"
        << debug_gt::fmtDouble(st.stepReward, 1)
        << ",\"trueHoles\":"
        << st.trueHoles
        << ",\"partialHoles\":"
        << debug_gt::fmtDouble(st.partialHoles, 2)
        << ",\"evalScore\":"
        << debug_gt::fmtDouble(st.evalScore, 1)
        << ",\"isLeaf\":"
        << (st.isLeaf ? "true" : "false")
        << ",\"expl\":\""
        << debug_gt::jsonEscape(st.expl)
        << "\""
        << ",\"boardBefore\":\""
        << st.boardBefore
        << "\""
        << ",\"boardAfter\":\""
        << st.boardAfter
        << "\"}";
  }
  os << "]";
}

inline std::string
serializePlayoutCompareJson(bool isNnb, int nextPieceIdx, int filterFirstPieceIdx, int effectiveN,
                            int requestedPlayouts, int playoutLength, double elapsedMs,
                            const EvalContext &evalContext, const PieceRangeContext contexts[4],
                            const int *seqBase, const CompareSideInput &sideA,
                            const CompareSideInput &sideB, const PairedAggregate &agg,
                            const std::vector<DepthPoint> &depthCurve) {
  auto quantileVal = [&](int pct) -> double {
    int idx = std::min(effectiveN - 1, std::max(0, (effectiveN * pct) / 100));
    return agg.sortedDiffs[idx].first;
  };
  auto quantileSeq = [&](int pct) -> int {
    int idx = std::min(effectiveN - 1, std::max(0, (effectiveN * pct) / 100));
    return agg.sortedDiffs[idx].second;
  };

  // Up to 4 representative sequences (deduplicated when N is small).
  struct TracePick {
    const char *label;
    int pct;
  };
  const TracePick picks[4] = {
      {"p90 (best for A)", 90},
      {"p70", 70},
      {"p50 (median)", 50},
      {"p10 (best for B)", 10},
  };
  std::vector<std::pair<std::string, int>> selectedTraces;
  std::set<int> usedSeqs;
  for (const auto &pk : picks) {
    int sIdx = quantileSeq(pk.pct);
    if (usedSeqs.insert(sIdx).second) {
      selectedTraces.push_back({pk.label, sIdx});
    }
  }

  const double denom = static_cast<double>(effectiveN);
  std::ostringstream out;
  out << "{";
  out << "\"aiMode\":\"" << debug_gt::aiModeName(evalContext.aiMode) << "\",";
  out << "\"isNoNextBox\":" << (isNnb ? "true" : "false") << ",";
  if (isNnb) {
    out << "\"nextPiece\":\"NNB\",";
  } else {
    out << "\"nextPiece\":\"" << PIECE_LIST[nextPieceIdx].id << "\",";
  }
  if (filterFirstPieceIdx >= 0) {
    out << "\"filterFirstPiece\":\"" << PIECE_LIST[filterFirstPieceIdx].id << "\",";
  } else {
    out << "\"filterFirstPiece\":null,";
  }
  out << "\"playoutCount\":" << effectiveN << ",";
  out << "\"requestedPlayoutCount\":" << requestedPlayouts << ",";
  out << "\"playoutLength\":" << playoutLength << ",";
  out << "\"elapsedMs\":" << debug_gt::fmtDouble(elapsedMs, 1) << ",";

  out << "\"sideA\":";
  writeSideInfoJson(out, sideA, agg.meanA, agg.cntA, effectiveN);
  out << ",\"sideB\":";
  writeSideInfoJson(out, sideB, agg.meanB, agg.cntB, effectiveN);

  out << ",\"meanDiff\":" << debug_gt::fmtDouble(agg.meanDiff, 2);
  out << ",\"pairedSe\":" << debug_gt::fmtDouble(agg.pairedSe, 2);
  out << ",\"winsA\":" << agg.winsA << ",\"winsB\":" << agg.winsB << ",\"ties\":" << agg.ties;
  out
      << ",\"survival\":{\"bothAlive\":"
      << agg.bothAlive
      << ",\"onlyAAlive\":"
      << agg.onlyAAlive
      << ",\"onlyBAlive\":"
      << agg.onlyBAlive
      << ",\"bothDead\":"
      << agg.bothDead
      << "}";
  out
      << ",\"quantiles\":{\"p10\":"
      << debug_gt::fmtDouble(quantileVal(10), 1)
      << ",\"p25\":"
      << debug_gt::fmtDouble(quantileVal(25), 1)
      << ",\"p50\":"
      << debug_gt::fmtDouble(quantileVal(50), 1)
      << ",\"p75\":"
      << debug_gt::fmtDouble(quantileVal(75), 1)
      << ",\"p90\":"
      << debug_gt::fmtDouble(quantileVal(90), 1)
      << "}";

  out
      << ",\"topDeltas\":["
      << "{\"name\":\""
      << (isNnb ? "ply1 immediateReward" : "ply1-2 immediateReward")
      << "\",\"aMean\":"
      << debug_gt::fmtDouble(sideA.immReward, 2)
      << ",\"bMean\":"
      << debug_gt::fmtDouble(sideB.immReward, 2)
      << ",\"delta\":"
      << debug_gt::fmtDouble(sideA.immReward - sideB.immReward, 2)
      << "},"
      << "{\"name\":\"intermediate playout rewards\",\"aMean\":"
      << debug_gt::fmtDouble(agg.sumInterA / denom, 2)
      << ",\"bMean\":"
      << debug_gt::fmtDouble(agg.sumInterB / denom, 2)
      << ",\"delta\":"
      << debug_gt::fmtDouble((agg.sumInterA - agg.sumInterB) / denom, 2)
      << "},"
      << "{\"name\":\"death penalty\",\"aMean\":"
      << debug_gt::fmtDouble(agg.sumDeathA / denom, 2)
      << ",\"bMean\":"
      << debug_gt::fmtDouble(agg.sumDeathB / denom, 2)
      << ",\"delta\":"
      << debug_gt::fmtDouble((agg.sumDeathA - agg.sumDeathB) / denom, 2)
      << "},"
      << "{\"name\":\"leaf fastEval (total)\",\"aMean\":"
      << debug_gt::fmtDouble(agg.sumLeafA / denom, 2)
      << ",\"bMean\":"
      << debug_gt::fmtDouble(agg.sumLeafB / denom, 2)
      << ",\"delta\":"
      << debug_gt::fmtDouble((agg.sumLeafA - agg.sumLeafB) / denom, 2)
      << "}]";

  out << ",\"leafTermDeltas\":[";
  for (size_t i = 0; i < agg.termDeltas.size(); i++) {
    if (i > 0) {
      out << ",";
    }
    out
        << "{\"name\":\""
        << debug_gt::jsonEscape(agg.termDeltas[i].name)
        << "\""
        << ",\"aMean\":"
        << debug_gt::fmtDouble(agg.termDeltas[i].aMean, 2)
        << ",\"bMean\":"
        << debug_gt::fmtDouble(agg.termDeltas[i].bMean, 2)
        << ",\"delta\":"
        << debug_gt::fmtDouble(agg.termDeltas[i].delta, 2)
        << "}";
  }
  out << "]";

  out << ",\"depthCurve\":[";
  for (size_t i = 0; i < depthCurve.size(); i++) {
    if (i > 0) {
      out << ",";
    }
    out
        << "{\"depth\":"
        << depthCurve[i].depth
        << ",\"scoreA\":"
        << debug_gt::fmtDouble(depthCurve[i].scoreA, 2)
        << ",\"scoreB\":"
        << debug_gt::fmtDouble(depthCurve[i].scoreB, 2)
        << ",\"diff\":"
        << debug_gt::fmtDouble(depthCurve[i].scoreA - depthCurve[i].scoreB, 2)
        << "}";
  }
  out << "]";

  out << ",\"traces\":[";
  for (size_t i = 0; i < selectedTraces.size(); i++) {
    if (i > 0) {
      out << ",";
    }
    int sIdx = selectedTraces[i].second;
    const int *seq = seqBase + sIdx * SEQUENCE_LENGTH;
    DetailedPlayoutRun tA = playSequenceDetailed(sideA.resultingState, contexts, seq, sIdx,
                                                 playoutLength, evalContext.aggressionLevel,
                                                 evalContext.aiMode, /* recordSteps= */ true);
    DetailedPlayoutRun tB = playSequenceDetailed(sideB.resultingState, contexts, seq, sIdx,
                                                 playoutLength, evalContext.aggressionLevel,
                                                 evalContext.aiMode, /* recordSteps= */ true);
    const double sA = sideA.immReward + tA.playoutScore;
    const double sB = sideB.immReward + tB.playoutScore;
    out
        << "{\"label\":\""
        << debug_gt::jsonEscape(selectedTraces[i].first)
        << "\""
        << ",\"seqIdx\":"
        << sIdx
        << ",\"scoreA\":"
        << debug_gt::fmtDouble(sA, 2)
        << ",\"scoreB\":"
        << debug_gt::fmtDouble(sB, 2)
        << ",\"diff\":"
        << debug_gt::fmtDouble(sA - sB, 2)
        << ",\"stepsA\":";
    writeStepsJson(out, tA.steps);
    out << ",\"stepsB\":";
    writeStepsJson(out, tB.steps);
    out << "}";
  }
  out << "]}";
  return out.str();
}

// Compares two post-ply-2 (firstKey;secondKey) pairs — or, when nextPieceIdx == NEXT_PIECE_NNB
// (-1), two 1-ply first-piece placements (firstKey) — across N paired canonical playouts,
// returning an additive eval-factor delta breakdown, operational counters, quantiles, depth curve,
// and up to 4 representative side-by-side sequence traces (deduplicated) as JSON.
// In NNB mode, filterFirstPieceIdx (0-6) optionally restricts canonical sequences to those whose
// first random playout piece matches filterFirstPieceIdx (scanning the full canonical batch up to
// playoutCount matches).
inline std::string runDebugPlayoutCompare(const std::string &enginePipe, int nextPieceIdx,
                                          const std::string &specA, const std::string &specB,
                                          int playoutCount, int filterFirstPieceIdx = -1) {
  const auto started = std::chrono::steady_clock::now();

  const bool isNnb = (nextPieceIdx == debug_tmt::NEXT_PIECE_NNB);
  if (!isNnb && (nextPieceIdx < 0 || nextPieceIdx > 6)) {
    return "Error: invalid next piece (expected 0-6, I/O/L/J/T/S/Z, or NNB/-1).";
  }
  if (filterFirstPieceIdx != -1) {
    if (!isNnb) {
      return "Error: first piece filter is only valid when next=NNB.";
    }
    if (filterFirstPieceIdx < 0 || filterFirstPieceIdx > 6) {
      return "Error: invalid first piece filter (expected 0-6 or I/O/L/J/T/S/Z).";
    }
  }
  if (playoutCount < 1 || playoutCount > CANONICAL_SEQUENCE_BATCH_SIZE) {
    return "Error: invalid n (expected 1-" + std::to_string(CANONICAL_SEQUENCE_BATCH_SIZE) + ").";
  }

  std::string aFirst, aSecond, bFirst, bSecond;
  if (isNnb) {
    int r = 0, x = 0, y = 0;
    if (!debug_tmt::parseLockKey(specA, r, x, y)) {
      return "Error: invalid move A (expected ply-1 key like 1|5|10 in NNB mode): " + specA;
    }
    if (!debug_tmt::parseLockKey(specB, r, x, y)) {
      return "Error: invalid move B (expected ply-1 key like 1|-1|13 in NNB mode): " + specB;
    }
    aFirst = specA;
    bFirst = specB;
  } else {
    if (!parsePairSpec(specA, aFirst, aSecond)) {
      return "Error: invalid pair A (expected firstKey;secondKey like 1|5|10;1|0|12): " + specA;
    }
    if (!parsePairSpec(specB, bFirst, bSecond)) {
      return "Error: invalid pair B (expected firstKey;secondKey like 1|-1|13;1|0|12): " + specB;
    }
  }

  debug_gt::ParsedDebugPipe req;
  std::string parseError;
  if (!debug_gt::parseDebugPipe(enginePipe, req, parseError)) {
    return parseError;
  }
  const int playoutLength =
      req.playoutLength > 0 ? req.playoutLength : DEFAULT_COMPARE_PLAYOUT_LENGTH;

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

  CompareSideInput sideA;
  CompareSideInput sideB;
  std::string resolveError;
  int lastSeenPieceIdx = req.curPiece->index;
  if (isNnb) {
    if (!resolveCompareSidesNnb(req, evalContext, useDas, contextTimeline, aFirst, bFirst, sideA,
                                sideB, resolveError)) {
      return resolveError;
    }
    lastSeenPieceIdx = req.curPiece->index;
  } else {
    const Piece *nextPiece = &PIECE_LIST[nextPieceIdx];
    if (!resolveCompareSides2Ply(req, evalContext, nextPiece, useDas, contextTimeline, specA, specB,
                                 aFirst, aSecond, bFirst, bSecond, sideA, sideB, resolveError)) {
      return resolveError;
    }
    lastSeenPieceIdx = nextPiece->index;
  }

  const int *seqBase = canonicalPieceSequencesForLastPiece[lastSeenPieceIdx];
  const std::vector<int> seqIndices =
      selectCanonicalSequenceIndices(seqBase, playoutCount, filterFirstPieceIdx);
  if (seqIndices.empty()) {
    return "Error: no canonical sequences match the requested first piece filter.";
  }

  const PairedAggregate agg = aggregatePairedPlayouts(sideA, sideB, contexts, evalContext, seqBase,
                                                      seqIndices, playoutLength);
  const std::vector<DepthPoint> depthCurve =
      computeDepthCurve(sideA, sideB, contexts, evalContext, seqBase, seqIndices, playoutLength,
                        agg.meanA, agg.meanB);

  const double elapsedMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  return serializePlayoutCompareJson(
      isNnb, nextPieceIdx, filterFirstPieceIdx, static_cast<int>(seqIndices.size()), playoutCount,
      playoutLength, elapsedMs, evalContext, contexts, seqBase, sideA, sideB, agg, depthCurve);
}

} // namespace debug_pc

#endif
