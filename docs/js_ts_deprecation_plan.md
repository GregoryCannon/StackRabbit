# JS/TS & `node-gyp` Deprecation Plan

Goal: Retire the legacy TypeScript/Express server (`src/server/*.ts`), the `node-gyp` / `nan` C++ addon (`binding.gyp`, `src/cpp_modules/src/module.cpp`, `install.js`), and all `node_modules` dependencies — leaving StackRabbit as a zero-dependency C++ engine (cross-platform native live HTTP server, CLI binaries, and WASM) with stdlib-only Node verification scripts, permanent versioned fixture baselines, and a faster, change-scoped test setup.

This plan completes the cutover phase ("Phase 5 — Live traffic") of [precompute_cpp_migration.md](precompute_cpp_migration.md).

---

## Current State & Motivation

Live gameplay in `src/fceux/stackrabbit.lua` already runs exclusively against the native C++ HTTP server (`src/cpp_modules/src/live_game_server.cpp` via `npm run live_server`), and web clients consume the Emscripten WASM build (`src/cpp_modules/src/wasm.cpp`).

The TS server (`src/server/precompute.ts`) has also diverged behind C++ `precompute.cpp` (e.g., timed `-2` budget calibration/cancellation, default-deviation penalties, and duplicate-piece commutation fixes landed in C++ only). Keeping TS and `node-gyp` around adds build/maintenance overhead (`tsc`, `node-gyp build`, `start_check`, 13 npm packages, and lockstep pipe-format updates across dead callers) with no remaining live-play use.

At the same time, the test battery still carries heavy migration-era redundancy: the same 42 production precompute fixtures are executed five separate times across a single battery run (~39s for C++ checks alone, ~50s+ including TS), and two separate golden files (`precompute_fixtures.jsonl` and `precompute_pipeline_fixtures.hpp`) must be re-baselined in lockstep on every intentional eval/precompute change.

What still depends on TS or `node-gyp` today:

1. **Unported HTTP routes**: `/precompute-sync` and `/get-move-ab-test` (consumed only by TetrisTrainer).
2. **Windows live-server support**: `live_game_server.cpp` currently uses POSIX sockets only; Windows users currently rely on the Node/`node-gyp` server (`install.js` + `npm start`).
3. **Precompute fixture re-baselining**: `src/server/precompute_fixture_generate.ts` already calls C++ (`handlePrecomputeCpp`), not TS `PreComputeManager`, but is still compiled by `tsc`, imports TS parser/handler modules, and calls the `cRabbit` `node-gyp` addon.
4. **Precompute differential verifier**: `src/server/precompute_verify.js` is plain JS, **but** loads `build/Release/cRabbit` (the `node-gyp` addon). Currently `npm run start_check` is the *only* battery item that runs `node-gyp build` — dropping `start_check` without decoupling `precompute_verify.js` would silently test a stale addon.
5. **Move-search oracle generator & legacy TS tests**: `move_search_fuzz_generate.ts`, `move_search_test.ts`, and `start_check.js`.
6. **Legacy browser test site & docs**: `src/browser_test_site/`, `README.md`, `install.js`, `AGENTS.md`, `TESTING.md`, and `docs/engine_endpoints.md`.

---

## End-State Architecture

