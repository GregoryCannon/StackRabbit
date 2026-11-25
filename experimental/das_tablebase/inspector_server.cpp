// inspector_server.cpp — Standalone local HTTP server for the 20x10 DAS Tablebase + NNUE Inspector.
//
// Build:
//   g++ -O2 -std=c++17 -Wall -Wextra -Werror experimental/das_tablebase/inspector_server.cpp \
//       -o build/das_inspector
//
// Run:
//   ./build/das_inspector --tb=PATH [--nn=PATH] [--level=19|18] [--port=3030]

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "inspector_routes.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr int DEFAULT_PORT = 3030;
constexpr int RECV_TIMEOUT_SECONDS = 5;
constexpr size_t READ_BUFFER_LIMIT = 16384;
constexpr int LISTEN_BACKLOG = 16;

#ifdef _WIN32
using SocketFd = SOCKET;
const SocketFd INVALID_SOCK = INVALID_SOCKET;
bool initSockets() {
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
void cleanupSockets() { WSACleanup(); }
void closeSocket(SocketFd fd) { closesocket(fd); }
void setRecvTimeout(SocketFd fd, int sec) {
  DWORD ms = static_cast<DWORD>(sec) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
}
void setListenReuseAddr(SocketFd) {}
bool isInterruptedAccept() { return WSAGetLastError() == WSAEINTR; }
#else
using SocketFd = int;
const SocketFd INVALID_SOCK = -1;
bool initSockets() {
  std::signal(SIGPIPE, SIG_IGN);
  return true;
}
void cleanupSockets() {}
void closeSocket(SocketFd fd) { ::close(fd); }
void setRecvTimeout(SocketFd fd, int sec) {
  timeval tv{};
  tv.tv_sec = sec;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
}
void setListenReuseAddr(SocketFd fd) {
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one));
}
bool isInterruptedAccept() { return errno == EINTR; }
#endif

struct SocketGuard {
  bool ok = false;
  SocketGuard() : ok(initSockets()) {}
  ~SocketGuard() {
    if (ok)
      cleanupSockets();
  }
};

const char *statusText(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 408:
    return "Request Timeout";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  default:
    return "Error";
  }
}

void sendAll(SocketFd fd, const char *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const size_t chunk = (len - sent) > 65536 ? 65536 : (len - sent);
    const auto n = ::send(fd, data + sent, static_cast<int>(chunk), 0);
    if (n <= 0)
      return;
    sent += static_cast<size_t>(n);
  }
}

void sendHttpResponse(SocketFd fd, int status, const std::string &contentType,
                      const std::string &body) {
  std::string head = "HTTP/1.1 "
                     + std::to_string(status)
                     + " "
                     + statusText(status)
                     + "\r\nContent-Type: "
                     + contentType
                     + "\r\nAccess-Control-Allow-Origin: *"
                       "\r\nAccess-Control-Allow-Methods: GET, OPTIONS"
                       "\r\nCache-Control: no-store"
                       "\r\nContent-Length: "
                     + std::to_string(body.size())
                     + "\r\nConnection: close\r\n\r\n";
  sendAll(fd, head.data(), head.size());
  if (!body.empty()) {
    sendAll(fd, body.data(), body.size());
  }
}

enum class HeaderReadStatus { OK, TIMEOUT_OR_EOF, TOO_LARGE };

HeaderReadStatus readRequestHeaders(SocketFd fd, std::string &out) {
  char buf[2048];
  out.clear();
  for (;;) {
    const auto n = ::recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
    if (n <= 0)
      return HeaderReadStatus::TIMEOUT_OR_EOF;
    out.append(buf, static_cast<size_t>(n));
    if (out.find("\r\n\r\n") != std::string::npos)
      return HeaderReadStatus::OK;
    if (out.size() > READ_BUFFER_LIMIT)
      return HeaderReadStatus::TOO_LARGE;
  }
}

