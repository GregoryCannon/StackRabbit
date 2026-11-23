// server.cpp — standalone local HTTP server for the Very Low Lineout tablebase harness.
//
// Build (from repo root):
//   g++ -O2 -std=c++17 -Wall -Wextra -Werror experimental/tablebase/server.cpp -o
//   build/tablebase_server
//
// Run:
//   ./build/tablebase_server [port] [path/to/file.tb.bin]
//   Default port: 3030 (overridable via argv[1] or PORT env var).

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

#include "server_routes.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr int DEFAULT_PORT = 3030;
constexpr int RECV_TIMEOUT_SECONDS = 5;
constexpr size_t READ_BUFFER_LIMIT = 8192;
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
void setRecvTimeout(SocketFd fd, int seconds) {
  DWORD timeoutMs = static_cast<DWORD>(seconds) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs),
             sizeof(timeoutMs));
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
void setRecvTimeout(SocketFd fd, int seconds) {
  timeval timeout{};
  timeout.tv_sec = seconds;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
             sizeof(timeout));
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
    return src.substr(0, slash + 1) + "index.html";
  }
  return "experimental/tablebase/index.html";
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

bool parsePortStrict(const char *text, int &port) {
  if (text == nullptr || *text == '\0')
    return false;
  long val = 0;
  for (const char *c = text; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9')
      return false;
    val = val * 10 + (*c - '0');
    if (val > 65535)
      return false;
  }
  if (val < 1)
    return false;
  port = static_cast<int>(val);
  return true;
}

void handleConnection(SocketFd fd, tb::ServerContext &ctx, const std::string &htmlPath) {
  setRecvTimeout(fd, RECV_TIMEOUT_SECONDS);
  std::string raw;
  const HeaderReadStatus readStatus = readRequestHeaders(fd, raw);
  if (readStatus == HeaderReadStatus::TOO_LARGE) {
    sendHttpResponse(fd, 431, "application/json; charset=utf-8",
                     "{\"error\":\"Request headers exceeded buffer limit\"}");
    return;
  }
  if (readStatus != HeaderReadStatus::OK) {
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

  if (path == "/" || path == "/index.html") {
    std::string html;
    if (!readTextFile(htmlPath, html)) {
      sendHttpResponse(fd, 500, "text/plain; charset=utf-8",
                       "Failed to read index.html at: " + htmlPath);
      return;
    }
    sendHttpResponse(fd, 200, "text/html; charset=utf-8", html);
    return;
  }

  const tb::HttpRouteResult res = tb::handleApiRequest(path, query, ctx);
  sendHttpResponse(fd, res.status, res.contentType, res.body);
}

} // namespace

int main(int argc, char **argv) {
  SocketGuard guard;
  if (!guard.ok) {
    std::fprintf(stderr, "tablebase_server: failed to initialize sockets\n");
    return 1;
  }

  if (argc >= 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
    std::printf("Usage: %s [port] [path/to/file.tb.bin]\n", argv[0]);
    return 0;
  }
  if (argc > 3) {
    std::fprintf(stderr, "Usage: %s [port] [path/to/file.tb.bin]\n", argv[0]);
    return 1;
  }

  int port = DEFAULT_PORT;
  if (argc >= 2) {
    if (!parsePortStrict(argv[1], port)) {
      std::fprintf(stderr, "tablebase_server: invalid port '%s' (expected 1-65535)\n", argv[1]);
      return 1;
    }
  } else {
    const char *envPort = std::getenv("PORT");
    if (envPort != nullptr && *envPort != '\0' && !parsePortStrict(envPort, port)) {
      std::fprintf(stderr, "tablebase_server: invalid PORT '%s' (expected 1-65535)\n", envPort);
      return 1;
    }
  }

  tb::ServerContext ctx;
  if (argc >= 3) {
    ctx.configuredTbPath = argv[2];
    std::string err;
    if (!ctx.tablebase.open(ctx.configuredTbPath, &err)) {
      std::fprintf(stderr, "tablebase_server: %s\n", err.c_str());
      return 1;
    }
    std::printf("tablebase_server: loaded %llu boards from %s (V(empty)=%.4f, residual=%.2e)\n",
                static_cast<unsigned long long>(ctx.tablebase.size()), ctx.configuredTbPath.c_str(),
                ctx.tablebase.emptyBoardValue(), ctx.tablebase.bellmanResidual());
  }

  const SocketFd listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd == INVALID_SOCK) {
    std::perror("tablebase_server: socket");
    return 1;
  }
  setListenReuseAddr(listenFd);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    std::perror("tablebase_server: bind");
    closeSocket(listenFd);
    return 1;
  }
  if (::listen(listenFd, LISTEN_BACKLOG) != 0) {
    std::perror("tablebase_server: listen");
    closeSocket(listenFd);
    return 1;
  }

  const std::string htmlPath = defaultHtmlPath();
  std::printf("tablebase_server: listening on http://127.0.0.1:%d/\n", port);
  std::fflush(stdout);

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
