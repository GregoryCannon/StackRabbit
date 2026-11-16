// live_game_server.cpp — standalone HTTP server serving the live-game
// precompute contract over POSIX sockets, zero dependencies (the route/flow
// reference below; also documented in docs/engine_endpoints.md).
//
// Build (from repo root):
//   g++ -O2 -std=c++17 -pthread -o build/live_game_server src/cpp_modules/src/live_game_server.cpp
//
// Run:
//   ./build/live_game_server [<port>]     (default port 3001, overridable via
//                                          argv[1] or the PORT env var)
//
// Routes (text/plain responses; `Connection: close` on every response):
//   GET /ping                  -> 200 "pong"
//   GET /precompute?p=<pipe>   -> 200 "Request accepted." and the pipe is
//                                 computed asynchronously (backup response
//                                 first, then the full precompute); 503
//                                 "Busy" if a request is still computing;
//                                 400 if the p parameter is absent
//   GET /async-result          -> 200 + full result once computed, else 200 +
//                                 backup response, else 504 "Still
//                                 calculating", else 404 "No previous async
//                                 request has been made"; a rejected/failed
//                                 engine run surfaces as 500 + message
//   anything else              -> 200 "Unknown request type. Received: <path>"
//
// The `p` parameter is the 12-field request string delimited by `,` so URLs
// work unencoded in browsers and stay readable (percent-encoded `,` is also
// accepted and decoded; `|` is rejected at the HTTP boundary). Internally the
// server translates `,` to `|` and delegates validation to mainProcess (in
// main.cpp). The agent's flow: kick /precompute as soon as the upcoming piece
// is known, then poll /async-result once, right before that piece spawns (~160ms
// later).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

// Pull in all the engine code the same way entrypoint.cpp does.
#include "main.cpp"

namespace {

const int DEFAULT_PORT = 3000;
const int RECV_TIMEOUT_SECONDS = 5; // header reads that outlive this can't wedge the accept loop
const size_t READ_BUFFER_LIMIT = 8192;
const int LISTEN_BACKLOG = 16;

// ---------------------------------------------------------------------------
// The async slot: shared state between the accept loop and the worker thread.
// This is the only file-scope mutable state in the server, and every access
// is guarded by its mutex. Only the worker thread calls into the engine.
// ---------------------------------------------------------------------------

struct AsyncSlot {
  enum class State { IDLE, RUNNING, DONE };

  std::mutex mutex;
  std::condition_variable jobReady;
  State state = State::IDLE;
  std::chrono::steady_clock::time_point requestReceived;
  std::string pipe; // input of the request being computed
  bool hasPartial = false;
  std::string partial; // backup response, stored a few ms after the kick
  bool hasFull = false;
  std::string full; // full precompute response
  bool hasError = false;
  std::string error; // engine rejection message or exception message
  bool backupUsedForRequest = false;
  long long totalBackupsUsed = 0;
  std::atomic<bool> cancelRequested{false};
};

AsyncSlot g_slot;

void storePartial(const std::string &response) {
  std::lock_guard<std::mutex> lock(g_slot.mutex);
  g_slot.partial = response;
  g_slot.hasPartial = true;
}

void storeFull(const std::string &response) {
  std::lock_guard<std::mutex> lock(g_slot.mutex);
  g_slot.full = response;
  g_slot.hasFull = true;
  double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                               - g_slot.requestReceived)
                         .count();
  printf("Precompute time: %.2fms       Total backups used: %lld\n", elapsedMs,
         g_slot.totalBackupsUsed);
  fflush(stdout);
}

void storeError(const std::string &message) {
  std::lock_guard<std::mutex> lock(g_slot.mutex);
  g_slot.error = message;
  g_slot.hasError = true;
}

void markDone() {
  std::lock_guard<std::mutex> lock(g_slot.mutex);
  g_slot.state = AsyncSlot::State::DONE;
}

// The engine reports malformed input by returning an "Error: ..." string
// rather than throwing (see main.cpp's validation). A rejected pipe therefore
// ends up as a 500 at /async-result instead of a rejected kick: validation
// happens on the worker thread, after the kick has already been answered.
// The agent treats a rejected kick and a failed poll the same way (fatal), so
// the contract holds either way.
bool isEngineRejection(const std::string &response) { return response.rfind("Error:", 0) == 0; }

// ---------------------------------------------------------------------------
// Startup calibration & worker thread
// ---------------------------------------------------------------------------

