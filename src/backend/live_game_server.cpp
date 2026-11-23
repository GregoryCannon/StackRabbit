// live_game_server.cpp — standalone HTTP server serving the live-game
// precompute and A/B test contracts over native sockets (POSIX and Winsock2),
// zero dependencies (also documented in docs/engine_endpoints.md).
//
// Build (from repo root):
//   macOS/Linux:
//     g++ -O2 -std=c++17 -pthread -o build/live_game_server src/backend/live_game_server.cpp
//   Windows (MinGW):
//     g++ -O2 -std=c++17 -pthread -o build/live_game_server.exe src/backend/live_game_server.cpp
//     -lws2_32
//   Windows (MSVC):
//     cl /O2 /std:c++17 /EHsc /Fe:build\live_game_server.exe src\backend\live_game_server.cpp
//     ws2_32.lib
//
// Run:
//   ./build/live_game_server [<port>]     (default port 3000, overridable via
//                                          argv[1] or the PORT env var)
//
// Routes (text/plain responses; `Connection: close` on every response):
//   GET /ping                       -> 200 "pong"
//   GET /precompute?p=<pipe>        -> 200 "Request accepted." and the pipe is
//                                      computed asynchronously (backup response
//                                      first, then the full precompute); 503
//                                      "Busy" if a request is still computing;
//                                      400 if the p parameter is absent
//   GET /precompute-sync?p=<pipe>   -> 200 + full precompute result computed
//                                      synchronously; 503 "Busy" if an async
//                                      precompute is running; 400 if p absent;
//                                      500 on delimiter/validation/engine error
//   GET /get-move-ab-test?p=<pipe>  -> 200 + JSON A/B move comparison computed
//                                      synchronously; 400 if p absent; 500 on
//                                      delimiter/validation/engine error
//   GET /debug-precompute?p=<pipe>  -> 200 + JSON phantom collapse breakdown
//                                      (ranked phantoms, per-next-piece raw lock
//                                      value, DAS penalty, input cost, reachable
//                                      adjustments, plus exact precompute response);
//                                      503 "Busy" if async precompute is running;
//                                      400 if p absent; 500 on pipe error
//   GET /debug-ground-truth?p=<pipe>[&keys=k1,k2][&n=2000][&conts=5][&nnb=0|1]
//                                   -> 200 + JSON high-N ground-truth playout
//                                      comparison across all 7 next pieces (or
//                                      1-ply NNB mode when nnb=1) for selected
//                                      first-piece lock keys (plus all reachable
//                                      placements); 503 "Busy" if an async
//                                      precompute is running; 400 on missing p
//                                      or invalid n/conts/keys/nnb; 500 on pipe
//                                      validation error
//   GET /debug-tournament?p=<pipe>&next=<0..6|I|O|L|J|T|S|Z|NNB|-1>
//                                   -> 200 + JSON round-by-round tournament
//                                      funnel trace (R1/Cut1/R2/Cut2/R3, per-cont
//                                      samples/mean/SE/LCB/death%) for one next
//                                      piece or 1-ply NNB mode; requires
//                                      pruningBreadth -1 or -2; 503 "Busy" if
//                                      async precompute running; 400 on missing
//                                      p/next; 500 on pipe error
//   GET
//   /debug-playout-compare?p=<pipe>&next=<0..6|I..Z|NNB|-1>&a=<spec>&b=<spec>[&n=500][&first=<0..6|I..Z>]
//                                   -> 200 + JSON paired playout decomposition
//                                      (additive eval-factor deltas, quantiles,
//                                      behavioral counters, depth curve, and up to
//                                      4 curated ply-by-ply sequence traces) for two
//                                      2-ply pairs (fk;sk) or two 1-ply NNB moves
//                                      (fk); 503 "Busy" if async precompute running;
//                                      400 on missing/invalid params; 500 on pipe
//                                      error or unreachable placement/pair
//   GET /async-result               -> 200 + full result once computed, else
//                                      200 + backup response, else 504 "Still
//                                      calculating", else 404 "No previous
//                                      async request has been made"; a
//                                      rejected/failed engine run surfaces as
//                                      500 + message
//   anything else                   -> 200 "Unknown request type. Received: <path>"
//
// The `p` parameter is the comma-delimited request string (12 fields for
// precompute; 11 or 12 fields for get-move-ab-test) so URLs work unencoded in
// browsers and stay readable (percent-encoded `,` is also accepted and decoded;
// `|` is rejected at the HTTP boundary). Internally the server translates `,`
// to `|` and delegates validation to mainProcess (in main.cpp). The agent's
// live-play flow: kick /precompute as soon as the upcoming piece is known, then
// poll /async-result once, right before that piece spawns (~160ms later).

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef OUT
#undef OUT
#endif
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

