# Testing Plan — C++ Modules (Phase 1)

Testing the `src/cpp_modules/` AI backend first: it is the core of StackRabbit, it is
deterministic, and its entry points are pure string-in/string-out functions.

## Test binary layout

- Vendor `doctest.h` into `src/cpp_modules/vendor/doctest.h` (v2.4.12)
- `src/cpp_modules/tests/stackrabbit_test.cpp`:
  - `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` + include `doctest.h`
  - Include the same source list as `main.cpp` (via `main.cpp` itself, the
    unity build) — but **not** `entrypoint.cpp` / `benchmark.cpp`
    (they define `main()`) or `module.cpp` / `wasm.cpp` (nan/emscripten glue)
- Test cases live in per-category headers included by the main file (the
  engine is a unity build, so everything must compile as one translation unit):
  - `tests/helpers.hpp` — shared board builders, `InputBuilder`, assertions
  - `tests/input_validation_tests.hpp` — Section A: input parsing & validation
  - `tests/request_type_tests.hpp` — Section B: one test per request type
- npm script: `"cpp_test_unit"` — one `g++` command, mirroring the existing
  `cpp_main` / `speed_benchmark` scripts (exceptions on, `-pthread` for
  `game_simulation.cpp`)
- Later: add a GitHub Actions job running it on push/PR

## Test strategy: black-box on `mainProcess`

`mainProcess` is the same surface the Node addon and WASM builds use, so tests
validate the full stack. No C++ internals need to be exposed for this.

Input format (verified in `main.cpp`'s arg loop; locked in
`docs/engine_boundaries.md`):

```
[board1 (200 chars 0/1)] | [optional board2 (200 chars 0/1)] | level | lines |
currentPieceIdx | nextPieceIdx | inputFrameTimeline | playoutCount |
playoutLength | pruningBreadth | aggressionLevel | dasCharge |
```

Every non-board field is **mandatory and strictly validated** (all TS encoders
and every hardcoded caller pass them explicitly):

- `level` / `lines` / `currentPiece` / `nextPiece` / `playoutCount` /
  `playoutLength` / `pruningBreadth` / `dasCharge`: integers (`atoi("junk")` and
  empty fields are rejected instead of silently becoming 0 -- a silent 0 would
  mean a degenerate game state, the I piece, or DAS mode).
- `nextPiece = -1` explicitly means no-next-box; `pruningBreadth = -1` means
  tournament mode.
- `inputFrameTimeline`: non-empty, `X`/`.` characters only, at least one `X`
  (an empty timeline used to make the engine never tap via `x % 0`, and a
  no-`X` timeline used to hang the move search -- finding 4 below).
- `aggressionLevel`: exactly `safe`, `aggro`, or `hyper` (unknown strings used
  to silently mean SAFE).
- `dasCharge`: `-1` = tap mode, `0..16` = DAS mode.

`secondBoard` remains genuinely optional (per request type).

Request types: `GET_LOCK_VALUE_LOOKUP`, `GET_LOCK_VALUE_LOOKUP_DAS`,
`GET_TOP_MOVES`, `GET_TOP_MOVES_HYBRID`, `RATE_MOVE`, `GET_MOVE`,
`GET_MOVE_AB_TEST`.

Shared helpers live in the test file: board-string builders (empty board, board
with a well, notch, full row), piece-letter → `PIECE_LIST` index mapping, and an
input-string encoder so tests read like the calls they exercise.

## Test matrix

### A. Input parsing & validation (fast, exact assertions)

| Case | Expectation |
|---|---|
| Missing `currentPiece` (`-1`) | exact error string `"Error: please provide a value for currentPiece."` |
| `nextPiece` = `-1` | valid; request behaves as no-next-box mode |
| Second-board detection: present for `RATE_MOVE`, optional for `GET_TOP_MOVES`/`HYBRID`, absent otherwise | correct board chosen |
| Malformed second board (non-`0/1` chars at positions 201–400, pipes at 200/401) | falls back to no-second-board path |
| `RATE_MOVE` with no second board | legacy fallback path ("player move not found" style), no OOB |
| Non-numeric args (e.g. `level=abc` via `atoi`) | characterization: currently silently becomes 0 — pin current behavior |
| Unknown aggression string | defaults to `SAFE` |

### B. Per request type (structural assertions on output)