// Benchmarks 7-thread precompute throughput at startup so Round 1 of timed (-2)
// requests starts with an accurate hardware/load estimate before Round 1's own
// runtime measurement takes over for Rounds 2-3.
double calibratePrecomputeSpeed7T() {
  const auto started = std::chrono::steady_clock::now();
  const int calibPlayouts = 1200;
  const int calibLength = 8;

  std::string flatBoard(200, '0');
  for (int r = 14; r < 20; r++) {
    for (int c = 0; c < 9; c++) {
      flatBoard[r * 10 + c] = '1';
    }
  }

  std::string overhangBoard(200, '0');
  for (int r = 8; r < 20; r++) {
    for (int c = 0; c < 9; c++) {
      overhangBoard[r * 10 + c] = '1';
    }
  }
  // Carve overhang pockets under filled minos to exercise tuck detection + fastEval.
  overhangBoard[9 * 10 + 2] = '0';
  overhangBoard[10 * 10 + 5] = '0';
  overhangBoard[11 * 10 + 7] = '0';

  auto measureBoard = [&](const std::string &board) {
    auto makePipe = [&](int playouts, int breadth) {
      return board
             + "|18|20|4|-1|X..|"
             + std::to_string(playouts)
             + "|"
             + std::to_string(calibLength)
             + "|"
             + std::to_string(breadth)
             + "|safe|16|18|";
    };
    const std::string zeroPipe = makePipe(0, -1);
    const std::string workPipe = makePipe(calibPlayouts, -1);

    auto t0 = std::chrono::steady_clock::now();
    mainProcess(zeroPipe.c_str(), PRECOMPUTE);
    double baselineMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    auto t1 = std::chrono::steady_clock::now();
    mainProcess(workPipe.c_str(), PRECOMPUTE);
    double workMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();

    double playoutMs = std::max(1.0, workMs - baselineMs);
    return (calibPlayouts * (double)calibLength) / playoutMs;
  };

  // Warm up on the first pass and take the peak of two runs per board so a single startup
  // OS scheduling pause doesn't lock calibration abnormally low for the session.
  double flatRate = std::max(measureBoard(flatBoard), measureBoard(flatBoard));
  double overhangRate = std::max(measureBoard(overhangBoard), measureBoard(overhangBoard));
  double blendedRate = (2.0 * flatRate * overhangRate) / (flatRate + overhangRate);
  double totalMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  printf("live_game_server: calibrated 7T speed=%.1f moves/ms (flat=%.1f, overhang=%.1f) in "
         "%.1fms\n",
         blendedRate, flatRate, overhangRate, totalMs);
  fflush(stdout);
  return blendedRate;
}

// Translates the URL's comma-delimited 12-field query value into the
// engine's internal '|'-delimited pipe string. Rejects '|' so the HTTP
// boundary enforces commas strictly rather than silently accepting both.
bool urlQueryToEnginePipe(const std::string &queryInput, std::string &enginePipe,
                          std::string &errorOut) {
  if (queryInput.find('|') != std::string::npos) {
    errorOut = "Error: invalid delimiter '|' (expected ',' between fields).";
    return false;
  }
  enginePipe = queryInput;
  for (char &ch : enginePipe) {
    if (ch == ',') {
      ch = '|';
    }
  }
  return true;
}

