#ifndef DEBUG_PRECOMPUTE_HPP
#define DEBUG_PRECOMPUTE_HPP

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

// Expects main.cpp and debug_ground_truth.hpp to be included before this file.

namespace debug_pre {

struct ParsedPrecomputePipe {
  GameState gameState{};
  const Piece *curPiece = nullptr;
  std::string inputFrameTimeline;
  int playoutCount = 0;
  int playoutLength = 0;
  int pruningBreadth = 0;
  AggressionLevel aggressionLevel = SAFE;
  int dasCharge = -1;
  int reactionTime = 0;
};

// Strictly parses a 12-field precompute engine pipe string ('|'-delimited).
inline bool parsePrecomputeDebugPipe(const std::string &pipeStr, ParsedPrecomputePipe &out,
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

  if (fields.size() - off < 11) {
    errorOut = "Error: incomplete precompute pipe string (expected 11 fields after board).";
    return false;
  }

  const std::string &levelStr = fields[off + 0];
  const std::string &linesStr = fields[off + 1];
  const std::string &curPieceStr = fields[off + 2];
  const std::string &timelineStr = fields[off + 4];
  const std::string &playoutCountStr = fields[off + 5];
  const std::string &playoutLengthStr = fields[off + 6];
  const std::string &breadthStr = fields[off + 7];
  const std::string &aggroStr = fields[off + 8];
  const std::string &dasStr = fields[off + 9];
  const std::string &rtStr = fields[off + 10];

  if (!debug_gt::isPlainInt(levelStr) || std::atoi(levelStr.c_str()) < 18) {
    errorOut = "Error: invalid or unsupported level: " + levelStr;
    return false;
  }
  if (!debug_gt::isPlainInt(linesStr) || std::atoi(linesStr.c_str()) < 0) {
    errorOut = "Error: invalid lines value: " + linesStr;
    return false;
  }
  if (!debug_gt::isPlainInt(curPieceStr)) {
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
  if (!debug_gt::isPlainInt(playoutCountStr) || std::atoi(playoutCountStr.c_str()) < 0) {
    errorOut = "Error: invalid playoutCount value: " + playoutCountStr;
    return false;
  }
  if (!debug_gt::isPlainInt(playoutLengthStr)) {
    errorOut = "Error: invalid playoutLength value: " + playoutLengthStr;
    return false;
  }
  int playoutLength = std::atoi(playoutLengthStr.c_str());
  if (playoutLength < 0 || playoutLength > 20) {
    errorOut = "Error: invalid playout length (0-20): " + playoutLengthStr;
    return false;
  }
  if (!debug_gt::isPlainInt(breadthStr)) {
    errorOut = "Error: invalid pruningBreadth value: " + breadthStr;
    return false;
  }
  int pruningBreadth = std::atoi(breadthStr.c_str());
  if (pruningBreadth < -2 || pruningBreadth > 1156) {
    errorOut = "Error: invalid pruningBreadth (expected -2, -1, or 0-1156): " + breadthStr;
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
  if (!debug_gt::isPlainInt(dasStr)) {
    errorOut = "Error: invalid dasCharge value: " + dasStr;
    return false;
  }
  int dasCharge = std::atoi(dasStr.c_str());
  if (dasCharge < -1 || dasCharge > 16) {
    errorOut = "Error: illegal DAS charge (expected -1 or 0-16): " + dasStr;
    return false;
  }
  if (!debug_gt::isPlainInt(rtStr)) {
    errorOut = "Error: invalid reactionTime value: " + rtStr;
    return false;
  }
  int reactionTime = std::atoi(rtStr.c_str());
  if (reactionTime < 0 || reactionTime > 60) {
    errorOut = "Error: invalid reactionTime (expected 0-60): " + rtStr;
    return false;
  }

  int playoutCount = std::atoi(playoutCountStr.c_str());
  long long totalPlayoutVolume = pruningBreadth < 0
                                     ? static_cast<long long>(playoutCount)
                                     : static_cast<long long>(playoutCount) * playoutLength;
  if (totalPlayoutVolume > MAX_CPP_PLAYOUT_MOVES) {
    errorOut = "Error: playout volume exceeds the current limit of "
               + std::to_string(MAX_CPP_PLAYOUT_MOVES)
               + " moves.";
    return false;
  }

  out.gameState = {};
  out.gameState.level = std::atoi(levelStr.c_str());
  out.gameState.lines = std::atoi(linesStr.c_str());
  encodeBoard(pipeStr.c_str(), out.gameState.board);
  getSurfaceArray(out.gameState.board, out.gameState.surfaceArray);
  auto holes = updateSurfaceAndHoles(out.gameState.surfaceArray, out.gameState.board, 9, false);
  out.gameState.numTrueHoles = holes.first;
  out.gameState.numPartialHoles = holes.second;
  out.curPiece = &PIECE_LIST[curIdx];
  out.inputFrameTimeline = timelineStr;
  out.playoutCount = playoutCount;
  out.playoutLength = playoutLength;
  out.pruningBreadth = pruningBreadth;
  out.aggressionLevel = aggro;
  out.dasCharge = dasCharge;
  out.reactionTime = reactionTime;
  return true;
}

// Runs preparePrecomputeState + collapseAndFormatPrecompute + analyzePrecomputeCollapse
// and serializes the full phantom collapse breakdown as JSON.
inline std::string runDebugPrecompute(const std::string &enginePipe,
                                      const EngineTimingContext *timing = nullptr) {
  const auto started = std::chrono::steady_clock::now();

  ParsedPrecomputePipe req;
  std::string parseError;
  if (!parsePrecomputeDebugPipe(enginePipe, req, parseError)) {
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

  SearchBudget timedBudget{};
  const SearchBudget *budgetPtr = nullptr;
  if (req.pruningBreadth == -2) {
    const auto anchor =
        (timing != nullptr && timing->hasRequestReceived) ? timing->requestReceived : started;
    const double calRate = timing != nullptr ? timing->calibratedMovesPerMs7T : 0.0;
    timedBudget = SearchBudget::fromDeadline(
        req.playoutCount, anchor + std::chrono::milliseconds(req.playoutCount), calRate);
    budgetPtr = &timedBudget;
  }

  const PrecomputeState preState = preparePrecomputeState(
      req.gameState, req.curPiece, req.inputFrameTimeline.c_str(), req.dasCharge, req.reactionTime,
      req.pruningBreadth, req.playoutCount, req.playoutLength, &evalContext, contexts, budgetPtr);
  const std::string responseStr =
      collapseAndFormatPrecompute(preState, req.gameState.board, req.curPiece, req.gameState.level,
                                  req.gameState.lines, SHOULD_PUSHDOWN);
  const std::vector<PrecomputePhantomBreakdown> phantoms =
      analyzePrecomputeCollapse(preState, req.curPiece);

  const double elapsedMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  static const char *PIECE_NAMES[7] = {"I", "O", "L", "J", "T", "S", "Z"};
  std::ostringstream out;
  out << "{";
  out << "\"aiMode\":\"" << debug_gt::aiModeName(evalContext.aiMode) << "\",";
  out << "\"useDas\":" << (preState.useDAS ? "true" : "false") << ",";
  out << "\"reactionTime\":" << preState.reactionTime << ",";
  out << "\"elapsedMs\":" << debug_gt::fmtDouble(elapsedMs, 1) << ",";
  out << "\"response\":\"" << debug_gt::jsonEscape(responseStr) << "\",";
  out << "\"totalPhantoms\":" << phantoms.size() << ",";

  out << "\"phantoms\":[";
  for (size_t i = 0; i < phantoms.size(); i++) {
    if (i > 0) {
      out << ",";
    }
    const PrecomputePhantomBreakdown &ph = phantoms[i];
    out
        << "{\"phantomIndex\":"
        << ph.phantomIndex
        << ",\"isWinner\":"
        << (ph.isWinner ? "true" : "false")
        << ",\"hasInitialPlacement\":"
        << (ph.hasInitialPlacement ? "true" : "false")
        << ",\"defaultKey\":\""
        << debug_gt::jsonEscape(ph.defaultKey)
        << "\""
        << ",\"rot\":"
        << ph.rot
        << ",\"x\":"
        << ph.x
        << ",\"y\":"
        << ph.y
        << ",\"relRot\":"
        << ph.relRot
        << ",\"relX\":"
        << ph.relX
        << ",\"relY\":"
        << ph.relY
        << ",\"dasChargeAfter\":"
        << ph.dasChargeAfter
        << ",\"linesCleared\":"
        << ph.linesCleared
        << ",\"inputPrefix\":\""
        << debug_gt::jsonEscape(ph.inputPrefix)
        << "\""
        << ",\"numAdjustments\":"
        << ph.numAdjustments
        << ",\"adjustmentKeys\":[";
    for (size_t j = 0; j < ph.adjustmentKeys.size(); j++) {
      if (j > 0) {
        out << ",";
      }
      out << "\"" << debug_gt::jsonEscape(ph.adjustmentKeys[j]) << "\"";
    }
    out
        << "],\"weightedAvg\":"
        << debug_gt::fmtDouble(ph.weightedAvg, 2)
        << ",\"noAdjWeightedAvg\":"
        << (ph.hasNoAdjAvg ? debug_gt::fmtDouble(ph.noAdjWeightedAvg, 2) : "null")
        << ",\"byPiece\":{";
    for (int p = 0; p < 7; p++) {
      if (p > 0) {
        out << ",";
      }
      out << "\"" << PIECE_NAMES[p] << "\":";
      const PrecomputePieceBreakdown &pb = ph.byPiece[p];
      if (!pb.valid) {
        out << "null";
      } else {
        out
            << "{\"key\":\""
            << debug_gt::jsonEscape(pb.key)
            << "\""
            << ",\"isContinue\":"
            << (pb.isContinue ? "true" : "false")
            << ",\"rot\":"
            << pb.rot
            << ",\"x\":"
            << pb.x
            << ",\"y\":"
            << pb.y
            << ",\"relRot\":"
            << pb.relRot
            << ",\"relX\":"
            << pb.relX
            << ",\"relY\":"
            << pb.relY
            << ",\"rawLockValue\":"
            << debug_gt::fmtDouble(pb.rawLockValue, 2)
            << ",\"dasPenalty\":"
            << debug_gt::fmtDouble(pb.dasPenalty, 2)
            << ",\"dasBucket\":\""
            << pb.dasBucket
            << "\""
            << ",\"inputCost\":"
            << debug_gt::fmtDouble(pb.inputCost, 2)
            << ",\"totalValue\":"
            << debug_gt::fmtDouble(pb.totalValue, 2)
            << ",\"dasChargeAfter\":"
            << pb.dasChargeAfter
            << ",\"linesCleared\":"
            << pb.linesCleared
            << "}";
      }
    }
    out << "}}";
  }
  out << "]}";
  return out.str();
}

} // namespace debug_pre

#endif
