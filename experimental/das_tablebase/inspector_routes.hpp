#ifndef DAS_TABLEBASE_INSPECTOR_ROUTES_HPP
#define DAS_TABLEBASE_INSPECTOR_ROUTES_HPP

// HTTP route handlers for the 20x10 DAS Tablebase + NNUE browser inspector.
// Supports /ping, /reload (transactional), /state-info, /moves, and /step-disagreement.

#include "eval_adapter.hpp"
#include "nnue_20row.hpp"
#include "tablebase_io_20row.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace das {

struct HttpRouteResult {
  int status = 200;
  std::string contentType = "application/json; charset=utf-8";
  std::string body;
};

struct InspectorContext {
  MappedDasTablebase tb;
  TinyDasNet nn;
  BackendContexts bc = BackendContexts::make10Hz();
  std::string configuredTbPath;
  std::string configuredNnPath;
  int level = 19;
  AggressionLevel aggro = AGGRO;
  double defaultMargin = 0.25;
  int defaultOutPly = 2;
  double defaultGamma = 0.995;

  bool isTbLoaded() const noexcept { return tb.isLoaded(); }
  bool isNnLoaded() const noexcept { return nn.isLoaded(); }
  int activeLevel() const noexcept { return tb.isLoaded() ? tb.level() : level; }
  int activeGravity() const noexcept { return getGravity20(activeLevel()); }
  double activeGamma() const noexcept { return tb.isLoaded() ? tb.gamma() : defaultGamma; }
  RngMode activeRngMode() const noexcept {
    if (tb.isLoaded())
      return tb.rngMode();
    if (nn.isLoaded())
      return nn.rngMode;
    return RngMode::Nes;
  }

  // Transactional reload: stages both TB and NN before committing either so any error preserves
  // the currently live state cleanly.
  bool reloadConfigured(std::string *errOut = nullptr) {
    MappedDasTablebase stagedTb;
    if (!configuredTbPath.empty() && !stagedTb.open(configuredTbPath, errOut)) {
      return false;
    }
    TinyDasNet stagedNn;
    if (!configuredNnPath.empty() && !stagedNn.load(configuredNnPath, errOut)) {
      return false;
    }
    if (!configuredTbPath.empty()) {
      tb = std::move(stagedTb);
    }
    if (!configuredNnPath.empty()) {
      nn = std::move(stagedNn);
    }
    return true;
  }
};

namespace inspector {

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

inline HttpRouteResult jsonError(int status, std::string_view msg) {
  return {status, "application/json; charset=utf-8",
          std::string("{\"error\":\"") + jsonEscape(msg) + "\"}"};
}

inline std::string fmtDouble(double v) {
  if (!std::isfinite(v))
    return "null";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return std::string(buf);
}

inline std::string fmtSci(double v) {
  if (!std::isfinite(v))
    return "null";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4e", v);
  return std::string(buf);
}

inline std::string formatBoard200(const uint16_t rows[BOARD_ROWS]) {
  std::string s(BOARD_ROWS * BOARD_WIDTH, '0');
  for (int r = 0; r < BOARD_ROWS; ++r) {
    const uint16_t mask = rows[r] & FULL_ROW_MASK;
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      if ((mask >> (9 - c)) & 1U) {
        s[static_cast<size_t>(r * BOARD_WIDTH + c)] = '1';
      }
    }
  }
  return s;
}

inline bool parseBoard200Strict(std::string_view text, uint16_t outRows[BOARD_ROWS]) {
  for (int r = 0; r < BOARD_ROWS; ++r)
    outRows[r] = 0;
  if (text == "empty") {
    return true;
  }
  if (text.size() != static_cast<size_t>(BOARD_ROWS * BOARD_WIDTH)) {
    return false;
  }
  for (int r = 0; r < BOARD_ROWS; ++r) {
    uint16_t row = 0;
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      const char ch = text[static_cast<size_t>(r * BOARD_WIDTH + c)];
      if (ch == '1' || ch == 'X' || ch == 'x' || ch == '#') {
        row |= static_cast<uint16_t>(1U << (9 - c));
      } else if (ch != '0' && ch != '.') {
        return false;
      }
    }
    outRows[r] = row;
  }
  return true;
}