// Computes one request in two tiers:
//   1. the backup response (a few ms: one move search plus cheap evals), so a
//      poll landing before the full result is answered with it instead of
//      504 — with the agent's playout config tuned to fill the ~160ms window,
//      overruns are routine and the backup is the designed answer;
//   2. the full precompute response.
// The first failure (engine rejection or thrown exception) wins: the message
// is stored as the slot's error, the remaining tiers are skipped, and the
// slot is marked done. Nothing escapes this thread.
void workerLoop(double calibratedMovesPerMs7T) {
  for (;;) {
    std::string pipe;
    std::chrono::steady_clock::time_point requestReceived;
    {
      std::unique_lock<std::mutex> lock(g_slot.mutex);
      g_slot.jobReady.wait(lock, [] { return g_slot.state == AsyncSlot::State::RUNNING; });
      pipe = g_slot.pipe;
      requestReceived = g_slot.requestReceived;
    }

    // One line per request with the input and the compute time of each stage,
    // so a live session's log can be replayed request-by-request.
    auto backupStarted = std::chrono::steady_clock::now();
    std::string enginePipe;
    std::string convError;
    double fullMs = -1;
    std::string backup;
    bool backupUsable = false;
    if (!urlQueryToEnginePipe(pipe, enginePipe, convError)) {
      storeError(convError);
    } else {
      try {
        backup = mainProcess(enginePipe.c_str(), PRECOMPUTE_BACKUP);
        if (isEngineRejection(backup)) {
          // Both request types share the same pipe validation, so a rejection
          // here means the full run would be rejected identically: fail fast.
          storeError(backup);
        } else {
          storePartial(backup);
          backupUsable = true;
        }
      } catch (const std::exception &e) {
        storeError(e.what());
      } catch (...) {
        storeError("Unknown engine error (non-std exception)");
      }
    }
    double backupMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - backupStarted)
            .count();

    if (backupUsable) {
      auto fullStarted = std::chrono::steady_clock::now();
      EngineTimingContext timing;
      timing.hasRequestReceived = true;
      timing.requestReceived = requestReceived;
      timing.calibratedMovesPerMs7T = calibratedMovesPerMs7T;
      timing.cancelFlag = &g_slot.cancelRequested;
      try {
        std::string full = mainProcess(enginePipe.c_str(), PRECOMPUTE, &timing);
        if (g_slot.cancelRequested.load(std::memory_order_relaxed)) {
          printf("Precompute halted (backup already served)       Total backups used: %lld\n",
                 g_slot.totalBackupsUsed);
          fflush(stdout);
        } else if (isEngineRejection(full)) {
          storeError(full);
        } else {
          storeFull(full);
        }
      } catch (const std::exception &e) {
        if (!g_slot.cancelRequested.load(std::memory_order_relaxed)) {
          storeError(e.what());
        }
      } catch (...) {
        if (!g_slot.cancelRequested.load(std::memory_order_relaxed)) {
          storeError("Unknown engine error (non-std exception)");
        }
      }
      fullMs =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fullStarted)
              .count();
    }

    printf("[compute] backup=%.2fms full=%.2fms input=%s\n", backupMs, fullMs, pipe.c_str());
    fflush(stdout);
    markDone();
  }
}

// ---------------------------------------------------------------------------
// Pipe decoding (the URL layer is deliberately tiny)
// ---------------------------------------------------------------------------

