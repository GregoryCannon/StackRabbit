# Plan: Standard Move Search Optimization (`fastMoveSearch`)

Goal: Optimize the standard move search (`src/cpp_modules/src/move_search.cpp`: `fastMoveSearch` / `moveSearchInternal`) to accelerate playout simulation and request throughput, while rigorously ensuring zero behavioral divergence and zero accuracy regression.

## 1. Scope & Motivation

- **Target Scope**:
  - Focus strictly on `src/cpp_modules/src/move_search.cpp` (`fastMoveSearch` and `moveSearchInternal`).
  - `src/cpp_modules/src/full_move_search.cpp` (`fullMoveSearch`) is intentionally **out of scope** for this phase.
- **Why Standard Move Search Has the Highest ROI**:
  - `fullMoveSearch` runs only for the root placement and adjustment phantoms (1–15 times per request).
  - In contrast, `fastMoveSearch` is invoked at **every step of every playout simulation** across the entire search tree (e.g. in `playout.cpp:55`). During production requests (playoutCount=49, playoutLength=2) and deep searches, `fastMoveSearch` is called thousands of times alongside `fastEval()`.
  - Accelerating `fastMoveSearch` directly translates to faster lock value lookups, faster precomputes, and higher overall engine throughput.

## 2. High-Yield Optimization Vectors in `move_search.cpp`

1. **Eliminate Heap Allocations in `moveSearchInternal`**:
   - `std::vector<SimState> legalMidairPlacements` is currently heap-allocated and resized dynamically on every search invocation.
   - Replace with a pre-sized stack buffer (e.g. `std::array<SimState, MAX_MIDAIR_PLACEMENTS>`) or fixed capacity container; the maximum number of legal midair states across any tetromino rotation is bounded and small.
2. **Eliminate Dynamic Hash Set in `findTucks`**:
   - `findTucks()` allocates a `std::unordered_set<int> tuckLockSpots` on the heap every time tucks are enabled.
   - Replace with a compact flat array, bitmask, or small lookup table to eliminate heap churn.
3. **Collision Detection Hot Path (`collision()`)**:
   - `collision()` is called repeatedly inside `exploreHorizontally` and `findTucks`.
   - Verify compiler inlining and streamline the 4-row loop and `SHIFTBY` operations.
4. **Pass-by-Value Overhead**:
   - Audit parameter passing (e.g. `GameState gameState`, `SimState simState`) to avoid unnecessary struct copying across the horizontal exploration loops.

## 3. Multi-Tier Verification & Accuracy Safety Net

Every optimization step must pass all three verification tiers before proceeding:

### Tier 1: Byte-Exact & Placement Parity Gates
* `npm run cpp_test_unit`:
  * Fast (~2s) doctest suite (64 test cases, 41,625 assertions).
  * Directly exercises `fastMoveSearch` against tap-mode fuzz fixtures (`Tap-mode fuzz (fastMoveSearch): 748 fixtures checked, 0 mismatched`).
* `npm run fuzz_verify_sequences`:
  * Verifies character-exact input sequences and tuples across all ~2,800 move search fixtures.
* `npm run precompute_verify`:
  * Verifies all 3,470 full precompute golden fixtures byte-for-byte through the Node addon.

### Tier 2: Decision Accuracy Gate (Hard Oracle Benchmark)
* Command:
  ```bash
  npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard
  ```
* Evaluates `mainProcess` on the 200 curated hard positions in `src/benchmark/data/hard_benchmark_positions.jsonl` against `src/benchmark/data/hard_oracle_positions.jsonl`.
* **Requirement**: Zero regressions in match rate, score delta distributions, or chosen move keys compared to baseline.

### Tier 3: Throughput & Concurrency Gates
* **Same-Session Interleaved A/B Benchmarking**:
  * Run `npm run speed_benchmark` (`src/benchmark/speed_benchmark.cpp`).
  * Alternates runs between candidate binary and a clean worktree baseline (`git worktree add <tmp> HEAD`) to filter out the ~±0.7% noise floor and layout-luck artifacts.
* **Concurrency Check**:
  * `npm run precompute_tsan` (race detection over concurrent worker threads).
  * `npm run precompute_static_lint` (ensures any scratch buffers or lookup tables are thread-safe and non-static/thread_local).

