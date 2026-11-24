#ifndef TABLEBASE_SERVER_ROUTES_HPP
#define TABLEBASE_SERVER_ROUTES_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "tablebase_io.hpp"
#include "tiny_nn.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace tb {

struct HttpRouteResult {
  int status = 200;
  std::string contentType = "application/json; charset=utf-8";
  std::string body;
};

struct ServerContext {
  MappedTablebase tablebase;
  MappedNextBoxTablebase nextBoxTablebase;
  TinyNextBoxNet nn;
  std::string configuredTbPath;
  std::string configuredNnPath;
  EvalWeights weights{};

  TablebaseFormat activeFormat() const noexcept {
    if (nextBoxTablebase.isLoaded()) {
      return TablebaseFormat::NextBoxV2;
    }
    if (tablebase.isLoaded()) {
      return TablebaseFormat::NnbV1;
    }
    return TablebaseFormat::Unknown;
  }

  bool isAnyTablebaseLoaded() const noexcept { return activeFormat() != TablebaseFormat::Unknown; }

  bool isNnLoaded() const noexcept { return nn.isLoaded(); }

  const char *activeRngModeName() const noexcept {
    const TablebaseFormat fmt = activeFormat();
    if (fmt == TablebaseFormat::NextBoxV2) {
      return rngModeName(nextBoxTablebase.rngMode());
    }
    if (fmt == TablebaseFormat::NnbV1) {
      return "uniform";
    }
    return nullptr;
  }

  const char *activeNnRngModeName() const noexcept {
    return nn.isLoaded() ? rngModeName(nn.rngMode) : nullptr;
  }

  // RNG distribution used for NN 2-ply expectations and E[V_hat(b)]: matches the active Next-Box
  // tablebase when one is loaded, otherwise falls back to the network's own trained RNG mode.
  RngMode effectiveNnRngMode() const noexcept {
    if (activeFormat() == TablebaseFormat::NextBoxV2) {
      return nextBoxTablebase.rngMode();
    }
    return nn.isLoaded() ? nn.rngMode : RngMode::Uniform;
  }

  uint64_t activeBoardCount() const noexcept {
    const TablebaseFormat fmt = activeFormat();
    if (fmt == TablebaseFormat::NextBoxV2) {
      return nextBoxTablebase.size();
    }
    if (fmt == TablebaseFormat::NnbV1) {
      return tablebase.size();
    }
    return 0;
  }

  double activeEmptyBoardValue() const noexcept {
    const TablebaseFormat fmt = activeFormat();
    if (fmt == TablebaseFormat::NextBoxV2) {
      return nextBoxTablebase.emptyBoardValue();
    }
    if (fmt == TablebaseFormat::NnbV1) {
      return tablebase.emptyBoardValue();
    }
    return MISSING_BOARD_VALUE;
  }

  double activeBellmanResidual() const noexcept {
    const TablebaseFormat fmt = activeFormat();
    if (fmt == TablebaseFormat::NextBoxV2) {
      return nextBoxTablebase.bellmanResidual();
    }
    if (fmt == TablebaseFormat::NnbV1) {
      return tablebase.bellmanResidual();
    }
    return 0.0;
  }

  struct StagedTablebase {
    TablebaseFormat format = TablebaseFormat::Unknown;
    MappedTablebase nnb;
    MappedNextBoxTablebase nb;
  };

  static bool stageTablebase(const std::string &path, StagedTablebase &staged,
                             std::string *errOut = nullptr) {
    TablebaseFormat fmt = TablebaseFormat::Unknown;
    if (!detectTablebaseFormat(path, fmt, errOut)) {
      return false;
    }
    if (fmt == TablebaseFormat::NnbV1) {
      if (!staged.nnb.open(path, errOut)) {
        return false;
      }
      staged.format = fmt;
      return true;
    }
    if (fmt == TablebaseFormat::NextBoxV2) {
      if (!staged.nb.open(path, errOut)) {
        return false;
      }
      staged.format = fmt;
      return true;
    }
    if (errOut != nullptr) {
      *errOut = "Unrecognized tablebase format";
    }
    return false;
  }

  void commitTablebase(StagedTablebase &staged) noexcept {
    if (staged.format == TablebaseFormat::NnbV1) {
      nextBoxTablebase.close();
      tablebase = std::move(staged.nnb);
    } else if (staged.format == TablebaseFormat::NextBoxV2) {
      tablebase.close();
      nextBoxTablebase = std::move(staged.nb);
    }
  }

  // Opens either an SRTBV001 (NNB) or SRTBV002 (Next-Box) tablebase file atomically so any open
  // failure leaves the previously loaded tablebase intact.
  bool openTablebase(const std::string &path, std::string *errOut = nullptr) {
    StagedTablebase staged;
    if (!stageTablebase(path, staged, errOut)) {
      return false;
    }
    commitTablebase(staged);
    return true;
  }

