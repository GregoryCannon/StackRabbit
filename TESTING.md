# Testing & Verification Plan

Testing the `src/cpp_modules/` AI backend and full engine stack: it is deterministic,
heavily optimized, and its core entry points are pure string-in/string-out functions.

## Change-Scoped Test Matrix

Match verification checks to what changed instead of running every heavy suite unconditionally:

| Change scope | Tests to run | Typical runtime |
|---|---|---:|
| **Always (default check for any C++ change)** | `npm run cpp_test_unit`, `npm run precompute_static_lint` | ~4s |
| **HTTP server, routes, or Lua wire format** (`live_game_server.cpp`, `main.cpp` parsing, `stackrabbit.lua`) | + `npm run live_server_check` | +1.5–2s |
| **Refactors / perf optimizations (behavior-preserving)** or **intentional eval / search / precompute changes** (inspect diffs & re-baseline) | + `npm run precompute_verify` (all 3,470 fixtures) | +9s |
| **Full move search / simulation** (`full_move_search.cpp`, `tetrominoes.hpp`, `utils.hpp`) | + `npm run fuzz_verify_sequences` | +3s |
| **Threading, shared state, or allocation / static-buffer changes** in `precompute.cpp` or lookup call tree (`high_level_search`, `tournament_search`, `playout`, `eval`, `move_search`, `piece_rng`, and the other files audited by `precompute_static_lint.js`) | + `npm run precompute_tsan` (6 fixtures + 1 timed `-2` request) | +5s |
| **Eval / search quality or throughput changes** | + `npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard` and/or interleaved `npm run speed_benchmark` | varies |
| **Shared engine changes or WASM glue** (`wasm.cpp`, `wasmRabbit-worker.js`, pipe format) | + `npm run wasm_build` | ~3s |

## Overview of Test Suites, Verifiers, and Benchmarks

StackRabbit maintains layered test harnesses with distinct verification depths. All Node test scripts use the Node.js standard library only (zero `node_modules`).

* **C++ Unit Tests** (`npm run cpp_test_unit`)
  * **Files**: `src/cpp_modules/tests/stackrabbit_test.cpp` and included test headers (`helpers.hpp`, `board_format_tests.hpp`, `input_validation_tests.hpp`, `move_search_fuzz_tests.hpp`, `precompute_collapse_tests.hpp`, `precompute_property_tests.hpp`, `request_type_tests.hpp`)
  * **Purpose**: Fast in-memory doctest suite compiled with `-O0 -std=c++17 -pthread` (~4s total). Validates pipe parsing, all request types, board compression, synthetic precompute collapse/formatting rules, precompute pipeline invariants (7 inline regime cases in `precompute_property_tests.hpp`, with no baked golden header), and move search placement tuples.
* **Move Search Regression & Sequence Verifiers**
  * **In-Suite Move Search Fuzz** (`npm run cpp_test_unit` — `src/cpp_modules/tests/move_search_fuzz_tests.hpp`): Replays all 2,783 fixtures inside the C++ test binary. Compares result order and `(rot, x, y, chargeAfter, inputCost)` tuples in-memory against bit-packed records in `src/cpp_modules/tests/move_search_fixtures.hpp` (no string sequences). Tests both `fullMoveSearch` and `fastMoveSearch`.
  * **Move Search Sequence Verifier** (`npm run fuzz_verify_sequences` — `src/cpp_modules/tests/fixture_dump.cpp`, `src/cpp_modules/tests/move_search_fuzz_verify.js`): The strongest move-search gate. Compiles and runs `build/fixture_dump` across all 2,783 fixtures and asserts **character-exact controller input sequences** (frame-by-frame) and tuple ordering against `src/cpp_modules/tests/data/move_search_fixtures.json`.