// Pull in all the engine code the same way entrypoint.cpp does.
#include "main.cpp"
#include "debug_ground_truth.hpp"
#include "debug_precompute.hpp"
#include "debug_tournament.hpp"
#include "debug_playout_compare.hpp"

namespace {

const int DEFAULT_PORT = 3000;
const int RECV_TIMEOUT_SECONDS = 5; // header reads that outlive this can't wedge the accept loop
const size_t READ_BUFFER_LIMIT = 8192;
const int LISTEN_BACKLOG = 16;

// ---------------------------------------------------------------------------
// Minimal cross-platform socket helpers (POSIX / Winsock2)
// ---------------------------------------------------------------------------

#ifdef _WIN32
using SocketFd = SOCKET;
const SocketFd INVALID_SOCK = INVALID_SOCKET;

bool initSockets() {
  WSADATA wsa{};
  int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
  if (rc != 0) {
    fprintf(stderr, "live_game_server: WSAStartup failed (%d)\n", rc);
    return false;
  }
  return true;
}

void cleanupSockets() { WSACleanup(); }

void closeSocket(SocketFd fd) { closesocket(fd); }

void setRecvTimeout(SocketFd fd, int seconds) {
  DWORD timeoutMs = static_cast<DWORD>(seconds) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs),
             sizeof(timeoutMs));
}

void setListenReuseAddr(SocketFd /* fd */) {
  // No-op on Windows: SO_REUSEADDR allows hijacking an active port, while
  // SO_EXCLUSIVEADDRUSE blocks restarts during TIME_WAIT for up to 120s.
}

bool isInterruptedAccept() { return WSAGetLastError() == WSAEINTR; }

void logSocketError(const char *label) {
  fprintf(stderr, "%s: WSA error %d\n", label, WSAGetLastError());
}
#else
using SocketFd = int;
const SocketFd INVALID_SOCK = -1;