| Category | Stays (Post-Cutover) | Deleted |
|---|---|---|
| **Engine & Server** | `src/cpp_modules/src/*` (engine + cross-platform macOS/Windows `live_game_server.cpp` + `wasm.cpp`), `entrypoint.cpp` | `src/cpp_modules/src/module.cpp`, `src/server/*.ts` (all 15 TS files) |
| **Build & Packaging** | `g++` / `em++` commands in `package.json` (`"scripts"` only, zero `node_modules`) | `binding.gyp`, `tsconfig.json`, `install.js`, `built/`, `package-lock.json`, all `dependencies` / `devDependencies` in `package.json` |
| **Test Harnesses & Verifiers** | `src/cpp_modules/tests/*` (fast doctest suite, `fixture_dump.cpp`, slim `precompute_tsan.cpp`, new `precompute_cli.cpp`, plus stdlib-only Node scripts relocated from `src/server/`: `move_search_fuzz_verify.js`, `precompute_verify.js`, `precompute_fixture_generate.js`, `live_server_check.js`, slim `live_server_verify.js`, `precompute_static_lint.js`) | `src/server/start_check.js`, `src/server/move_search_test.ts`, `src/server/move_search_fuzz_generate.ts`, `src/server/precompute_fixture_generate.ts`, `src/cpp_modules/tests/precompute_pipeline_fixtures.hpp` |
| **Golden Fixtures** | `src/cpp_modules/tests/data/move_search_fixtures.json`, `src/cpp_modules/tests/move_search_fixtures.hpp`, `src/cpp_modules/tests/data/precompute_fixtures.jsonl` (all 3,470 fixtures — single precompute golden file) | `src/cpp_modules/tests/precompute_pipeline_fixtures.hpp` (redundant duplicate of `precompute_fixtures.jsonl`) |
| **Clients & Benchmarks** | `src/fceux/*`, `src/wasm/*`, `src/benchmark/*` (incl. `sample_benchmark_positions.js`), `src/console_client/*` (kept as-is as historical relic) | `src/browser_test_site/` |

---

## Locked Design Decisions

1. **URL contract for `/precompute-sync` and `/get-move-ab-test`**
   - Both routes use the **same comma-delimited `?p=<fields>` wire contract** as `/precompute` — no named query parameters (`?board=...&currentPiece=...`).
   - TetrisTrainer is the sole consumer of both routes and its callers will be updated to match `?p=`.
   - For `/get-move-ab-test`, `main.cpp` requires the 11 standard fields (`board,level,lines,curPiece,nextPiece,timeline,playoutCount,playoutLength,pruningBreadth,aggressionLevel,dasCharge,`) with `nextPiece` set to `-1` (no next box) or `0..6`, and already ignores a trailing 12th `reactionTime` field if provided — so callers can pass the exact same 12-field shape as `/precompute` or omit `reactionTime`.

2. **Response format of `/get-move-ab-test` (`GET_MOVE_AB_TEST`)**
   - The TetrisTrainer consumer checks three things: `agree`/`disagree`, the **input sequence** (formatted placement string), and the **explanation**.
   - Run `fullMoveSearch` inside C++ (`GET_MOVE_AB_TEST` in `main.cpp` using a shared formatting helper from `precompute.cpp`) so the response carries formatted placement strings (`rot,x,y|inputSequence|boardAfter|level|lines|dasCharge`) alongside the explanation object as valid JSON:
     - Agree: `["agree", "<mainFormatted>", {"explanationMain":"..."}]`
     - Disagree: `["disagree", "<mainFormatted>", "<testFormatted>", {"explanationMain":"...", "explanationTest":"..."}]`
     - No legal move (topped-out board / `NULL_LOCK_LOCATION`): emit `"none"` as the formatted placement string instead of huge sentinel coordinates.
   - `GET_MOVE_AB_TEST` is not exposed in `wasm.cpp`, so updating its output directly in `main.cpp` keeps `live_game_server.cpp` a thin pass-through and keeps `request_type_tests.hpp` aligned.

3. **Drop `/get-move-cpp` and `/get-move-async-cpp`**
   - Neither is ported to `live_game_server.cpp`; both retire with the TS server.

4. **Cross-platform (Windows + macOS/Linux) support in `live_game_server.cpp`**
   - Add a minimal `#ifdef _WIN32` Winsock2 layer in `live_game_server.cpp` so the native server builds and runs cleanly on both Windows and macOS/Linux without external libraries.
   - Update `README.md` setup instructions for both macOS and Windows.

5. **Relocate permanent non-TS files out of `src/server/`**
   - Move golden fixtures to `src/cpp_modules/tests/data/` and stdlib-only Node scripts to `src/cpp_modules/tests/` in a dedicated pure-move commit so `src/server/` is removed cleanly.

6. **Legacy in-repo clients**
   - `src/browser_test_site/`: **delete** in Phase 5.
   - `src/console_client/`: **leave as-is** as a historical relic (no updates needed now).