* **Precompute Pipeline, Concurrency & Live Server Gates**
  * **Precompute Verifier** (`npm run precompute_verify` — `src/cpp_modules/tests/precompute_cli.cpp`, `src/cpp_modules/tests/precompute_verify.js`): Builds `build/precompute_cli`, streams all 3,470 fixtures from `src/cpp_modules/tests/data/precompute_fixtures.jsonl` in a single batch invocation, and verifies byte-identical responses with categorized mismatch reporting.
  * **Precompute Fixture Generator** (`npm run precompute_fixture_generate` — `src/cpp_modules/tests/precompute_cli.cpp`, `src/cpp_modules/tests/precompute_fixture_generate.js`): Builds `build/precompute_cli` and regenerates the single golden file `src/cpp_modules/tests/data/precompute_fixtures.jsonl` (3,470 fixtures) from `src/benchmark/data/position_log.jsonl` with strict coverage quotas across pieces, reaction times, and DAS buckets.
  * **Precompute TSan Gate** (`npm run precompute_tsan` — `src/cpp_modules/tests/precompute_tsan.cpp`): Compiles with `-fsanitize=thread` and replays 6 diverse non-`"none"` production fixtures covering `(tap, DAS) × (reactionTime 0, 8, 20)` plus 1 derived timed `-2` (150ms budget) DAS precompute request to catch data races across the 7 per-next-piece worker threads.
  * **Live Server Verifier** (`npm run live_server_check` — `src/cpp_modules/tests/live_server_check.js`, `src/cpp_modules/tests/live_server_verify.js`): Builds and boots `build/live_game_server` on isolated battery port 3101, verifies 3 representative HTTP golden fixtures (production tap, production DAS, and whole-response `"none"`), backup-and-halt cancellation, `503 Busy` under concurrent load, `/precompute-sync`, `/get-move-ab-test` (11- and 12-field pipes), and HTTP error paths (`400` missing `p`, `500` on junk pipe or `|` delimiter).
  * **Precompute Static Lint** (`npm run precompute_static_lint` — `src/cpp_modules/tests/precompute_static_lint.js`): Static tripwire ensuring no non-const, non-thread-local file-scope static variables exist in files reachable from concurrent precompute lookup calls (waivers tracked in `src/cpp_modules/tests/thread_static_allowlist.txt`).
* **Benchmarks**
  * **Throughput Benchmark** (`npm run speed_benchmark` — `src/benchmark/speed_benchmark.cpp`): Measures raw engine throughput (ms/call, ops/sec) for `GET_MOVE` across varying board topologies, speeds, and playout parameters.
  * **Accuracy Benchmark Suite** (`npm run benchmark_run_agent` — `src/benchmark/run_agent.cpp`): Evaluates engine move choice and decision quality against a deep Monte Carlo oracle (1,000–8,000 playouts) on standard or hard positions (e.g. `npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard`).
  * **Associated Generators**:
    * `npm run benchmark_sample_positions` (`src/benchmark/sample_benchmark_positions.js`): Samples 200 diverse game positions from `position_log.jsonl` into `src/benchmark/data/benchmark_positions.jsonl`.
    * `npm run benchmark_find_hard` (`src/benchmark/find_hard_positions.cpp`): Mines positions where shallow eval diverges from deep search into `src/benchmark/data/hard_benchmark_positions.jsonl`.
    * `generate_oracle.cpp` (`src/benchmark/generate_oracle.cpp`): Generates ground-truth deep-playout ratings into `src/benchmark/data/oracle_positions.jsonl` and `hard_oracle_positions.jsonl`.

### Distinguishing Key Overlapping Tests

* **`move_search_fuzz_tests.hpp` vs. `move_search_fuzz_verify.js`**:
  * `move_search_fuzz_tests.hpp` is an in-memory C++ doctest suite (`npm run cpp_test_unit`). It checks placement tuples `(rotation, x, y, dasChargeAfter, inputCost)` in result order against bit-packed integers in `src/cpp_modules/tests/move_search_fixtures.hpp`. It runs fast and also guards `fastMoveSearch` set equivalence, but it does *not* carry or check controller input sequences.
  * `move_search_fuzz_verify.js` (`npm run fuzz_verify_sequences`) is a separate Node.js differential harness. It compiles and runs `build/fixture_dump --all` and diffs both the placement tuples and the **character-exact controller input sequences** (frame-by-frame) against `src/cpp_modules/tests/data/move_search_fixtures.json`. It is the authoritative gate for sequence correctness.