bool initSockets() {
  signal(SIGPIPE, SIG_IGN);
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

void logSocketError(const char *label) { perror(label); }
#endif

struct SocketSubsystemGuard {
  bool ok = false;
  SocketSubsystemGuard() : ok(initSockets()) {}
  ~SocketSubsystemGuard() {
    if (ok) {
      cleanupSockets();
    }
  }
};

// ---------------------------------------------------------------------------
// The async slot: shared state between the accept loop and the worker thread.
// This is the only file-scope mutable state in the server, and every access
// is guarded by its mutex. Async precompute runs on the worker thread;
// synchronous routes run on the accept-loop thread (/precompute-sync rejects
// with 503 Busy while the async worker is RUNNING to avoid oversubscribing
// two concurrent 7-thread precomputes).
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

// Translates the URL's comma-delimited query value (12 fields for precompute;
// 11 or 12 for get-move-ab-test) into the engine's internal '|'-delimited pipe
// string. Rejects '|' so the HTTP boundary enforces commas strictly rather than
// silently accepting both.
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

// Extracts the value of a named query parameter, still percent-encoded.
// The name is matched at a parameter boundary (start of the query string or
// right after '&') so a differently-named parameter can never capture it.
bool extractQueryParam(const std::string &query, const char *name, std::string &value) {
  const std::string prefix = std::string(name) + "=";
  size_t searchFrom = 0;
  for (;;) {
    size_t found = query.find(prefix, searchFrom);
    if (found == std::string::npos) {
      return false;
    }
    if (found == 0 || query[found - 1] == '&') {
      size_t valueStart = found + prefix.size();
      size_t valueEnd = query.find('&', valueStart);
      value = query.substr(valueStart, valueEnd == std::string::npos ? std::string::npos
                                                                     : valueEnd - valueStart);
      return true;
    }
    searchFrom = found + 1;
  }
}

bool extractPipeParam(const std::string &query, std::string &value) {
  return extractQueryParam(query, "p", value);
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
// Returns 503 "Busy" if a request is still computing so an overlapping kick
// cannot clobber a running computation.
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

bool parseEnginePipeQuery(const std::string &query, std::string &enginePipe,
                          RouteResult &errorResult) {
  std::string encodedPipe;
  if (!extractPipeParam(query, encodedPipe)) {
    errorResult = {400, "Missing required query parameter: p", ""};
    return false;
  }
  std::string convError;
  if (!urlQueryToEnginePipe(percentDecode(encodedPipe), enginePipe, convError)) {
    errorResult = {500, convError, ""};
    return false;
  }
  return true;
}

std::string formatDurationSuffix(std::chrono::steady_clock::time_point started) {
  double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  char buf[32];
  snprintf(buf, sizeof(buf), " (%.2fms)", ms);
  return std::string(buf);
}

RouteResult handlePrecomputeSync(const std::string &query, double calibratedMovesPerMs7T) {
  auto received = std::chrono::steady_clock::now();
  std::string enginePipe;
  RouteResult parseError;
  if (!parseEnginePipeQuery(query, enginePipe, parseError)) {
    return parseError;
  }
  {
    std::lock_guard<std::mutex> lock(g_slot.mutex);
    if (g_slot.state == AsyncSlot::State::RUNNING) {
      return {503, "Busy", ""};
    }
  }
  EngineTimingContext timing;
  timing.hasRequestReceived = true;
  timing.requestReceived = received;
  timing.calibratedMovesPerMs7T = calibratedMovesPerMs7T;
  std::string response = mainProcess(enginePipe.c_str(), PRECOMPUTE, &timing);
  std::string suffix = formatDurationSuffix(received);
  if (isEngineRejection(response)) {
    return {500, response, suffix};
  }
  return {200, response, suffix};
}

RouteResult handleGetMoveAbTest(const std::string &query) {
  auto started = std::chrono::steady_clock::now();
  std::string enginePipe;
  RouteResult parseError;
  if (!parseEnginePipeQuery(query, enginePipe, parseError)) {
    return parseError;
  }
  std::string response = mainProcess(enginePipe.c_str(), GET_MOVE_AB_TEST);
  std::string suffix = formatDurationSuffix(started);
  if (isEngineRejection(response)) {
    return {500, response, suffix};
  }
  return {200, response, suffix};
}

RouteResult handleDebugPrecompute(const std::string &query, double calibratedMovesPerMs7T) {
  auto received = std::chrono::steady_clock::now();
  std::string enginePipe;
  RouteResult parseError;
  if (!parseEnginePipeQuery(query, enginePipe, parseError)) {
    return parseError;
  }
  {
    std::lock_guard<std::mutex> lock(g_slot.mutex);
    if (g_slot.state == AsyncSlot::State::RUNNING) {
      return {503, "Busy", ""};
    }
  }
  EngineTimingContext timing;
  timing.hasRequestReceived = true;
  timing.requestReceived = received;
  timing.calibratedMovesPerMs7T = calibratedMovesPerMs7T;
  std::string response = debug_pre::runDebugPrecompute(enginePipe, &timing);
  std::string suffix = formatDurationSuffix(received);
  if (isEngineRejection(response)) {
    return {500, response, suffix};
  }
  return {200, response, suffix};
}

RouteResult handleDebugGroundTruth(const std::string &query) {
  auto started = std::chrono::steady_clock::now();
  std::string enginePipe;
  RouteResult parseError;
  if (!parseEnginePipeQuery(query, enginePipe, parseError)) {
    return parseError;
  }
  {
    std::lock_guard<std::mutex> lock(g_slot.mutex);
    if (g_slot.state == AsyncSlot::State::RUNNING) {
      return {503, "Busy", ""};
    }
  }

  int playoutCount = debug_gt::DEFAULT_DEBUG_PLAYOUTS;
  std::string nRaw;
  if (extractQueryParam(query, "n", nRaw) && !nRaw.empty()) {
    std::string nDecoded = percentDecode(nRaw);
    if (!debug_gt::isPlainInt(nDecoded)) {
      return {400, "Error: invalid n parameter (expected integer): " + nDecoded, ""};
    }
    playoutCount = std::atoi(nDecoded.c_str());
    if (playoutCount < 0 || playoutCount > CANONICAL_SEQUENCE_BATCH_SIZE) {
      return {400,
              "Error: invalid n parameter (expected 0-"
                  + std::to_string(CANONICAL_SEQUENCE_BATCH_SIZE)
                  + "): "
                  + nDecoded,
              ""};
    }
  }

  int maxConts = debug_gt::DEFAULT_DEBUG_CONTS;
  std::string contsRaw;
  if (extractQueryParam(query, "conts", contsRaw) && !contsRaw.empty()) {
    std::string contsDecoded = percentDecode(contsRaw);
    if (!debug_gt::isPlainInt(contsDecoded)) {
      return {400, "Error: invalid conts parameter (expected integer): " + contsDecoded, ""};
    }
    maxConts = std::atoi(contsDecoded.c_str());
    if (maxConts < 1 || maxConts > debug_gt::MAX_DEBUG_CONTS) {
      return {400,
              "Error: invalid conts parameter (expected 1-"
                  + std::to_string(debug_gt::MAX_DEBUG_CONTS)
                  + "): "
                  + contsDecoded,
              ""};
    }
  }

  std::vector<std::string> requestedKeys;
  std::string keysRaw;
  if (extractQueryParam(query, "keys", keysRaw) && !keysRaw.empty()) {
    std::string keysDecoded = percentDecode(keysRaw);
    size_t pos = 0;
    while (pos <= keysDecoded.size()) {
      size_t comma = keysDecoded.find(',', pos);
      std::string token =
          keysDecoded.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      if (!token.empty()) {
        requestedKeys.push_back(token);
      }
      if (comma == std::string::npos) {
        break;
      }
      pos = comma + 1;
    }
    if (static_cast<int>(requestedKeys.size()) > debug_gt::MAX_DEBUG_KEYS) {
      return {400, "Error: too many keys (max " + std::to_string(debug_gt::MAX_DEBUG_KEYS) + ").",
              ""};
    }
  }

  bool noNextBox = false;
  std::string nnbRaw;
  if (extractQueryParam(query, "nnb", nnbRaw) && !nnbRaw.empty()) {
    std::string nnbDecoded = percentDecode(nnbRaw);
    if (nnbDecoded == "1" || nnbDecoded == "true") {
      noNextBox = true;
    } else if (nnbDecoded == "0" || nnbDecoded == "false") {
      noNextBox = false;
    } else {
      return {400, "Error: invalid nnb parameter (expected 0/1 or false/true): " + nnbDecoded, ""};
    }
  }

  std::string response =
      debug_gt::runDebugGroundTruth(enginePipe, requestedKeys, playoutCount, maxConts, noNextBox);
  std::string suffix = formatDurationSuffix(started);
  if (isEngineRejection(response)) {
    return {500, response, suffix};
  }
  return {200, response, suffix};
}

RouteResult handleDebugTournament(const std::string &query, double calibratedMovesPerMs7T) {
  auto received = std::chrono::steady_clock::now();
  std::string enginePipe;
  RouteResult parseError;
  if (!parseEnginePipeQuery(query, enginePipe, parseError)) {
    return parseError;
  }
  {
    std::lock_guard<std::mutex> lock(g_slot.mutex);
    if (g_slot.state == AsyncSlot::State::RUNNING) {
      return {503, "Busy", ""};
    }
  }

  std::string nextRaw;
  if (!extractQueryParam(query, "next", nextRaw) || nextRaw.empty()) {
    return {400, "Missing required query parameter: next (expected 0-6, I/O/L/J/T/S/Z, or NNB/-1)",
            ""};
  }
  int nextIdx = debug_tmt::parseNextPieceArg(percentDecode(nextRaw));
  if (nextIdx == debug_tmt::NEXT_PIECE_INVALID) {
    return {400,
            "Error: invalid next parameter (expected 0-6, I/O/L/J/T/S/Z, or NNB/-1): " + nextRaw,
            ""};
  }

  EngineTimingContext timing;
  timing.hasRequestReceived = true;
  timing.requestReceived = received;
  timing.calibratedMovesPerMs7T = calibratedMovesPerMs7T;
  std::string response = debug_tmt::runDebugTournament(enginePipe, nextIdx, &timing);
  std::string suffix = formatDurationSuffix(received);
  if (isEngineRejection(response)) {
    return {500, response, suffix};
  }
  return {200, response, suffix};
}

RouteResult handleDebugPlayoutCompare(const std::string &query) {
  auto started = std::chrono::steady_clock::now();
  std::string enginePipe;
  RouteResult parseError;
  if (!parseEnginePipeQuery(query, enginePipe, parseError)) {
    return parseError;
  }
  {
    std::lock_guard<std::mutex> lock(g_slot.mutex);
    if (g_slot.state == AsyncSlot::State::RUNNING) {
      return {503, "Busy", ""};
    }
  }

  std::string nextRaw;
  if (!extractQueryParam(query, "next", nextRaw) || nextRaw.empty()) {
    return {400, "Missing required query parameter: next (expected 0-6, I/O/L/J/T/S/Z, or NNB/-1)",
            ""};
  }
  const std::string nextDecoded = percentDecode(nextRaw);
  const int nextIdx = debug_tmt::parseNextPieceArg(nextDecoded);
  if (nextIdx == debug_tmt::NEXT_PIECE_INVALID) {
    return {400,
            "Error: invalid next parameter (expected 0-6, I/O/L/J/T/S/Z, or NNB/-1): "
                + nextDecoded,
            ""};
  }
  const bool isNnb = (nextIdx == debug_tmt::NEXT_PIECE_NNB);

  int filterFirstIdx = -1;
  std::string firstRaw;
  if (extractQueryParam(query, "first", firstRaw) && !firstRaw.empty()) {
    if (!isNnb) {
      return {400, "Error: first piece filter is only valid when next=NNB.", ""};
    }
    const std::string firstDecoded = percentDecode(firstRaw);
    const int parsedFirst = debug_tmt::parsePieceIndex(firstDecoded);
    if (parsedFirst == debug_tmt::NEXT_PIECE_INVALID) {
      return {400,
              "Error: invalid first parameter (expected 0-6 or I/O/L/J/T/S/Z): " + firstDecoded,
              ""};
    }
    filterFirstIdx = parsedFirst;
  }

  std::string aRaw, bRaw;
  if (!extractQueryParam(query, "a", aRaw) || aRaw.empty()) {
    return {400,
            isNnb ? "Missing required query parameter: a (expected ply-1 key like 1|5|10)"
                  : "Missing required query parameter: a (expected firstKey;secondKey)",
            ""};
  }
  if (!extractQueryParam(query, "b", bRaw) || bRaw.empty()) {
    return {400,
            isNnb ? "Missing required query parameter: b (expected ply-1 key like 1|-1|13)"
                  : "Missing required query parameter: b (expected firstKey;secondKey)",
            ""};
  }
  const std::string aDecoded = percentDecode(aRaw);
  const std::string bDecoded = percentDecode(bRaw);
  if (isNnb) {
    int r = 0, x = 0, y = 0;
    if (!debug_tmt::parseLockKey(aDecoded, r, x, y)) {
      return {400, "Error: invalid a parameter (expected ply-1 key like 1|5|10): " + aDecoded, ""};
    }
    if (!debug_tmt::parseLockKey(bDecoded, r, x, y)) {
      return {400, "Error: invalid b parameter (expected ply-1 key like 1|-1|13): " + bDecoded, ""};
    }
  } else {
    std::string fk, sk;
    if (!debug_pc::parsePairSpec(aDecoded, fk, sk)) {
      return {400, "Error: invalid a parameter (expected firstKey;secondKey): " + aDecoded, ""};
    }
    if (!debug_pc::parsePairSpec(bDecoded, fk, sk)) {
      return {400, "Error: invalid b parameter (expected firstKey;secondKey): " + bDecoded, ""};
    }
  }

  int playoutCount = debug_pc::DEFAULT_COMPARE_PLAYOUTS;
  std::string nRaw;
  if (extractQueryParam(query, "n", nRaw) && !nRaw.empty()) {
    std::string nDecoded = percentDecode(nRaw);
    if (!debug_gt::isPlainInt(nDecoded)) {
      return {400, "Error: invalid n parameter (expected integer): " + nDecoded, ""};
    }
    playoutCount = std::atoi(nDecoded.c_str());
    if (playoutCount < 1 || playoutCount > CANONICAL_SEQUENCE_BATCH_SIZE) {
      return {400,
              "Error: invalid n parameter (expected 1-"
                  + std::to_string(CANONICAL_SEQUENCE_BATCH_SIZE)
                  + "): "
                  + nDecoded,
              ""};
    }
  }

  std::string response = debug_pc::runDebugPlayoutCompare(enginePipe, nextIdx, aDecoded, bDecoded,
                                                          playoutCount, filterFirstIdx);
  std::string suffix = formatDurationSuffix(started);
  if (isEngineRejection(response)) {
    return {500, response, suffix};
  }
  return {200, response, suffix};
}

RouteResult routeRequest(const std::string &path, const std::string &query,
                         double calibratedMovesPerMs7T) {
  if (path == "/ping") {
    return {200, "pong", ""};
  }
  if (path == "/precompute") {
    return handlePrecomputeKick(query);
  }
  if (path == "/precompute-sync") {
    return handlePrecomputeSync(query, calibratedMovesPerMs7T);
  }
  if (path == "/get-move-ab-test") {
    return handleGetMoveAbTest(query);
  }
  if (path == "/debug-precompute") {
    return handleDebugPrecompute(query, calibratedMovesPerMs7T);
  }
  if (path == "/debug-ground-truth") {
    return handleDebugGroundTruth(query);
  }
  if (path == "/debug-tournament") {
    return handleDebugTournament(query, calibratedMovesPerMs7T);
  }
  if (path == "/debug-playout-compare") {
    return handleDebugPlayoutCompare(query);
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

void sendAll(SocketFd fd, const char *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    size_t remaining = length - sent;
    int chunk = remaining > 65536 ? 65536 : static_cast<int>(remaining);
    ptrdiff_t n = ::send(fd, data + sent, chunk, 0);
    if (n <= 0) {
      return; // client gone; SIGPIPE is ignored on POSIX so this surfaces as an error
    }
    sent += static_cast<size_t>(n);
  }
}

void sendResponse(SocketFd fd, int status, const std::string &body) {
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
bool readRequestHeaders(SocketFd fd, std::string &out) {
  char buf[2048];
  out.clear();
  for (;;) {
    ptrdiff_t n = ::recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
    if (n <= 0) {
      return false; // timeout, disconnect, or error
    }
    out.append(buf, static_cast<size_t>(n));
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
void handleConnection(SocketFd fd, double calibratedMovesPerMs7T) {
  setRecvTimeout(fd, RECV_TIMEOUT_SECONDS);
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
    result = routeRequest(path, query, calibratedMovesPerMs7T);
  } catch (const std::exception &e) {
    result = {500, std::string(e.what()), ""};
  } catch (...) {
    result = {500, "Unknown server error", ""};
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
  SocketSubsystemGuard sockets;
  if (!sockets.ok) {
    return 1;
  }

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

  SocketFd listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd == INVALID_SOCK) {
    logSocketError("live_game_server: socket");
    return 1;
  }
  setListenReuseAddr(listenFd);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((unsigned short)port);
  if (::bind(listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    logSocketError("live_game_server: bind");
    closeSocket(listenFd);
    return 1;
  }
  if (listen(listenFd, LISTEN_BACKLOG) != 0) {
    logSocketError("live_game_server: listen");
    closeSocket(listenFd);
    return 1;
  }

  double calibratedMovesPerMs7T = calibratePrecomputeSpeed7T();
  std::thread worker(workerLoop, calibratedMovesPerMs7T);
  worker.detach();

  printf("live_game_server: listening on 127.0.0.1:%d\n", port);
  fflush(stdout);

  for (;;) {
    SocketFd fd = accept(listenFd, nullptr, nullptr);
    if (fd == INVALID_SOCK) {
      if (isInterruptedAccept()) {
        continue;
      }
      // Back off briefly so an unexpected error can't spin the loop hot.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    handleConnection(fd, calibratedMovesPerMs7T);
    closeSocket(fd);
  }
}