7. **Post-cutover `precompute_fixture_generate`**
   - Port `precompute_fixture_generate.ts` to plain stdlib-only JS (`precompute_fixture_generate.js`) backed by the new native `build/precompute_cli` binary.

8. **Test suite streamlining & change-scoped verification**
   - **Delete `precompute_pipeline_fixtures.hpp`**: `precompute_fixtures.jsonl` becomes the single precompute golden file. Keep all **3,470** fixtures in `precompute_fixtures.jsonl` so broad coverage continues to surface unexpected edge cases.
   - **Slim `cpp_test_unit`**: compile with `-O2`, drop the duplicate golden byte-equality test from `precompute_property_tests.hpp`, and run the pipeline property/invariant checks on ~6–8 inline representative cases (tap, DAS, rt=0, `continue`, per-piece `none`, whole-response `none`, level transition) instead of 48 baked fixtures.
   - **Slim `live_server_check`**: cut the redundant 42-fixture HTTP golden loop down to ~3 representative fixtures (tap, DAS, `none`) while retaining all HTTP protocol, backup-halt, error-handling, and new-route checks.
   - **Slim `precompute_tsan`**: cut the TSan replay set from 42 production fixtures to ~6–8 diverse production fixtures (covering tap, DAS, rt=0/8/20, and timed `-2`) so it hits every 7-thread code path in ~3–4s instead of ~13s.
   - **Change-scoped test matrix**: replace the monolithic "run everything every time" rule in `TESTING.md` and `AGENTS.md` with a fast default check (~2–3s, or ~4–5s with `live_server_check`) plus targeted checks triggered by what changed (see [Post-Cutover Change-Scoped Test Matrix](#post-cutover-change-scoped-test-matrix)).

---

## Phased Execution Roadmap

Follow `AGENTS.md` change discipline: land each step as a small, independently revertible commit and verify after each step.

### Phase 1 — Decouple Verification & Fixture Generation from `node-gyp` and TS

Do this first so no permanent test or re-baseline workflow depends on `cRabbit` (`module.cpp`) or `tsc` before touching server routes or deleting files.

1. **Add a native C++ precompute CLI runner (`src/cpp_modules/tests/precompute_cli.cpp`)**
   - Unity-includes `../src/main.cpp` (same pattern as `fixture_dump.cpp` and `precompute_tsan.cpp`).
   - Reads `PRECOMPUTE` pipe strings from stdin, one per line, runs `mainProcess(line.c_str(), PRECOMPUTE)` (catching `std::exception` and emitting `Error: <what>`), and writes one output line per request with `\n` escaped as `\x01` (matching `precompute_tsan.cpp`'s framing — pipe strings and outputs never contain `\x01`).
   - Processes all requests in a single process invocation so startup cost is paid once.
   - Add `g++ -O2 -std=c++17 -pthread -o build/precompute_cli src/cpp_modules/tests/precompute_cli.cpp` directly into `npm run precompute_verify` (mirroring `fuzz_verify_sequences`) so `precompute_verify` always compiles fresh C++ and never tests a stale binary.

2. **Switch `precompute_verify.js` off `cRabbit` onto `build/precompute_cli`**
   - Remove `require("../../build/Release/cRabbit")`; spawn `build/precompute_cli` once, stream all 3,470 pipe strings on stdin, read 3,470 `\x01`-unescaped lines from stdout, and run the existing byte-comparison and categorized mismatch report unchanged.
   - Keep `--dump-inputs` working for `precompute_tsan`.

3. **Port `precompute_fixture_generate.ts` → plain JS (`precompute_fixture_generate.js`)**
   - Remove imports of `RequestHandler`, `request_parser.ts`, `types.ts`, and `cRabbit`.
   - Inline the small `parseCompressedBoard` decoder (same 32-char scheme already in `src/benchmark/sample_benchmark_positions.js`) and pipe builder.
   - Invoke `build/precompute_cli` for the natural pass and synthetic pass, preserving the exact seed, spec ordering, synthetic fallback logic, and hard-asserted coverage quotas.
   - Update `package.json`'s `precompute_fixture_generate` script to build `build/precompute_cli` and run `node .../precompute_fixture_generate.js`.
   - Delete `src/server/precompute_fixture_generate.ts`.

**Gate**: `npm run precompute_verify` reports **3,470/3,470 byte-identical**, and running `precompute_fixture_generate.js` twice to temporary output paths is byte-identical both run-vs-run and run-vs-committed `precompute_fixtures.jsonl` (aside from any deliberate update to the header's `generator` string).

### Phase 2 — Port Remaining HTTP Routes & Add Windows Support in `live_game_server.cpp`

1. **Upgrade `GET_MOVE_AB_TEST` in `main.cpp` to format moves via `fullMoveSearch`**
   - Expose or share the `PrecomputePossibility` / `formatPossibility` helper from `precompute.cpp` (for `rot,x,y|inputSequence|boardAfter|level|lines|dasCharge` formatting).
   - After `playOneMove` finds `bestMove` (main weights) and `bestMoveTest` (test weights), run `fullMoveSearch` once on the starting state (using the real `inputFrameTimeline` and `dasCharge`, `trackInputSequences = true`) and match each lock location `(rot, x, y)` to format `<mainFormatted>` and `<testFormatted>` (falling back to `"none"` if `bestMove` is `NULL_LOCK_LOCATION` / no legal moves, or an explicit error string if a legal move fails to match `fullMoveSearch`).
   - Return valid JSON:
     - Agree: `["agree", "<mainFormatted>", {"explanationMain":"..."}]`
     - Disagree: `["disagree", "<mainFormatted>", "<testFormatted>", {"explanationMain":"...", "explanationTest":"..."}]`
   - Update/expand `GET_MOVE_AB_TEST` tests in `src/cpp_modules/tests/request_type_tests.hpp` to assert valid JSON structure, formatted placement fields (including input sequence, 200-char board, level, lines, dasCharge), both `agree` and `disagree` shapes, topped-out `"none"` handling, and `explanationMain`/`explanationTest`.
   - *(Note: `GET_MOVE_AB_TEST` is not in `wasm.cpp`, and `precompute.cpp` is guarded by `#ifndef __EMSCRIPTEN__`, so keep any `fullMoveSearch`-based formatting helper guarded or in a non-WASM-excluded header so `npm run wasm_build` stays clean.)*

2. **Add `GET /precompute-sync` and `GET /get-move-ab-test` to `src/cpp_modules/src/live_game_server.cpp`**
   - **`GET /precompute-sync?p=<pipe>`**:
     - Parse `p` via `extractPipeParam` + `percentDecode` + `urlQueryToEnginePipe` (400 if `p` missing; 500 on `|` delimiter, engine `Error:` rejection, or thrown exception).
     - Return `503 Busy` if an async `/precompute` job is currently `RUNNING` (avoids oversubscribing two concurrent 7-thread precomputes).
     - Execute `mainProcess(enginePipe.c_str(), PRECOMPUTE, &timing)` synchronously with calibrated 7T speed and return `200 text/plain` with the full precompute response.
   - **`GET /get-move-ab-test?p=<pipe>`**:
     - Parse `p` via the exact same `extractPipeParam` + `percentDecode` + `urlQueryToEnginePipe` path (400 if `p` missing; 500 on `|` delimiter, engine `Error:` rejection, or thrown exception).
     - Execute `mainProcess(enginePipe.c_str(), GET_MOVE_AB_TEST)` synchronously and return `200 text/plain` with the JSON response.
   - Expand `live_server_verify.js` to test both new routes:
     - `/precompute-sync`: production fixture byte-identical to golden, missing `p` → 400, junk pipe / `|` delimiter → 500.
     - `/get-move-ab-test`: valid pipe returns valid JSON with `"agree"`/`"disagree"`, formatted placement + input sequence, and `"explanationMain"`; missing `p` → 400; malformed pipe → 500.

3. **Make `live_game_server.cpp` cross-platform (Windows + macOS/Linux)**
   - Replace POSIX-only `usleep(10000)` with standard C++ `std::this_thread::sleep_for(std::chrono::milliseconds(10))`.
   - Guard POSIX headers (`<arpa/inet.h>`, `<netinet/in.h>`, `<sys/socket.h>`, `<sys/time.h>`, `<unistd.h>`, `<csignal>`) with `#ifndef _WIN32`, and include `<winsock2.h>` / `<ws2tcpip.h>` under `#ifdef _WIN32`.
   - Add tiny platform helpers in the anonymous namespace:
     - `SocketFd` (`SOCKET` vs `int`), `INVALID_SOCK` (`INVALID_SOCKET` vs `-1`), `closeSocket()` (`closesocket` vs `::close`), `setRecvTimeout()` (`DWORD` ms on Windows vs `timeval` on POSIX), `isInterruptedAccept()` (`WSAGetLastError() == WSAEINTR` vs `errno == EINTR`), and `initSockets()` / `cleanupSockets()` (`WSAStartup` / `WSACleanup` on Windows, `signal(SIGPIPE, SIG_IGN)` on POSIX).
     - Guard `SO_REUSEADDR` (POSIX only, or `SO_EXCLUSIVEADDRUSE` on Windows) and pass `const char *` casts to `setsockopt`.
     - Use portable `int` / `ptrdiff_t` lengths for `send()` / `recv()` (MSVC has no `ssize_t`).
   - Add a `live_server_build_win` npm script (or cross-platform build note) for MinGW (`g++ -O2 -std=c++17 -pthread -o build/live_game_server.exe src/cpp_modules/src/live_game_server.cpp -lws2_32`) and MSVC (`cl /O2 /std:c++17 /EHsc /Fe:build\live_game_server.exe src\cpp_modules\src\live_game_server.cpp ws2_32.lib`).

4. **Small live-server hygiene**
   - Fix stale port `3001` comments in `live_game_server.cpp` header (code default is `3000`) and `live_server_verify.js`.
   - Remove stale TS-server port comment in `src/fceux/stackrabbit.lua` (lines 12–13).

**Gate**: Full battery green, including expanded `npm run live_server_check` and `npm run wasm_build`.

### Phase 3 — Streamline the Test Suite

Eliminate redundant golden-file maintenance and cut everyday verification from ~39s down to ~2–5s without losing any unique bug-catching coverage:

1. **Delete `src/cpp_modules/tests/precompute_pipeline_fixtures.hpp` and slim `precompute_property_tests.hpp`**
   - Remove the duplicate golden byte-equality test case (`prepare + collapse reproduce the recorded goldens byte for byte`) — `precompute_verify` is the sole golden byte-parity runner over `precompute_fixtures.jsonl`.
   - Replace the 48-fixture `PRECOMPUTE_PIPELINE_FIXTURES` table in `precompute_property_tests.hpp` with ~6–8 small inline test cases covering each structural regime (tap, DAS, `reactionTime=0`, `continue` fallback, per-piece `none`, whole-response `none`, level transition). Keep all pipeline invariant assertions (well-formedness, finite/bounded map values, fallback-marker conditions, winning phantom consistency).
   - Add `-O2` (or `-O1`) to `npm run cpp_test_unit` in `package.json` (`g++ -O2 -std=c++17 -pthread ...`).
   - **Target**: `npm run cpp_test_unit` runs in ~2–3s total (down from 8.2s) and never needs re-baselining on eval changes.

2. **Slim `live_server_verify.js` HTTP loop**
   - Keep all HTTP/protocol checks: junk pipe → 500, `|` delimiter → 500, backup-and-halt cancellation, percent-encoded commas, `/precompute-sync`, and `/get-move-ab-test`.
   - Replace the 42-fixture production golden loop with ~3 representative production fixtures (1 tap, 1 DAS, 1 `none` / edge case) byte-compared against `precompute_fixtures.jsonl`.
   - **Target**: `npm run live_server_check` runs in ~1.5–2s (down from 5.2s).

3. **Slim `precompute_tsan` replay set**
   - In `precompute_verify.js --dump-inputs`, select a small diverse subset of ~6–8 production fixtures (covering tap + DAS + rt=0/8/20 + one timed `-2` request) instead of all 42 production fixtures.
   - **Target**: `npm run precompute_tsan` exercises every 7-thread code path in ~3–4s (down from 12.7s).

**Gate**: `cpp_test_unit`, `live_server_check`, `precompute_verify` (all 3,470 fixtures), `precompute_tsan`, `precompute_static_lint`, and `fuzz_verify_sequences` all pass.

### Phase 4 — Relocate Permanent Non-TS Test Files out of `src/server/`

Land as a pure move + path-update commit (no logic changes) so git preserves file history cleanly:

1. **Move permanent fixtures & stdlib-only Node verifiers out of `src/server/`**:
   - `src/server/move_search_fixtures.json` → `src/cpp_modules/tests/data/move_search_fixtures.json`
   - `src/server/data/precompute_fixtures.jsonl` → `src/cpp_modules/tests/data/precompute_fixtures.jsonl`
   - `src/server/move_search_fuzz_verify.js` → `src/cpp_modules/tests/move_search_fuzz_verify.js`
   - `src/server/precompute_verify.js` → `src/cpp_modules/tests/precompute_verify.js`
   - `src/server/precompute_fixture_generate.js` → `src/cpp_modules/tests/precompute_fixture_generate.js`
   - `src/server/live_server_check.js` → `src/cpp_modules/tests/live_server_check.js`
   - `src/server/live_server_verify.js` → `src/cpp_modules/tests/live_server_verify.js`
2. **Update relative paths** inside those scripts and in `package.json`.

**Gate**: All test scripts locate fixtures at their new paths and pass.

### Phase 5 — Delete TS Server, `node-gyp` Addon, `browser_test_site`, and npm Dependencies

1. **Delete `src/server/` completely**:
   - `app.ts`, `board_helper.ts`, `cmodules.ts`, `dfs.ts`, `move_search.ts`, `move_search_fuzz_generate.ts`, `move_search_test.ts`, `params.ts`, `precompute.ts`, `request_handler.ts`, `request_parser.ts`, `start_check.js`, `types.ts`, `utils.ts`, `worker_thread.ts`.
2. **Delete `src/browser_test_site/`**:
   - `src/browser_test_site/index.html`, `src/browser_test_site/index.js`.
   - *(Leave `src/console_client/` untouched as a historical relic.)*
3. **Delete `node-gyp`, `nan`, and TypeScript build infrastructure**:
   - Delete `binding.gyp`, `src/cpp_modules/src/module.cpp`, `install.js`, `tsconfig.json`.
   - Remove local untracked build artifacts (`built/`, `build/Release/`).
4. **Strip `package.json` to zero-dependency scripts**:
   - Remove dead scripts: `start_check`, `move_test`, `fuzz_generate`.
   - Repoint `"start"` to `"npm run live_server"`.
   - Remove dead `"main": "src/web_client/index.js"`.
   - Remove `"dependencies"` and `"devDependencies"` completely; delete `package-lock.json` (and local `node_modules/`).
5. **Resolve cutover comments & TODOs in C++**:
   - `src/cpp_modules/src/main.cpp`: update lines 5–7 comment referencing `node-gyp` unity-build constraints. (Keep `GET_MOVE`'s `NULL_LOCK_LOCATION` wire behavior at line 441 untouched unless WASM/TetrisTrainer consumers are updated in lockstep.)
   - `src/cpp_modules/tests/fixture_dump.cpp` and `src/cpp_modules/tests/move_search_fuzz_verify.js`: replace `TODO(Phase 5 cutover)` comments (from `precompute_cpp_migration.md`) with plain end-state baseline notes.

**Gate**: Delete `node_modules/` locally and run the [Post-Cutover Full Verification Checklist](#post-cutover-full-verification-checklist) cleanly without running `npm install`.

### Phase 6 — Documentation & Change-Scoped Verification Policy

1. **`TESTING.md`**:
   - Document the [Post-Cutover Change-Scoped Test Matrix](#post-cutover-change-scoped-test-matrix) as the canonical guide for which tests to run based on what files/behaviors changed.
   - Remove TS unit test (`move_test`), `start_check`, `fuzz_generate`, `cRabbit` addon, and `precompute_pipeline_fixtures.hpp` sections.
   - Update fixture paths (`src/cpp_modules/tests/data/...`) and simplify the re-baselining workflow (now just one golden file: `precompute_fixtures.jsonl` via `npm run precompute_fixture_generate`).
2. **`AGENTS.md`**:
   - Replace the monolithic verification battery with the tiered change-scoped matrix (fast default checks on every change + triggered checks based on what changed, pointing to `TESTING.md` for details).
   - Remove `npx tsc`, `npm run move_test`, `npm run start_check`, and the stale `node-gyp` addon-in-memory warning (replace with a note to rebuild/restart `live_game_server`).
   - Update the lockstep pipe-format caller list (remove `request_parser.ts`, `worker_thread.ts`, `cmodules.ts`; list `live_game_server.cpp`, `precompute_cli.cpp`, `entrypoint.cpp`, `speed_benchmark.cpp`, `run_agent.cpp`, `wasmRabbit-worker.js`, `stackrabbit.lua`).
   - Fix the broken doc link (`docs/engine_boundaries.md` → `docs/engine_endpoints.md`).
3. **`README.md`**:
   - Replace `node install.js` + `npm start` setup steps with building and running `live_game_server` on both macOS/Linux (`npm run live_server` / `g++`) and Windows (MinGW `g++ ... -lws2_32` or MSVC `cl ... ws2_32.lib`).
   - Update "Overview of Code Segments" (native C++ live server, WASM, CLI) and remove Python / `node-gyp` prerequisites.
4. **`docs/engine_endpoints.md`**:
   - Reframe from "TS ↔ C++ Contracts" to C++ engine & HTTP wire contracts; document `/precompute-sync` and `/get-move-ab-test` (input `?p=` format and JSON output format); fix default port `3001` → `3000`.
5. **`docs/precompute_cpp_migration.md`**:
   - Mark the TS cutover ("Phase 5 — Live traffic") complete and link to this plan.
6. **WASM check**:
   - Run `npm run wasm_build` to confirm Emscripten compilation is clean after all changes.

**Gate**: All docs match the final tree and `npm run wasm_build` succeeds.

---

## File-by-File Inventory

| File / Path | Phase | Action |
|---|---|---|
| `src/cpp_modules/tests/precompute_cli.cpp` | 1 | **Create** — native stdin/stdout C++ runner for `PRECOMPUTE` |
| `src/server/precompute_verify.js` | 1, 3 → 4 | **Update** (use `precompute_cli`, drop `cRabbit`, slim `--dump-inputs` to ~6–8 fixtures) → **Move** to `src/cpp_modules/tests/` |
| `src/server/precompute_fixture_generate.ts` | 1 | **Replace** with plain JS `precompute_fixture_generate.js` (using `precompute_cli`), then **Delete** `.ts` |
| `src/cpp_modules/src/main.cpp` | 2, 5 | **Update** `GET_MOVE_AB_TEST` to format via `fullMoveSearch` + clean up `node-gyp` comment |
| `src/cpp_modules/tests/request_type_tests.hpp` | 2 | **Update** `GET_MOVE_AB_TEST` unit tests for formatted placement + explanations |
| `src/cpp_modules/src/live_game_server.cpp` | 2 | **Update** — add `/precompute-sync`, `/get-move-ab-test`, Windows Winsock2 shim, fix port comments |
| `src/server/live_server_verify.js` | 2, 3 → 4 | **Update** (add tests for new routes, slim 42-fixture loop to ~3 fixtures) → **Move** to `src/cpp_modules/tests/` |
| `src/cpp_modules/tests/precompute_pipeline_fixtures.hpp` | 3 | **Delete** (single golden file is `precompute_fixtures.jsonl`) |
| `src/cpp_modules/tests/precompute_property_tests.hpp` | 3 | **Update** — inline ~6–8 representative cases, drop duplicate golden byte-check |
| `src/server/live_server_check.js` | 4 | **Move** to `src/cpp_modules/tests/` |
| `src/server/move_search_fuzz_verify.js` | 4 → 5 | **Move** to `src/cpp_modules/tests/`, clear cutover TODO |
| `src/server/move_search_fixtures.json` | 4 | **Move** to `src/cpp_modules/tests/data/` |
| `src/server/data/precompute_fixtures.jsonl` | 4 | **Move** to `src/cpp_modules/tests/data/` (keep all 3,470 fixtures) |
| `src/server/*.ts` (remaining 14 files) & `start_check.js` | 5 | **Delete** (removes `src/server/` entirely) |
| `src/browser_test_site/` | 5 | **Delete** |
| `src/console_client/` | — | **Keep as-is** (historical relic) |
| `binding.gyp`, `src/cpp_modules/src/module.cpp`, `install.js`, `tsconfig.json`, `package-lock.json` | 5 | **Delete** |
| `package.json` | 1, 2, 3, 4, 5 | **Update** scripts (`cpp_test_unit` `-O2`, `start` → `live_server`, Windows build script, new paths), drop `main` / `dependencies` / `devDependencies` |
| `src/fceux/stackrabbit.lua` | 2 | **Update** stale port/TS comment (lines 12–13) |
| `src/cpp_modules/tests/fixture_dump.cpp` | 5 | **Clean up** cutover TODO comment |
| `AGENTS.md`, `TESTING.md`, `README.md`, `docs/engine_endpoints.md`, `docs/precompute_cpp_migration.md` | 6 | **Update** to final zero-TS, zero-`node-gyp`, change-scoped test state |

---

## Post-Cutover Change-Scoped Test Matrix

*(To be documented in `TESTING.md` and `AGENTS.md` in Phase 6.)*

Instead of running every heavy check unconditionally, match tests to what changed:

| Change scope | Tests to run | Target runtime |
|---|---|---:|
| **Always (default check for any C++ change)** | `npm run cpp_test_unit`, `npm run precompute_static_lint` | ~2–3s |
| **HTTP server, routes, or Lua wire format** (`live_game_server.cpp`, `main.cpp` parsing, `stackrabbit.lua`) | + `npm run live_server_check` | +1.5–2s |
| **Refactors / perf optimizations (behavior-preserving)** or **intentional eval / search / precompute changes** (inspect diffs & re-baseline) | + `npm run precompute_verify` (all 3,470 fixtures) | +9s |
| **Full move search / simulation** (`full_move_search.cpp`, `tetrominoes.hpp`, `utils.hpp`) | + `npm run fuzz_verify_sequences` | +3s |
| **Threading, shared state, or allocation / static-buffer changes** in `precompute.cpp` or lookup call tree (`high_level_search`, `tournament_search`, `playout`, `eval`, `move_search`, `piece_rng`) | + `npm run precompute_tsan` (~6–8 fixtures) | +3–4s |
| **Eval / search quality or throughput changes** | + `npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard` and/or interleaved `npm run speed_benchmark` | varies |
| **Shared engine changes or WASM glue** (`wasm.cpp`, `wasmRabbit-worker.js`, pipe format) | + `npm run wasm_build` | ~3s |

---

## Post-Cutover Full Verification Checklist

One-time sign-off after Phase 5 — all checks must pass with **no `node_modules/` directory installed**:

- [ ] `npm run cpp_test_unit` — compiles with `-O2`, all doctest cases pass (including updated `GET_MOVE_AB_TEST` formatted output tests and slimmed `precompute_property_tests.hpp`)
- [ ] `npm run fuzz_verify_sequences` — all 2,783 move-search fixtures character-exact
- [ ] `npm run precompute_verify` — builds `build/precompute_cli`, 3,470/3,470 byte-identical
- [ ] `npm run precompute_tsan` — slimmed ~6–8 production fixtures byte-identical, zero TSan warnings
- [ ] `npm run precompute_static_lint` — 0 unwaived non-const file-scope statics
- [ ] `npm run live_server_check` — builds `build/live_game_server`, verifies `/precompute`, `/async-result`, backup-halt, `/precompute-sync`, `/get-move-ab-test`, error paths, and ~3 golden fixtures over HTTP
- [ ] `npm run precompute_fixture_generate -- /tmp/precompute_check.jsonl` — deterministic and byte-identical to committed `precompute_fixtures.jsonl`
- [ ] `npm run wasm_build` — compiles cleanly
- [ ] Windows build check (when on Windows / CI) — `live_game_server.cpp` compiles cleanly with Winsock2 (`-lws2_32` / `ws2_32.lib`)
