#ifndef DEBUG_GROUND_TRUTH_HPP
#define DEBUG_GROUND_TRUTH_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Expects main.cpp (and all engine headers/implementations) to be included before this file.

namespace debug_gt {

constexpr int MAX_DEBUG_KEYS = 6;
constexpr int DEFAULT_DEBUG_PLAYOUTS = 2000;
constexpr int DEFAULT_DEBUG_CONTS = 5;
constexpr int MAX_DEBUG_CONTS = 10;

inline std::string jsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
        out += buf;
      } else {
        out.push_back(c);
      }
      break;
    }
  }
  return out;
}

inline std::string boardToBitString(const unsigned int board[20]) {
  std::string out(200, '0');
  for (int r = 0; r < 20; r++) {
    const unsigned int row = board[r];
    for (int c = 0; c < 10; c++) {
      if ((row >> (9 - c)) & 1u) {
        out[r * 10 + c] = '1';
      }
    }
  }
  return out;
}

inline const char *aiModeName(AiMode mode) {
  switch (mode) {
  case STANDARD:
    return "STANDARD";
  case DIG:
    return "DIG";
  case LINEOUT:
    return "LINEOUT";
  default:
    return "UNKNOWN";
  }
}

// NES PRNG transition probability P(next | current), matching precompute.cpp's table.
inline double nesPieceProb(const Piece *current, const Piece *next) {
  static const int TRANSITIONS[7][7] = {{2, 10, 12, 10, 10, 10, 10}, {12, 2, 10, 10, 10, 10, 10},
                                        {10, 12, 2, 10, 10, 10, 10}, {10, 10, 10, 4, 10, 10, 10},
                                        {10, 10, 10, 10, 4, 10, 10}, {12, 10, 10, 10, 10, 2, 10},
                                        {10, 10, 10, 10, 12, 10, 2}};
  auto idx = [](char id) {
    switch (id) {
    case 'T':
      return 0;
    case 'J':
      return 1;
    case 'Z':
      return 2;
    case 'O':
      return 3;
    case 'S':
      return 4;
    case 'L':
      return 5;
    case 'I':
      return 6;
    default:
      return 0;
    }
  };
  return static_cast<double>(TRANSITIONS[idx(current->id)][idx(next->id)]) / 64.0;
}