* **`precompute_verify.js` vs. `live_server_check.js`**:
  * `precompute_verify.js` tests `build/precompute_cli` across all 3,470 fixtures (both eval-only and production tiers).
  * `live_server_check.js` tests the standalone multi-threaded HTTP server (`build/live_game_server`) on port 3101 across 3 representative golden fixtures plus protocol checks for `/precompute`, `/async-result` backup-and-halt cancellation, `503 Busy`, `/precompute-sync`, `/get-move-ab-test`, error handling, and socket teardown.
* **`precompute_tsan.cpp` vs. `precompute_static_lint.js`**:
  * `precompute_tsan.cpp` is a dynamic concurrency check: it replays 6 diverse production requests plus 1 timed `-2` request under ThreadSanitizer to detect data races during multi-threaded execution.
  * `precompute_static_lint.js` is a static tripwire: it checks source code at build time for un-waived mutable file-scope statics in the concurrency call tree.

## Test binary layout

- Vendored `doctest.h` in `src/cpp_modules/vendor/doctest.h` (v2.4.12)
- `src/cpp_modules/tests/stackrabbit_test.cpp`:
  - `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` + include `doctest.h`
  - Includes `../src/main.cpp` (the unity build) — deliberately **not** the other `main()` definers (`entrypoint.cpp`, `live_game_server.cpp`, `fixture_dump.cpp`, `precompute_cli.cpp`, `precompute_tsan.cpp`, `src/benchmark/*.cpp`) or `wasm.cpp` (Emscripten glue)
- Test cases live in per-category headers included by `stackrabbit_test.cpp`:
  - `src/cpp_modules/tests/helpers.hpp` — shared board builders, `InputBuilder`, assertions
  - `src/cpp_modules/tests/board_format_tests.hpp` — `formatBoard` / `parseCompressedBoard` round trips
  - `src/cpp_modules/tests/input_validation_tests.hpp` — Section A: input parsing & validation
  - `src/cpp_modules/tests/move_search_fuzz_tests.hpp` — in-memory move-search placement-tuple regression checks
  - `src/cpp_modules/tests/precompute_collapse_tests.hpp` — eval-free synthetic-state collapse & formatter pins
  - `src/cpp_modules/tests/precompute_property_tests.hpp` — pipeline invariants on 7 inline regime cases
  - `src/cpp_modules/tests/request_type_tests.hpp` — Section B: tests per request type
- npm script: `"cpp_test_unit"` compiles with `g++ -O0 -std=c++17 -pthread` (fast edit-compile-test loop, ~4s total)

## Test strategy: black-box on `mainProcess`

`mainProcess` is the same surface `live_game_server.cpp`, `precompute_cli.cpp`, `entrypoint.cpp`, and `wasm.cpp` call, so tests validate the full stack without exposing C++ internals.