std::string defaultHtmlPath() {
  std::string src = __FILE__;
  const size_t slash = src.find_last_of("/\\");
  if (slash != std::string::npos) {
    return src.substr(0, slash + 1) + "inspector.html";
  }
  return "experimental/das_tablebase/inspector.html";
}

bool readTextFile(const std::string &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

template <typename IntT> bool parseStrictInt(std::string_view s, IntT &out) {
  if (s.empty())
    return false;
  IntT val = 0;
  const auto res = std::from_chars(s.data(), s.data() + s.size(), val);
  if (res.ec != std::errc() || res.ptr != s.data() + s.size())
    return false;
  out = val;
  return true;
}

void handleConnection(SocketFd fd, das::InspectorContext &ctx, const std::string &htmlPath) {
  setRecvTimeout(fd, RECV_TIMEOUT_SECONDS);
  std::string raw;
  const HeaderReadStatus rs = readRequestHeaders(fd, raw);
  if (rs == HeaderReadStatus::TOO_LARGE) {
    sendHttpResponse(fd, 431, "application/json; charset=utf-8",
                     "{\"error\":\"Request headers exceeded buffer limit\"}");
    return;
  }
  if (rs != HeaderReadStatus::OK) {
    sendHttpResponse(fd, 408, "application/json; charset=utf-8",
                     "{\"error\":\"Request headers incomplete or timed out\"}");
    return;
  }

  const size_t lineEnd = raw.find("\r\n");
  const std::string requestLine = raw.substr(0, lineEnd);
  const size_t sp1 = requestLine.find(' ');
  const size_t sp2 =
      (sp1 == std::string::npos) ? std::string::npos : requestLine.find(' ', sp1 + 1);
  const bool wellFormed = sp1 != std::string::npos
                          && sp2 != std::string::npos
                          && sp2 + 1 < requestLine.size()
                          && requestLine.find(' ', sp2 + 1) == std::string::npos
                          && requestLine.compare(sp2 + 1, 5, "HTTP/") == 0;
  if (!wellFormed) {
    sendHttpResponse(fd, 400, "application/json; charset=utf-8",
                     "{\"error\":\"Malformed HTTP request line\"}");
    return;
  }

  const std::string method = requestLine.substr(0, sp1);
  if (method == "OPTIONS") {
    sendHttpResponse(fd, 204, "text/plain; charset=utf-8", "");
    return;
  }
  if (method != "GET") {
    sendHttpResponse(fd, 400, "application/json; charset=utf-8",
                     "{\"error\":\"Only GET requests are supported\"}");
    return;
  }

  const std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string path = target;
  std::string query;
  const size_t qPos = target.find('?');
  if (qPos != std::string::npos) {
    path = target.substr(0, qPos);
    query = target.substr(qPos + 1);
  }

  if (path == "/" || path == "/inspector.html" || path == "/index.html") {
    std::string html;
    if (!readTextFile(htmlPath, html)) {
      sendHttpResponse(fd, 500, "text/plain; charset=utf-8",
                       "Failed to read inspector.html at: " + htmlPath);
      return;
    }
    sendHttpResponse(fd, 200, "text/html; charset=utf-8", html);
    return;
  }

  const das::HttpRouteResult res = das::inspector::handleApiRequest(path, query, ctx);
  sendHttpResponse(fd, res.status, res.contentType, res.body);
}

void printUsage(FILE *stream, const char *prog) {
  fprintf(stream, "Usage: %s [--tb=PATH] [--nn=PATH] [--level=19|18] [--port=3030]\n", prog);
}

} // namespace