| Request | Assertions |
|---|---|
| `GET_MOVE` | output matches `[rot, xOffset, yOffset]`; rot ∈ 0–3; xOffset in valid range; **determinism** (same input twice → same output, run early to confirm) |
| `GET_TOP_MOVES` | valid JSON; ≤ `NUM_ENGINE_MOVES` entries; player's move (from `secondBoard`) included even if not top-ranked |
| `GET_TOP_MOVES_HYBRID` | output has `"noNextBox"` and `"nextBox"` keys, each a valid list |
| `RATE_MOVE` | numeric score for a known player board; sane with/without `nextPiece` |
| `GET_LOCK_VALUE_LOOKUP` / `_DAS` | non-empty encoded lookup; both variants produce consistent structure; DAS-charge variant accepted |
| `GET_MOVE_AB_TEST` | first element `"agree"` or `"disagree"`; explanation strings present and JSON-escaped |
| All types | empty board, tall board, and notch board inputs all complete without crash |

### C. Characterization / regression goldens

- Pin 2–3 representative exact output strings (one `GET_MOVE`, one
  `GET_TOP_MOVES`, one `RATE_MOVE`). The Node and WASM clients parse these
  formats — any drift breaks prod, so goldens guard it.
- Exercise levels 18, 19, and 29 (kill-screen gravity) per core request type.
- Use small `playoutCount`/`playoutLength` to keep the suite fast; heavy
  parameter runs stay in `benchmark.cpp`, not tests.

## Findings (from building the suite)

1. **`RATE_MOVE` without a second board segfaulted.** The legacy fallback at
   `main.cpp:84-88` did `std::string(inputStr + 402)`, which reads past the
   buffer's null terminator. Reachable in production (the TS server only
   appends `secondBoard` to the encoded input when the URL provides one).
   **Fixed** in `main.cpp`: such requests now return
   `"Error: please provide a value for secondBoard."` (covered by tests).
2. **Inputs shorter than 201 characters caused out-of-bounds reads** in the
   arg-slicing paths (`std::string(inputStr + 201)`). **Fixed** in `main.cpp`:
   a length guard rejects them upfront (covered by tests).