Input format (verified in `main.cpp`'s arg loop; see [docs/engine_endpoints.md](docs/engine_endpoints.md)):

```
[board1 (200 chars 0/1)] | [optional board2 (200 chars 0/1)] | level | lines |
currentPieceIdx | nextPieceIdx | inputFrameTimeline | playoutCount |
playoutLength | pruningBreadth | aggressionLevel | dasCharge |
```

Every non-board field is **mandatory and strictly validated** by `main.cpp`:

- `level` / `lines` / `currentPiece` / `nextPiece` / `playoutCount` /
  `playoutLength` / `pruningBreadth` / `dasCharge`: integers (`atoi("junk")` and
  empty fields are rejected instead of silently becoming 0 — a silent 0 would
  mean a degenerate game state, the I piece, or DAS mode).
- `nextPiece = -1` explicitly means no-next-box; `pruningBreadth = -1` means
  fixed-playout tournament mode (`playoutCount` = total playouts), and
  `pruningBreadth = -2` means timed tournament mode (`playoutCount` =
  wall-clock budget in milliseconds, for lock-value lookups and precompute).
- `inputFrameTimeline`: non-empty, `X`/`.` characters only, at least one `X`
  (an empty timeline used to make the engine never tap via `x % 0`, and a
  no-`X` timeline used to hang the move search — finding 4 below).
- `aggressionLevel`: exactly `safe`, `aggro`, or `hyper` (unknown strings are
  rejected instead of silently defaulting to `SAFE`).
- `dasCharge`: `-1` = tap mode, `0..16` = DAS mode.

`secondBoard` remains genuinely optional (per request type).

Request types: `GET_LOCK_VALUE_LOOKUP`, `GET_LOCK_VALUE_LOOKUP_DAS`,
`GET_TOP_MOVES`, `GET_TOP_MOVES_HYBRID`, `RATE_MOVE`, `GET_MOVE`,
`GET_MOVE_AB_TEST`, `PRECOMPUTE`, `PRECOMPUTE_BACKUP`.

`PRECOMPUTE` and `PRECOMPUTE_BACKUP` speak the 12-field precompute pipe format:
the 11 fields above plus a mandatory `reactionTime` (`0..60` frames) appended
after `dasCharge`; `nextPiece` is unused there (`-1` — the searches cover all
seven pieces). Both validate identically. The precompute response shape is
`Default:` + one line per next piece (`I`..`Z` in `IOLJTSZ` order).
`PRECOMPUTE_BACKUP` (the live game server's partial-result answer; see
`live_game_server.cpp`'s header and `runPrecomputeBackup` in `precompute.cpp`)
runs only the initial move search and answers with the best-eval placement —
scored the same way depth-1 move choice evaluates candidates, ties keeping the
first result in search order — as
`Default:<relRot>,<relX>,<relY>|<sequence>~~|<200-char board>|<level>|<lines>|<dasChargeAfter>`
plus seven `<piece>:continue` lines. The sequence is emitted unsliced (the
whole sequence sits before `~~`), and a piece with no legal placement answers
exactly `none`.

Shared helpers live in `src/cpp_modules/tests/helpers.hpp`: board-string
builders (empty board, board with a well, notch, full row), piece-letter →
`PIECE_LIST` index mapping, and `InputBuilder` so tests read like the calls
they exercise.

## Test matrix

### A. Input parsing & validation (fast, exact assertions)

| Case | Expectation |
|---|---|
| Missing `currentPiece` (`-1`) | exact error string `"Error: please provide a value for currentPiece."` |
| `nextPiece` = `-1` | valid; request behaves as no-next-box mode |
| Second-board detection: present for `RATE_MOVE`, optional for `GET_TOP_MOVES`/`HYBRID`, absent otherwise | correct board chosen |
| `RATE_MOVE` with no second board | exact error string `"Error: RATE_MOVE requests require a value for secondBoard."` (no OOB read) |
| Non-numeric integer args or empty fields | rejected with `"Error: ..."` |
| Invalid `inputFrameTimeline` (empty, invalid chars, or no `X`) | rejected with `"Error: ..."` |
| Unknown `aggressionLevel` string | rejected with `"Error: ..."` |
| Out-of-range `dasCharge` or `reactionTime` | rejected with `"Error: ..."` |

### B. Per request type (structural assertions on output)

| Request | Assertions |
|---|---|
| `GET_MOVE` | output matches `[rot, xOffset, yOffset]`; rot ∈ 0–3; xOffset in valid range; **determinism** (same input twice → same output) |
| `GET_TOP_MOVES` | valid JSON; ≤ `NUM_ENGINE_MOVES` entries; player's move (from `secondBoard`) included even if not top-ranked |
| `GET_TOP_MOVES_HYBRID` | output has `"noNextBox"` and `"nextBox"` keys, each a valid list |
| `RATE_MOVE` | valid JSON object (`playerMoveNoAdjustment`, `bestMoveNoAdjustment`, plus `*AfterAdjustment` keys when `nextPiece` is provided) for a known player board |
| `GET_LOCK_VALUE_LOOKUP` / `_DAS` | non-empty encoded lookup; both variants produce consistent structure; DAS-charge variant accepted |
| `GET_MOVE_AB_TEST` | valid JSON; first element `"agree"` or `"disagree"`; formatted placement strings (`rot,x,y|seq|board|level|lines|das`) or `"none"` on topped-out boards; JSON-escaped `explanationMain` (and `explanationTest` on `"disagree"`) |
| `PRECOMPUTE` / `PRECOMPUTE_BACKUP` | well-formed `Default:` + 7 piece lines (`IOLJTSZ`) or `"none"`; pipeline invariants and synthetic collapse/formatter pins |
| All types | empty board, tall board, and notch board inputs all complete without crash |

### C. Characterization / regression goldens

- Representative output checks for `GET_MOVE`, `GET_TOP_MOVES`, and `RATE_MOVE` across levels 18, 19, and 29 (kill-screen gravity) guard wire-format stability for WASM and HTTP clients.
- Use small `playoutCount`/`playoutLength` in unit tests to keep the suite fast; heavy parameter runs stay in `src/benchmark/`.

## Findings (from building the suite)

1. **`RATE_MOVE` without a second board segfaulted.** The legacy fallback read
   past the buffer's null terminator (`std::string(inputStr + 402)`). **Fixed**
   in `main.cpp`: such requests now return
   `"Error: RATE_MOVE requests require a value for secondBoard."` (covered by tests).
2. **Inputs shorter than 201 characters caused out-of-bounds reads** in the
   arg-slicing paths (`std::string(inputStr + 201)`). **Fixed** in `main.cpp`:
   a length guard rejects them upfront (covered by tests).
3. **A malformed second board silently shifted every parsed field by two** and
   could produce degenerate parameters. Guarded by strict field validation in
   `main.cpp`.
4. **A degenerate `inputFrameTimeline` with no `X` frames hung the move
   search** at any level. **Fixed** in `main.cpp`: the C++ parser validates
   `inputFrameTimeline` strictly (non-empty, `X`/`.` only, at least one `X`)
   and rejects malformed timelines upfront.

## Move-search regression fixtures (DAS & tap move search)

`src/cpp_modules/tests/data/move_search_fixtures.json` (2,783 fixtures) and its
companion bit-packed C++ header `src/cpp_modules/tests/move_search_fixtures.hpp`
are the permanent versioned specification of `fullMoveSearch` behavior.

- `npm run cpp_test_unit` (`move_search_fuzz_tests.hpp`) replays all fixtures
  in-memory via `fullMoveSearch` and compares `(rotationIndex, x, y,
  dasChargeAfter, inputCostX10)` tuples **order-exactly** — element `i` of the
  golden list must equal element `i` of the search output. Result order is
  load-bearing downstream (`possibleMoves[0]` is the default placement; the
  phantom sort is stable over it), so an order divergence is a real bug, not
  noise. Tap fixtures are asserted twice: through `fastMoveSearch` (placement
  **set** equivalence only, since its tuck semantics and order differ) and
  through `fullMoveSearch`'s ARR-gated tap path.
- `npm run fuzz_verify_sequences` compiles `src/cpp_modules/tests/fixture_dump.cpp`,
  dumps C++ results for every fixture (both modes from `fullMoveSearch`), and
  diffs tuples **and character-exact controller input sequences** against
  `src/cpp_modules/tests/data/move_search_fixtures.json` (exiting nonzero on
  any mismatch).

### The two "tuck" definitions (`fastMoveSearch` vs `fullMoveSearch`)

The two searches define "tuck" differently, by design:

- **`fastMoveSearch`** finds gameplay tucks: destination cells flagged as
  overhang setups by the board analysis (`analyzeHole` / `TUCK_SETUP_BIT`).
- **`fullMoveSearch`** appends any input that produces a novel lock during the
  fall. This covers cases the rigid assumptions of `fastMoveSearch` miss:
  placements that depend on the *ordering* of shifts and rotations during an
  otherwise non-tuck placement.

Concretely: the greedy goal-walk merges shift+rotation into the earliest
possible input, which can catch on a stack corner and lock one cell away, while
the DFS's re-ordered inputs (e.g. shift first, rotate later) slip past and
reach cells the walk can never express. On the jagged-overhang board (`tap_tuck`
tag), `fullMoveSearch` finds 46 spins + 14 spin-tucks that `fastMoveSearch`
intentionally does not model, so those fixtures are skipped in the
`fastMoveSearch` guard only and verified via `fullMoveSearch`.

Fixture coverage axes: all 7 pieces; `dasCharge ∈ {0..16}` with emphasis on
`{0, 5, 6, 10, 14, 15, 16}`; levels 18 / 19 / 29; timelines `"X.."`, `"X..."`,
`"X....."`; boards spanning flat heights 0–16, overhang/tuck setups, edge/wall
adjacency, and near-ceiling spawns; zero-shift and max-shift placements.
Tall-stack boards (stacks reaching rows 0–2, `tap_tall`/`das_tall` tags)
exercise locks in the top rows, including a one-row lid where only the I piece
has any placement; adjustment fixtures include `existingYOffset = 20`
past-bottom entries (empty result). The tap fixtures additionally include a
jagged overhang board (`tap_tuck` tag) exercising the tuck/spin DFS tap
branches.

### Adjustment-mode fixtures (mid-air entry)

`adjustmentFixtures` (JSON key + `ADJUSTMENT_*` pools in
`move_search_fixtures.hpp`, separate index space from spawn fixtures) record
mid-air states derived the same way precompute derives phantoms: run spawn
`fullMoveSearch` with a reaction time (`8 / 13 / 20`), stable-sort by
`countInputs`, dedupe by the first `reactionTime` chars of `inputSequence`,
then capture `adjTimeSimState` (`framesAlreadyElapsed`, `existingXOffset`,
`existingYOffset`, `existingRotation`, `dasCharge`, `adjustmentState`) and
record the adjustment-mode search output (no reaction time).

Phantom selection is coverage-driven across `(isArrContinued, dasButtonHeld,
rotation-class)` buckets (tap-mode `dasButtonHeld != NONE` at the reaction-time
frame is reached when `reactionTime - 1` is an ARR input frame, e.g.
`reactionTime = 13`). The doctest suite asserts entry tuples; sequences are
compared character-exactly by `fuzz_verify_sequences` under `adjustment <idx>`
dump headers.

### Reaction-time fixtures (`adjTimeSimState` snapshot, spawn entry)

`reactionTimeFixtures` (JSON key + `REACTION_TIME_*` pools in
`move_search_fixtures.hpp`, separate index space) run spawn `fullMoveSearch` with
a reaction time (`0 / 1 / 2 / 5 / 8 / 13 / 20 / 40`, both modes) and record the
full possibility list in result order, each entry carrying its walk's
`adjTimeSimState` snapshot (or null). This section is compared **order-exactly**
with snapshot fields included:

- A walk that reaches its goal before the reaction-time frame keeps simulating
  (the piece keeps falling at the goal column) until the reaction-time frame,
  so its last registered sim state carries the snapshot, while walks whose
  piece locks before the reaction-time frame save no snapshot.
- The DFS novel-keeping comparison (`inputCost + (adjTimeSimState == null ?
  -1000 : 0)`) reads snapshot presence: of two tuck routes to the same cell,
  the one whose parent walk saved its `adjTimeSimState` wins.

`reactionTime <= 0` never saves a snapshot and never waits. Dumped under
`reactiontime <idx>` headers by `fixture_dump` (no sorting); the doctest
compares tuples + snapshots in order, and `fuzz_verify_sequences` additionally
compares sequences character-exactly.

## Precompute golden fixtures (full-request regression baseline)

`src/cpp_modules/tests/data/precompute_fixtures.jsonl` is the **single**
precompute golden file (3,470 fixtures: exact request parameters plus verbatim
output string per fixture). `npm run precompute_verify` builds
`build/precompute_cli` and runs `src/cpp_modules/tests/precompute_verify.js`
to assert 3,470/3,470 byte-identical outputs.

`npm run precompute_fixture_generate` builds `build/precompute_cli` and runs
`src/cpp_modules/tests/precompute_fixture_generate.js` over logged positions
from `src/benchmark/data/position_log.jsonl` to regenerate
`src/cpp_modules/tests/data/precompute_fixtures.jsonl` (accepts an optional
output path argument, e.g.
`npm run precompute_fixture_generate -- /tmp/precompute_check.jsonl`).

Design notes:

- Two tiers: **eval_only** (3,428 fixtures, `playoutCount=0`,
  `playoutLength=0` — fast, still exercises the full pipeline) and
  **production** (42 fixtures with default engine params `49/2/20/safe`).
- Each logged position contributes 2 fixtures with cycled `(reactionTime,
  dasCharge)` combos (`reactionTime ∈ {0, 8, 20}`; `dasCharge ∈ {-1, 0, 5, 10,
  15, 16}`) and a cycled current piece; the logged `nextPiece` is ignored
  (precompute searches all 7 next pieces).
- Coverage quotas are hard-asserted by the generator (per-piece, per-`reactionTime`,
  per-`dasCharge`, per-combo floors; `continue` lines; per-piece `none` lines;
  whole-response `none`; default placements). Synthetic fixtures are appended
  only when a category is unreachable from the log.
- Generation is deterministic (fixed seed, no timestamps): two runs must be
  byte-identical.
- With `reactionTime = 0`, a piece whose adjustments all miss the lock value
  lookup emits the normal per-piece `none` fallback (never an error or crash).

### Re-baselining after an intentional eval or precompute change

The full-pipeline goldens are **reviewable snapshots, not immovable walls**.
An intentional eval or precompute change (weights, eval-context setup, playout
handling, or collapse scoring) follows a single-file re-baseline workflow:

1. Run `npm run precompute_verify` and review the categorized mismatch report.
   Value-flip churn (`default line` / `piece X line` categories: a changed
   winner or per-piece value with the response shape intact) is the expected
   face of an eval change. Anything structural — `fallback vs content` or
   `whole-response none vs content` categories, a malformed line, a changed
   `~~` slice — means pipeline behavior changed, not just values; verify or fix
   that before re-baselining.
2. Regenerate the single golden file with `npm run precompute_fixture_generate`
   (verify determinism by running to a temporary path and diffing) and commit
   `src/cpp_modules/tests/data/precompute_fixtures.jsonl` alongside the change.
   `cpp_test_unit` needs no re-baselining: `precompute_property_tests.hpp` tests
   eval-independent pipeline invariants on 7 inline cases, and
   `precompute_collapse_tests.hpp` pins tie-breaking, rounding, fallback, and
   formatting rules on synthetic constant maps so a C++-generated re-baseline
   cannot silently redefine pipeline semantics.

The move-search fixtures (`move_search_fixtures.json` / `.hpp`) are eval-immune
(pure simulation with no lock values), so an eval change never moves them.

## Differential runner house recipe

The pattern for any parity or regression-snapshot work:

1. Freeze a **golden fixture file** — verbatim outputs for a deterministic,
   coverage-driven request set produced by a fixed-seed generator (two runs
   must be byte-identical).
2. Write a **small runner** that derives each request string from the records
   (nothing beyond the encoding contract), invokes the binary under test, and
   **byte-compares** against the recorded output. Exit nonzero on any mismatch.
3. Report **match counts plus categorized diffs**. Categorization separates
   value churn (a changed number or winner with the response shape intact —
   reviewable, re-baselineable) from structural breaks (changed shape,
   fallback, ordering, malformed output — never re-baseline over these).

Live instances: `src/cpp_modules/tests/move_search_fuzz_verify.js` (over
`src/cpp_modules/tests/data/move_search_fixtures.json` via `build/fixture_dump`)
and `src/cpp_modules/tests/precompute_verify.js` (over
`src/cpp_modules/tests/data/precompute_fixtures.jsonl` via
`build/precompute_cli`).

## Versioned fixture and tooling inventory

The golden fixture files stay versioned permanently:

- `src/cpp_modules/tests/data/move_search_fixtures.json` and
  `src/cpp_modules/tests/move_search_fixtures.hpp` — authoritative definition
  of `fullMoveSearch` behavior (2,783 fixtures).
- `src/cpp_modules/tests/data/precompute_fixtures.jsonl` — single precompute
  regression baseline (3,470 fixtures).
- `src/benchmark/data/*` — positions and deep-playout oracles feeding the
  accuracy benchmark workflow.

Permanent verification harnesses under `src/cpp_modules/tests/`:

- `stackrabbit_test.cpp` (+ included `.hpp` suites) — `npm run cpp_test_unit`
- `fixture_dump.cpp` + `move_search_fuzz_verify.js` — `npm run fuzz_verify_sequences`
- `precompute_cli.cpp` + `precompute_verify.js` — `npm run precompute_verify`
- `precompute_cli.cpp` + `precompute_fixture_generate.js` — `npm run precompute_fixture_generate`
- `precompute_tsan.cpp` — `npm run precompute_tsan`
- `precompute_static_lint.js` + `thread_static_allowlist.txt` — `npm run precompute_static_lint`
- `live_server_check.js` + `live_server_verify.js` — `npm run live_server_check`

The standalone C++ live game server (`src/cpp_modules/src/live_game_server.cpp`,
default port 3000) is verified end-to-end by `npm run live_server_check` on
isolated port 3101 (see the overview above for the full list of HTTP routes,
backup-and-halt cancellation, error recovery, and representative golden checks).

When touching `precompute_fixture_generate.js`, run it twice to a temporary
output path and require run-vs-run **and** (for non-eval changes)
run-vs-committed fixture lines (`tail -n +2`) to be byte-identical (line 1 is
metadata only).

## Benchmarking methodology

The benchmarks evaluate two orthogonal axes: **engine throughput** (execution
speed) and **agent accuracy / decision quality** (comparison against a deep
Monte Carlo oracle).

### 1. Throughput Benchmarking (`speed_benchmark`)

- The throughput benchmark is `npm run speed_benchmark` (`src/benchmark/speed_benchmark.cpp`, `GET_MOVE`-focused).
- Always A/B against a **same-session** baseline: `git worktree add <tmp> HEAD`,
  build both binaries, and alternate runs (baseline, current, baseline, ...).
  Cross-session comparisons produced a phantom +5.5% "regression" here; the
  same-session true cost was +0.2%.
- Establish the noise floor first by building the **same source twice** and
  interleaving those runs (measured on this machine: ~±0.7% per case). Deltas
  outside that band are real; interpret the sign carefully — codegen/layout
  luck from touching a unity-build translation unit can swing either direction.
- Guard hot-path struct sizes: adding ~40 bytes to `LockPlacement` (which is
  copied tens of thousands of times per request) regressed the playout benchmark
  ~9%. Flow new feature data through side-channel structures (see
  `DasPossibility`) instead of widening structs copied per-placement.

### 2. Accuracy & Decision Quality Benchmark Suite

This suite evaluates how well different engine configurations (pruning breadth,
playout count, weight sets) choose placements on a standardized test set
compared to a deep-playout oracle:

1. **Dataset Sampling (`sample_benchmark_positions.js`)**:
   - Run: `npm run benchmark_sample_positions`
   - Samples 200 diverse game positions from `src/benchmark/data/position_log.jsonl` into
     `src/benchmark/data/benchmark_positions.jsonl`.
   - Stratifies by gravity groups (100 from level 18, 100 from levels 19–28) with move
     spacing to avoid adjacent near-duplicates.
2. **Hard Position Mining (`find_hard_positions.cpp`)**:
   - Run: `npm run benchmark_find_hard`
   - Re-randomizes pieces and compares shallow eval (0 playouts) vs. deep search (2,000 playouts).
   - Filters for positions where the two depths disagree, writing them to
     `src/benchmark/data/hard_benchmark_positions.jsonl` to test non-trivial evaluation boundaries.
3. **Deep Oracle Generation (`generate_oracle.cpp`)**:
   - Build and run: `g++ -O2 src/benchmark/generate_oracle.cpp -o build/generate_oracle && ./build/generate_oracle [start] [end]`
   - Rates every placement pair using a heavy two-round playout budget (1,000 playouts for all pairs,
     7,000 additional playouts for the top 10%).
   - Emits pooled mean ratings and standard errors to `src/benchmark/data/oracle_positions.jsonl`
     (specification in `src/benchmark/oracle_format.md`).
4. **Agent Scoring Harness (`run_agent.cpp`)**:
   - Run: `npm run benchmark_run_agent` (e.g. `./build/run_agent GET_MOVE 49 2 20 standard`)
   - Runs `mainProcess` on the benchmark positions, compares chosen placements against the oracle,
     and writes timestamped score reports into `src/benchmark/agent_results/`.

## Future additions (out of scope here)

- Unit tests for C++ internals (`getSurfaceArray`, `updateSurfaceAndHoles`,
  `getPieceRangeContext`) if black-box coverage proves insufficient.
- CI workflow job running the verification suites on push/PR (macOS, Linux, and Windows).
