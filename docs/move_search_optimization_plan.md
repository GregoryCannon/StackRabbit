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
- [ ] Create clean baseline worktree (`git worktree add ../sr-baseline HEAD`).
- [ ] Record baseline throughput with interleaved `speed_benchmark` runs.
- [ ] Record baseline accuracy benchmark on hard positions:
  ```bash
  npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard
  ```
  Snapshot results in `src/benchmark/agent_results/`.

### Phase 2: Allocation Elimination in `move_search.cpp`
- [ ] Replace `vector<SimState> legalMidairPlacements` with a fixed-size stack container.
- [ ] Replace `std::unordered_set<int> tuckLockSpots` in `findTucks` with a flat bitset or array.
- [ ] Run Tier 1 (`cpp_test_unit`, `fuzz_verify_sequences`, `precompute_verify`).
- [ ] Run Tier 2 (`benchmark_run_agent -- GET_MOVE 49 2 20 hard`).
- [ ] Run Tier 3 (interleaved `speed_benchmark` vs. worktree baseline).

### Phase 3: Collision & Loop Optimization
- [ ] Optimize `collision()` hot-path and boundary checking.
- [ ] Streamline horizontal shift/rotation step loops.
- [ ] Run Tier 1 + Tier 2 + Tier 3 battery.

### Phase 4: Final Verification & Performance Sign-Off
- [ ] Run full battery:
  - `npm run cpp_test_unit`
  - `npm run fuzz_verify_sequences`
  - `npx tsc`
  - `npm run move_test`
  - `npm run precompute_verify`
  - `npm run precompute_tsan`
  - `npm run precompute_static_lint`
  - `npm run start_check`
  - `npm run live_server_check`
  - `npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard`
- [ ] Measure net throughput improvement across all cases in `speed_benchmark`.