3. **A malformed second board silently shifts every parsed field by two** and
   can produce degenerate parameters. Not covered by tests (degenerate
   combinations hang the engine; see #4). Worth guarding in the parser.
4. **A degenerate `inputFrameTimeline` with no `X` frames hangs the move
   search** at any level — bisected via the shifted-parse scenario. The TS
   `request_parser.ts` rejects invalid timeline characters, and the C++ parser
   now also validates the field (non-empty, `X`/`.` only, at least one `X`) and
   rejects empty timelines outright, so the engine can no longer be handed such
   input by any caller.

## TS oracle fuzz fixtures (DAS move search)

`npm run fuzz_generate` generates ~2800 board/timeline/level fixtures through
the TypeScript `fullMoveSearch` (the reference implementation) and writes them
to `src/server/move_search_fixtures.json`. The C++ doctest suite replays them
via `fullMoveSearch` and compares (rotationIndex, x, y, dasChargeAfter,
inputCostX10) tuples **order-exactly** — element i of the golden list must
equal element i of the search output. Result order is load-bearing downstream
(`possibleMoves[0]` is the default placement; the phantom sort is stable over
it), so an order divergence is a real bug, not noise. The one multiset
exception is the fastMoveSearch hot-path guard, which asserts the placement
SET only (a different search with different tuck semantics; its output order
is its own).

`npm run fuzz_verify_sequences` is a second, stronger gate: it compiles
`src/cpp_modules/tests/fixture_dump.cpp`, dumps C++ results for every fixture
(both modes come out of `fullMoveSearch`; tap fixtures exercise its ARR-gated
tap path), and diffs tuples **and character-exact input sequences** against the
oracle JSON (exit nonzero on any mismatch). Use it after any change to either
implementation's move search. In the doctest suite, tap fixtures are asserted
twice: through fastMoveSearch (the hot-path guard) and through fullMoveSearch's
tap path.

### The two "tuck" definitions (fastMoveSearch vs fullMoveSearch)

The two searches define "tuck" differently, by design:

- **fastMoveSearch** finds gameplay tucks: destination cells flagged as
  overhang setups by the board analysis (`analyzeHole` / `TUCK_SETUP_BIT`).
- **TS dfs / fullMoveSearch** append any input that produces a novel lock
  during the fall. This covers cases the rigid assumptions of fastMoveSearch
  miss: placements that depend on the *ordering* of shifts and rotations during
  an otherwise non-tuck placement.

Concretely: the greedy goal-walk merges shift+rotation into the earliest
possible input, which can catch on a stack corner and lock one cell away, while
the dfs's re-ordered inputs (e.g. shift first, rotate later) slip past and
reach cells the walk can never express. On the jagged-overhang board the TS
definition yields 46 spins + 14 spin-tucks that fastMoveSearch intentionally
does not model, so those fixtures are skipped in the fastMoveSearch guard only
(asserted via fullMoveSearch, which must match TS byte-for-byte for the
precompute pipeline).

Fixture coverage axes: all 7 pieces; `dasCharge ∈ {0..16}` with emphasis on
`{0, 5, 6, 10, 14, 15, 16}`; levels 18 / 19 / 29; timelines `"X.."`, `"X..."`,
`"X....."`; boards spanning flat heights 0–16, overhang/tuck setups, edge/wall
adjacency, and near-ceiling spawns; zero-shift and max-shift placements.
Tall-stack boards (stacks reaching rows 0-2, `tap_tall`/`das_tall` tags)
exercise locks in the top rows, including a one-row lid where only the I piece
has any placement; adjustment fixtures include `existingYOffset = 20`
past-bottom entries (empty result on both sides). The
tap fixtures additionally include a jagged overhang board (`tap_tuck` tag) --
the only structured board whose tap-mode search produces tuck/spin/spin-tuck
possibilities, exercising the tuck/spin DFS's tap branches. The fixtures are
generated with a fixed seed, so `npm run fuzz_generate` is byte-stable — commit
the diff if TS semantics change intentionally.

### Adjustment-mode fixtures (mid-air entry)

Precompute migration Phase 1 step 2 added an `adjustmentFixtures` section
(JSON key + `ADJUSTMENT_*` pools in the C++ header, separate index space from
the spawn fixtures). Each fixture records a mid-air state exactly the way
`PreComputeManager._calculatePhantomPlacements` derives one: run the TS spawn
`getPossibleMoves` WITH a reaction time (8 / 13 / 20), stable-sort by
`countInputs`, dedupe by the first `reactionTime` chars of `inputSequence`,
then save the `adjTimeSimState` snapshot as `framesAlreadyElapsed / existingXOffset /
existingYOffset / existingRotation / dasCharge / adjustmentState` and run the
adjustment `getPossibleMoves` (no reaction time) as the golden.

 Phantom selection is deterministic and coverage-driven (first phantom + the
first phantom of each rare `(isArrContinued, dasButtonHeld, rotation-class)`
bucket in a fixed priority order) — naive first/last/random sampling starves
`existingRotation=3` and DAS `isArrContinued=true`, which only multi-rotation
goals reach and which sort last by `countInputs`. Tap-mode
`dasButtonHeld != NONE` at the reaction-time frame is only reachable when
`reactionTime - 1` is an ARR input frame (rt=13 works for all three
timelines). The doctest suite asserts the entry tuples; sequences are
compared character-exactly by `fuzz_verify_sequences` under `adjustment <idx>`
dump headers.

### Reaction-time fixtures (adjTimeSimState snapshot, spawn entry)

Phase 1 step 3 added a `reactionTimeFixtures` section (JSON key +
`REACTION_TIME_*` pools in the C++ header, separate index space). Each fixture
runs the TS spawn `getPossibleMoves` WITH a reaction time (0 / 1 / 2 / 5 / 8 /
13 / 20 / 40, both modes) and records the FULL possibility list in result
order, each entry carrying its walk's `adjTimeSimState` snapshot (or null).
Unlike the other sections this one is compared **order-exactly** (array
equality, no sorting) with the snapshot fields included — the
result-to-snapshot mapping is part of the golden, not just the placement set.

Two semantics the fixtures pin down:

- A walk that reaches its goal before the reaction-time frame keeps
  simulating (the piece keeps falling at the goal column) until the
  reaction-time frame, so its LAST registered sim state — and therefore the
  deduped last-of-group possibility — carries the snapshot, while the
  first-of-group possibility (registered before the reaction-time frame)
  carries none. Walks whose piece locks before the reaction-time frame save
  no snapshot at all.
- The dfs novel-keeping comparison (`inputCost + (adjTimeSimState == null ?
  -1000 : 0)`) reads snapshot presence: of two tuck routes to the same cell,
  the one whose parent walk saved its adjTimeSimState wins. The tuck-coverage board at
  reaction times 6/20 produces such collisions (an instrumented TS run over
  the section observed 21 outcome-changing comparisons in 3 fixtures; the
  doctest asserts the resulting possibilities order-exactly).

Reaction time 0 never saves a snapshot and never waits; the C++ side treats
any `reactionTime <= 0` the same. Dumped under `reactiontime <idx>` headers
by fixture_dump (no sorting); the doctest compares tuples + snapshots in
order,
`fuzz_verify_sequences` additionally compares the sequences character-exactly.

## Precompute golden fixtures (full-request parity oracle)

`npm run precompute_fixture_generate` drives the **real TS precompute path**
(`RequestHandler.handlePrecomputeRequestSync` with a live `PreComputeManager`)
over positions from `src/benchmark/data/position_log.jsonl` and writes
`src/server/data/precompute_fixtures.jsonl` — the exact request parameters plus
the verbatim output string per fixture. This is the parity oracle for the C++
precompute port (see docs/precompute_cpp_migration.md, Phase 3 differential
harness): the C++ implementation must reproduce each output byte-for-byte.

Design notes:

- Two tiers: **eval_only** (3428 fixtures, `playoutCount=0&playoutLength=0` —
  fast, still exercises the full pipeline) and **production** (42 fixtures with
  default engine params 49/2/20/safe).
- Each logged position contributes 2 fixtures with cycled `(reactionTime,
  dasCharge)` combos (rt ∈ {0, 8, 20}; dasCharge ∈ {-1, 0, 5, 10, 15, 16}) and
  a cycled current piece; the logged `nextPiece` is ignored (precompute
  searches all 7 next pieces).
- Coverage quotas are hard-asserted by the generator (per-piece, per-rt,
  per-dasCharge, per-combo floors; `continue` lines; per-piece `none` lines;
  whole-response `none`; default placements). Synthetic fixtures are appended
  only when a category is unreachable from the log.
- Generation is deterministic (fixed seed, no timestamps): two runs must be
  byte-identical. Commit the diff if TS precompute semantics change
  intentionally.
- Known pre-existing corner, now guarded in TS: with `reactionTime=0`, a piece
  whose adjustments all miss the lock value lookup used to throw a TypeError
  in `formatPrecomputeResult` (null `defaultPlacement`); it now emits the
  normal per-piece `none` fallback. The C++ port must reproduce the guarded
  behavior.

## Benchmarking methodology

- The throughput benchmark is `npm run speed_benchmark` (GET_MOVE-focused).
- Always A/B against a **same-session** baseline: `git worktree add <tmp> HEAD`,
  build both binaries, and alternate runs (baseline, current, baseline, ...).
  Cross-session comparisons produced a phantom +5.5% "regression" here; the
  same-session true cost was +0.2%.
- Establish the noise floor first by building the **same source twice** and
  interleaving those runs (measured on this machine: ~±0.7% per case). Deltas
  outside that band are real; interpret the sign carefully — codegen/layout luck
  from touching a unity-build translation unit can swing either direction.
- Guard hot-path struct sizes: adding ~40 bytes to `LockPlacement` (which is
  copied tens of thousands of times per request) regressed the playout benchmark
  ~9%. Flow new feature data through side-channel structures (see
  `DasPossibility`) instead of widening structs copied per-placement.

## Phases

1. **Setup** — vendor doctest, create test binary + npm script, one smoke test
   (`GET_MOVE` on an empty board returns valid output) ✅
2. **Core matrix** — sections A + B: 17 test cases, 83 assertions, all passing
   (`npm run cpp_test_unit`, ~2s) ✅
3. **Goldens** — section C, output files under `src/cpp_modules/tests/goldens/`
4. **CI** — workflow job running `cpp_test_unit` on push/PR (macOS + Linux)

## Later phases (out of scope here)

- Unit tests for C++ internals (`getSurfaceArray`, `updateSurfaceAndHoles`,
  `getPieceRangeContext`) if black-box coverage proves insufficient
- TS-side tests per the earlier plan (which also guard the same pipe-format
  contract from the other side)
