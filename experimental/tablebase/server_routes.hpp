#ifndef TABLEBASE_SERVER_ROUTES_HPP
#define TABLEBASE_SERVER_ROUTES_HPP

#include "core.hpp"
#include "heuristic.hpp"
#include "tablebase_io.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>

namespace tb {

struct HttpRouteResult {
  int status = 200;
  std::string contentType = "application/json; charset=utf-8";
  std::string body;
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

} // namespace detail

struct ServerContext {
  MappedTablebase tablebase;
  std::string configuredTbPath;
  EvalWeights weights{};
};

inline HttpRouteResult handleApiRequest(std::string_view path, std::string_view query,
                                        ServerContext &ctx) {
  std::map<std::string, std::string> params;
  std::string parseErr;
  if (!detail::parseQueryStrict(query, params, parseErr)) {
    return detail::jsonErrorResponse(400, parseErr);
  }

  if (path == "/ping") {
    if (!params.empty()) {
      return detail::jsonErrorResponse(400, "/ping does not accept query parameters");
    }
    std::string json = "{\"ok\":true,\"tablebaseLoaded\":";
    json += (ctx.tablebase.isLoaded() ? "true" : "false");
    json += ",\"configuredPath\":\"" + detail::jsonEscape(ctx.configuredTbPath) + "\"";
    json += ",\"boardCount\":" + std::to_string(ctx.tablebase.size());
    json += ",\"emptyBoardValue\":"
            + (ctx.tablebase.isLoaded() && ctx.tablebase.emptyBoardValue() >= 0.0
                   ? detail::fmtDouble(ctx.tablebase.emptyBoardValue())
                   : "null");
    json += ",\"bellmanResidual\":"
            + (ctx.tablebase.isLoaded() ? detail::fmtSci(ctx.tablebase.bellmanResidual()) : "null");
    json += "}";
    return {200, "application/json; charset=utf-8", std::move(json)};
  }

  if (path == "/reload") {
    if (!params.empty()) {
      return detail::jsonErrorResponse(400, "/reload does not accept query parameters");
    }
    if (ctx.configuredTbPath.empty()) {
      return detail::jsonErrorResponse(400, "No tablebase path was configured on server startup");
    }
    std::string err;
    if (!ctx.tablebase.open(ctx.configuredTbPath, &err)) {
      return detail::jsonErrorResponse(500, err);
    }
    return handleApiRequest("/ping", "", ctx);
  }

  if (path == "/moves") {
    for (const auto &kv : params) {
      if (kv.first != "board" && kv.first != "piece" && kv.first != "ply") {
        return detail::jsonErrorResponse(400, "Unknown query parameter: " + kv.first);
      }
    }
    const auto bIt = params.find("board");
    const auto pIt = params.find("piece");
    if (bIt == params.end() || pIt == params.end()) {
      return detail::jsonErrorResponse(400, "Missing required 'board' and/or 'piece' parameter");
    }
    uint64_t board = 0;
    if (!detail::parseBoardParamStrict(bIt->second, board)) {
      return detail::jsonErrorResponse(
          400, "Invalid 'board' (expected 1-10 hex chars or 40 board chars)");
    }
    int pieceIdx = -1;
    if (!detail::parsePieceParamStrict(pIt->second, pieceIdx)) {
      return detail::jsonErrorResponse(400, "Invalid 'piece' (expected 0..6 or I,O,L,J,T,S,Z)");
    }
    int ply = 1;
    if (!detail::parsePlyParamStrict(params, ply)) {
      return detail::jsonErrorResponse(400, "Invalid 'ply' (expected 1 or 2)");
    }

    float boardTbVal = 0.0f;
    const bool boardInTb = ctx.tablebase.lookup(board, boardTbVal);

    PlacementList placements;
    generatePlacements(board, pieceIdx, placements);

    std::string json;
    json.reserve(512 + static_cast<size_t>(placements.size()) * 240);
    json += "{\"boardHex\":\"" + formatBoardHex(board) + "\"";
    json += ",\"isCanonical\":";
    json += (isCanonicalBoard(board) ? "true" : "false");
    json += ",\"piece\":" + std::to_string(pieceIdx);
    json += ",\"pieceChar\":\"";
    json.push_back(PIECE_CHARS[pieceIdx]);
    json += "\"";
    json += ",\"heuristicPly\":" + std::to_string(ply);
    json += ",\"tablebaseLoaded\":";
    json += (ctx.tablebase.isLoaded() ? "true" : "false");
    json += ",\"inTablebase\":";
    json += (boardInTb ? "true" : "false");
    json += ",\"boardValue\":";
    json += (boardInTb ? detail::fmtDouble(boardTbVal) : "null");
    json += ",\"moves\":[";

    for (int i = 0; i < placements.size(); ++i) {
      if (i > 0)
        json.push_back(',');
      const Placement &m = placements[i];
      const double heurQ =
          detail::scoreTransitionByPly(m.afterBoard, m.linesCleared, ply, ctx.weights);
      float afterVal = 0.0f;
      const bool afterInTb = ctx.tablebase.lookup(m.afterBoard, afterVal);
      // Matches Value Iteration's closed-world boundary rule: V(b') = 0.0 when b' is outside S.
      const double tbQ =
          static_cast<double>(m.linesCleared) + (afterInTb ? static_cast<double>(afterVal) : 0.0);

      json += "{\"index\":" + std::to_string(i);
      json += ",\"rot\":" + std::to_string(m.rot);
      json += ",\"x\":" + std::to_string(m.x);
      json += ",\"lockY\":" + std::to_string(m.lockY);
      json += ",\"tuckInput\":\"";
      json.push_back(m.tuckInput);
      json += "\"";
      json += ",\"lockedMaskHex\":\"" + formatBoardHex(m.lockedMask) + "\"";
      json += ",\"afterBoardHex\":\"" + formatBoardHex(m.afterBoard) + "\"";
      json += ",\"linesCleared\":" + std::to_string(m.linesCleared);
      json += ",\"afterHoles\":" + std::to_string(countHoles(m.afterBoard));
      json += ",\"heuristicScore\":" + detail::fmtDouble(heurQ);
      json += ",\"afterInTablebase\":";
      json += (afterInTb ? "true" : "false");
      json += ",\"afterBoardValue\":";
      json += (afterInTb ? detail::fmtDouble(afterVal) : "null");
      json += ",\"tablebaseQ\":";
      json += (ctx.tablebase.isLoaded() ? detail::fmtDouble(tbQ) : "null");
      json.push_back('}');
    }
    json += "]}";
    return {200, "application/json; charset=utf-8", std::move(json)};
  }

  if (path == "/state-info") {
    for (const auto &kv : params) {
      if (kv.first != "board" && kv.first != "ply") {
        return detail::jsonErrorResponse(400, "Unknown query parameter: " + kv.first);
      }
    }
    const auto bIt = params.find("board");
    if (bIt == params.end()) {
      return detail::jsonErrorResponse(400, "Missing required 'board' parameter");
    }
    uint64_t board = 0;
    if (!detail::parseBoardParamStrict(bIt->second, board)) {
      return detail::jsonErrorResponse(
          400, "Invalid 'board' (expected 1-10 hex chars or 40 board chars)");
    }
    int ply = 1;
    if (!detail::parsePlyParamStrict(params, ply)) {
      return detail::jsonErrorResponse(400, "Invalid 'ply' (expected 1 or 2)");
    }

    int heights[BOARD_WIDTH];
    getColumnHeights(board, heights);
    const HeuristicBreakdown hb = evaluateBoardBreakdown(board, ctx.weights);

    float boardTbVal = 0.0f;
    const bool boardInTb = ctx.tablebase.lookup(board, boardTbVal);
    const bool tbLoaded = ctx.tablebase.isLoaded();

    std::string json;
    json.reserve(1024);
    json += "{\"boardHex\":\"" + formatBoardHex(board) + "\"";
    json += ",\"isCanonical\":";
    json += (isCanonicalBoard(board) ? "true" : "false");
    json += ",\"heuristicPly\":" + std::to_string(ply);
    json += ",\"popcount\":" + std::to_string(popcount64(board));
    json += ",\"maxHeight\":" + std::to_string(getMaxHeight(board));
    json += ",\"holes\":" + std::to_string(hb.holes);
    json += ",\"overhangs\":" + std::to_string(hb.overhangs);
    json += ",\"sealedHoles\":" + std::to_string(hb.sealedHoles);
    json += ",\"bumpiness\":" + std::to_string(hb.bumpiness);
    json += ",\"heuristicBoardScore\":" + detail::fmtDouble(hb.boardScore);
    json += ",\"heights\":[";
    for (int c = 0; c < BOARD_WIDTH; ++c) {
      if (c > 0)
        json.push_back(',');
      json += std::to_string(heights[c]);
    }
    json += "]";
    json += ",\"tablebaseLoaded\":";
    json += (tbLoaded ? "true" : "false");
    json += ",\"inTablebase\":";
    json += (boardInTb ? "true" : "false");
    json += ",\"boardValue\":";
    json += (boardInTb ? detail::fmtDouble(boardTbVal) : "null");
    json += ",\"perPiece\":[";

    double sumBestTbQ = 0.0;
    TransitionList tl;
    for (int p = 0; p < NUM_PIECES; ++p) {
      if (p > 0)
        json.push_back(',');
      generateTransitions(board, p, tl);

      double bestHeur = 0.0;
      uint64_t bestHeurAfter = 0;
      // Top-out (tl.empty()) contributes 0.0 to the 7-piece Bellman expectation sumBestTbQ;
      // when tl is non-empty, iteration i == 0 unconditionally initializes bestTbQ.
      double bestTbQ = 0.0;
      double bestTbTieHeur = 0.0;
      uint64_t bestTbAfter = 0;
      bool anyInTb = false;
      bool allInTb = !tl.empty();

      for (int i = 0; i < tl.size(); ++i) {
        const Transition &t = tl[i];
        const double hScore =
            detail::scoreTransitionByPly(t.afterBoard, t.linesCleared, ply, ctx.weights);
        if (i == 0 || hScore > bestHeur) {
          bestHeur = hScore;
          bestHeurAfter = t.afterBoard;
        }
        if (tbLoaded) {
          float childVal = 0.0f;
          const bool inTb = ctx.tablebase.lookup(t.afterBoard, childVal);
          anyInTb = anyInTb || inTb;
          allInTb = allInTb && inTb;
          const double q =
              static_cast<double>(t.linesCleared) + (inTb ? static_cast<double>(childVal) : 0.0);
          if (i == 0 || q > bestTbQ || (q == bestTbQ && hScore > bestTbTieHeur)) {
            bestTbQ = q;
            bestTbTieHeur = hScore;
            bestTbAfter = t.afterBoard;
          }
        }
      }
      sumBestTbQ += bestTbQ;

      // Mirror play-time policy: if no child lands in TB, TB policy falls back to heuristic.
      const uint64_t effectiveTbAfter = anyInTb ? bestTbAfter : bestHeurAfter;
      const bool disagree = tbLoaded && !tl.empty() && (effectiveTbAfter != bestHeurAfter);

      json += "{\"piece\":" + std::to_string(p);
      json += ",\"pieceChar\":\"";
      json.push_back(PIECE_CHARS[p]);
      json += "\"";
      json += ",\"numTransitions\":" + std::to_string(tl.size());
      json += ",\"bestHeuristicScore\":";
      json += (tl.empty() ? "null" : detail::fmtDouble(bestHeur));
      json += ",\"bestTablebaseQ\":";
      json += ((!tbLoaded || tl.empty()) ? "null" : detail::fmtDouble(bestTbQ));
      json += ",\"anyInTablebase\":";
      json += (anyInTb ? "true" : "false");
      json += ",\"allInTablebase\":";
      json += ((tbLoaded && allInTb) ? "true" : "false");
      json += ",\"disagree\":";
      json += (disagree ? "true" : "false");
      json.push_back('}');
    }

    json += "]";
    json += ",\"bellmanBackupValue\":";
    json += (tbLoaded ? detail::fmtDouble(sumBestTbQ / static_cast<double>(NUM_PIECES)) : "null");
    json += "}";
    return {200, "application/json; charset=utf-8", std::move(json)};
  }

  return detail::jsonErrorResponse(404, std::string("Unknown route: ") + std::string(path));
}

} // namespace tb

#endif // TABLEBASE_SERVER_ROUTES_HPP