int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Decodes %XX sequences (case-insensitive hex). Invalid escapes are left
// as-is; whatever they decode to is rejected by the engine's pipe validation.
// '+' is left alone: the pipe format has no spaces, and the agent encodes
// with percent escapes only.
std::string percentDecode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%'
        && i + 2 < s.size()
        && hexDigitValue(s[i + 1]) >= 0
        && hexDigitValue(s[i + 2]) >= 0) {
      out.push_back((char)((hexDigitValue(s[i + 1]) << 4) | hexDigitValue(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// Extracts the value of the `p` query parameter, still percent-encoded.
// The name is matched at a parameter boundary (start of the query string or
// right after '&') so a differently-named parameter that merely contains
// "p=" can never capture the value.
bool extractPipeParam(const std::string &query, std::string &value) {
  size_t searchFrom = 0;
  for (;;) {
    size_t found = query.find("p=", searchFrom);
    if (found == std::string::npos) {
      return false;
    }
    if (found == 0 || query[found - 1] == '&') {
      size_t valueStart = found + 2;
      size_t valueEnd = query.find('&', valueStart);
      value = query.substr(valueStart, valueEnd == std::string::npos ? std::string::npos
                                                                     : valueEnd - valueStart);
      return true;
    }
    searchFrom = found + 1;
  }
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

struct RouteResult {
  int status;
  std::string body;
  std::string logSuffix;
};

// Kicks off an async precompute: hands the decoded pipe to the worker and
// answers immediately — the agent ignores this body and only needs the 200.
// 503 "Busy" if a request is still computing. That is a deliberate hardening
// over the legacy TS server, whose in-progress flag was toggled after an
// event-loop tick and could race a second kick into overwriting a running
// computation; the agent's waitingOnAsyncRequest gate never overlaps requests,
// so this path is defensive only.
RouteResult handlePrecomputeKick(const std::string &query) {
  auto received = std::chrono::steady_clock::now();
  std::string encodedPipe;
  if (!extractPipeParam(query, encodedPipe)) {
    return {400, "Missing required query parameter: p", ""};
  }
  std::string pipe = percentDecode(encodedPipe);
  {
    std::lock_guard<std::mutex> lock(g_slot.mutex);
    if (g_slot.state == AsyncSlot::State::RUNNING) {
      return {503, "Busy", ""};
    }
    g_slot.requestReceived = received;
    g_slot.pipe = pipe;
    g_slot.partial.clear();
    g_slot.hasPartial = false;
    g_slot.full.clear();
    g_slot.hasFull = false;
    g_slot.error.clear();
    g_slot.hasError = false;
    g_slot.backupUsedForRequest = false;
    g_slot.cancelRequested.store(false, std::memory_order_relaxed);
    g_slot.state = AsyncSlot::State::RUNNING;
  }
  g_slot.jobReady.notify_one();
  return {200, "Request accepted.", ""};
}

// The slot state machine. Once a request finishes, the full result supersedes
// the backup for later polls; the slot persists until the next /precompute
// kick resets it. A failed run (rejection or exception) reports 500 with the
// message and takes priority over any stored result.
RouteResult handleAsyncResult() {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_slot.mutex);
  if (g_slot.state == AsyncSlot::State::IDLE) {
    return {404, "No previous async request has been made", ""};
  }
  double sincePrecomputeMs =
      std::chrono::duration<double, std::milli>(now - g_slot.requestReceived).count();
  char elapsedBuf[64];
  snprintf(elapsedBuf, sizeof(elapsedBuf), "%.2fms after /precompute", sincePrecomputeMs);

  if (g_slot.hasError) {
    return {500, g_slot.error, std::string(" (") + elapsedBuf + ")"};
  }
  if (g_slot.hasFull) {
    return {200, g_slot.full, std::string(" (") + elapsedBuf + ")"};
  }
  if (g_slot.hasPartial) {
    if (!g_slot.backupUsedForRequest) {
      g_slot.backupUsedForRequest = true;
      g_slot.totalBackupsUsed++;
    }
    g_slot.cancelRequested.store(true, std::memory_order_relaxed);
    return {200, g_slot.partial,
            std::string(" (")
                + elapsedBuf
                + ", backup)       Total backups used: "
                + std::to_string(g_slot.totalBackupsUsed)};
  }
  return {504, "Still calculating", std::string(" (") + elapsedBuf + ")"};
}

RouteResult routeRequest(const std::string &path, const std::string &query) {
  if (path == "/ping") {
    return {200, "pong", ""};
  }
  if (path == "/precompute") {
    return handlePrecomputeKick(query);
  }
  if (path == "/async-result") {
    return handleAsyncResult();
  }
  return {200, "Unknown request type. Received: " + path, ""};
}

// ---------------------------------------------------------------------------
// HTTP plumbing
// ---------------------------------------------------------------------------

const char *statusText(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 408:
    return "Request Timeout";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  case 504:
    return "Gateway Timeout";
  default:
    return "";
  }
}

void sendAll(int fd, const char *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    ssize_t n = ::send(fd, data + sent, length - sent, 0);
    if (n <= 0) {
      return; // client gone; SIGPIPE is ignored so this surfaces as EPIPE
    }
    sent += (size_t)n;
  }
}

void sendResponse(int fd, int status, const std::string &body) {
  std::string head = "HTTP/1.1 "
                     + std::to_string(status)
                     + " "
                     + statusText(status)
                     + "\r\n"
                       "Content-Type: text/plain\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Content-Length: "
                     + std::to_string(body.size())
                     + "\r\n"
                       "Connection: close\r\n"
                       "\r\n";
  sendAll(fd, head.data(), head.size());
  sendAll(fd, body.data(), body.size());
}

// Reads from the socket until the end of the request headers ("\r\n\r\n").
// Returns false on disconnect, receive timeout, or when the header block
// exceeds the read buffer cap.
bool readRequestHeaders(int fd, std::string &out) {
  char buf[2048];
  out.clear();
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      return false; // timeout (EAGAIN), disconnect, or error
    }
    out.append(buf, (size_t)n);
    if (out.find("\r\n\r\n") != std::string::npos) {
      return true;
    }
    if (out.size() > READ_BUFFER_LIMIT) {
      return false;
    }
  }
}

