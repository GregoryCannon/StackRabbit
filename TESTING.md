# Testing Plan — C++ Modules (Phase 1)

Testing the `src/cpp_modules/` AI backend first: it is the core of StackRabbit, it is
deterministic, and its entry points are pure string-in/string-out functions.

## Framework: doctest

Chosen because:

- Single-header (vendored, MIT licensed) — no new build system, no dependencies
- Works natively with the repo's unity-build style: `main.cpp` already
  `#include`s every `.cpp` file (node-gyp single-source workaround), so the test
  binary copies that exact pattern
- Fastest compile times of the viable options, which matters here (the data
  files are large)

Rejected alternatives: Catch2 v3 (compiled library, fights the unity build),
GoogleTest (needs Homebrew/vendoring + CI wiring), CMake/CTest (build-system
refactor out of scope).

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

Input format (verified in `main.cpp:63-148`):

```
[board1 (200 chars 0/1)] | [optional board2 (200 chars 0/1)] | level | lines |
currentPieceIdx | nextPieceIdx | inputFrameTimeline | playoutCount |
playoutLength | pruningBreadth | aggressionLevel |
```

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
   `request_parser.ts` rejects invalid timeline characters, so this is guarded
   at the boundary, but the C++ engine would hang if ever handed such input.

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