inline bool isPlainInt(const std::string &s) {
  if (s.empty()) {
    return false;
  }
  size_t start = s[0] == '-' ? 1 : 0;
  if (s.size() <= start) {
    return false;
  }
  for (size_t i = start; i < s.size(); i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

inline std::string fmtDouble(double v, int prec = 2) {
  if (!std::isfinite(v)) {
    return "null";
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f", prec, v);
  return std::string(buf);
}

struct ParsedDebugPipe {
  GameState gameState{};
  const Piece *curPiece = nullptr;
  std::string inputFrameTimeline;
  int playoutLength = 0;
  AggressionLevel aggressionLevel = SAFE;
  int dasCharge = -1;
};

// Strictly parses an 11- or 12-field engine pipe string ('|'-delimited) for ground-truth debug.
inline bool parseDebugPipe(const std::string &pipeStr, ParsedDebugPipe &out,
                           std::string &errorOut) {
  if (pipeStr.size() < 201 || pipeStr[200] != '|') {
    errorOut = "Error: invalid board string (expected 200 characters of 0/1).";
    return false;
  }
  for (int i = 0; i < 200; i++) {
    if (pipeStr[i] != '0' && pipeStr[i] != '1') {
      errorOut = "Error: invalid board string (expected 200 characters of 0/1).";
      return false;
    }
  }

  std::vector<std::string> fields;
  size_t pos = 201;
  while (pos < pipeStr.size()) {
    size_t bar = pipeStr.find('|', pos);
    if (bar == std::string::npos) {
      fields.push_back(pipeStr.substr(pos));
      break;
    }
    fields.push_back(pipeStr.substr(pos, bar - pos));
    pos = bar + 1;
  }

  // Skip optional second board if present.
  size_t off = 0;
  if (!fields.empty() && fields[0].size() == 200) {
    bool allBits = true;
    for (char c : fields[0]) {
      if (c != '0' && c != '1') {
        allBits = false;
        break;
      }
    }
    if (allBits) {
      off = 1;
    }
  }

  if (fields.size() - off < 10) {
    errorOut = "Error: incomplete pipe string (expected at least 10 fields after board).";
    return false;
  }

  const std::string &levelStr = fields[off + 0];
  const std::string &linesStr = fields[off + 1];
  const std::string &curPieceStr = fields[off + 2];
  const std::string &timelineStr = fields[off + 4];
  const std::string &playoutLengthStr = fields[off + 6];
  const std::string &aggroStr = fields[off + 8];
  const std::string &dasStr = fields[off + 9];

  if (!isPlainInt(levelStr)) {
    errorOut = "Error: invalid level value: " + levelStr;
    return false;
  }
  int level = std::atoi(levelStr.c_str());
  if (level < 18) {
    errorOut = "Error: unsupported level (minimum 18): " + levelStr;
    return false;
  }

  if (!isPlainInt(linesStr)) {
    errorOut = "Error: invalid lines value: " + linesStr;
    return false;
  }
  int lines = std::atoi(linesStr.c_str());
  if (lines < 0) {
    errorOut = "Error: invalid line count: " + linesStr;
    return false;
  }

  if (!isPlainInt(curPieceStr)) {
    errorOut = "Error: invalid currentPiece value: " + curPieceStr;
    return false;
  }
  int curIdx = std::atoi(curPieceStr.c_str());
  if (curIdx < 0 || curIdx > 6) {
    errorOut = "Error: unknown current piece: " + curPieceStr;
    return false;
  }

  if (timelineStr.empty()
      || timelineStr.find_first_not_of("X.") != std::string::npos
      || timelineStr.find('X') == std::string::npos) {
    errorOut = "Error: invalid inputFrameTimeline: " + timelineStr;
    return false;
  }

  if (!isPlainInt(playoutLengthStr)) {
    errorOut = "Error: invalid playoutLength value: " + playoutLengthStr;
    return false;
  }
  int playoutLength = std::atoi(playoutLengthStr.c_str());
  if (playoutLength < 0 || playoutLength > 20) {
    errorOut = "Error: invalid playout length (0-20): " + playoutLengthStr;
    return false;
  }

  AggressionLevel aggro = SAFE;
  if (aggroStr == "safe") {
    aggro = SAFE;
  } else if (aggroStr == "aggro") {
    aggro = AGGRO;
  } else if (aggroStr == "hyper") {
    aggro = HYPER;
  } else {
    errorOut = "Error: invalid aggressionLevel (expected safe, aggro, or hyper): " + aggroStr;
    return false;
  }

  if (!isPlainInt(dasStr)) {
    errorOut = "Error: invalid dasCharge value: " + dasStr;
    return false;
  }
  int dasCharge = std::atoi(dasStr.c_str());
  if (dasCharge < -1 || dasCharge > 16) {
    errorOut = "Error: illegal DAS charge (expected -1 or 0-16): " + dasStr;
    return false;
  }

  out.gameState = {};
  out.gameState.level = level;
  out.gameState.lines = lines;
  encodeBoard(pipeStr.c_str(), out.gameState.board);
  getSurfaceArray(out.gameState.board, out.gameState.surfaceArray);
  auto holes = updateSurfaceAndHoles(out.gameState.surfaceArray, out.gameState.board, 9, false);
  out.gameState.numTrueHoles = holes.first;
  out.gameState.numPartialHoles = holes.second;
  out.curPiece = &PIECE_LIST[curIdx];
  out.inputFrameTimeline = timelineStr;
  out.playoutLength = playoutLength;
  out.aggressionLevel = aggro;
  out.dasCharge = dasCharge;
  return true;
}

struct PieceGtResult {
  bool valid = false;
  double mean = 0.0;
  double se = 0.0;
  double deathPct = 0.0;
  std::string bestCont;
  int bestContRelRot = 0;
  int bestContRelX = 0;
  int bestContRelY = 0;
  int bestContEvalRank = -1;
  double contEval = 0.0;
  std::string contEvalExpl;
  int linesCleared = 0;
  double partialPenalty = 0.0;
  double minPenalty = 0.0;
  std::string boardAfter;
};

struct CandidateGtResult {
  std::string key;
  bool reachable = false;
  int rot = 0;
  int x = 0;
  int y = 0;
  int relRot = 0;
  int relX = 0;
  int relY = 0;
  double fastEvalScore = 0.0;
  std::string fastEvalExpl;
  int dasAfter = -1;
  int linesCleared = 0;
  std::string boardAfter;
  double weightedAvg = 0.0;
  double weightedSe = 0.0;
  double deathPct = 0.0;      // 1-ply NNB death percentage
  double avgDasPenalty = 0.0; // 1-ply NNB: NES-weighted DAS penalty for this placement's dasAfter
  double avgPartialPenalty = 0.0; // 1-ply NNB: NES-weighted partial DAS penalty
  double avgMinPenalty = 0.0;     // 1-ply NNB: NES-weighted min DAS penalty
  PieceGtResult byPiece[7];
};

// Runs a high-N ground-truth canonical playout comparison across all 7 next pieces (2-ply,
// default) or directly from post-ply-1 states (1-ply NNB when noNextBox == true) for the
// requested first-piece lock keys (or top 4 by fastEval if requestedKeys is empty).
// Returns a JSON string, or an "Error: ..." string on invalid inputs.
inline std::string runDebugGroundTruth(const std::string &enginePipe,
                                       const std::vector<std::string> &requestedKeys,
                                       int playoutCount, int maxConts, bool noNextBox = false) {
  const auto started = std::chrono::steady_clock::now();

  if (playoutCount < 0 || playoutCount > CANONICAL_SEQUENCE_BATCH_SIZE) {
    return "Error: invalid n (expected 0-" + std::to_string(CANONICAL_SEQUENCE_BATCH_SIZE) + ").";
  }
  if (maxConts < 1 || maxConts > MAX_DEBUG_CONTS) {
    return "Error: invalid conts (expected 1-" + std::to_string(MAX_DEBUG_CONTS) + ").";
  }
  if (static_cast<int>(requestedKeys.size()) > MAX_DEBUG_KEYS) {
    return "Error: too many keys (max " + std::to_string(MAX_DEBUG_KEYS) + ").";
  }

  ParsedDebugPipe req;
  std::string parseError;
  if (!parseDebugPipe(enginePipe, req, parseError)) {
    return parseError;
  }

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

  const FirstMoveSearchResult allFirstMoves = buildFirstMoveBranches(
      req.gameState, req.curPiece, useDas ? req.dasCharge : -1,
      useDas ? req.inputFrameTimeline.c_str() : contextTimeline, &evalContext);

  struct ReachableInfo {
    std::string key;
    size_t branchIdx;
    float fastEvalScore;
    std::string fastEvalExpl;
  };
  std::vector<ReachableInfo> reachable;
  reachable.reserve(allFirstMoves.branches.size());
  std::unordered_map<std::string, size_t> keyToReachIdx;
  for (size_t i = 0; i < allFirstMoves.branches.size(); i++) {
    const FirstMoveBranch &b = allFirstMoves.branches[i];
    std::string k = encodeLockPosition(
        {b.firstPlacement.x, b.firstPlacement.y, b.firstPlacement.rotationIndex});
    if (keyToReachIdx.count(k) > 0) {
      continue;
    }
    std::string expl;
    float ev = b.firstMoveReward
               + fastEval(req.gameState, b.afterFirstMove, b.firstPlacement, &evalContext, &expl);
    keyToReachIdx[k] = reachable.size();
    reachable.push_back({k, i, ev, std::move(expl)});
  }
  std::sort(reachable.begin(), reachable.end(), [](const ReachableInfo &a, const ReachableInfo &b) {
    return a.fastEvalScore > b.fastEvalScore;
  });
  // Rebuild lookup after sorting so indices match `reachable`.
  keyToReachIdx.clear();
  for (size_t i = 0; i < reachable.size(); i++) {
    keyToReachIdx[reachable[i].key] = i;
  }

  // Decide which keys to evaluate.
  std::vector<std::string> evalKeys;
  if (requestedKeys.empty()) {
    int lim = std::min(static_cast<int>(reachable.size()), 4);
    for (int i = 0; i < lim; i++) {
      evalKeys.push_back(reachable[i].key);
    }
  } else {
    std::unordered_set<std::string> seen;
    for (const std::string &k : requestedKeys) {
      if (!k.empty() && seen.insert(k).second) {
        evalKeys.push_back(k);
      }
    }
  }

  // Build a filtered FirstMoveSearchResult containing only the requested reachable branches.
  FirstMoveSearchResult filteredFirstMoves;
  filteredFirstMoves.useDas = allFirstMoves.useDas;
  filteredFirstMoves.firstPiece = allFirstMoves.firstPiece;

  std::vector<CandidateGtResult> results(evalKeys.size());
  std::unordered_map<std::string, size_t> keyToResultIdx;
  for (size_t i = 0; i < evalKeys.size(); i++) {
    CandidateGtResult &res = results[i];
    res.key = evalKeys[i];
    keyToResultIdx[res.key] = i;
    auto it = keyToReachIdx.find(res.key);
    if (it == keyToReachIdx.end()) {
      res.reachable = false;
      continue;
    }
    const ReachableInfo &info = reachable[it->second];
    const FirstMoveBranch &b = allFirstMoves.branches[info.branchIdx];
    res.reachable = true;
    res.rot = b.firstPlacement.rotationIndex;
    res.x = b.firstPlacement.x;
    res.y = b.firstPlacement.y;
    res.relRot = b.firstPlacement.rotationIndex;
    res.relX = b.firstPlacement.x - INITIAL_X;
    res.relY = b.firstPlacement.y - req.curPiece->initialY;
    res.fastEvalScore = info.fastEvalScore;
    res.fastEvalExpl = info.fastEvalExpl;
    res.dasAfter = b.dasChargeAfter;
    res.linesCleared = b.afterFirstMove.lines - req.gameState.lines;
    res.boardAfter = boardToBitString(b.afterFirstMove.board);
    filteredFirstMoves.branches.push_back(b);
  }

  // Run ground-truth evaluations in parallel.
  if (!filteredFirstMoves.branches.empty()) {
    if (noNextBox) {
      // 1-ply NNB ground truth: evaluate each requested first-piece placement directly with
      // curPiece as lastSeenPiece, plus compute the 7 shallow DAS penalty maps when in DAS mode.
      std::vector<size_t> reachableResultIndices;
      reachableResultIndices.reserve(filteredFirstMoves.branches.size());
      for (size_t i = 0; i < results.size(); i++) {
        if (results[i].reachable) {
          reachableResultIndices.push_back(i);
        }
      }

      const bool runPlayouts = playoutCount > 0 && req.playoutLength > 0;
      const size_t numCands = reachableResultIndices.size();
      std::vector<std::exception_ptr> candErrors(numCands);
      std::vector<std::thread> candThreads(numCands);
      for (size_t idx = 0; idx < numCands; idx++) {
        candThreads[idx] = std::thread([&, idx]() {
          try {
            const FirstMoveBranch &b = filteredFirstMoves.branches[idx];
            CandidateGtResult &cand = results[reachableResultIndices[idx]];
            double total = b.firstMoveEval;
            double se = 0.0;
            double deathPct = 0.0;
            if (runPlayouts) {
              std::vector<float> scores;
              float plMean = getPlayoutScore(
                  b.afterFirstMove, playoutCount, req.playoutLength, contexts, req.curPiece->index,
                  evalContext.aggressionLevel, evalContext.aiMode, /* playoutDataList= */ nullptr,
                  &scores, /* sequenceOffset= */ 0);
              total = static_cast<double>(b.firstMoveReward) + static_cast<double>(plMean);
              if (scores.size() > 1) {
                double sumSq = 0.0;
                int deaths = 0;
                for (float s : scores) {
                  double d = static_cast<double>(s) - static_cast<double>(plMean);
                  sumSq += d * d;
                  if (s <= evalContext.weights.deathCoef + 100.0f) {
                    deaths++;
                  }
                }
                se = std::sqrt(sumSq
                               / static_cast<double>(scores.size() - 1)
                               / static_cast<double>(scores.size()));
                deathPct = 100.0 * static_cast<double>(deaths) / static_cast<double>(scores.size());
              } else if (scores.size() == 1) {
                deathPct = scores[0] <= evalContext.weights.deathCoef + 100.0f ? 100.0 : 0.0;
              }
            }
            cand.weightedAvg = total;
            cand.weightedSe = se;
            cand.deathPct = deathPct;
            for (int p = 0; p < 7; p++) {
              PieceGtResult &pr = cand.byPiece[p];
              pr.valid = true;
              pr.mean = total;
              pr.se = se;
              pr.deathPct = deathPct;
              pr.bestCont = "none";
              pr.contEval = b.firstMoveEval;
              pr.linesCleared = cand.linesCleared;
              pr.boardAfter = cand.boardAfter;
            }
          } catch (...) {
            candErrors[idx] = std::current_exception();
          }
        });
      }
      for (std::thread &t : candThreads) {
        if (t.joinable()) {
          t.join();
        }
      }
      for (size_t idx = 0; idx < numCands; idx++) {
        if (candErrors[idx]) {
          std::rethrow_exception(candErrors[idx]);
        }
      }

      if (useDas) {
        DasPenaltyMaps perPiece[7];
        std::exception_ptr penErrors[7] = {};
        std::thread penThreads[7];
        for (int p = 0; p < 7; p++) {
          penThreads[p] = std::thread([&, p]() {
            try {
              perPiece[p] = getDasPenaltyMapsWithFirstMoves(
                  filteredFirstMoves, &PIECE_LIST[p], req.inputFrameTimeline.c_str(), &evalContext);
            } catch (...) {
              penErrors[p] = std::current_exception();
            }
          });
        }
        for (std::thread &t : penThreads) {
          if (t.joinable()) {
            t.join();
          }
        }
        for (int p = 0; p < 7; p++) {
          if (penErrors[p]) {
            std::rethrow_exception(penErrors[p]);
          }
        }
        for (size_t rIdx : reachableResultIndices) {
          CandidateGtResult &cand = results[rIdx];
          double avgPartial = 0.0;
          double avgMin = 0.0;
          double avgActual = 0.0;
          for (int p = 0; p < 7; p++) {
            const double w = nesPieceProb(req.curPiece, &PIECE_LIST[p]);
            const bool hasBoth = perPiece[p].partialPenalty.count(cand.key)
                                 && perPiece[p].minPenalty.count(cand.key);
            const double pp =
                hasBoth ? static_cast<double>(perPiece[p].partialPenalty.at(cand.key)) : 0.0;
            const double mp =
                hasBoth ? static_cast<double>(perPiece[p].minPenalty.at(cand.key)) : 0.0;
            cand.byPiece[p].partialPenalty = pp;
            cand.byPiece[p].minPenalty = mp;
            avgPartial += w * pp;
            avgMin += w * mp;
            const double applied = cand.dasAfter >= 15 ? 0.0 : (cand.dasAfter >= 10 ? pp : mp);
            avgActual += w * applied;
          }
          cand.avgPartialPenalty = avgPartial;
          cand.avgMinPenalty = avgMin;
          cand.avgDasPenalty = avgActual;
        }
      }
    } else {
      // Standard 2-ply ground truth: run all 7 next-piece evaluations in parallel.
      auto evalOneNextPiece = [&](int pieceIdx) {
        const Piece *nextPiece = &PIECE_LIST[pieceIdx];

        // Compute shallow DAS penalties on the filtered branches when in DAS mode.
        DasPenaltyMaps dasPenalties;
        if (useDas) {
          dasPenalties = getDasPenaltyMapsWithFirstMoves(
              filteredFirstMoves, nextPiece, req.inputFrameTimeline.c_str(), &evalContext);
        }

        std::vector<Possibility> poss;
        searchDepth2FromFirstMoves(filteredFirstMoves, nextPiece,
                                   /* secondPieceDasCharge= */ useDas ? 15 : -1,
                                   useDas ? req.inputFrameTimeline.c_str() : contextTimeline,
                                   &evalContext, poss, /* generateExplanation= */ true);

        // Group possibilities by first placement key.
        std::unordered_map<std::string, std::vector<const Possibility *>> byKey;
        for (const Possibility &p : poss) {
          byKey[encodeLockPosition(p.firstPlacement)].push_back(&p);
        }

        for (size_t rIdx = 0; rIdx < results.size(); rIdx++) {
          CandidateGtResult &cand = results[rIdx];
          if (!cand.reachable) {
            continue;
          }
          auto it = byKey.find(cand.key);
          if (it == byKey.end() || it->second.empty()) {
            continue;
          }
          auto &vec = it->second;
          std::sort(vec.begin(), vec.end(), [](const Possibility *a, const Possibility *b) {
            return a->evalScoreInclReward > b->evalScoreInclReward;
          });

          PieceGtResult best;
          best.mean = -1e18;
          if (useDas) {
            if (dasPenalties.partialPenalty.count(cand.key)) {
              best.partialPenalty = dasPenalties.partialPenalty.at(cand.key);
            }
            if (dasPenalties.minPenalty.count(cand.key)) {
              best.minPenalty = dasPenalties.minPenalty.at(cand.key);
            }
          }

          const bool runPlayouts = playoutCount > 0 && req.playoutLength > 0;
          const int lim = runPlayouts ? std::min(static_cast<int>(vec.size()), maxConts) : 1;
          for (int c = 0; c < lim; c++) {
            const Possibility *p = vec[c];
            double total = 0.0;
            double se = 0.0;
            double deathPct = 0.0;
            if (runPlayouts) {
              std::vector<float> scores;
              float plMean =
                  getPlayoutScore(p->resultingState, playoutCount, req.playoutLength, contexts,
                                  nextPiece->index, evalContext.aggressionLevel, evalContext.aiMode,
                                  /* playoutDataList= */ nullptr, &scores, /* sequenceOffset= */ 0);
              total = static_cast<double>(p->immediateReward) + static_cast<double>(plMean);
              if (scores.size() > 1) {
                double sumSq = 0.0;
                int deaths = 0;
                for (float s : scores) {
                  double d = static_cast<double>(s) - static_cast<double>(plMean);
                  sumSq += d * d;
                  if (s <= evalContext.weights.deathCoef + 100.0f) {
                    deaths++;
                  }
                }
                se = std::sqrt(sumSq
                               / static_cast<double>(scores.size() - 1)
                               / static_cast<double>(scores.size()));
                deathPct = 100.0 * static_cast<double>(deaths) / static_cast<double>(scores.size());
              } else if (scores.size() == 1) {
                deathPct = scores[0] <= evalContext.weights.deathCoef + 100.0f ? 100.0 : 0.0;
              }
            } else {
              total = p->evalScoreInclReward;
            }

            if (!best.valid || total > best.mean) {
              best.valid = true;
              best.mean = total;
              best.se = se;
              best.deathPct = deathPct;
              best.bestCont = encodeLockPosition(p->secondPlacement);
              best.bestContRelRot = p->secondPlacement.rotationIndex;
              best.bestContRelX = p->secondPlacement.x - INITIAL_X;
              best.bestContRelY = p->secondPlacement.y - nextPiece->initialY;
              best.bestContEvalRank = c;
              best.contEval = p->evalScoreInclReward;
              best.contEvalExpl = p->evalExplanation;
              best.linesCleared = p->resultingState.lines - req.gameState.lines;
              best.boardAfter = boardToBitString(p->resultingState.board);
            }
          }
          cand.byPiece[pieceIdx] = std::move(best);
        }
      };

      std::exception_ptr errors[7] = {};
      std::thread threads[7];
      for (int i = 0; i < 7; i++) {
        threads[i] = std::thread([i, &evalOneNextPiece, &errors]() {
          try {
            evalOneNextPiece(i);
          } catch (...) {
            errors[i] = std::current_exception();
          }
        });
      }
      for (int i = 0; i < 7; i++) {
        threads[i].join();
      }
      for (int i = 0; i < 7; i++) {
        if (errors[i]) {
          std::rethrow_exception(errors[i]);
        }
      }

      // Aggregate NES-weighted 7-piece averages and combined SE.
      for (CandidateGtResult &cand : results) {
        if (!cand.reachable) {
          continue;
        }
        double avg = 0.0;
        double varSum = 0.0;
        double weightSum = 0.0;
        for (int i = 0; i < 7; i++) {
          if (!cand.byPiece[i].valid) {
            continue;
          }
          double w = nesPieceProb(req.curPiece, &PIECE_LIST[i]);
          avg += w * cand.byPiece[i].mean;
          varSum += w * w * cand.byPiece[i].se * cand.byPiece[i].se;
          weightSum += w;
        }
        cand.weightedAvg = weightSum > 0.0 ? avg / weightSum : 0.0;
        cand.weightedSe = weightSum > 0.0 ? std::sqrt(varSum) / weightSum : 0.0;
      }
    }
  }

  const double elapsedMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  // Serialize JSON.
  std::ostringstream out;
  out << "{";
  out << "\"aiMode\":\"" << aiModeName(evalContext.aiMode) << "\",";
  out << "\"isNoNextBox\":" << (noNextBox ? "true" : "false") << ",";
  out << "\"playoutCount\":" << playoutCount << ",";
  out << "\"playoutLength\":" << req.playoutLength << ",";
  out << "\"contsEvaluated\":" << maxConts << ",";
  out << "\"useDas\":" << (useDas ? "true" : "false") << ",";
  out << "\"firstPieceDasCharge\":" << req.dasCharge << ",";
  out << "\"elapsedMs\":" << fmtDouble(elapsedMs, 1) << ",";

  out << "\"reachablePlacements\":[";
  for (size_t i = 0; i < reachable.size(); i++) {
    if (i > 0) {
      out << ",";
    }
    const ReachableInfo &ri = reachable[i];
    const FirstMoveBranch &b = allFirstMoves.branches[ri.branchIdx];
    out
        << "{\"key\":\""
        << jsonEscape(ri.key)
        << "\","
        << "\"rot\":"
        << b.firstPlacement.rotationIndex
        << ","
        << "\"x\":"
        << b.firstPlacement.x
        << ","
        << "\"y\":"
        << b.firstPlacement.y
        << ","
        << "\"relRot\":"
        << b.firstPlacement.rotationIndex
        << ","
        << "\"relX\":"
        << (b.firstPlacement.x - INITIAL_X)
        << ","
        << "\"relY\":"
        << (b.firstPlacement.y - req.curPiece->initialY)
        << ","
        << "\"fastEval\":"
        << fmtDouble(ri.fastEvalScore, 2)
        << ","
        << "\"dasAfter\":"
        << b.dasChargeAfter
        << ","
        << "\"linesCleared\":"
        << (b.afterFirstMove.lines - req.gameState.lines)
        << ","
        << "\"boardAfter\":\""
        << boardToBitString(b.afterFirstMove.board)
        << "\"}";
  }
  out << "],";

  static const char *PIECE_NAMES[7] = {"I", "O", "L", "J", "T", "S", "Z"};
  out << "\"candidates\":[";
  for (size_t i = 0; i < results.size(); i++) {
    if (i > 0) {
      out << ",";
    }
    const CandidateGtResult &c = results[i];
    out
        << "{\"key\":\""
        << jsonEscape(c.key)
        << "\","
        << "\"reachable\":"
        << (c.reachable ? "true" : "false");
    if (c.reachable) {
      out
          << ",\"rot\":"
          << c.rot
          << ",\"x\":"
          << c.x
          << ",\"y\":"
          << c.y
          << ",\"relRot\":"
          << c.relRot
          << ",\"relX\":"
          << c.relX
          << ",\"relY\":"
          << c.relY
          << ",\"fastEval\":"
          << fmtDouble(c.fastEvalScore, 2)
          << ",\"fastEvalExpl\":\""
          << jsonEscape(c.fastEvalExpl)
          << "\""
          << ",\"dasAfter\":"
          << c.dasAfter
          << ",\"linesCleared\":"
          << c.linesCleared
          << ",\"boardAfter\":\""
          << c.boardAfter
          << "\""
          << ",\"weightedAvg\":"
          << fmtDouble(c.weightedAvg, 2)
          << ",\"weightedSe\":"
          << fmtDouble(c.weightedSe, 2)
          << ",\"deathPct\":"
          << fmtDouble(c.deathPct, 2)
          << ",\"avgDasPenalty\":"
          << fmtDouble(c.avgDasPenalty, 2)
          << ",\"avgPartialPenalty\":"
          << fmtDouble(c.avgPartialPenalty, 2)
          << ",\"avgMinPenalty\":"
          << fmtDouble(c.avgMinPenalty, 2)
          << ",\"byPiece\":{";
      for (int p = 0; p < 7; p++) {
        if (p > 0) {
          out << ",";
        }
        out << "\"" << PIECE_NAMES[p] << "\":";
        const PieceGtResult &pr = c.byPiece[p];
        if (!pr.valid) {
          out << "null";
        } else {
          out
              << "{\"mean\":"
              << fmtDouble(pr.mean, 2)
              << ",\"se\":"
              << fmtDouble(pr.se, 2)
              << ",\"deathPct\":"
              << fmtDouble(pr.deathPct, 2)
              << ",\"bestCont\":\""
              << jsonEscape(pr.bestCont)
              << "\""
              << ",\"bestContRel\":["
              << pr.bestContRelRot
              << ","
              << pr.bestContRelX
              << ","
              << pr.bestContRelY
              << "]"
              << ",\"bestContEvalRank\":"
              << pr.bestContEvalRank
              << ",\"contEval\":"
              << fmtDouble(pr.contEval, 2)
              << ",\"contEvalExpl\":\""
              << jsonEscape(pr.contEvalExpl)
              << "\""
              << ",\"linesCleared\":"
              << pr.linesCleared
              << ",\"partialPenalty\":"
              << fmtDouble(pr.partialPenalty, 2)
              << ",\"minPenalty\":"
              << fmtDouble(pr.minPenalty, 2)
              << ",\"boardAfter\":\""
              << pr.boardAfter
              << "\"}";
        }
      }
      out << "}";
    }
    out << "}";
  }
  out << "]}";
  return out.str();
}

} // namespace debug_gt

#endif