  // Loads an SRNNV001 neural evaluator atomically so any failure leaves the existing NN intact.
  bool openNn(const std::string &path, std::string *errOut = nullptr) {
    return nn.load(path, errOut);
  }

  // Reloads all configured assets (TB and/or NN) transactionally: if either fails, all previously
  // loaded state remains untouched.
  bool reloadConfigured(std::string *errOut = nullptr) {
    StagedTablebase stagedTb;
    if (!configuredTbPath.empty() && !stageTablebase(configuredTbPath, stagedTb, errOut)) {
      return false;
    }

    TinyNextBoxNet stagedNn;
    if (!configuredNnPath.empty() && !stagedNn.load(configuredNnPath, errOut)) {
      return false;
    }

    if (!configuredTbPath.empty()) {
      commitTablebase(stagedTb);
    }
    if (!configuredNnPath.empty()) {
      nn = std::move(stagedNn);
    }
    return true;
  }

  void closeAll() noexcept {
    tablebase.close();
    nextBoxTablebase.close();
    nn = TinyNextBoxNet{};
  }
};

namespace detail {

inline std::string jsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (unsigned char c : s) {
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
      if (c < 0x20) {
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
        out += buf;
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

inline HttpRouteResult jsonErrorResponse(int status, std::string_view message) {
  return {status, "application/json; charset=utf-8",
          std::string("{\"error\":\"") + jsonEscape(message) + "\"}"};
}

// Formats finite doubles with 17 significant digits (std::numeric_limits<double>::max_digits10),
// guaranteeing exact IEEE-754 double round-trip parity between C++ /state-info and JS /moves.
inline std::string fmtDouble(double v) {
  if (!std::isfinite(v)) {
    return "null";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return std::string(buf);
}

inline std::string fmtSci(double v) {
  if (!std::isfinite(v)) {
    return "null";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6e", v);
  return std::string(buf);
}

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

// Strictly decodes URL percent-encoding; rejects truncated '%', non-hex digits, and '%00'.
inline bool urlDecodeStrict(std::string_view raw, std::string &out) {
  out.clear();
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '%') {
      if (i + 2 >= raw.size()) {
        return false;
      }
      const int hi = hexNibble(raw[i + 1]);
      const int lo = hexNibble(raw[i + 2]);
      if (hi < 0 || lo < 0) {
        return false;
      }
      const char decoded = static_cast<char>((hi << 4) | lo);
      if (decoded == '\0') {
        return false;
      }
      out.push_back(decoded);
      i += 2;
    } else if (c == '+') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return true;
}

// Parses "k1=v1&k2=v2" strictly: rejects empty keys, missing '=', or duplicate parameter names.
inline bool parseQueryStrict(std::string_view query, std::map<std::string, std::string> &outParams,
                             std::string &errMsg) {
  outParams.clear();
  if (query.empty()) {
    return true;
  }
  size_t pos = 0;
  while (pos <= query.size()) {
    const size_t amp = query.find('&', pos);
    const std::string_view part =
        (amp == std::string_view::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
    if (part.empty()) {
      errMsg = "Empty query parameter segment";
      return false;
    }
    const size_t eq = part.find('=');
    if (eq == std::string_view::npos
        || eq == 0
        || part.find('=', eq + 1) != std::string_view::npos) {
      errMsg = "Malformed query parameter (expected key=value)";
      return false;
    }
    std::string key;
    std::string val;
    if (!urlDecodeStrict(part.substr(0, eq), key) || !urlDecodeStrict(part.substr(eq + 1), val)) {
      errMsg = "Malformed percent-encoding in query string";
      return false;
    }
    if (outParams.find(key) != outParams.end()) {
      errMsg = "Duplicate query parameter: " + key;
      return false;
    }
    outParams.emplace(std::move(key), std::move(val));
    if (amp == std::string_view::npos) {
      break;
    }
    pos = amp + 1;
  }
  return true;
}

// Accepts either 1..10 hex digits OR an exact 40-character 0/1/./X cell string (no whitespace).
inline bool parseBoardParamStrict(std::string_view text, uint64_t &outBoard) {
  if (text.size() == BOARD_CELLS) {
    for (char c : text) {
      if (c != '.' && c != '0' && c != 'X' && c != 'x' && c != '1' && c != '#') {
        return false;
      }
    }
    return parseBoardAscii(text, outBoard);
  }
  if (text.size() >= 1 && text.size() <= 10) {
    return parseBoardHex(text, outBoard);
  }
  return false;
}

// Accepts either '0'..'6' or 'I','O','L','J','T','S','Z'.
inline bool parsePieceParamStrict(std::string_view text, int &outPiece) {
  if (text.size() != 1) {
    return false;
  }
  const char c = text[0];
  if (c >= '0' && c <= '6') {
    outPiece = c - '0';
    return true;
  }
  const int p = parsePieceChar(c);
  if (p >= 0) {
    outPiece = p;
    return true;
  }
  return false;
}

inline bool parsePlyParamStrict(const std::map<std::string, std::string> &params, int &outPly) {
  const auto it = params.find("ply");
  if (it == params.end()) {
    outPly = 1;
    return true;
  }
  if (it->second == "1") {
    outPly = 1;
    return true;
  }
  if (it->second == "2") {
    outPly = 2;
    return true;
  }
  return false;
}

inline double scoreTransitionByPly(uint64_t afterBoard, int linesCleared, int ply,
                                   const EvalWeights &w) {
  return (ply == 2) ? scoreTransition2Ply(afterBoard, linesCleared, w)
                    : scoreTransition1Ply(afterBoard, linesCleared, w);
}

inline double scoreMoveHeuristic(uint64_t afterBoard, int linesCleared, int nextPieceIdx, int ply,
                                 const EvalWeights &w) {
  return (nextPieceIdx >= 0)
             ? scoreNextBoxHeuristicTransition(afterBoard, linesCleared, nextPieceIdx, w)
             : scoreTransitionByPly(afterBoard, linesCleared, ply, w);
}

inline const char *tablebaseFormatName(TablebaseFormat fmt) noexcept {
  switch (fmt) {
  case TablebaseFormat::NnbV1:
    return "nnb";
  case TablebaseFormat::NextBoxV2:
    return "nextbox";
  case TablebaseFormat::Unknown:
  default:
    return "none";
  }
}

inline void appendJsonNullableStr(std::string &json, const char *s) {
  if (s == nullptr) {
    json += "null";
  } else {
    json.push_back('"');
    json += jsonEscape(s);
    json.push_back('"');
  }
}

inline void appendJsonNullableFloatArray(std::string &json, const float *vals, int count,
                                         bool present) {
  if (!present) {
    json += "null";
    return;
  }
  json.push_back('[');
  for (int i = 0; i < count; ++i) {
    if (i > 0) {
      json.push_back(',');
    }
    json += fmtDouble(vals[i]);
  }
  json.push_back(']');
}

inline double expectedOpeningValue(const float vals[NUM_PIECES], RngMode mode) noexcept {
  double sum = 0.0;
  for (int c = 0; c < NUM_PIECES; ++c) {
    sum += openingPieceProb(mode, c) * static_cast<double>(vals[c]);
  }
  return sum;
}

inline void appendTablebaseStatusFields(std::string &json, const ServerContext &ctx,
                                        bool includeRngMode) {
  const TablebaseFormat fmt = ctx.activeFormat();
  json += ",\"tablebaseLoaded\":";
  json += (fmt != TablebaseFormat::Unknown ? "true" : "false");
  json += ",\"tablebaseFormat\":\"";
  json += tablebaseFormatName(fmt);
  json.push_back('"');
  if (includeRngMode) {
    json += ",\"rngMode\":";
    appendJsonNullableStr(json, ctx.activeRngModeName());
  }
}

inline void appendNnStatusFields(std::string &json, const ServerContext &ctx, bool includeRngMode,
                                 bool includePingDetails = false) {
  const bool loaded = ctx.isNnLoaded();
  json += ",\"nnLoaded\":";
  json += (loaded ? "true" : "false");
  if (includePingDetails) {
    json += ",\"configuredNnPath\":\"" + jsonEscape(ctx.configuredNnPath) + "\"";
  }
  if (includeRngMode) {
    json += ",\"nnRngMode\":";
    appendJsonNullableStr(json, ctx.activeNnRngModeName());
  }
  if (includePingDetails) {
    json += ",\"nnHidden\":";
    if (loaded) {
      json += "[" + std::to_string(ctx.nn.h1) + "," + std::to_string(ctx.nn.h2) + "]";
    } else {
      json += "null";
    }
    json += ",\"nnParamCount\":" + std::to_string(loaded ? ctx.nn.params.size() : size_t{0});
  }
}

inline void appendOptionalNextFields(std::string &json, int nextPieceIdx) {
  if (nextPieceIdx >= 0) {
    json += ",\"nextPiece\":" + std::to_string(nextPieceIdx);
    json += ",\"nextPieceChar\":\"";
    json.push_back(PIECE_CHARS[nextPieceIdx]);
    json.push_back('"');
  }
}

struct ParsedBoardQuery {
  uint64_t board = 0;
  int pieceIdx = -1;
  int nextPieceIdx = -1;
  int ply = 1;
};

inline bool parseBoardQueryParams(const std::map<std::string, std::string> &params, bool allowPiece,
                                  bool requireNext, ParsedBoardQuery &out, std::string &errOut) {
  for (const auto &kv : params) {
    const bool valid = kv.first == "board"
                       || kv.first == "ply"
                       || kv.first == "next"
                       || (allowPiece && kv.first == "piece");
    if (!valid) {
      errOut = "Unknown query parameter: " + kv.first;
      return false;
    }
  }
  const auto bIt = params.find("board");
  const auto pIt = params.find("piece");
  if (bIt == params.end() || (allowPiece && pIt == params.end())) {
    errOut = allowPiece ? "Missing required 'board' and/or 'piece' parameter"
                        : "Missing required 'board' parameter";
    return false;
  }
  if (!parseBoardParamStrict(bIt->second, out.board)) {
    errOut = "Invalid 'board' (expected 1-10 hex chars or 40 board chars)";
    return false;
  }
  if (allowPiece && !parsePieceParamStrict(pIt->second, out.pieceIdx)) {
    errOut = "Invalid 'piece' (expected 0..6 or I,O,L,J,T,S,Z)";
    return false;
  }
  const auto nIt = params.find("next");
  if (nIt != params.end()) {
    if (!parsePieceParamStrict(nIt->second, out.nextPieceIdx)) {
      errOut = "Invalid 'next' (expected 0..6 or I,O,L,J,T,S,Z)";
      return false;
    }
  } else if (requireNext) {
    errOut = "Missing required 'next' parameter when a Next-Box tablebase is loaded";
    return false;
  }
  if (!parsePlyParamStrict(params, out.ply)) {
    errOut = "Invalid 'ply' (expected 1 or 2)";
    return false;
  }
  return true;
}

// Evaluates a single hybrid 2-ply move (curPiece placed -> afterBoard, then optimal nextPiece
// placement scored by NNB tablebase with V = 0 outside S). Sets outAnyLeafInTb if any 2-ply leaf
// lands in nnbTb, matching chooseNextBoxHybridMove's fallback criterion.
inline void evalHybridNnbMove(uint64_t afterBoard, int linesCleared, int nextPieceIdx,
                              const MappedTablebase &nnbTb, TransitionList &tl2, double &outTbQ,
                              bool &outBestLeafInTb, bool &outAnyLeafInTb, float &outAfterVal) {
  const double r1 = static_cast<double>(linesCleared);
  outTbQ = r1;
  outBestLeafInTb = false;
  outAnyLeafInTb = false;
  outAfterVal = 0.0f;
  if (!nnbTb.isLoaded()) {
    return;
  }
  generateTransitions(afterBoard, nextPieceIdx, tl2);
  for (int j = 0; j < tl2.size(); ++j) {
    float leafVal = 0.0f;
    const bool leafInTb = nnbTb.lookup(tl2[j].afterBoard, leafVal);
    outAnyLeafInTb = outAnyLeafInTb || leafInTb;
    const double q = r1
                     + static_cast<double>(tl2[j].linesCleared)
                     + (leafInTb ? static_cast<double>(leafVal) : 0.0);
    if (j == 0 || q > outTbQ || (q == outTbQ && leafInTb && !outBestLeafInTb)) {
      outTbQ = q;
      outBestLeafInTb = leafInTb;
      outAfterVal = leafVal;
    }
  }
}

inline HttpRouteResult handlePing(const std::map<std::string, std::string> &params,
                                  const ServerContext &ctx) {
  if (!params.empty()) {
    return jsonErrorResponse(400, "/ping does not accept query parameters");
  }
  const bool tbLoaded = ctx.isAnyTablebaseLoaded();
  std::string json = "{\"ok\":true";
  appendTablebaseStatusFields(json, ctx, /*includeRngMode=*/true);
  json += ",\"configuredPath\":\"" + jsonEscape(ctx.configuredTbPath) + "\"";
  json += ",\"boardCount\":" + std::to_string(ctx.activeBoardCount());
  const double ev = ctx.activeEmptyBoardValue();
  json += ",\"emptyBoardValue\":" + (tbLoaded && ev >= 0.0 ? fmtDouble(ev) : "null");
  json += ",\"bellmanResidual\":" + (tbLoaded ? fmtSci(ctx.activeBellmanResidual()) : "null");
  appendNnStatusFields(json, ctx, /*includeRngMode=*/true, /*includePingDetails=*/true);
  json.push_back('}');
  return {200, "application/json; charset=utf-8", std::move(json)};
}

inline HttpRouteResult handleReload(const std::map<std::string, std::string> &params,
                                    ServerContext &ctx) {
  if (!params.empty()) {
    return jsonErrorResponse(400, "/reload does not accept query parameters");
  }
  if (ctx.configuredTbPath.empty() && ctx.configuredNnPath.empty()) {
    return jsonErrorResponse(
        400, "No tablebase or neural evaluator path was configured on server startup");
  }
  std::string err;
  if (!ctx.reloadConfigured(&err)) {
    return jsonErrorResponse(500, err);
  }
  return handlePing(params, ctx);
}

inline HttpRouteResult handleMoves(const std::map<std::string, std::string> &params,
                                   const ServerContext &ctx) {
  const bool tbLoaded = ctx.isAnyTablebaseLoaded();
  const bool isNextBoxTb = (ctx.activeFormat() == TablebaseFormat::NextBoxV2);
  const bool nnLoaded = ctx.isNnLoaded();
  const RngMode nnRngMode = ctx.effectiveNnRngMode();

  ParsedBoardQuery q{};
  std::string err;
  if (!parseBoardQueryParams(params, /*allowPiece=*/true, /*requireNext=*/isNextBoxTb, q, err)) {
    return jsonErrorResponse(400, err);
  }

  float boardTbVal = 0.0f;
  const bool boardInTb = isNextBoxTb
                             ? ctx.nextBoxTablebase.lookupPiece(q.board, q.pieceIdx, boardTbVal)
                             : ctx.tablebase.lookup(q.board, boardTbVal);
  const double nnBoardVal =
      nnLoaded ? static_cast<double>(ctx.nn.evalPiece(q.board, q.pieceIdx)) : 0.0;

  PlacementList placements;
  generatePlacements(q.board, q.pieceIdx, placements);

  std::string json;
  json.reserve(512 + static_cast<size_t>(placements.size()) * 320);
  json += "{\"boardHex\":\"" + formatBoardHex(q.board) + "\"";
  json += ",\"isCanonical\":";
  json += (isCanonicalBoard(q.board) ? "true" : "false");
  json += ",\"piece\":" + std::to_string(q.pieceIdx);
  json += ",\"pieceChar\":\"";
  json.push_back(PIECE_CHARS[q.pieceIdx]);
  json.push_back('"');
  appendOptionalNextFields(json, q.nextPieceIdx);
  json += ",\"heuristicPly\":" + std::to_string(q.ply);
  appendTablebaseStatusFields(json, ctx, /*includeRngMode=*/false);
  json += ",\"inTablebase\":";
  json += (boardInTb ? "true" : "false");
  json += ",\"boardValue\":";
  json += (boardInTb ? fmtDouble(boardTbVal) : "null");
  appendNnStatusFields(json, ctx, /*includeRngMode=*/false);
  json += ",\"nnBoardValue\":";
  json += (nnLoaded ? fmtDouble(nnBoardVal) : "null");
  json += ",\"moves\":[";

  TransitionList tl2;
  const bool scoreNnMoves = nnLoaded && q.nextPieceIdx >= 0;
  for (int i = 0; i < placements.size(); ++i) {
    if (i > 0)
      json.push_back(',');
    const Placement &m = placements[i];
    const double heurQ =
        scoreMoveHeuristic(m.afterBoard, m.linesCleared, q.nextPieceIdx, q.ply, ctx.weights);
    float afterVal = 0.0f;
    bool afterInTb = false;
    double tbQ = 0.0;

    if (isNextBoxTb) {
      afterInTb = ctx.nextBoxTablebase.lookupPiece(m.afterBoard, q.nextPieceIdx, afterVal);
      tbQ = static_cast<double>(m.linesCleared) + (afterInTb ? static_cast<double>(afterVal) : 0.0);
    } else if (q.nextPieceIdx >= 0) {
      bool anyLeafInTb = false;
      evalHybridNnbMove(m.afterBoard, m.linesCleared, q.nextPieceIdx, ctx.tablebase, tl2, tbQ,
                        afterInTb, anyLeafInTb, afterVal);
      // Reflect whether any 2-ply leaf hit TB so JS fallback matches chooseNextBoxHybridMove.
      afterInTb = afterInTb || anyLeafInTb;
    } else {
      afterInTb = ctx.tablebase.lookup(m.afterBoard, afterVal);
      tbQ = static_cast<double>(m.linesCleared) + (afterInTb ? static_cast<double>(afterVal) : 0.0);
    }

    const double nnQ1 =
        scoreNnMoves ? scoreNextBoxNn1Ply(m.afterBoard, m.linesCleared, q.nextPieceIdx, ctx.nn)
                     : 0.0;
    const double nnQ2 = scoreNnMoves ? scoreNextBoxNn2Ply(m.afterBoard, m.linesCleared,
                                                          q.nextPieceIdx, ctx.nn, nnRngMode)
                                     : 0.0;

    json += "{\"index\":" + std::to_string(i);
    json += ",\"rot\":" + std::to_string(m.rot);
    json += ",\"x\":" + std::to_string(m.x);
    json += ",\"lockY\":" + std::to_string(m.lockY);
    json += ",\"tuckInput\":\"";
    json.push_back(m.tuckInput);
    json.push_back('"');
    json += ",\"lockedMaskHex\":\"" + formatBoardHex(m.lockedMask) + "\"";
    json += ",\"afterBoardHex\":\"" + formatBoardHex(m.afterBoard) + "\"";
    json += ",\"linesCleared\":" + std::to_string(m.linesCleared);
    json += ",\"afterHoles\":" + std::to_string(countHoles(m.afterBoard));
    json += ",\"heuristicScore\":" + fmtDouble(heurQ);
    json += ",\"afterInTablebase\":";
    json += (afterInTb ? "true" : "false");
    json += ",\"afterBoardValue\":";
    json += (afterInTb ? fmtDouble(afterVal) : "null");
    json += ",\"tablebaseQ\":";
    json += (tbLoaded ? fmtDouble(tbQ) : "null");
    json += ",\"nnQ1\":";
    json += (scoreNnMoves ? fmtDouble(nnQ1) : "null");
    json += ",\"nnQ2\":";
    json += (scoreNnMoves ? fmtDouble(nnQ2) : "null");
    json.push_back('}');
  }
  json += "]}";
  return {200, "application/json; charset=utf-8", std::move(json)};
}

inline void appendStateInfoPieceEntry(std::string &json, uint64_t board, int pieceIdx,
                                      int nextPieceIdx, int ply, bool boardInTb,
                                      const float boardVals[NUM_PIECES],
                                      const float nnBoardVals[NUM_PIECES], const ServerContext &ctx,
                                      TransitionList &tl, TransitionList &tl2,
                                      double &expectedBackup) {
  const bool tbLoaded = ctx.isAnyTablebaseLoaded();
  const bool isNextBoxTb = (ctx.activeFormat() == TablebaseFormat::NextBoxV2);
  const bool nnLoaded = ctx.isNnLoaded();
  const bool scoreNnMoves = nnLoaded && nextPieceIdx >= 0;
  const RngMode nnRngMode = ctx.effectiveNnRngMode();

  generateTransitions(board, pieceIdx, tl);

  double bestHeur = 0.0;
  uint64_t bestHeurAfter = 0;
  double bestTbQ = 0.0;
  double bestTbTieHeur = 0.0;
  uint64_t bestTbAfter = 0;
  double bestNnQ1 = 0.0;
  double bestNnQ2 = 0.0;
  uint64_t bestNn2After = 0;
  bool anyInTb = false;
  bool allInTb = !tl.empty();
  double pieceBackup = 0.0;

  double maxQPerNext[NUM_PIECES] = {};
  float childRow[NUM_PIECES] = {};
  double nnbBestQ = 0.0;

  for (int i = 0; i < tl.size(); ++i) {
    const Transition &t = tl[i];
    const double hScore =
        scoreMoveHeuristic(t.afterBoard, t.linesCleared, nextPieceIdx, ply, ctx.weights);
    if (i == 0 || hScore > bestHeur) {
      bestHeur = hScore;
      bestHeurAfter = t.afterBoard;
    }
    if (scoreNnMoves) {
      const double q1 = scoreNextBoxNn1Ply(t.afterBoard, t.linesCleared, nextPieceIdx, ctx.nn);
      const double q2 =
          scoreNextBoxNn2Ply(t.afterBoard, t.linesCleared, nextPieceIdx, ctx.nn, nnRngMode);
      if (i == 0 || q1 > bestNnQ1) {
        bestNnQ1 = q1;
      }
      if (i == 0 || q2 > bestNnQ2) {
        bestNnQ2 = q2;
        bestNn2After = t.afterBoard;
      }
    }

    if (!tbLoaded) {
      continue;
    }

    bool updateBestTb = false;
    double q = 0.0;
    if (isNextBoxTb) {
      const bool inTb = ctx.nextBoxTablebase.lookup(t.afterBoard, childRow);
      anyInTb = anyInTb || inTb;
      allInTb = allInTb && inTb;
      const double rew = static_cast<double>(t.linesCleared);
      for (int n = 0; n < NUM_PIECES; ++n) {
        const double qn = rew + (inTb ? static_cast<double>(childRow[n]) : 0.0);
        if (i == 0 || qn > maxQPerNext[n]) {
          maxQPerNext[n] = qn;
        }
      }
      if (nextPieceIdx >= 0) {
        q = rew + (inTb ? static_cast<double>(childRow[nextPieceIdx]) : 0.0);
        updateBestTb = true;
      }
    } else {
      bool inTb = false;
      float childVal = 0.0f;
      if (nextPieceIdx >= 0) {
        bool anyLeafInTb = false;
        evalHybridNnbMove(t.afterBoard, t.linesCleared, nextPieceIdx, ctx.tablebase, tl2, q, inTb,
                          anyLeafInTb, childVal);
        anyInTb = anyInTb || anyLeafInTb;
        allInTb = allInTb && inTb;
        float nnbVal = 0.0f;
        const bool nnbIn = ctx.tablebase.lookup(t.afterBoard, nnbVal);
        const double nnbQ =
            static_cast<double>(t.linesCleared) + (nnbIn ? static_cast<double>(nnbVal) : 0.0);
        if (i == 0 || nnbQ > nnbBestQ) {
          nnbBestQ = nnbQ;
        }
      } else {
        inTb = ctx.tablebase.lookup(t.afterBoard, childVal);
        q = static_cast<double>(t.linesCleared) + (inTb ? static_cast<double>(childVal) : 0.0);
        anyInTb = anyInTb || inTb;
        allInTb = allInTb && inTb;
      }
      updateBestTb = true;
    }

    if (updateBestTb && (i == 0 || q > bestTbQ || (q == bestTbQ && hScore > bestTbTieHeur))) {
      bestTbQ = q;
      bestTbTieHeur = hScore;
      bestTbAfter = t.afterBoard;
    }
  }

  if (isNextBoxTb) {
    const RngMode mode = ctx.nextBoxTablebase.rngMode();
    if (!tl.empty()) {
      for (int n = 0; n < NUM_PIECES; ++n) {
        pieceBackup += pieceTransitionProb(mode, pieceIdx, n) * maxQPerNext[n];
      }
    }
    expectedBackup += openingPieceProb(mode, pieceIdx) * pieceBackup;
    if (nextPieceIdx < 0) {
      bestTbQ = pieceBackup;
    }
  } else {
    expectedBackup +=
        ((tbLoaded && nextPieceIdx >= 0) ? nnbBestQ : bestTbQ) / static_cast<double>(NUM_PIECES);
  }

  // Only flag TB disagreements when at least one legal move lands in TB; on all-OUT boards
  // the TB policy has no entry and delegates completely to its fallback.
  const bool canComparePolicy = tbLoaded && anyInTb && (!isNextBoxTb || nextPieceIdx >= 0);
  const bool disagree = canComparePolicy && !tl.empty() && (bestTbAfter != bestHeurAfter);
  const bool nnDisagree =
      canComparePolicy && scoreNnMoves && !tl.empty() && (bestTbAfter != bestNn2After);

  json += "{\"piece\":" + std::to_string(pieceIdx);
  json += ",\"pieceChar\":\"";
  json.push_back(PIECE_CHARS[pieceIdx]);
  json.push_back('"');
  json += ",\"numTransitions\":" + std::to_string(tl.size());
  json += ",\"bestHeuristicScore\":";
  json += (tl.empty() ? "null" : fmtDouble(bestHeur));
  json += ",\"bestTablebaseQ\":";
  json += ((!tbLoaded || tl.empty()) ? "null" : fmtDouble(bestTbQ));
  if (isNextBoxTb) {
    json += ",\"tablebaseValue\":";
    json += (boardInTb ? fmtDouble(boardVals[pieceIdx]) : "null");
    json += ",\"bellmanBackupValue\":" + fmtDouble(pieceBackup);
  }
  json += ",\"nnValue\":";
  json += (nnLoaded ? fmtDouble(nnBoardVals[pieceIdx]) : "null");
  json += ",\"nnError\":";
  json +=
      (nnLoaded && isNextBoxTb && boardInTb ? fmtDouble(static_cast<double>(nnBoardVals[pieceIdx])
                                                        - static_cast<double>(boardVals[pieceIdx]))
                                            : "null");
  json += ",\"bestNnQ1\":";
  json += ((scoreNnMoves && !tl.empty()) ? fmtDouble(bestNnQ1) : "null");
  json += ",\"bestNnQ2\":";
  json += ((scoreNnMoves && !tl.empty()) ? fmtDouble(bestNnQ2) : "null");
  json += ",\"anyInTablebase\":";
  json += (anyInTb ? "true" : "false");
  json += ",\"allInTablebase\":";
  json += ((tbLoaded && allInTb) ? "true" : "false");
  json += ",\"disagree\":";
  json += (disagree ? "true" : "false");
  json += ",\"nnDisagree\":";
  json += (nnDisagree ? "true" : "false");
  json.push_back('}');
}

inline HttpRouteResult handleStateInfo(const std::map<std::string, std::string> &params,
                                       const ServerContext &ctx) {
  ParsedBoardQuery q{};
  std::string err;
  if (!parseBoardQueryParams(params, /*allowPiece=*/false, /*requireNext=*/false, q, err)) {
    return jsonErrorResponse(400, err);
  }

  const bool tbLoaded = ctx.isAnyTablebaseLoaded();
  const bool isNextBoxTb = (ctx.activeFormat() == TablebaseFormat::NextBoxV2);
  const bool nnLoaded = ctx.isNnLoaded();

  int heights[BOARD_WIDTH];
  getColumnHeights(q.board, heights);
  const HeuristicBreakdown hb = evaluateBoardBreakdown(q.board, ctx.weights);

  float boardVals[NUM_PIECES] = {};
  double boardTbVal = 0.0;
  bool boardInTb = false;
  if (isNextBoxTb) {
    boardInTb = ctx.nextBoxTablebase.lookup(q.board, boardVals);
    if (boardInTb) {
      boardTbVal = expectedOpeningValue(boardVals, ctx.nextBoxTablebase.rngMode());
    }
  } else if (tbLoaded) {
    float scalarVal = 0.0f;
    boardInTb = ctx.tablebase.lookup(q.board, scalarVal);
    boardTbVal = static_cast<double>(scalarVal);
  }

  float nnBoardVals[NUM_PIECES] = {};
  double nnBoardVal = 0.0;
  if (nnLoaded) {
    ctx.nn.evalAll(q.board, nnBoardVals);
    nnBoardVal = expectedOpeningValue(nnBoardVals, ctx.effectiveNnRngMode());
  }

  std::string json;
  json.reserve(3072);
  json += "{\"boardHex\":\"" + formatBoardHex(q.board) + "\"";
  json += ",\"isCanonical\":";
  json += (isCanonicalBoard(q.board) ? "true" : "false");
  json += ",\"heuristicPly\":" + std::to_string(q.ply);
  appendOptionalNextFields(json, q.nextPieceIdx);
  json += ",\"popcount\":" + std::to_string(popcount64(q.board));
  json += ",\"maxHeight\":" + std::to_string(getMaxHeight(q.board));
  json += ",\"holes\":" + std::to_string(hb.holes);
  json += ",\"overhangs\":" + std::to_string(hb.overhangs);
  json += ",\"sealedHoles\":" + std::to_string(hb.sealedHoles);
  json += ",\"bumpiness\":" + std::to_string(hb.bumpiness);
  json += ",\"heuristicBoardScore\":" + fmtDouble(hb.boardScore);
  json += ",\"heights\":[";
  for (int c = 0; c < BOARD_WIDTH; ++c) {
    if (c > 0)
      json.push_back(',');
    json += std::to_string(heights[c]);
  }
  json.push_back(']');
  appendTablebaseStatusFields(json, ctx, /*includeRngMode=*/true);
  json += ",\"inTablebase\":";
  json += (boardInTb ? "true" : "false");
  json += ",\"boardValue\":";
  json += (boardInTb ? fmtDouble(boardTbVal) : "null");
  if (isNextBoxTb) {
    json += ",\"boardValues\":";
    appendJsonNullableFloatArray(json, boardVals, NUM_PIECES, boardInTb);
  }
  appendNnStatusFields(json, ctx, /*includeRngMode=*/true);
  json += ",\"nnBoardValue\":";
  json += (nnLoaded ? fmtDouble(nnBoardVal) : "null");
  json += ",\"nnBoardValues\":";
  appendJsonNullableFloatArray(json, nnBoardVals, NUM_PIECES, nnLoaded);
  json += ",\"perPiece\":[";

  double expectedBackup = 0.0;
  TransitionList tl;
  TransitionList tl2;
  for (int p = 0; p < NUM_PIECES; ++p) {
    if (p > 0)
      json.push_back(',');
    appendStateInfoPieceEntry(json, q.board, p, q.nextPieceIdx, q.ply, boardInTb, boardVals,
                              nnBoardVals, ctx, tl, tl2, expectedBackup);
  }

  json += "]";
  json += ",\"bellmanBackupValue\":";
  json += (tbLoaded ? fmtDouble(expectedBackup) : "null");
  json.push_back('}');
  return {200, "application/json; charset=utf-8", std::move(json)};
}

} // namespace detail

inline HttpRouteResult handleApiRequest(std::string_view path, std::string_view query,
                                        ServerContext &ctx) {
  std::map<std::string, std::string> params;
  std::string parseErr;
  if (!detail::parseQueryStrict(query, params, parseErr)) {
    return detail::jsonErrorResponse(400, parseErr);
  }

  if (path == "/ping") {
    return detail::handlePing(params, ctx);
  }
  if (path == "/reload") {
    return detail::handleReload(params, ctx);
  }
  if (path == "/moves") {
    return detail::handleMoves(params, ctx);
  }
  if (path == "/state-info") {
    return detail::handleStateInfo(params, ctx);
  }
  return detail::jsonErrorResponse(404, std::string("Unknown route: ") + std::string(path));
}

} // namespace tb

#endif // TABLEBASE_SERVER_ROUTES_HPP