int main(int argc, char **argv) {
  SocketGuard guard;
  if (!guard.ok) {
    fprintf(stderr, "das_inspector: failed to initialize sockets\n");
    return 1;
  }

  int port = DEFAULT_PORT;
  bool levelExplicit = false;
  das::InspectorContext ctx;

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
    if (tryPrefix("--tb=", val)) {
      if (val.empty()) {
        fprintf(stderr, "das_inspector: empty --tb path\n");
        return 1;
      }
      ctx.configuredTbPath = std::string(val);
      continue;
    }
    if (tryPrefix("--nn=", val)) {
      if (val.empty()) {
        fprintf(stderr, "das_inspector: empty --nn path\n");
        return 1;
      }
      ctx.configuredNnPath = std::string(val);
      continue;
    }
    if (tryPrefix("--level=", val)) {
      if (!parseStrictInt(val, ctx.level) || (ctx.level != 18 && ctx.level != 19)) {
        fprintf(stderr, "das_inspector: invalid --level (expected 18 or 19)\n");
        return 1;
      }
      levelExplicit = true;
      continue;
    }
    if (tryPrefix("--port=", val)) {
      if (!parseStrictInt(val, port) || port < 1 || port > 65535) {
        fprintf(stderr, "das_inspector: invalid --port (expected 1..65535)\n");
        return 1;
      }
      continue;
    }
    fprintf(stderr, "das_inspector: unknown option '%.*s'\n", static_cast<int>(arg.size()),
            arg.data());
    printUsage(stderr, argv[0]);
    return 1;
  }

  std::string err;
  if (!ctx.configuredTbPath.empty() || !ctx.configuredNnPath.empty()) {
    if (!ctx.reloadConfigured(&err)) {
      fprintf(stderr, "das_inspector: %s\n", err.c_str());
      return 1;
    }
  }
  if (ctx.isTbLoaded() && levelExplicit && ctx.tb.level() != ctx.level) {
    fprintf(stderr, "das_inspector: --level=%d does not match --tb level (%d)\n", ctx.level,
            ctx.tb.level());
    return 1;
  }
  if (ctx.isTbLoaded() && ctx.isNnLoaded() && ctx.tb.rngMode() != ctx.nn.rngMode) {
    fprintf(stderr, "das_inspector: --tb rng (%s) does not match --nn rng (%s)\n",
            das::rngModeName(ctx.tb.rngMode()), das::rngModeName(ctx.nn.rngMode));
    return 1;
  }
  if (ctx.isTbLoaded()) {
    printf("das_inspector: loaded SRTBV003 (%llu boards, L%d, rng=%s, gamma=%.4f, E[V(0)]=%.5f, "
           "res=%.2e) from %s\n",
           static_cast<unsigned long long>(ctx.tb.size()), ctx.tb.level(),
           das::rngModeName(ctx.tb.rngMode()), ctx.tb.gamma(), ctx.tb.emptyBoardValue(),
           ctx.tb.bellmanResidual(), ctx.configuredTbPath.c_str());
  }
  if (ctx.isNnLoaded()) {
    printf("das_inspector: loaded TinyDasNet (%dx%d, %zu params, %.1f KB, rng=%s) from %s\n",
           ctx.nn.h1, ctx.nn.h2, ctx.nn.params.size(),
           static_cast<double>(ctx.nn.params.size() * sizeof(float)) / 1024.0,
           das::rngModeName(ctx.nn.rngMode), ctx.configuredNnPath.c_str());
  }

  const SocketFd listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd == INVALID_SOCK) {
    perror("das_inspector: socket");
    return 1;
  }
  setListenReuseAddr(listenFd);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    perror("das_inspector: bind");
    closeSocket(listenFd);
    return 1;
  }
  if (::listen(listenFd, LISTEN_BACKLOG) != 0) {
    perror("das_inspector: listen");
    closeSocket(listenFd);
    return 1;
  }

  const std::string htmlPath = defaultHtmlPath();
  printf("das_inspector: listening on http://127.0.0.1:%d/\n", port);
  fflush(stdout);

  for (;;) {
    const SocketFd clientFd = ::accept(listenFd, nullptr, nullptr);
    if (clientFd == INVALID_SOCK) {
      if (isInterruptedAccept())
        continue;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    handleConnection(clientFd, ctx, htmlPath);
    closeSocket(clientFd);
  }
}