inline bool parsePieceStrict(std::string_view text, int &outPiece) {
  if (text.size() != 1)
    return false;
  const char c = text[0];
  if (c >= '0' && c <= '6') {
    outPiece = c - '0';
    return true;
  }
  const char up = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  for (int p = 0; p < NUM_PIECES; ++p) {
    if (PIECE_CHARS[p] == up) {
      outPiece = p;
      return true;
    }
  }
  return false;
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

inline bool urlDecodeStrict(std::string_view raw, std::string &out) {
  out.clear();
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '%') {
      if (i + 2 >= raw.size())
        return false;
      const int hi = hexNibble(raw[i + 1]);
      const int lo = hexNibble(raw[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      const char decoded = static_cast<char>((hi << 4) | lo);
      if (decoded == '\0')
        return false;
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

inline bool parseQueryStrict(std::string_view query, std::map<std::string, std::string> &outParams,
                             std::string &errMsg) {
  outParams.clear();
  if (query.empty())
    return true;
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
    std::string k, v;
    if (!urlDecodeStrict(part.substr(0, eq), k) || !urlDecodeStrict(part.substr(eq + 1), v)) {
      errMsg = "Malformed percent-encoding in query string";
      return false;
    }
    if (outParams.find(k) != outParams.end()) {
      errMsg = "Duplicate query parameter: " + k;
      return false;
    }
    outParams.emplace(std::move(k), std::move(v));
    if (amp == std::string_view::npos)
      break;
    pos = amp + 1;
  }
  return true;
}

inline int countBoardHoles20(const uint16_t rows[BOARD_ROWS]) noexcept {
  uint32_t covered = 0U;
  int holes = 0;
  for (int r = 0; r < BOARD_ROWS; ++r) {
    const uint32_t row = static_cast<uint32_t>(rows[r]) & FULL_ROW_MASK;
    holes += __builtin_popcount((~row) & covered);
    covered |= row;
  }
  return holes;
}

inline int boardMaxHeight20(const uint16_t rows[BOARD_ROWS]) noexcept {
  for (int r = 0; r < BOARD_ROWS; ++r) {
    if ((rows[r] & FULL_ROW_MASK) != 0U) {
      return BOARD_ROWS - r;
    }
  }
  return 0;
}

inline double parseMarginParam(const std::map<std::string, std::string> &params, double fallback,
                               bool &ok) {
  ok = true;
  auto it = params.find("margin");
  if (it == params.end())
    return fallback;
  const std::string &s = it->second;
  if (s.empty() || std::isspace(static_cast<unsigned char>(s[0]))) {
    ok = false;
    return fallback;
  }
  if (s == "inf")
    return std::numeric_limits<double>::infinity();
  char *endPtr = nullptr;
  const double v = std::strtod(s.c_str(), &endPtr);
  if (endPtr != s.c_str() + s.size() || !std::isfinite(v) || v < 0.0) {
    ok = false;
    return fallback;
  }
  return v;
}

template <typename IntT> inline bool parseQueryIntStrict(std::string_view s, IntT &out) {
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

inline bool rejectUnknownParams(const std::map<std::string, std::string> &params,
                                std::initializer_list<const char *> allowed, std::string &badKey) {
  for (const auto &kv : params) {
    bool found = false;
    for (const char *a : allowed) {
      if (kv.first == a) {
        found = true;
        break;
      }
    }
    if (!found) {
      badKey = kv.first;
      return false;
    }
  }
  return true;
}

inline int parsePlyParam(const std::map<std::string, std::string> &params, int fallback, bool &ok) {
  ok = true;
  auto it = params.find("ply");
  if (it == params.end())
    return fallback;
  if (it->second == "1")
    return 1;
  if (it->second == "2")
    return 2;
  ok = false;
  return fallback;
}

inline void appendFloatArrayOrNull(std::string &json, const float *vals, bool present) {
  if (!present) {
    json += "null";
    return;
  }
  json.push_back('[');
  for (int i = 0; i < NUM_PIECES; ++i) {
    if (i > 0)
      json.push_back(',');
    json += fmtDouble(vals[i]);
  }
  json.push_back(']');
}

struct Eval2PlyMoveInfo {
  double score = 0.0;
  bool hasMove = false;
  int rot = 0;
  int x = 0;
  int y = 0;
  char tuckInput = '.';
  uint8_t lc = 0;
};

inline void appendPlacementCellsJson(std::string &json, int pieceIdx, int rot, int x, int y) {
  json += "[";
  const int xIdx = x + detail::PIECE_MASK_X_OFFSET;
  if (pieceIdx >= 0 && pieceIdx < NUM_PIECES && rot >= 0 && rot < 4 && xIdx >= 0 && xIdx < 16) {
    const uint64_t pieceMask = detail::PIECE_MASK_64.mask[pieceIdx][rot][xIdx];
    bool firstCell = true;
    for (int dr = 0; dr < 4; ++dr) {
      const int r = y + dr;
      if (r < 0 || r >= BOARD_ROWS)
        continue;
      const uint16_t rowBits = static_cast<uint16_t>((pieceMask >> (dr * 16)) & FULL_ROW_MASK);
      for (int c = 0; c < BOARD_WIDTH; ++c) {
        if ((rowBits >> (9 - c)) & 1U) {
          if (!firstCell)
            json.push_back(',');
          firstCell = false;
          json += "[" + std::to_string(r) + "," + std::to_string(c) + "]";
        }
      }
    }
  }
  json += "]";
}

// Evaluates 2-ply eval.cpp score and best implied 2nd placement for an after-board + nextPiece.
inline Eval2PlyMoveInfo eval2PlyForAfterBoard(const GameState &parentGs, const Board20 &afterBoard,
                                              int linesCleared, int nextPieceIdx,
                                              const InspectorContext &ctx,
                                              std::vector<LockPlacement> &m2Buf) {
  Eval2PlyMoveInfo out{};
  const EvalContext ctx1 = getEvalContext(parentGs, ctx.bc.lookup, false, ctx.aggro);
  const float r1 = getLineClearFactor(linesCleared, ctx1.weights, ctx1.shouldRewardLineClears,
                                      parentGs.lines, ctx1.lineoutAt29);
  GameState after1 = board20ToGameState(afterBoard, parentGs.level, parentGs.lines + linesCleared,
                                        ctx.bc, ctx.aggro);
  const EvalContext ctx2 = getEvalContext(after1, ctx.bc.lookup, false, ctx.aggro);
  m2Buf.clear();
  fastMoveSearch(after1, &PIECE_LIST[nextPieceIdx], ctx2.pieceRangeContext.inputFrameTimeline,
                 m2Buf);
  if (m2Buf.empty()) {
    out.score = static_cast<double>(-1e6f + r1);
    return out;
  }
  float best = FLOAT_MIN;
  bool found = false;
  for (const LockPlacement &p2 : m2Buf) {
    GameState after2 = advanceGameState(after1, p2, &ctx2);
    const int lc2 = after2.lines - after1.lines;
    after2.level = parentGs.level;
    const float sc = r1 + fastEval(after1, after2, p2, &ctx2, nullptr);
    if (!found || sc > best) {
      found = true;
      best = sc;
      out.hasMove = true;
      out.rot = p2.rotationIndex;
      out.x = p2.x;
      out.y = p2.y;
      out.tuckInput = p2.tuckInput;
      out.lc = static_cast<uint8_t>(std::max(0, lc2));
    }
  }
  out.score = static_cast<double>(best);
  return out;
}

inline HttpRouteResult handlePing(const std::map<std::string, std::string> &params,
                                  const InspectorContext &ctx) {
  if (!params.empty())
    return jsonError(400, "/ping does not accept query parameters");
  std::string json = "{\"ok\":true";
  json += ",\"level\":" + std::to_string(ctx.activeLevel());
  json += ",\"gamma\":" + fmtDouble(ctx.activeGamma());
  json += ",\"rngMode\":\"" + std::string(rngModeName(ctx.activeRngMode())) + "\"";
  json += ",\"tbLoaded\":";
  json += (ctx.isTbLoaded() ? "true" : "false");
  json += ",\"configuredTbPath\":\"" + jsonEscape(ctx.configuredTbPath) + "\"";
  json += ",\"boardCount\":" + std::to_string(ctx.tb.size());
  json +=
      ",\"emptyBoardValue\":"
      + (ctx.isTbLoaded() && ctx.tb.emptyBoardValue() >= 0.0 ? fmtDouble(ctx.tb.emptyBoardValue())
                                                             : "null");
  json += ",\"bellmanResidual\":" + (ctx.isTbLoaded() ? fmtSci(ctx.tb.bellmanResidual()) : "null");
  json += ",\"nnLoaded\":";
  json += (ctx.isNnLoaded() ? "true" : "false");
  json += ",\"configuredNnPath\":\"" + jsonEscape(ctx.configuredNnPath) + "\"";
  if (ctx.isNnLoaded()) {
    json += ",\"nnHidden\":[" + std::to_string(ctx.nn.h1) + "," + std::to_string(ctx.nn.h2) + "]";
    json += ",\"nnParamCount\":" + std::to_string(ctx.nn.params.size());
  } else {
    json += ",\"nnHidden\":null,\"nnParamCount\":0";
  }
  json += "}";
  return {200, "application/json; charset=utf-8", std::move(json)};
}

inline HttpRouteResult handleReload(const std::map<std::string, std::string> &params,
                                    InspectorContext &ctx) {
  if (!params.empty())
    return jsonError(400, "/reload does not accept query parameters");
  if (ctx.configuredTbPath.empty() && ctx.configuredNnPath.empty()) {
    return jsonError(400, "No TB or NNUE path was configured at server startup");
  }
  std::string err;
  if (!ctx.reloadConfigured(&err)) {
    return jsonError(500, err);
  }
  return handlePing(params, ctx);
}

inline HttpRouteResult handleStateInfo(const std::map<std::string, std::string> &params,
                                       const InspectorContext &ctx) {
  std::string badKey;
  if (!rejectUnknownParams(params, {"board", "next", "margin", "ply"}, badKey)) {
    return jsonError(400, "Unknown query parameter: " + badKey);
  }
  auto bIt = params.find("board");
  if (bIt == params.end())
    return jsonError(400, "Missing 'board' parameter");
  uint16_t rows[BOARD_ROWS];
  if (!parseBoard200Strict(bIt->second, rows)) {
    return jsonError(400, "Invalid 'board' (expected 200 chars of 0/1 or 'empty')");
  }
  int nextPiece = -1;
  auto nIt = params.find("next");
  if (nIt != params.end()) {
    if (!parsePieceStrict(nIt->second, nextPiece)) {
      return jsonError(400, "Invalid 'next' piece");
    }
  }
  bool ok = true;
  const double margin = parseMarginParam(params, ctx.defaultMargin, ok);
  if (!ok)
    return jsonError(400, "Invalid 'margin'");
  const int ply = parsePlyParam(params, ctx.defaultOutPly, ok);
  if (!ok)
    return jsonError(400, "Invalid 'ply' (expected 1 or 2)");

  const Board20 board = Board20::fromRows(rows);
  BoardKey key{};
  const bool fitsKey = encodeBoardKey(rows, key, board.topNonEmptyRow);
  float tbRow[NUM_PIECES] = {};
  const bool inTb = ctx.isTbLoaded() && fitsKey && ctx.tb.lookup(key, tbRow);
  float nnRow[NUM_PIECES] = {};
  if (ctx.isNnLoaded()) {
    ctx.nn.evalAll(rows, nnRow);
  }
  const RngMode rngMode = ctx.activeRngMode();
  const double eTb = inTb ? expectedValueFromRow(tbRow, rngMode) : 0.0;
  const double eNn = ctx.isNnLoaded() ? expectedValueFromRow(nnRow, rngMode) : 0.0;

  std::string json = "{\"board\":\"" + formatBoard200(rows) + "\"";
  json += ",\"fitsKey\":";
  json += (fitsKey ? "true" : "false");
  json += ",\"inTablebase\":";
  json += (inTb ? "true" : "false");
  json += ",\"holes\":" + std::to_string(countBoardHoles20(rows));
  json += ",\"maxHeight\":" + std::to_string(boardMaxHeight20(rows));
  json += ",\"tbValues\":";
  appendFloatArrayOrNull(json, tbRow, inTb);
  json += ",\"nnValues\":";
  appendFloatArrayOrNull(json, nnRow, ctx.isNnLoaded());
  json += ",\"expectedTb\":" + (inTb ? fmtDouble(eTb) : "null");
  json += ",\"expectedNn\":" + (ctx.isNnLoaded() ? fmtDouble(eNn) : "null");
  json += ",\"expectedErr\":" + ((inTb && ctx.isNnLoaded()) ? fmtDouble(eNn - eTb) : "null");

  // If nextPiece is provided, also report per-current-piece top-move agreement summary.
  if (nextPiece >= 0) {
    const int level = ctx.activeLevel();
    const int gravity = ctx.activeGravity();
    const double gamma = ctx.activeGamma();
    const int startLines = (level == 19) ? 130 : 0;
    const GameState gs = board20ToGameState(board, level, startLines, ctx.bc, ctx.aggro);
    std::vector<LockPlacement> m1, m2;
    const NnSearchConfig20 outCfg{ply, NN_DEFAULT_3PLY_TOPK};

    json += ",\"pieceSummary\":[";
    for (int c = 0; c < NUM_PIECES; ++c) {
      if (c > 0)
        json.push_back(',');
      const HybridMoveChoice20 hyb = chooseHybridMove20(
          board, c, nextPiece, gravity, gamma, ctx.isTbLoaded() ? &ctx.tb : nullptr,
          ctx.isNnLoaded() ? &ctx.nn : nullptr, margin, rngMode, outCfg);
      const HybridMoveChoice20 nnOnly =
          ctx.isNnLoaded() ? chooseHybridMove20(board, c, nextPiece, gravity, gamma,
                                                static_cast<const MappedDasTablebase *>(nullptr),
                                                &ctx.nn, margin, rngMode, outCfg)
                           : HybridMoveChoice20{};
      const Eval2PlyResult ev =
          chooseEval2PlyMove10Hz(gs, c, nextPiece, ctx.bc, ctx.aggro, true, m1, m2);

      const bool diffEval = hyb.found
                            && ev.hasMove
                            && (hyb.placement.rot != ev.bestMove.rotationIndex
                                || hyb.placement.x != ev.bestMove.x
                                || hyb.placement.y != ev.bestMove.y);
      const bool diffNn = hyb.found
                          && nnOnly.found
                          && (hyb.placement.rot != nnOnly.placement.rot
                              || hyb.placement.x != nnOnly.placement.x
                              || hyb.placement.y != nnOnly.placement.y);

      json += "{\"piece\":" + std::to_string(c);
      json += ",\"pieceChar\":\"";
      json.push_back(PIECE_CHARS[c]);
      json += "\",\"found\":";
      json += (hyb.found ? "true" : "false");
      json += ",\"chosenInTb\":";
      json += (hyb.chosenInTb ? "true" : "false");
      json += ",\"diffEval\":";
      json += (diffEval ? "true" : "false");
      json += ",\"diffNn\":";
      json += (diffNn ? "true" : "false");
      json += ",\"hybridScore\":" + (hyb.found ? fmtDouble(hyb.score) : "null");
      json += "}";
    }
    json += "]";
  }

  json += "}";
  return {200, "application/json; charset=utf-8", std::move(json)};
}

inline HttpRouteResult handleMoves(const std::map<std::string, std::string> &params,
                                   const InspectorContext &ctx) {
  std::string badKey;
  if (!rejectUnknownParams(params, {"board", "piece", "next", "margin", "ply"}, badKey)) {
    return jsonError(400, "Unknown query parameter: " + badKey);
  }
  auto bIt = params.find("board");
  auto pIt = params.find("piece");
  auto nIt = params.find("next");
  if (bIt == params.end() || pIt == params.end() || nIt == params.end()) {
    return jsonError(400, "Missing required 'board', 'piece', and/or 'next' parameter");
  }
  uint16_t rows[BOARD_ROWS];
  int curPiece = -1, nextPiece = -1;
  if (!parseBoard200Strict(bIt->second, rows))
    return jsonError(400, "Invalid 'board'");
  if (!parsePieceStrict(pIt->second, curPiece))
    return jsonError(400, "Invalid 'piece'");
  if (!parsePieceStrict(nIt->second, nextPiece))
    return jsonError(400, "Invalid 'next'");

  bool ok = true;
  const double margin = parseMarginParam(params, ctx.defaultMargin, ok);
  if (!ok)
    return jsonError(400, "Invalid 'margin'");
  const int ply = parsePlyParam(params, ctx.defaultOutPly, ok);
  if (!ok)
    return jsonError(400, "Invalid 'ply'");

  const Board20 board = Board20::fromRows(rows);
  const int level = ctx.activeLevel();
  const int gravity = ctx.activeGravity();
  const double gamma = ctx.activeGamma();
  const RngMode rngMode = ctx.activeRngMode();
  const int startLines = (level == 19) ? 130 : 0;
  const GameState parentGs = board20ToGameState(board, level, startLines, ctx.bc, ctx.aggro);

  PlacementList pl;
  findPlacements10Hz(board, curPiece, gravity, pl);

  const NnSearchConfig20 outCfg{ply, NN_DEFAULT_3PLY_TOPK};
  const HybridMoveChoice20 hybBest = chooseHybridMove20(
      board, curPiece, nextPiece, gravity, gamma, ctx.isTbLoaded() ? &ctx.tb : nullptr,
      ctx.isNnLoaded() ? &ctx.nn : nullptr, margin, rngMode, outCfg);

  struct MoveView {
    CompactPlacement p;
    uint8_t lc;
    uint16_t afterRows[BOARD_ROWS];
    bool fitsKey;
    bool inTb;
    bool ply2ReenteredTb;
    float tbVal;
    detail::Ply2HybridEval20 ply2;
    double tbQ;
    double hybridQ;
    double nnQ1;
    double nnQ2;
    Eval2PlyMoveInfo evalInfo;
  };
  std::vector<MoveView> views(static_cast<size_t>(pl.count));
  bool anyInTb = false;
  for (int i = 0; i < pl.count; ++i) {
    auto &mv = views[static_cast<size_t>(i)];
    mv.p = pl.data[i];
    mv.lc = applyPlacement20(board, curPiece, mv.p.rot, mv.p.x, mv.p.y, mv.afterRows);
    const int sr = (mv.lc == 0) ? std::max(0, std::min(static_cast<int>(board.topNonEmptyRow),
                                                       static_cast<int>(mv.p.y)))
                                : 0;
    BoardKey ak{};
    mv.fitsKey = encodeBoardKey(mv.afterRows, ak, sr);
    mv.inTb = false;
    mv.ply2ReenteredTb = false;
    mv.tbVal = 0.0f;
    if (ctx.isTbLoaded() && mv.fitsKey && ctx.tb.lookupPiece(ak, nextPiece, mv.tbVal)) {
      mv.inTb = true;
      anyInTb = true;
    }
  }

  bool anyTbGrounded = anyInTb;
  if (ply == 2) {
    PlacementList pl2;
    for (int i = 0; i < pl.count; ++i) {
      auto &mv = views[static_cast<size_t>(i)];
      if (!mv.inTb) {
        mv.ply2 = detail::bestReplyHybrid2Ply20(
            mv.afterRows, nextPiece, gravity, gamma, ctx.isTbLoaded() ? &ctx.tb : nullptr,
            ctx.isNnLoaded() ? &ctx.nn : nullptr, margin, rngMode, pl2);
        if (mv.ply2.anyInTb) {
          anyTbGrounded = true;
        }
      }
    }
  }

  std::vector<LockPlacement> m2Buf;
  m2Buf.reserve(64);
  int bestNnIdx = -1, bestEvalIdx = -1;
  double bestNnQ = -std::numeric_limits<double>::infinity();
  double bestEvalQ = -std::numeric_limits<double>::infinity();

  for (int i = 0; i < pl.count; ++i) {
    auto &mv = views[static_cast<size_t>(i)];
    const double rew = REWARD_NORM_TETRIS[mv.lc];
    mv.tbQ = mv.inTb ? (rew + gamma * static_cast<double>(mv.tbVal)) : rew;
    mv.nnQ1 = ctx.isNnLoaded()
                  ? scoreMoveNn1Ply20(mv.afterRows, mv.lc, nextPiece, gravity, gamma, ctx.nn)
                  : 0.0;
    mv.nnQ2 = ctx.isNnLoaded() ? scoreMoveNn2Ply20(mv.afterRows, mv.lc, nextPiece, gravity, gamma,
                                                   ctx.nn, rngMode)
                               : 0.0;
    const double chosenNnQ = (ply == 2) ? mv.nnQ2 : mv.nnQ1;
    if (mv.inTb) {
      mv.hybridQ = mv.tbQ;
    } else if (ply == 2) {
      mv.hybridQ = rew + gamma * (anyTbGrounded ? mv.ply2.bestIfTbGrounded : mv.ply2.bestIfAllOut);
      mv.ply2ReenteredTb = anyTbGrounded && mv.ply2.bestGroundedFromTb;
    } else if (ctx.isNnLoaded()) {
      if (anyInTb) {
        if (std::isfinite(margin)) {
          const double estCont = std::max(0.0, (chosenNnQ - rew) / std::max(1e-12, gamma));
          mv.hybridQ = rew + gamma * std::max(0.0, estCont - std::max(0.0, margin));
        } else {
          mv.hybridQ = rew;
        }
      } else {
        mv.hybridQ = chosenNnQ;
      }
    } else {
      mv.hybridQ = rew;
    }

    mv.evalInfo = eval2PlyForAfterBoard(parentGs, Board20::fromRows(mv.afterRows), mv.lc, nextPiece,
                                        ctx, m2Buf);

    if (ctx.isNnLoaded() && (bestNnIdx < 0 || chosenNnQ > bestNnQ)) {
      bestNnQ = chosenNnQ;
      bestNnIdx = i;
    }
    if (bestEvalIdx < 0 || mv.evalInfo.score > bestEvalQ) {
      bestEvalQ = mv.evalInfo.score;
      bestEvalIdx = i;
    }
  }

  std::string json;
  json.reserve(512 + static_cast<size_t>(pl.count) * 420);
  json += "{\"board\":\"" + formatBoard200(rows) + "\"";
  json += ",\"piece\":" + std::to_string(curPiece);
  json += ",\"pieceChar\":\"";
  json.push_back(PIECE_CHARS[curPiece]);
  json += "\",\"next\":" + std::to_string(nextPiece);
  json += ",\"nextChar\":\"";
  json.push_back(PIECE_CHARS[nextPiece]);
  json += "\",\"level\":" + std::to_string(level);
  json += ",\"anyInTb\":";
  json += (anyInTb ? "true" : "false");
  json += ",\"moves\":[";

  for (int i = 0; i < pl.count; ++i) {
    if (i > 0)
      json.push_back(',');
    const auto &mv = views[static_cast<size_t>(i)];
    const bool isHybBest = hybBest.found
                           && mv.p.rot == hybBest.placement.rot
                           && mv.p.x == hybBest.placement.x
                           && mv.p.y == hybBest.placement.y;
    json += "{\"index\":" + std::to_string(i);
    json += ",\"rot\":" + std::to_string(mv.p.rot);
    json += ",\"x\":" + std::to_string(mv.p.x);
    json += ",\"y\":" + std::to_string(mv.p.y);
    json += ",\"tuckInput\":\"";
    json.push_back(mv.p.tuckInput);
    json += "\",\"linesCleared\":" + std::to_string(mv.lc);
    json += ",\"afterBoard\":\"" + formatBoard200(mv.afterRows) + "\"";
    json += ",\"afterFitsKey\":";
    json += (mv.fitsKey ? "true" : "false");
    json += ",\"afterInTb\":";
    json += (mv.inTb ? "true" : "false");
    json += ",\"ply2ReenteredTb\":";
    json += (mv.ply2ReenteredTb ? "true" : "false");
    json += ",\"ply2Move\":";
    if (!mv.inTb && ply == 2 && mv.ply2.hasMove) {
      const CompactPlacement p2 = anyTbGrounded ? mv.ply2.bestGroundedMove : mv.ply2.bestAllOutMove;
      const uint8_t lc2 = anyTbGrounded ? mv.ply2.bestGroundedLc : mv.ply2.bestAllOutLc;
      const bool inTb2 = anyTbGrounded && mv.ply2.bestGroundedFromTb;
      const uint64_t pieceMask =
          detail::PIECE_MASK_64.mask[nextPiece][p2.rot][p2.x + detail::PIECE_MASK_X_OFFSET];
      json += "{\"rot\":" + std::to_string(p2.rot);
      json += ",\"x\":" + std::to_string(p2.x);
      json += ",\"y\":" + std::to_string(p2.y);
      json += ",\"tuckInput\":\"";
      json.push_back(p2.tuckInput);
      json += "\",\"lc\":" + std::to_string(lc2);
      json += ",\"inTb\":";
      json += (inTb2 ? "true" : "false");
      json += ",\"cells\":[";
      bool firstCell = true;
      for (int dr = 0; dr < 4; ++dr) {
        const int r = static_cast<int>(p2.y) + dr;
        if (r < 0 || r >= BOARD_ROWS)
          continue;
        const uint16_t rowBits = static_cast<uint16_t>((pieceMask >> (dr * 16)) & FULL_ROW_MASK);
        for (int c = 0; c < BOARD_WIDTH; ++c) {
          if ((rowBits >> (9 - c)) & 1U) {
            if (!firstCell)
              json.push_back(',');
            firstCell = false;
            json += "[" + std::to_string(r) + "," + std::to_string(c) + "]";
          }
        }
      }
      json += "]}";
    } else {
      json += "null";
    }
    json += ",\"ply2Move\":";
    if (!mv.inTb && ply == 2 && mv.ply2.hasMove) {
      const CompactPlacement p2 = anyTbGrounded ? mv.ply2.bestGroundedMove : mv.ply2.bestAllOutMove;
      const uint8_t lc2 = anyTbGrounded ? mv.ply2.bestGroundedLc : mv.ply2.bestAllOutLc;
      const bool inTb2 = anyTbGrounded && mv.ply2.bestGroundedFromTb;
      json += "{\"rot\":" + std::to_string(p2.rot);
      json += ",\"x\":" + std::to_string(p2.x);
      json += ",\"y\":" + std::to_string(p2.y);
      json += ",\"tuckInput\":\"";
      json.push_back(p2.tuckInput);
      json += "\",\"lc\":" + std::to_string(lc2);
      json += ",\"inTb\":";
      json += (inTb2 ? "true" : "false");
      json += ",\"cells\":";
      appendPlacementCellsJson(json, nextPiece, p2.rot, p2.x, p2.y);
      json += "}";
    } else {
      json += "null";
    }
    json += ",\"evalPly2Move\":";
    if (mv.evalInfo.hasMove) {
      json += "{\"rot\":" + std::to_string(mv.evalInfo.rot);
      json += ",\"x\":" + std::to_string(mv.evalInfo.x);
      json += ",\"y\":" + std::to_string(mv.evalInfo.y);
      json += ",\"tuckInput\":\"";
      json.push_back(mv.evalInfo.tuckInput);
      json += "\",\"lc\":" + std::to_string(mv.evalInfo.lc);
      json += ",\"cells\":";
      appendPlacementCellsJson(json, nextPiece, mv.evalInfo.rot, mv.evalInfo.x, mv.evalInfo.y);
      json += "}";
    } else {
      json += "null";
    }
    json += ",\"afterHoles\":" + std::to_string(countBoardHoles20(mv.afterRows));
    json += ",\"afterMaxHeight\":" + std::to_string(boardMaxHeight20(mv.afterRows));
    json += ",\"tbQ\":" + (mv.inTb ? fmtDouble(mv.tbQ) : "null");
    json += ",\"hybridQ\":" + fmtDouble(mv.hybridQ);
    json += ",\"nnQ1\":" + (ctx.isNnLoaded() ? fmtDouble(mv.nnQ1) : "null");
    json += ",\"nnQ2\":" + (ctx.isNnLoaded() ? fmtDouble(mv.nnQ2) : "null");
    json += ",\"evalQ\":" + fmtDouble(mv.evalInfo.score);
    json += ",\"isHybridBest\":";
    json += (isHybBest ? "true" : "false");
    json += ",\"isNnBest\":";
    json += (i == bestNnIdx ? "true" : "false");
    json += ",\"isEvalBest\":";
    json += (i == bestEvalIdx ? "true" : "false");
    json += "}";
  }
  json += "]}";
  return {200, "application/json; charset=utf-8", std::move(json)};
}

// Steps forward from (board, piece, next) under the Hybrid TB+NNUE policy until finding a
// disagreement between TB+NNUE vs eval.cpp, TB+NNUE vs NN-only, or NN-only vs eval.cpp.
inline HttpRouteResult handleStepDisagreement(const std::map<std::string, std::string> &params,
                                              const InspectorContext &ctx) {
  std::string badKey;
  if (!rejectUnknownParams(params,
                           {"board", "piece", "next", "mode", "seed", "maxSteps", "margin", "ply"},
                           badKey)) {
    return jsonError(400, "Unknown query parameter: " + badKey);
  }
  auto bIt = params.find("board");
  auto pIt = params.find("piece");
  auto nIt = params.find("next");
  if (bIt == params.end() || pIt == params.end() || nIt == params.end()) {
    return jsonError(400, "Missing required 'board', 'piece', and/or 'next' parameter");
  }
  uint16_t rows[BOARD_ROWS];
  int curP = -1, nextP = -1;
  if (!parseBoard200Strict(bIt->second, rows))
    return jsonError(400, "Invalid 'board'");
  if (!parsePieceStrict(pIt->second, curP))
    return jsonError(400, "Invalid 'piece'");
  if (!parsePieceStrict(nIt->second, nextP))
    return jsonError(400, "Invalid 'next'");

  std::string mode = "tb-vs-eval";
  auto mIt = params.find("mode");
  if (mIt != params.end()) {
    if (mIt->second != "tb-vs-eval" && mIt->second != "tb-vs-nn" && mIt->second != "nn-vs-eval") {
      return jsonError(400, "Invalid 'mode' (expected tb-vs-eval, tb-vs-nn, or nn-vs-eval)");
    }
    mode = mIt->second;
  }
  if ((mode == "tb-vs-nn" || mode == "nn-vs-eval") && !ctx.isNnLoaded()) {
    return jsonError(400, "Requested mode requires a loaded NNUE model");
  }

  uint64_t seed = 30000000ULL;
  auto sIt = params.find("seed");
  if (sIt != params.end()) {
    if (!parseQueryIntStrict(sIt->second, seed)) {
      return jsonError(400, "Invalid 'seed'");
    }
  }
  int maxSteps = 200;
  auto msIt = params.find("maxSteps");
  if (msIt != params.end()) {
    if (!parseQueryIntStrict(msIt->second, maxSteps) || maxSteps < 1 || maxSteps > 1000) {
      return jsonError(400, "Invalid 'maxSteps' (expected 1..1000)");
    }
  }

  bool ok = true;
  const double margin = parseMarginParam(params, ctx.defaultMargin, ok);
  if (!ok)
    return jsonError(400, "Invalid 'margin'");
  const int ply = parsePlyParam(params, ctx.defaultOutPly, ok);
  if (!ok)
    return jsonError(400, "Invalid 'ply'");

  const int level = ctx.activeLevel();
  const int gravity = ctx.activeGravity();
  const double gamma = ctx.activeGamma();
  const RngMode rngMode = ctx.activeRngMode();
  const int startLines = (level == 19) ? 130 : 0;
  const NnSearchConfig20 outCfg{ply, NN_DEFAULT_3PLY_TOPK};

  Board20 board = Board20::fromRows(rows);
  Rng rng(seed);
  std::vector<LockPlacement> m1, m2;

  for (int step = 0; step < maxSteps; ++step) {
    const HybridMoveChoice20 hyb =
        chooseHybridMove20(board, curP, nextP, gravity, gamma, ctx.isTbLoaded() ? &ctx.tb : nullptr,
                           ctx.isNnLoaded() ? &ctx.nn : nullptr, margin, rngMode, outCfg);
    if (!hyb.found) {
      // Restart from empty board on top-out so stepping always keeps searching smoothly.
      board = Board20::empty();
      curP = rng.sampleOpeningPiece(rngMode);
      nextP = rng.sampleNextPiece(rngMode, curP);
      continue;
    }

    if (step > 0) {
      bool disagree = false;
      if (mode == "tb-vs-eval") {
        const GameState gs = board20ToGameState(board, level, startLines, ctx.bc, ctx.aggro);
        const Eval2PlyResult ev =
            chooseEval2PlyMove10Hz(gs, curP, nextP, ctx.bc, ctx.aggro, true, m1, m2);
        disagree = ev.hasMove
                   && (hyb.placement.rot != ev.bestMove.rotationIndex
                       || hyb.placement.x != ev.bestMove.x
                       || hyb.placement.y != ev.bestMove.y);
      } else if (mode == "tb-vs-nn" && ctx.isNnLoaded()) {
        const HybridMoveChoice20 nnOnly = chooseHybridMove20(
            board, curP, nextP, gravity, gamma, static_cast<const MappedDasTablebase *>(nullptr),
            &ctx.nn, margin, rngMode, outCfg);
        disagree = nnOnly.found
                   && (hyb.placement.rot != nnOnly.placement.rot
                       || hyb.placement.x != nnOnly.placement.x
                       || hyb.placement.y != nnOnly.placement.y);
      } else if (mode == "nn-vs-eval" && ctx.isNnLoaded()) {
        const HybridMoveChoice20 nnOnly = chooseHybridMove20(
            board, curP, nextP, gravity, gamma, static_cast<const MappedDasTablebase *>(nullptr),
            &ctx.nn, margin, rngMode, outCfg);
        const GameState gs = board20ToGameState(board, level, startLines, ctx.bc, ctx.aggro);
        const Eval2PlyResult ev =
            chooseEval2PlyMove10Hz(gs, curP, nextP, ctx.bc, ctx.aggro, true, m1, m2);
        disagree = nnOnly.found
                   && ev.hasMove
                   && (nnOnly.placement.rot != ev.bestMove.rotationIndex
                       || nnOnly.placement.x != ev.bestMove.x
                       || nnOnly.placement.y != ev.bestMove.y);
      }

      if (disagree) {
        std::string json = "{\"found\":true,\"stepsTaken\":" + std::to_string(step);
        json += ",\"board\":\"" + formatBoard200(board.rows()) + "\"";
        json += ",\"piece\":" + std::to_string(curP);
        json += ",\"next\":" + std::to_string(nextP);
        json += ",\"nextSeed\":" + std::to_string(rng.nextU64() & 0xFFFFFFFFULL);
        json += "}";
        return {200, "application/json; charset=utf-8", std::move(json)};
      }
    }

    board = Board20::fromRows(hyb.afterRows);
    curP = nextP;
    nextP = rng.sampleNextPiece(rngMode, curP);
  }

  std::string json = "{\"found\":false,\"stepsTaken\":" + std::to_string(maxSteps);
  json += ",\"board\":\"" + formatBoard200(board.rows()) + "\"";
  json += ",\"piece\":" + std::to_string(curP);
  json += ",\"next\":" + std::to_string(nextP);
  json += ",\"nextSeed\":" + std::to_string(rng.nextU64() & 0xFFFFFFFFULL);
  json += "}";
  return {200, "application/json; charset=utf-8", std::move(json)};
}

inline HttpRouteResult handleApiRequest(std::string_view path, std::string_view query,
                                        InspectorContext &ctx) {
  std::map<std::string, std::string> params;
  std::string err;
  if (!parseQueryStrict(query, params, err)) {
    return jsonError(400, err);
  }
  if (path == "/ping")
    return handlePing(params, ctx);
  if (path == "/reload")
    return handleReload(params, ctx);
  if (path == "/state-info")
    return handleStateInfo(params, ctx);
  if (path == "/moves")
    return handleMoves(params, ctx);
  if (path == "/step-disagreement")
    return handleStepDisagreement(params, ctx);
  return jsonError(404, "Unknown route");
}

} // namespace inspector
} // namespace das

#endif // DAS_TABLEBASE_INSPECTOR_ROUTES_HPP