## 4. Phased Execution Roadmap

### Phase 1: Baseline Establishment & Worktree Setup
- [x] Create clean baseline worktree (`/tmp/sr-baseline` from HEAD `5f3690d`).
- [x] Record baseline throughput with interleaved `speed_benchmark` runs (Mean: 1466.1 ms, noise floor ~±0.7%).
- [x] Record baseline accuracy benchmark on hard positions:
  ```bash
  npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard
  ```
  Snapshot saved in `src/benchmark/agent_results/baseline_hard_get_move_49x2_b20.jsonl` (Match rate: 0.580).

### Phase 2: Allocation Elimination in `move_search.cpp`
- [x] Replace `vector<SimState> legalMidairPlacements` with fixed-size stack container `LegalPlacementsList`.
- [x] Replace `std::unordered_set<int> tuckLockSpots` in `findTucks` with stack array `bool seenTuckSpots[25][16][4]`.
- [x] Eliminate repeated `strlen(inputFrameTimeline)` calls inside the `exploreHorizontally` frame simulation loop.
- [x] Run Tier 1 (`cpp_test_unit` 41,625 assertions, `fuzz_verify_sequences` 2,783 fixtures character-exact, `precompute_verify` 3,470/3,470 byte-identical).
- [x] Run Tier 2 (`benchmark_run_agent -- GET_MOVE 49 2 20 hard` — 0 decision differences vs. baseline).
- [x] Run Tier 3:
  - `precompute_static_lint`: 0 non-const file-scope statics.
  - `precompute_tsan`: 42/42 byte-identical under ThreadSanitizer.
  - Interleaved `speed_benchmark`: **-7.33% latency reduction** (1500.88 ms → 1390.80 ms).

### Phase 3: Collision, Inlining, and Loop Optimization
- [x] Inline `collision()` and `rotateTowardsGoal()`.
- [x] Streamline `collision()`: hoisted `x >= 0` check out of row loop to eliminate per-row ternary branching and bounds shifting; pre-clamped `rStart = (y < 0) ? -y : 0` to eliminate ceiling branches inside loop.
- [x] Optimize `isGravityFrame`: avoid integer division for level 19–28 (`simState.frameIndex & 1` for `gravity == 2`).
- [x] Pre-reserve `lockPlacements.reserve(40)` in `moveSearchInternal()` to eliminate dynamic vector reallocations during placement collection.
- [x] Replace dynamic `std::list` tables in `tetrominoes.hpp` (`TUCK_SPOTS_LIST` and `TUCK_INPUTS`) with fixed-size `constexpr` lookup arrays (`constexpr TuckSpotList` and `constexpr TuckInput[8]`), eliminating heap node allocations and pointer dereferencing in `findTucks` / `findTuckInput`.
- [x] Run Tier 1:
  - `npm run cpp_test_unit`: 64/64 test cases, 41,625/41,625 assertions passed.
  - `npm run fuzz_verify_sequences`: 2,783/2,783 fixtures character-exact.
  - `npm run precompute_verify`: 3,470/3,470 fixtures byte-identical.
- [x] Run Tier 2:
  - `npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard`: 200/200 positions evaluated, 0 decision differences vs baseline.
- [x] Run Tier 3:
  - `npm run precompute_static_lint`: 0 non-const file-scope statics.
  - `npm run precompute_tsan`: 42/42 byte-identical under ThreadSanitizer.
  - Interleaved `speed_benchmark` (10 iterations): **-10.51% net latency reduction** vs baseline (1,495.51 ms → 1,338.39 ms).

### Phase 4: Final Verification & Performance Sign-Off
- [x] Run core verification battery:
  - `npm run cpp_test_unit` (41,625 assertions passed)
  - `npm run fuzz_verify_sequences` (2,783 fixtures character-exact)
  - `npx tsc` (0 errors)
  - `npm run move_test` (passed)
  - `npm run precompute_verify` (3,470/3,470 byte-identical)
  - `npm run precompute_tsan` (42/42 byte-identical)
  - `npm run precompute_static_lint` (0 violations)
  - `npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard` (0 decision diffs)
- [x] Final net throughput improvement: **-10.51% total latency reduction** across all 23 cases in `speed_benchmark`.