// Reads and answers one connection, then closes it. One concise log line per
// request goes to stdout (path, status, duration).
void handleConnection(int fd) {
  timeval timeout{};
  timeout.tv_sec = RECV_TIMEOUT_SECONDS;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  auto started = std::chrono::steady_clock::now();
  auto elapsedMs = [started] {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
        .count();
  };

  std::string raw;
  if (!readRequestHeaders(fd, raw)) {
    printf("GET <incomplete request> 408 %.2fms (headers never completed)\n", elapsedMs());
    fflush(stdout);
    sendResponse(fd, 408, "Request headers timeout.");
    return;
  }

  // Request line: GET <target> HTTP/1.x
  size_t lineEnd = raw.find("\r\n");
  std::string requestLine = raw.substr(0, lineEnd);
  size_t firstSpace = requestLine.find(' ');
  size_t secondSpace =
      (firstSpace == std::string::npos) ? std::string::npos : requestLine.find(' ', firstSpace + 1);
  bool wellFormed = firstSpace != std::string::npos
                    && secondSpace != std::string::npos
                    && secondSpace + 1 < requestLine.size()
                    && requestLine.find(' ', secondSpace + 1) == std::string::npos
                    && requestLine.compare(secondSpace + 1, 5, "HTTP/") == 0;
  std::string method = wellFormed ? requestLine.substr(0, firstSpace) : "";
  std::string target =
      wellFormed ? requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1) : "";
  if (!wellFormed) {
    printf("GET <malformed request line> 400 %.2fms\n", elapsedMs());
    fflush(stdout);
    sendResponse(fd, 400, "Malformed request line.");
    return;
  }
  if (method != "GET") {
    printf("%s <non-GET> 400 %.2fms\n", method.c_str(), elapsedMs());
    fflush(stdout);
    sendResponse(fd, 400, "Only GET requests are supported.");
    return;
  }

  // Target: split path and query at the first '?'.
  std::string path = target;
  std::string query;
  size_t queryStart = target.find('?');
  if (queryStart != std::string::npos) {
    path = target.substr(0, queryStart);
    query = target.substr(queryStart + 1);
  }

  RouteResult result;
  try {
    result = routeRequest(path, query);
  } catch (const std::exception &e) {
    result = {500, std::string(e.what())};
  } catch (...) {
    result = {500, "Unknown server error"};
  }
  sendResponse(fd, result.status, result.body);
  const char *prefix = (path == "/precompute") ? "\n" : "";
  if (result.status == 200) {
    printf("%sGET %s%s\n", prefix, path.c_str(), result.logSuffix.c_str());
  } else {
    printf("%sGET %s %d%s\n", prefix, path.c_str(), result.status, result.logSuffix.c_str());
  }
  fflush(stdout);
}

// Strict port parsing (digits only) so a junk value fails loudly instead of
// silently binding to port 0 via atoi's 0-on-garbage behavior.
bool parsePort(const char *text, int &port) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  for (const char *c = text; *c != '\0'; c++) {
    if (*c < '0' || *c > '9') {
      return false;
    }
  }
  long value = atol(text);
  if (value < 1 || value > 65535) {
    return false;
  }
  port = (int)value;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  // Port: argv[1] if given, else the PORT env var, else the default. A
  // non-numeric or out-of-range explicit value fails loudly rather than
  // silently falling back (atoi's 0-on-garbage would otherwise bind to a
  // kernel-chosen port); an empty PORT counts as unset.
  int port = DEFAULT_PORT;
  if (argc > 1) {
    if (!parsePort(argv[1], port)) {
      fprintf(stderr, "live_game_server: invalid port '%s' (expected 1-65535)\n", argv[1]);
      return 1;
    }
  } else {
    const char *envPort = getenv("PORT");
    if (envPort != nullptr && *envPort != '\0' && !parsePort(envPort, port)) {
      fprintf(stderr, "live_game_server: invalid PORT value '%s' (expected 1-65535)\n", envPort);
      return 1;
    }
  }

  int listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd < 0) {
    perror("live_game_server: socket");
    return 1;
  }
  int one = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((unsigned short)port);
  if (::bind(listenFd, (sockaddr *)&address, sizeof(address)) < 0) {
    perror("live_game_server: bind");
    return 1;
  }
  if (listen(listenFd, LISTEN_BACKLOG) < 0) {
    perror("live_game_server: listen");
    return 1;
  }

  double calibratedMovesPerMs7T = calibratePrecomputeSpeed7T();
  std::thread worker(workerLoop, calibratedMovesPerMs7T);
  worker.detach();

  printf("live_game_server: listening on 127.0.0.1:%d\n", port);
  fflush(stdout);

  for (;;) {
    int fd = accept(listenFd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      // Back off briefly so an unexpected error can't spin the loop hot.
      usleep(10000);
      continue;
    }
    handleConnection(fd);
    ::close(fd);
  }
}
