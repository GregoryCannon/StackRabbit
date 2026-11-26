# Plan: 20-Row NES Tetris (Level 18/19 DAS) Tablebase + NNUE

**Audience:** Engineering agents executing Phases 0–6.
**Goal:** Build a Hybrid Tablebase + NNUE engine under `experimental/das_tablebase/` that beats production StackRabbit's eval + playout engine on **average score in full NTSC Level 18 → 29 DAS games** (`18`-frame reaction time, `10 Hz` pure DAS / no quicktaps).

Companion references (read before starting):
- [docs/tetris_20row_tablebase_handoff.md](tetris_20row_tablebase_handoff.md) — full transfer spec from 4×10.
- [docs/tablebase_retrospective.md](tablebase_retrospective.md), [docs/tablebase_distillation_retrospective.md](tablebase_distillation_retrospective.md), [docs/small_tablebase_improvement_retrospective.md](small_tablebase_improvement_retrospective.md) — empirical 4×10 results & negative results.
- [AGENTS.md](../AGENTS.md), [docs/DAS_explainer.md](DAS_explainer.md), [docs/engine_endpoints.md](engine_endpoints.md).

---

## 1. Locked Design Decisions & Constraints

### Target Ruleset & Headline Metric
- **Game mode**: NTSC NES Tetris (`IS_PAL = false`), Level 18 start (`L = 0..129`, gravity `3`), Level 19–28 (`L = 130..229`, gravity `2`), Level 29 killscreen terminal (`L >= 230`, continuation value `0.0`).
- **Input rules**: **No quicktaps** — pure 10 Hz DAS (`INPUT_TIMELINE = "X....."`). Offline move search assumes full DAS (`"X....."`), which matches 10 Hz no-quicktap live DAS play closely; live/final evaluation still uses `src/backend/full_move_search.cpp` + `dasPenaltyLookupMap` for exact parity.
- **Reaction time**: **`18` frames** (`REACTION_TIME_FRAMES = 18`). On Level 19 (`gravity = 2`), a piece drops `9` rows and executes up to `3` shifts before reaction; on Level 18 (`gravity = 3`), `6` rows.
- **Headline metric**: **Average NES score on held-out NTSC 18 → 29 DAS games** (18-frame reaction time, 10 Hz) vs. production StackRabbit (`precompute` tournament `-2` real-time deadline and fixed playout baselines, e.g., `160×8` / `300×6`). Secondary metrics: Level 19 start score, maxout rate, `docs/benchmark_hard_positions.md` accuracy.
- **Primary deployment target**: **Hybrid TB + NNUE** (desktop / `live_game_server`). Standalone NNUE (for WASM) is a secondary follow-up after TB+NNUE wins.

### Proven Architecture (Port from `experimental/tablebase/`)
1. **Native 1-Piece Next-Box `V(b, c)` (`7` floats/board) + Exact 7×7 NES RNG**:
   - Remap `src/backend/piece_rng.cpp` (`T,J,Z,O,S,L,I`) → `PIECE_LIST` (`I,O,L,J,T,S,Z`); weight openings by exact stationary $\pi_{\text{NES}}$.
   - Generate `Moves(b, c)` and hash children **once** per `(b, c)` and reuse across all 7 next pieces `n = 0..6`.
2. **Soft-Boundary Value-Bootstrapped MPI + `bestOutStore`**:
   - Frozen NN boundary payoff `B(b', n) = max(0, V_nn(b', n) - m)` precomputed into `bestOutStore` once on outer iteration 1 (`15×` speedup), disk-streamed `PolicyActionStore` (`49 × 4 B = 196 B/board`), `double` working buffers `vCur`/`vNext` (`112 B/board`), adaptive inner early-stop (`innerDiff <= max(tol * 0.05, outerDiff * 0.05)`), warm-start (`--init`), `maxValueDivergenceGuard`, and automatic Soft-MC verification (`<= 3.5σ` vs `E[V(0)]`).
3. **NN-Guided `nn-error` State Selection Under Board Cap**:
   - Round 0 seeded via thin read-only adapter to `src/backend/eval.cpp`; **drop `eval.cpp` completely from Round 1 onward**.
   - 50/50 exploration split (TB runner-ups + NN frontier probes excluding greedy TB move) + core sibling expansion + `protectedCore` + `nn-error` priority eviction (only enable non-visit eviction once Soft-Boundary MPI is active).
4. **Pure-Supervised Multi-Task NNUE (`SRNNV001`, `featureVersion = 2`) + Calibrated Partial-OUT**:
   - On-policy trajectory sampling (`70%` greedy TB + `30%` `ε`-recovery — never uniform over `.tb.bin`).
   - Mandatory 4-part loss: parent 7-output Huber + candidate-child Huber + explicit ply-2 top-out supervision (`Q* = r_m + 0.0`) + Boltzmann move-ranking CE.
   - `128×64` (`~175 KB`) for fast flywheel rounds; `256×64` (`~345 KB`) for final 2-ply play (keep fast stack buffer `< 4 KB` to avoid `___chkstk_darwin`).
   - Runtime Partial-OUT selector: all-OUT ranks directly via NN without clamping; mixed IN/OUT prices OUT at `r_m + max(0, estCont(b'_m, n) - m)` (in-TB wins ties; runtime `m` lighter than solver `m`).

### Explicitly Excluded From v1
- **No self-play RL (`TD(λ)` / Monte Carlo / Expert Iteration)** — lost to pure supervised 2-ply on 4×10 (`-5.21` lines) with severe margin/scale fragility.
- **No structural enumeration**, **no NNB `V(b) + 2-ply`**, **no horizontal board-mirroring augmentation** (asymmetric NES spawn/kicks/RNG), **no MSE-only linear fits**.

### Reward Formulation & 4-Regime Staging
- **Stationary regimes (Phases 2–3: Level 19, Level 18)**:
  - Normalized NES line-clear points: `r(0..4) = {0.0, 1/30, 1/12, 1/4, 1.0}` (`40/100/300/1200` ratio).
  - Discount $\gamma \approx 0.995$ (sweep `0.992..0.997` on validation).
  - Rescaled value-unit hyperparameters (vs. 4×10 lines): `boundaryMargin ≈ 0.5–1.0`, `huberDelta ≈ 0.5–1.0`, `Boltzmann T ≈ 0.3–0.5`, `errCap ≈ 3.0`, `prescreenSlack ≈ 1.5–2.0`.
- **Finite-horizon backward induction (Phases 4–5)**:
  - **Phase 4 (`L = 229 → 130`, all 100 Level 19–28 slices)**: **Undiscounted ($\gamma = 1.0$) exact NES points** (`{0, 40, 100, 300, 1200} × (levelAfter + 1)`, stored in `1,000`-point or `1,200`-point units so fixed-point terminal words stay high-resolution without overflow). Shared `states.bin` across the band → zero cross-slice hashing, only 5 slices (`L..L+4`) in RAM at once, terminal `0.0` at `L >= 230`. `V(b, c, L)` directly reconstructs exact expected remaining NES score to killscreen with exact `20× → 29×` level-up multipliers and boundary-crossing Tetris bonuses.
  - **Phase 5 (`L = 129 → 0` / transition band `L = 129 → ~115` + Stationary 18)**: Constant `19×` multiplier throughout Level 18, terminating cleanly into Phase 4's exact `V(b, c, 130)` at `L >= 130`. Distill all regimes into one line-count-aware NNUE `V_nn(b, c, L)` (plus evaluate how many slices to retain on disk for Hybrid TB+NNUE vs. NNUE-only).

### Locked Board Key Encoding & `20M–50M` Scale Requirement
- **Locked key (`K16-Opt2c`, `16 B = uint64_t[2]`, 128 bits)** — selected from Phase 0 data (`99.98%–99.99%` of revisited boards, `98.3%` of all live moves, zero well-column assumption, pure bit-shift encode/decode):
  - `surface[0..9]` (`0..20` each, `5 bits × 10 = 50 bits`)
  - `numHoles` (`0..9`, `4 bits`)
  - Sorted hole coordinates (`(r - 1) * 10 + c ∈ 0..189`, `8 bits × 9 = 72 bits`; unused slots zeroed)
  - Total = `126 bits <= 128 bits`; `EMPTY_KEY = {~0ULL, ~0ULL}` (`surface[0] = 31 > 20`, collision-free sentinel).
  - Any board with `>= 10` empty cells under the surface exits cleanly to the Soft-Boundary NNUE.
- **Scale requirement (`20M–50M` boards)**: Every data structure (`FlatBoardIndex`, `ShardedVisitCounter`, `PrioritizedBoard`, `PolicyActionStore`, `bestOutStore`, `SRSTV002`/`SRTBV003`/`SRTBS001` I/O) **must be designed and audited for `20M–50M` boards** (16 GB RAM budget, 64-bit file offsets `off_t`/`uint64_t`, zero `size_t` / 32-bit multiplication overflows, strict buffer lifetime scoping so solver & evaluator arrays never overlap in RAM, uncorrelated shard/slot hashes, and disk-streamed policy caching above `~6M` boards):
  - Per-board footprint (`K16-Opt2c`): `168 B/board` stationary solver RAM (`boards[16] + FlatBoardIndex SoA[~40] + vCur/vNext[112]`), `44 B/board` `.tb.bin`, `196 B/board` SSD policy cache (`28-bit childId` supports up to `268M` boards).
  - Concrete budgets: **`5M`** (`0.84 GB` RAM, `220 MB` `.tb.bin`, ~5 min warm solve — fast early iteration & 18-frame `V_RT` checks) → **`20M`** (`3.36 GB` RAM, `880 MB` `.tb.bin`, ~20–30 min warm solve) → **`40M–50M`** (`6.7–8.4 GB` RAM, `1.76–2.20 GB` `.tb.bin`, still fits in 16 GB RAM).
  - Multi-slice Phases 4–5 (`1` active `double[7]` + `4` frozen `float[7]` = `224 B/board` RAM, `28 B/board` per slice on disk): size the shared state set (`10M–20M`) and slice-retention stride to stay within disk/RAM budgets.

### Isolation, Verification & Seed Hygiene
- **Strict isolation**: Put all new code in `experimental/das_tablebase/` (data in gitignored `experimental/das_tablebase/data/`). Keep `git diff --stat -- src/` empty and `experimental/tablebase/` untouched & passing through Phase 5. Only touch `src/` in an explicit user-approved integration step.
- **Disjoint RNG seeds**:
  - Training / state generation: `seed = 10,000,000+`
  - Validation (margin/$\gamma$/hyperparam sweeps only): `seed = 30,000,000` (`500` games for full 18→29 / slow `precompute`; up to `2,000` for fast stationary checks)
  - Held-out test (final reported numbers only, never tune on it): `seed = 700,000` (`1,000` paired games for full 18→29 & `precompute` comparisons; up to `2,000–5,000` for fast stationary checks)
- **Per-phase gates**: `npm run format_cpp` (revert drift on untouched files), `-Wall -Wextra -Werror` + `-DNDEBUG -fsyntax-only`, full unit-test suite, `npm run cpp_test_unit`, `npm run precompute_static_lint`, and parallel subagent reviews (`logic_reviewer` 4-point seam checklist audited at `N = 50M` + `readability_reviewer`, plus `perf_scale_reviewer` on Phases 1, 2, 4).

---

## 2. Phased Implementation Plan

### Phase 0: Revisit-Rate, Key-Encoding & Move-Gen Throughput Spike
Measure real 20-row revisit rates, encoding coverage, and move-gen speed BEFORE locking file formats or writing any solver.

- [x] **Built lightweight diagnostic CLI (`experimental/das_tablebase/phase0_spike.cpp`)**:
  - Read-only wrapper calling `src/backend/move_search.cpp` (`fastMoveSearch` with `"X....."`), `move_result.cpp`, `eval.cpp` (2-ply current+next lookahead with `AGGRO` context), and exact 7×7 NES RNG.
- [x] **Ran 2-step Level 19 & Level 18 (10 Hz) diagnostic** (`10.0M` L19 train moves / `50k` games in `121s`, `10.7M` L18 train moves / `35k` games in `142s`, evaluated out-of-sample on disjoint `500`-game test splits):
  - **Out-of-sample TB hit rate (L19 / L18)**: `100k`: `34.7% / 35.2%` | `500k`: `49.1% / 51.9%` | `1M`: `53.2% / 57.0%` | `2.5M`: **`56.5% / 60.7%`** | `ALL (~4.8M)`: **`59.5% / 63.7%`** (0-hole boards: **`83.4% / 83.0%`**). Strong green light for Hybrid TB + NNUE!
  - **Height stratification (L19 / L18)**: `H <= 8`: `76.1% / 62.3%` of moves (`hit@ALL = 70.6% / 74.9%`); `H = 9..12`: `19.5% / 33.7%` (`hit@ALL = 29.7% / 50.0%`); `H = 13..15`: `2.4% / 3.3%` (`hit@ALL = 0.3% / 5.4%`); `H = 16`: `0.5% / 0.3%` (`hit@ALL = 0.00% / 0.23%`); `H = 17+`: `1.5% / 0.5%` (`hit@ALL = 0.00% / 0.00%`).
  - **Hole stratification (L19 / L18)**: `0h`: `64.7% / 71.8%` (`hit@ALL = 83.4% / 83.0%`); `1h`: `9.5% / 7.5%` (`23.8% / 23.5%`); `2–4h`: `13.4% / 8.4%` (`12–19% / 8–17%`); `5..9h` (incl. covered right-well overhangs): `10.8% / 10.7%` (`10.4% / 11.8%`); `10+h`: `1.7% / 1.7%` (`0.4% / 1.0%`).
  - **Candidate key coverage (`ALL moves` | `Hit@2.5M` | `Hit@ALL`, L19 / L18)**:
    - `K8-Opt2a` (`8 B: H<=15, <=4h`): `87.4% / 87.6%` | **`98.60% / 98.61%`** | **`98.11% / 98.00%`**
    - `K8-Opt2b` (`8 B: H<=16, <=3h`): `83.0% / 85.2%` | `97.54% / 98.32%` | `96.67% / 97.70%`
    - `K8-Opt1a` (`8 B: rw + <=6 act`): `82.2% / 83.1%` | `98.06% / 97.60%` | `97.62% / 96.78%`
    - **`K16-Opt2c` (`16 B: H<=20, <=9h`)**: **`98.32% / 98.35%`** | **`99.99% / 99.98%`** | **`99.99% / 99.97%`**
    - `K16-Opt1b` (`16 B: aw + <=12 act`): `97.93% / 98.99%` | `100.00% / 100.00%` | `100.00% / 100.00%`
  - **Move-gen throughput (`fastMoveSearch`, 10 Hz, 1T)**: **`461 ns/call`** (L19, `22.4` moves/call) and **`431 ns/call`** (L18, `23.0` moves/call) → projected move-gen `T_outer` (10T): `0.9s` (`2.5M`), `1.9s` (`5M`), `3.8s` (`10M`). Full warm-start MPI solve at `5M–10M` boards projects to `~5–15 min`.
- [x] **Decision Gate (Locked)**:
  - Locked **`K16-Opt2c`** (`16 B`: `H <= 20, <= 9 holes`, `99.98%–99.99%` of revisited boards, captures `5..9`-hole covered-well overhangs), **`20M–50M`** scale architecture requirement, and **`5M` fast iteration → `20M+` final stationary TB** progression.

---

### Phase 1: Isolated 20-Row Core, Fast Move Gen, Parity Tests & Baseline Harness
Build the rock-solid, zero-allocation 20-row engine primitives and record exact production StackRabbit baselines.

- [x] **Core primitives (`experimental/das_tablebase/core_20row.hpp`, `board_key.hpp`, `eval_adapter.hpp`)**:
  - Implemented locked `K16-Opt2c` `BoardKey` encode/decode (stackless branchless encode, Prefix-OR decode with defensive clamping, strict `isValidBoardKey`, sentinel `EMPTY_KEY`, 2-round 128-bit `wymix` uncorrelated shard/slot hashes) and exact 7×7 NES RNG (`NES_TRANSITION_NUM`, `NES_STATIONARY_PROB`, `Rng` splitmix64).
  - Implemented allocation-free `PlacementList` (`MAX_PLACEMENTS = 256`), `TransitionList`, padded `alignas(64)` `Board20` + 64-bit `PIECE_MASK_64`, SWAR `applyPlacement20`, `uint64_t seenTuck[20]`, 6-frame-cycle `findPlacements10Hz` (**`133 ns/call`** uncontended 1T — 3.5× faster than Phase 0 `461 ns/call`; `generateTransitions10Hz` = `1,101 ns/call`), and O(1) `predictAdjState10Hz`.
  - Built thin read-only `eval_adapter.hpp` bridging `Board20` ↔ `src/backend/` strictly for parity tests, Round 0 seeding, and baseline benchmarking.
- [x] **Fixed-seed benchmark CLI (`experimental/das_tablebase/benchmark_20row.cpp`)**:
  - Supports Stationary Level 19, Stationary Level 18, and Full NTSC **18 → 29** across `eval2ply`, production `precompute`, 0-frame `tbnn`, and exact 18-frame `tbnn-precompute` (`tbnn_precompute_adapter.hpp`).
  - Recorded production StackRabbit baselines (`seed=30000000` val, `seed=700000` held-out test, 10 Hz `"X....."`, `AGGRO`):
    - **`eval2ply` (0-frame)**:
      - `l19` (`100l` goal): Val (`500g`) **`244,031 ± 5,774`** (`10.168` normT, `71.63l`, `45.8%` goal) | Test (`1000g`) **`248,574 ± 4,080`** (`10.357` normT, `73.31l`, `48.2%` goal)
      - `l18` (`130l` goal): Val (`500g`) **`442,124 ± 6,889`** (`19.391` normT, `114.72l`, `75.2%` goal) | Test (`1000g`) **`460,804 ± 4,518`** (`20.211` normT, `118.31l`, `80.0%` goal)
      - `full18` (`230l` ks): Val (`500g`) **`640,950 ± 13,570`** (`162.64l`, `72.6%` trans, `27.6%` ks, `11.8%` maxout) | Test (`1000g`) **`666,482 ± 9,134`** (`168.53l`, `78.2%` trans, `27.0%` ks, `12.7%` maxout)
    - **Production `precompute` (`rt=18, das=1`, `seed=30000000` val)**:
      - `l19` `0×0 b=12` (`200g`): **`248,760 ± 10,496`** (`10.365` normT, `63.77l`, `36.0%` goal, `3.89 ms/mv`)
      - `l19` `50ms b=-2` (`50g`, `1T`, *uncalibrated — predates `calibratePrecomputeSpeed7T`, overshot to `264.0 ms/mv`*): **`265,680 ± 18,160`** (`11.070` normT, `75.38l`, `54.0%` goal, `50.8%` TRT, `avgMaxH=6.87`)
      - `l19` `49×2 b=20` (`100g`): **`343,228 ± 13,114`** (`14.301` normT, `82.91l`, `66.0%` goal, `13.62 ms/mv`)
      - `l19` `160×8 b=20` (`50g`): **`345,656 ± 18,217`** (`14.402` normT, `88.16l`, `80.0%` goal, `58.6%` TRT, `avgMaxH=6.87`, `125.96 ms/mv`)
      - `l19` **`200ms b=-2`** (`80g`, `1T`, calibrated `131.0 mv/ms` 7T): **`338,305 ± 12,534`** (`med=374,200`, `p90=440,040`, `max=483,600`, `14.096` normT, `90.49l`, `238.0` pcs, **`81.2%`** goal, `55.1%` TRT, `avgMaxH=6.66`, **`195.5 ms/mv`**)
      - `full18` `49×2 b=20` (`50g`, capped at `230l`): **`1,042,307 ± 29,312`** (`41.204` normT, `216.32l`, `98.0%` trans, `76.0%` ks, `74.0%` maxout, `71.5%` TRT, `9.34 ms/mv`)
      - `full18 → topout` **`200ms b=-2`** (`40g`, `1T`, `--max-lines=uncapped`, calibrated `131.8 mv/ms` 7T): **`1,074,642 ± 13,120`** (`med=1,078,730`, `p90=1,165,416`, `max=1,272,000`, **`scoreAt230=1,071,786`**, `42.293` normT, `231.60l`, `599.3` pcs, **`100.0%`** trans, **`95.0%`** ks, **`82.5%`** maxout, `67.8%` TRT, `avgMaxH=7.15`, **`214.3 ms/mv`**)
- [x] **Verification & Review (Logic + Simplicity + Perf/Scale)**:
  - `das_tb_test`: `6/6` Phase 1 suites (`11,415,688` assertions): 5,000 random round-trips + 9 hostile-key invariants, uncorrelated hash test (incl. `k.hi == 0`), exact NES RNG & `getLevelAfterLineClears` parity, 10,500+ live & random board move-gen/tuck/apply/adj parity across all 3 gravities (`1, 2, 3`) + `c==8` vertical I-tuck, precompute adapter + topout test, and `1T == 4T` determinism.
  - Compiled `-O3 -Wall -Wextra -Werror` and `-DNDEBUG -fsyntax-only`; addressed all findings from all 3 subagent reviewers (`logic_reviewer`, `readability_reviewer`, `perf_scale_reviewer`).

---

### Phase 2: Stationary Level 19 Flywheel (Sampler, Soft-Boundary MPI, NNUE, Inspector & RT Check)
Prove the full 4-pillar flywheel on Stationary Level 19 (`gravity = 2`, 10 Hz) and verify 18-frame reaction-time calibration (`V(b, c)` vs. `V_RT(b, c)`).

- [x] **Binary formats & shared I/O (`tablebase_io_20row.hpp`)**:
  - Implemented `SRSTV002` (32 B header), `SRTBV003` (80 B header + 44 B `PackedDasRecord`), `SRNNV001` (`featureVersion = 2`, transactional load), shared `PersistentWorkerPool`, and cache-line-aligned 6-slot AoS `FlatBoardIndex` with Lemire `fastrange` (`32.8 B/board` → `1.64 GB` at `50M`, single cache-line fetch on hits).
  - Enforced strict I/O guards: `uint64_t`/`size_t` overflow checks, pre-truncation validation in `writeTablebaseV3`, transactional `MappedDasTablebase` & `IndexedDasTablebase` (zero temp `keys` vector), `adviseDontNeed()` on warm-start, and `isSameFilePath` aliasing guards across all CLIs.
- [x] **State sampler (`state_gen_20row.hpp`, `generate_20row.cpp`)**:
  - Implemented `ShardedVisitCounter20` (64 `alignas(128)` shards, 70% `FlatVisitMap20`, hoisted `ShardBuckets`), `expandCoreSiblings20` (geometric dedup watermark + pairwise `set_union`), exact-count 3-way `mergeFilterAndCap20`, `visits`/`nn-error` eviction, `protectedCore`, 50/50 exploration split with lazy `nn->evalPiece`, and Round 0 `--seed-eval`.
- [x] **Soft-Boundary MPI solver (`solver_20row.hpp`, `solve_20row.cpp`)**:
  - Implemented disk-streamed `PolicyActionStore20` (auto-unlinked 64-bit `pread`/`pwrite` above `6M` boards), exact `double` `SoftBoundaryCache20` (`65K` slots, freed after iter 1), iter-1 `bestOutStore` (`~20–30×` faster iters 2+), zero-sentinel `vCur`/`vNext`, `PersistentWorkerPool`, adaptive inner early-stop, warm-start `--init`, `maxValueDivergenceGuard`, terminal overflow abort, and `MappedDasTablebase` Soft-MC (`<= 3.5σ` on every solve).
  - Ran `250k`-board micro-benchmark before `5M` runs (`7.86s` Hard MPI, `0.71s/outer`, `z=0.42`; `21.1s` Soft iter-1 → `0.61–0.79s` iters 2–7, `z=0.01`; `0.08–0.11s/epoch` NNUE).
- [x] **20-Row Multi-Task NNUE (`nnue_20row.hpp`, `train_nn_20row.cpp`)**:
  - Implemented `featureVersion = 2` (`NN_INPUT_DIM = 272`, fused single bit-scan), specialized branchless `(128, 64)` & `(256, 64)` Layer 2 NEON (`alignas(16)` `< 2 KB` stack), top-3-rows fast path in `NnSoftBoundaryEval20`, cached `(x, a1, a2)` + branchless backprop + parallel Adam & validation in `trainTinyDasNet` (`~3.5s/epoch` for `300k` decisions `128×64`!), 4-part multi-task loss, `70/30` on-policy sampler, and Calibrated Partial-OUT (`1/2/3`-ply Top-K).
- [x] **20×10 Browser Inspector (`inspector_routes.hpp`, `inspector_server.cpp`, `inspector.html`)**:
  - Built and live-verified zero-dep POSIX HTTP server (`/`, `/ping`, `/state-info`, `/moves`, `/step-disagreement`, transactional `/reload`) with 20×10 interactive canvas, `V_tb` vs `V_nn` error table, `eval.cpp` comparison, autoplay, and monotonic sequence / `autoplayRunId` race guards.
- [x] **Execute Stationary Level 19 `5M` Flywheel (Rounds 0–5, `seed=30000000` val `500g`, 0-frame `tbnn`)**:
  - **Round 0** (`eval.cpp` seed, `5.0M` Hard MPI): `E[V(0)] = 2.557` (`z=0.73`), `r0_128x64` (`MAE=0.228`, `top1=78.6%`) → Hybrid **`155,505 ± 5,462`** (`13.2%` goal, `82.6%` hit).
  - **Round 1** (`eval.cpp` dropped; `5.0M` Soft MPI `m=0.6`): `E[V(0)] = 3.497` (`z=0.06`), `r1_128x64` (`MAE=0.202`, `top1=80.5%`) → Hybrid **`223,825 ± 5,996`** (`35.4%` goal), NN-only **`241,489 ± 6,295`** (`47.2%` goal).
  - **Round 2** (`518k` Soft MPI `m=0.45`, `eps=0.14` high-churn): `E[V(0)] = 3.547` (`z=0.79`), `r2_128x64` (`MAE=0.108`, `top1=82.1%`) → Hybrid **`292,943 ± 4,330`** (`74.6%` goal), NN-only **`295,141 ± 4,490`** (`77.0%` goal).
  - **Round 3** (`2.75M` `nn-error` `eps=0.05`, Soft MPI `m=0.25`): `E[V(0)] = 5.115` (`z=0.29`), `r3_128x64` (`MAE=0.176`, `top1=81.4%`) → Hybrid **`327,724 ± 3,833`** (`84.2%` goal, `90.5%` hit), NN-only **`329,159 ± 3,465`** (`88.4%` goal).
  - **Round 4** (`3.51M` `nn-error` `eps=0.08`, Soft MPI `m=0.25`): `E[V(0)] = 6.581` (`z=0.34`), `r4_128x64` (`MAE=0.185`, `top1=80.6%`) → Hybrid **`347,786 ± 3,323`** (`92.0%` goal, `94.2%` hit), NN-only **`339,474 ± 3,360`** (`90.0%` goal).
  - **Round 5** (full **`5.0M`** `nn-error` `eps=0.10`, Soft MPI `m=0.25`): `E[V(0)] = 6.950` (`z=0.33`), `r5_128x64` (`MAE=0.152`, `top1=82.0%`, `regret=0.0347`) → Hybrid **`356,715 ± 3,049`** (`14.863` normT, **`93.4%` goal**, `53.3%` TRT, **`94.0%` TB hit**, **`0.214 ms/mv`**); NN-only **`346,682 ± 3,095`** (`92.2%` goal); cold 30-ep `r5_256x64` (`339 KB`, `MAE=0.162`, `top1=81.2%`) → Hybrid `349,727 ± 3,445` (`m=0.50`, `90.0%` goal).
  - **Ply-2 TB Re-Entry (`OUT → TB2`)**: Added `detail::bestReplyHybrid2Ply20` in `nnue_20row.hpp`, `tbnn_precompute_adapter.hpp`, and `inspector_routes.hpp` (plus Undo/Redo, visual `CUR`/`NEXT` boxes, and dual hover previews in `inspector.html`). On `r5.tb.bin + r5_128x64`: 0-frame jumped **`356,715 → 361,994 ± 2,877`** (`+5,279`, `94.0%` goal, `54.0%` TRT, **`0.102 ms/mv`, `2.1×` faster**); 18f DAS jumped **`305,110 → 313,338 ± 5,816`** (`+8,228`, `81.0%` goal, `47.6%` TRT, **`2.53 ms/mv`**).
- [x] **6-Way Reaction-Time & DAS Effect 1 vs. Effect 2 Diagnostic (`seed=30000000`, `200g` `tbnn-precompute` on `r5.tb.bin + r5_128x64`, `m=0.40`, `ply=2`, `OUT→TB2`)**:
  - `rt=0, das=0`: **`361,724 ± 4,394`** (`96.0%` goal, `53.5%` TRT) — 0-frame baseline.
  - `rt=18, das=0`: **`340,354 ± 5,295`** (`88.5%` goal, `51.1%` TRT) → pure 18f RT cost = **`-21,370` (`44%`)**.
  - `rt=18, das=1` (`reset16`, penalties OFF): **`331,284 ± 5,436`** (`85.5%` goal, `50.0%` TRT) → true **Effect 1** (mid-air DAS reversal) = **`-9,070` (`19%`)** (`reset16` with penalties ON = `315,438`, confounded by phantom penalty aversion).
  - `rt=18, das=1` (live DAS, penalties ON): **`313,338 ± 5,816`** (`81.0%` goal, `47.6%` TRT, `afterDAS: 79.2% full / 11.7% part / 9.1% min`) → true **Effect 2** (cross-piece DAS carryover) = **`-17,946` (`37%`)**.
  - `rt=18, das=1` (live DAS, penalties OFF): **`215,402 ± 8,277`** (`43.5%` goal, `42.8%` TRT) → runtime DAS penalties save **`+97,936`** points.
- [x] **Per-Line Discounting (`DiscountMode::PerLine`), Danger-Zone Sampling & `!fitsKey → 0.0` Guard**:
  - Added `DiscountMode::PerLine` (`TB_FLAG_DISCOUNT_PER_LINE`), danger-zone NNUE sampling (`12%` uniform `ε=0.35`, `hasRankingSignal` pure-terminal supervision), and the permanent `!fitsKey → 0.0` Soft-Boundary guard (eliminating uncorrectable `>= 10`-hole OOD reward hacking).
  - **`perline98_r1` (`5.0M` `r5.states.bin`, `γ=0.98 [line]`, warm-started from `r5`, `m=0.30`)**: converged in 11 iters (`413s`), `E[V(0)] = 7.0424`, `maxV = 7.8612` (0-hole `H=11`), Soft-MC `z=0.70`, NNUE **`MAE=0.0990 / top1=83.45% / regret=0.0204`** →
    - 0-frame Hybrid (`500g`, `m=0.25`): **`391,326 ± 3,268`** (**`+29,332`** over `r5`, **`60.8%` TRT**, `90.2%` goal; `m=0.40` = `389,862`, `91.8%` goal, `max=500.0k`).
    - 0-frame Standalone `170 KB` NNUE (`200g`): **`364,626 ± 6,940`** (`86.5%` goal, `58.6%` TRT — beats `r5` Hybrid).
    - 18f Live DAS (`200g`, `rt=18, das=1, m=0.40`): **`323,186 ± 7,840`** (`+9,848` over `r5`, **`54.2%` TRT**, `74.5%` goal, `3.11 ms/mv`).
- [x] **Verification & 3-Reviewer Passes (Phase 2 + Post-Phase-2 Diff)**:
  - Phase 2 + Post-Phase-2 3-reviewer subagent passes (**Logic**, **Perf & Scale**, **Readability**) completed and all HIGH/MEDIUM findings fixed (including `valDs` bijective interleaving `(g*37)%100`, `PerLine` 3-ply & inspector routes, zero-alloc `FlatSecondMoveCache`, DAS penalty grounded-margin parity, duplicate `"ply2Move"` JSON removal, and `2×` Soft-Boundary disk threshold).
  - `das_tb_test`: `13/13` suites (`11,424,479 / 11,424,479` assertions), `format_cpp`, `-O3 -Wall -Wextra -Werror`, `-DNDEBUG -fsyntax-only`, `git diff --stat -- src/` & `experimental/tablebase/` empty.

---

## Horizon 1 (Active Focus): Phase 2B — Pure Stationary Level 19 Next-Box Stacking (`rt = 0`, no DAS)

*Goal:* Iterate deeply in our fastest, cleanest sandbox (`~5–8 min/round`, pure 0-frame 10 Hz Level 19 Next-Box stacking) until 0-frame L19 stacking is genuinely elite (target: **`>= 410k+` mean score**, **`60–65%` TRT**, **`>= 93–95%` 100-line survival**, clean `H = 14..18` NNUE extrapolation, and strong standalone NNUE) **before** multiplying complexity for DAS/RT or multi-level backward induction. Every code change in Phase 2B requires a parallel 3-reviewer subagent pass per `AGENTS.md`.

### Brainstormed Pure L19 NB Stacking Levers (Test Independently)
- [x] **2B.1 — On-Policy `PerLine γ = 0.98` Flywheel (`5.0M`, `--merge`, Soft-Boundary `m = 0.25`)**:
  - **`perline98_r2`** (`5.0M`, `E[V(0)] = 6.915`, `z = 1.27`, `maxV = 7.68` 0-hole `H = 10`, `128×64` `MAE = 0.0793 / top1 = 83.82% / regret = 0.0152`):
    - 0-frame Hybrid (`500g`, `m = 0.25`): **`407,186 ± 2,971`** (**`+45.2k`** over `r5`!, **`62.6%` TRT**, **`94.6%` goal**, `med = 418.4k`, `max = 523.6k`, `90.5%` hit, `0.132 ms/mv`); `m = 0.40` = **`405,430 ± 2,918`** (**`95.8%` goal**, `62.1%` TRT).
    - 0-frame Standalone `170 KB` NNUE (`200g`): **`394,208 ± 5,473`** (`92.0%` goal, `61.2%` TRT).
    - 18f Live DAS (`200g`, `rt = 18, das = 1, m = 0.40`): **`346,320 ± 7,049`** (`82.0%` goal, `55.1%` TRT, `3.21 ms/mv`).
  - **`perline98_r3`** (`5.0M`, `E[V(0)] = 6.854`, `z = 0.57`, `maxV = 7.62` 0-hole `H = 10`, `128×64` **`MAE = 0.0735 / top1 = 83.45% / regret = 0.0157`**):
    - 0-frame Hybrid (`500g`): `398,461 ± 3,665` (`m = 0.25`, `92.4%` goal, `62.4%` TRT; `m = 0.40` = `398,507`) — slight `-8.7k` dip vs. `r2` coincides with `H = 14..18` / 17-hole `V_nn` drifting positive (`+1.36` / `+2.37`).
    - 0-frame Standalone `170 KB` NNUE (`200g`): **`396,818 ± 3,766`** (**`95.0%` goal**, `60.0%` TRT, `med = 401.4k`).
    - 18f Live DAS (`200g`, `rt = 18, das = 1, m = 0.40`): **`346,670 ± 7,796`** (**`82.0%` goal**, **`56.9%` TRT**, `med = 379.0k`, `p90 = 438.5k`, `max = 485.6k`, `3.07 ms/mv`) — **already beats production StackRabbit `200 ms b = -2` (`338,305`, `81.2%` goal, `55.1%` TRT, `195.5 ms/mv`) on every metric at `64×` faster speed, before `V_RT`!**
- [x] **2B.2 — Per-Line Horizon Sweep (`γ ∈ {0.975, 0.98, 0.985}`) & Tighter Boundary Margin (`m = 0.12`)**:
  - **`γ = 0.985`** (`perline985_r1`, `m = 0.20`): `E[V(0)] = 7.419` (`z = 1.07`), `MAE = 0.1091 / top1 = 83.11%` → 0-frame **`378,911 ± 3,142`** (`92.2%` goal, `57.7%` TRT, `92.3%` hit), NN-only `365,334` (`92.0%` goal).
  - **`γ = 0.980, m = 0.12`** (`perline98_m12`, vs. `r2`'s `m = 0.25`): `E[V(0)] = 6.954` (`+0.039`, zero inflation, `z = 0.41`, `steps = 45.8`), **`MAE = 0.0715 / top1 = 83.54%`** → 0-frame **`406,282 ± 3,326`** (`med = 420.2k`, **`63.2%` TRT**, `93.2%` goal; `m = 0.40` = `404,576`, `95.0%` goal), NN-only `393,266`. Statistically tied with `r2` (`407.2k`), confirming `m = 0.12` is completely stable.
  - **`γ = 0.975, m = 0.12`** (`perline975_m12`, Round 1 on `perline98_r2.states.bin`): `E[V(0)] = 6.761` (`z = 1.31`, `steps = 18.4`), `MAE = 0.0557 / top1 = 84.46% / regret = 0.0111` → 0-frame Hybrid **`416,939 ± 3,410`** (`med = 437.2k`, `65.4%` TRT, `92.4%` goal, `86.0%` hit), Standalone NNUE `408,228 ± 6,096` (`64.6%` TRT, `90.0%` goal).
  - **`γ = 0.975, m = 0.15` self-consistent on-policy rounds (`5.0M` `--merge`)**:
    - **`perline975_r2`**: `E[V(0)] = 6.638` (`z = 0.14`, `steps = 22.0`, `maxV = 7.206` 0-hole `H = 10`), `128×64` **`MAE = 0.0493 / top1 = 84.33% / regret = 0.0100`** → 0-frame Hybrid `424,270 ± 3,222` (`m = 0.25`, `93.2%` goal, `66.4%` TRT; `m = 0.40` = `420,789`, `93.8%` goal), Standalone `170 KB` NNUE **`435,584 ± 4,134`** (**`97.5%` goal**, **`67.2%` TRT**, `med = 442.2k`, `max = 540.4k`, `0.195 ms/mv`)!
    - **`perline975_r3`**: `E[V(0)] = 6.540` (`z = 0.36`, `steps = 27.2`, `maxV = 7.118` 0-hole `H = 10`), `128×64` **`MAE = 0.0461 / top1 = 84.22% / regret = 0.0094`** →
      - 0-frame Hybrid (`500g`, `m = 0.15`): **`433,578 ± 3,185`** (**`+71.6k`** over `r5`!, **`+26.4k`** over `perline98_r2`!, `med = 444.0k`, `p90 = 489.1k`, `max = 524.0k`, **`95.0%` goal**, **`68.1%` TRT**, `hit = 86.9%`, `0.192 ms/mv`); `m = 0.25` = `427,638` (`93.2%` goal, `67.5%` TRT, `max = 544.0k`); `m = 0.40` = `425,262` (`94.2%` goal, `66.6%` TRT).
      - 0-frame Standalone `170 KB` NNUE (`200g`): **`426,154 ± 4,938`** (`95.0%` goal, `66.1%` TRT, `max = 544.4k`, `0.210 ms/mv`).
      - 18f Live DAS (`200g`, `rt = 18, das = 1, m = 0.40`): **`357,554 ± 7,996`** (**`+19.2k` over production `200 ms b = -2`'s `338.3k`**, `med = 385.6k`, `p90 = 460.4k`, `max = 521.2k`, `78.0%` goal, **`59.2%` TRT**, `4.10 ms/mv`, `48×` faster — still before `V_RT`!).
    - **Runtime `m` sweep & `500g` Standalone NNUE on `perline975_r3` (`seed = 30,000,000`)**:
      - 0-frame Hybrid (`500g`): `m = 0.00` (`434,428`, `93.0%` goal, `68.6%` TRT) | `m = 0.05` (`429,396`, `92.2%`) | **`m = 0.10` (`435,329 ± 3,219`, `med = 445.0k`, `p90 = 497.2k`, `max = 560.8k`, `94.4%` goal, `68.3%` TRT, `0.130 ms/mv`)** | **`m = 0.15` (`433,578 ± 3,185`, `95.0%` goal, `68.1%` TRT)** | `m = 0.25` (`427,638`, `93.2%`) | `m = 0.40` (`425,262`, `94.2%`). Sweet spot = `m = 0.10–0.15` (`~2–3 × valMAE`).
      - 0-frame Standalone `170 KB` NNUE (`500g`, apples-to-apples): `perline975_r2` = `426,528 ± 3,070` (`93.8%` goal, `66.5%` TRT); `perline975_r3` = `423,158 ± 3,488` (`92.2%` goal, `66.6%` TRT) — Hybrid (`435.3k`) cleanly leads Standalone NNUE by **`+8.8k to +12.2k` (`~2.8–3.5 SE`)** once runtime `m` is calibrated.
      - 18f Live DAS (`200g`, `rt = 18, das = 1`): `m = 0.15` (`357,412 ± 8,910`, `76.0%` goal, `61.0%` TRT) | **`m = 0.25` (`363,516 ± 7,920`, `+25.2k` over production `200 ms b = -2`, `med = 400.4k`, `77.0%` goal, `60.5%` TRT, `3.48 ms/mv`, `56×` faster!)** | `m = 0.40` (`357,554`, `78.0%` goal, `59.2%` TRT).
    - **`γ = 0.970, m = 0.15` upper-aggression bracket (`perline97_r1` & `perline97_r2`, `5.0M` `--merge`)**:
      - `perline97_r1`: `E[V(0)] = 6.363` (`z = 1.56`, `steps = 14.6`), `MAE = 0.0369 / top1 = 85.01% / regret = 0.0072` → 0-frame `433,310 ± 4,111` (`m = 0.10`, `92.4%` goal, `69.6%` TRT), NN-only `424,622` (`88.0%` goal).
      - `perline97_r2`: `E[V(0)] = 6.212` (`z = 0.61`, `steps = 16.6`, `maxV = 6.648` 0-hole `H = 10`), **`MAE = 0.0329 / top1 = 85.78% / regret = 0.0063`** → 0-frame Hybrid **`436,383 ± 4,531`** (`m = 0.05`, `med = 461.6k`, `p90 = 518.4k`, `max = 561.2k`, **`71.5%` TRT**, but survival drops to **`88.6%` goal**, `avgMaxH = 7.08`, `hit = 77.0%`; `m = 0.10` = `432,473`, `88.8%`; `m = 0.15` = `428,670`, `88.4%`; `m = 0.25` = `413,333`, `85.2%`), Standalone NNUE `428,430 ± 4,576` (`88.6%` goal, `69.7%` TRT), 18f Live DAS `361,432 ± 8,810` (`m = 0.15`, `74.0%` goal, `62.5%` TRT; `m = 0.40` = `340,928`, `68.5%` goal).
    - **Locked Horizon Finding:** `γ` curve is completely bracketed (`0.985 → 0.980 → 0.975 → 0.970`). **`γ = 0.975`** is the clear Pareto sweet spot (`435.3k` / `94.4%–95.0%` goal / `68.3%` TRT 0-frame, `363.5k` 18f DAS): pushing to `γ = 0.970` buys `+3.2%` TRT (`med = 461.6k`) at the cost of `-6%` 0-frame survival (`95% → 88.6%`), `-10%` TB hit (`86.9% → 77.0%`), and worse 18f DAS (`361.4k`, `74.0%` goal).
  - *Toolbox note — **1-Time Boundary Jumpstart** (for high-`γ` or new-regime cold starts):* Because `γ` sets both the expected horizon `1 / (1 − γ)` and risk tolerance (higher `γ` like `0.99` raises `V_∞ ≈ 17` and intentionally plays *safer*, cutting risk tolerance in half vs. `γ = 0.98`'s 50-line midpoint), solving high-`γ` regimes from a low initial `V(0)` normally takes 5–10 slow Soft-Boundary rounds. Scaling `--boundary-nn` up **once on Round 1 only** (never repeatedly, to avoid divergence) so `V_nn(0)` starts near expected `V_∞` — then running normal unscaled Rounds 2+ — skips the slow warm-up cleanly. Most relevant for **Phase 4 Stationary Level 18** (`130 + 100` remaining lines → `γ ≈ 0.99` is naturally appropriate) or if we ever test `γ >= 0.985` multi-round on L19. Not needed in Phase 5 finite-horizon backward induction (`γ = 1.0`).
- [x] **2B.3 — `5M` State-Set & Soft-Boundary Solver Tuning**:
  - Validated tighter `--boundary-margin=0.12..0.15` across `PerLine γ ∈ {0.970, 0.975, 0.980}` (`Soft-MC z = 0.14–0.61`, zero inflation; locked **`m = 0.15`** for solver and **`m = 0.10–0.15`** for 0-frame runtime).
  - **Skipped `--boundary-ply=2` during training (deliberate decision):** Expanding all 7 next pieces × ~22 moves inside `NnSoftBoundaryEval20` costs `~150×` more NNUE evals on outer iter 1, while `128×64_cal` already has `valMAE = 0.0448` (`regret = 0.0085`). Put compute into runtime selective 3-ply (2B.5) and `15M–20M` TB capacity (2B.7) instead.
- [x] **2B.4 — NNUE Architecture, Features & High-Stack Calibration**:
  - [x] Cold-start `256×64` sanity check on `perline98_r2.tb.bin` (`35` epochs, `--danger-frac=0.18 --danger-eps=0.40`): `H = 12..18` cleanly turned negative (`V_nn(H=14, T) = -1.11`), **but** cold 35-epoch `256×64` severely underfit (`MAE = 0.0924 / top1 = 80.65% / regret = 0.0227` vs. 160-epoch warm-started `128×64` at `0.0793 / 83.82% / 0.0152`) → dropped to `250.8k` Hybrid (`158.7k` NN-only).
  - [x] **Wave 1 on `perline975_r3.tb.bin` (`600k` decisions, `seed = 30,000,000`)**:
    - **Exp A — Calibrated warm-started `128×64`** (`perline975_r3_128x64_cal.nn.bin`, `danger=0.20/0.40`, `30` ep, `110s`, `170 KB`): **`MAE = 0.0448 / top1 = 85.06% / regret = 0.0085`** (down from `0.0461 / 84.22% / 0.0094`), flattened `H = 14/16/18 V_nn(T)` to `0.49 / 0.30 / 0.23` → new best 0-frame Hybrid **`436,288 ± 2,908`** (`m = 0.10`, **`95.2%` goal**, `67.9%` TRT, `0.134 ms/mv`), lifted `500g` Standalone NNUE by **`+6.2k`** to **`429,338 ± 3,075`** (`93.8%` goal, `66.9%` TRT, `0.147 ms/mv`), and 18f Live DAS `358,732 ± 8,361` (`78.5%` goal, `2.87 ms/mv`).
    - **Exp B — Full-budget `256×64`** (`perline975_r3_256x64.nn.bin`, `danger=0.18/0.40`, `80` ep cold, `591s`, `339 KB`): `MAE = 0.0455 / top1 = 85.20% / regret = 0.0090`, cleanest OOD extrapolation yet (`H = 14/16/18 V_nn(T) = +0.14 / -0.11 / -0.09`, `17-hole = 0.88 / 0.82 / 0.73`), lifted 18f Live DAS survival to **`80.0%`** (`361,778 ± 7,957`, `5.42 ms/mv`), while 0-frame Hybrid (`431,766 ± 3,514`, `94.2%` goal, `0.298 ms/mv`) and Standalone (`426,282 ± 3,573`, `93.2%`) essentially tied `128×64` at `2.2×` slower inference.
    - *Takeaway:* `128×64` is NOT strongly capacity-bottlenecked at `5M` (`regret = 0.0085` ≈ `0.13%` of `V(0)`!), keeps inference `2.2×` faster (`0.134 ms/mv`, `170 KB`), and `600k` decisions + `danger = 0.20 / 0.40` cleanly improved both Hybrid (`436.3k`, `95.2%` goal) and Standalone NNUE (`+6.2k → 429.3k`). Keep `128×64` as default fast engine; reserve `256×64` for final/offline models if needed (especially if `V_RT`'s 21 outputs need wider capacity).
- [x] **2B.5 — 0-Frame Runtime Search & Margin Tuning**:
  - [x] `out-margin` sweep on `perline975_r3` (`0.00..0.40`): peak at `m = 0.10–0.15` (`436.3k`, `95.2%` goal on `128x64_cal`).
  - [x] Unconditional `--out-ply=3` (`500g`, `perline975_r3 + 128x64_cal`, `m = 0.10`): `438,555 ± 2,213` (`98.2%` goal, `3.87 ms/mv` — skipped `OUT → TB2` re-entry).
  - [x] **Selective runtime 3-ply on top of `OUT → TB2`** (`OutPly3Trigger::AllOut` and `Risk`, `bestTbOnly` floor, `bestTbGroundedQ` prescreen, `rt == 0` 7× skip, strict CLI validation; **3-reviewer sign-off: Logic, Perf & Scale, Readability** all HIGH/MEDIUM findings fixed; `das_tb_test` `14/14` / `11,424,542` assertions, `cpp_test_unit` `80/80`, `precompute_static_lint` `0`):
    - **0-frame `allout` (`500g`, `m = 0.10`)**: **`442,006 ± 2,493`** (`med = 446.0k`, `p90 = 496.8k`, `max = 560.0k`, `lines = 99.99`, **`97.6%` goal**, **`68.3%` TRT**, `ply3 = 2.91%`, **`0.214 ms/mv`** — `18×` faster than unconditional `ply=3` and `+5.7k` over `ply=2`!).
    - **0-frame `risk (H>=10, holes>=2)` (`500g`, `m = 0.10`)**: **`443,303 ± 2,330`** (`med = 446.6k`, `p90 = 496.0k`, `max = 560.0k`, `lines = 100.15`, **`97.8%` goal**, **`68.4%` TRT**, `ply3 = 14.13%`, **`0.567 ms/mv`** — `6.8×` faster than unconditional `ply=3`!).
    - **18f Live DAS `allout` (`200g`, `rt = 18, das = 1, m = 0.25`)**: **`380,972 ± 6,379`** (**`+22.2k` over `ply=2`** on `128x64_cal`, **`+42.7k` ahead of production `200 ms b = -2`'s `338.3k`**, **`85.0%` goal**, **`60.6%` TRT**, `4.89 ms/mv`, `40×` faster — still before `V_RT`!).
- [x] **2B.6 — Automated Topout & Disagreement Audit (`scratch/audit_topouts.cpp`)**:
  - Audited all `24 / 500` (`4.8%`) 0-frame topouts of `perline975_r3 + 128x64_cal` (`m = 0.10`, `ply = 2`) across two automated passes (final 10 moves + exact TB-exit moment):
    - **`24 / 24` (`100%`)** of topouts occurred after protracted `16–76`-move digouts outside the TB (avg **`32.6` moves** since last TB hit; `10/10` final moves `allOut`; `24/24` touched `!fitsKey`).
    - **`24 / 24` (`100%`)** of the **exact TB-exit boards still fit `K16-Opt2c` (`<= 9` holes)** — mostly clean `0`-hole `H = 8..11` stacks (`13/24`) and `1–7`-hole covered right-well overhangs missing from the `5M` cap (`17/24` at `H >= 9`, `7/24` forced `allOut`).
    - **`24 / 24` (`100%`)** had `ply=3` disagreements during the fatal digout (directly explaining why `--out-ply=3` cut topouts `24 → 9`, `95.2% → 98.2%`!).
    - *Direct implication:* Both **2B.7 capacity scaling (`5M → 15M–20M`)** (pulling `H = 8..12` & `1–7`-hole overhang boards into the TB) and **selective 3-ply digout search** directly attack the exact root cause of virtually all remaining 0-frame topouts.
- [x] **2B.7 — Capacity Scaling (`5M → 20M`)**:
  - Landed 20M-scale I/O, cache & dedup optimizations with full **3-reviewer sign-off (Logic, Perf & Scale, Readability)** (`tablebase_io_20row.hpp` single-pass `IndexedDasTablebase::buildFromMapped`, private `FlatBoardIndex::beginBuild`/`insertBuiltKey` with `buildReservedCount_` & `INVALID_BOARD_ID` guards, `MADV_SEQUENTIAL`/`MADV_RANDOM`/`MADV_DONTNEED` across open/warm-start/key-load/Soft-MC; `solver_20row.hpp` `262K` `SoftBoundaryCache20` + power-of-2 `static_assert`; `state_gen_20row.hpp` geometric sibling dedup watermark).
  - **`perline975_20m` (`20,000,000` boards, `PerLine γ = 0.975`, `m = 0.15`, `seed = 30,000,000`)**:
    - `generate_20row` (`500k` games, `45.35M` moves, `19.76M` raw unique, `1.38M` siblings, `--merge=perline975_r3.states.bin`, `2M` core): **`20,000,000` boards (`305 MB`) in `235s`** (`< 2.8 GB` RAM).
    - `solve_20row` (warm-started from `5M` `perline975_r3.tb.bin`, `disk` cache, `3.4 GB` RAM): converged in `17` outer (`457` inner), **`E[V(0)] = 6.7896`** (`+0.249` over `5M`!), `maxV = 7.3295` (0-hole `H = 10`), `residual = 5.81e-07`, **Soft-MC `z = 1.71`, `steps = 108.8` (`4×` longer in-TB runs than `5M`'s `27.2`!)**.
    - `train_nn_20row` (`128×64`, `600k` dec, `30` ep warm-start, `151s`): `valMAE = 0.0618 / top1 = 83.58% / regret = 0.0127`.
    - **0-frame Hybrid `ply = 2` (`500g`, `m = 0.10`)**: `439,905 ± 3,603` (`med = 459.6k`, `93.2%` goal, `69.8%` TRT, **`93.77%` hit**, `0.134 ms/mv`).
    - **0-frame Hybrid `ply = 3[allout]` (`500g`, `m = 0.10`)**: **`449,257 ± 2,626`** (`+7.3k` vs. `5M`, `med = 459.4k`, `p90 = 500.4k`, **`97.4%` goal**, **`69.9%` TRT**, **`94.33%` hit**, `ply3 = 1.59%`, **`0.204 ms/mv`** — faster than `5M`!).
    - **0-frame Hybrid `ply = 3[risk, h>=10, holes>=2]` (`500g`, `m = 0.10`)**: **`450,345 ± 2,676`** (`+7.0k` vs. `5M`, `med = 459.8k`, `p90 = 501.6k`, **`97.4%` goal**, **`70.2%` TRT**, **`94.57%` hit**, `ply3 = 13.58%`, **`0.470 ms/mv`** — faster than `5M`!).
    - **0-frame Standalone `170 KB` NNUE (`500g`)**: `420,874 ± 3,944` (`91.2%` goal, `67.0%` TRT — `20M` TB covers `4×` more complex states; a longer/wider NNUE schedule could narrow the gap if standalone WASM is needed).
    - **18f Live DAS `ply = 2` (`200g`, `m = 0.25`)**: `385,314 ± 7,321` (`+26.6k` vs. `5M` `ply=2`, `82.5%` goal, `63.3%` TRT, `90.89%` hit).
    - **18f Live DAS `ply = 3[allout]` (`200g`, `m = 0.25`)**: **`395,360 ± 6,769`** (**`+14.4k` vs. `5M`**, **`+57.1k` ahead of production `200 ms b = -2`'s `338.3k`!!**, **`med = 421.2k`**, **`90.0%` goal** vs. `81.2%` production, **`63.3%` TRT**, **`91.58%` hit**, `ply3 = 5.09%`, **`6.14 ms/mv`** — `32×` faster than `200 ms`, still before `V_RT`!).
- [x] **2B Exit Gate & 3-Reviewer Sign-Off**:
  - Crushed every Phase 2B target (`450.3k` vs. `>= 410k+`, `70.2%` vs. `60–65%` TRT, `97.4%` vs. `>= 93–95%` goal, `395.4k` / `90.0%` goal on live 18f DAS), all tests pass (`das_tb_test` `14/14` / `11,424,542` assertions, `cpp_test_unit` `80/80`, `precompute_static_lint` `0`, `-Wall -Wextra -Werror`, `-DNDEBUG`), and 3-reviewer sign-offs recorded for selective 3-ply and 20M-scale prep. Ready for **Horizon 2 (Phase 3 — 3-Bucket `V_RT(b, c, d)`)**!

---

## Horizon 2 (Active Planning — Ready to Build): Phase 3 — Live 18-Frame Reaction Time & DAS Integration (Stationary L19)

*Goal:* Close the remaining **`-55k` gap** between our 0-frame `20M` ceiling (`450.3k`, `97.4%` goal) and live 18-frame DAS (`395.4k`, `90.0%` goal, `6.14 ms/mv`), targeting **`~415k–430k+`** (`93–96%` goal) at **`~1–3 ms/mv`** (already `+57.1k` ahead of production `200 ms b = -2`'s `338.3k`, and eliminating `~70%` of runtime latency by baking DAS carryover into `V_RT`). Every code milestone requires a parallel 3-reviewer pass per `AGENTS.md`.

### Locked Phase 3 Design Decisions
- **Exact RT + DAS Bellman Equation (`PerLine γ = 0.975`, `m = 0.15`):**
  $$V_{\text{RT}}(b, c, d) = \max_{\phi \in \Phi(b, c, d)} \sum_{n=0}^{6} P(n \mid c) \max_{a \in \text{Adj}(b, c, \phi)} \Big[ r(\text{lc}_a) + \delta(\text{lc}_a) \cdot V_{\text{RT}}(b'_a, n, d'_a) \Big]$$
- **DAS Charge Buckets (Parameterized `numDasBuckets ∈ {2, 3}`):**
  - **3-bucket (`21` floats/board):** `d ∈ {FULL = 15, PART = 10, MIN = 5}` with exact StackRabbit thresholds (`>= 15 → 15`, `10..14 → 10`, `< 10 → 5`).
  - **2-bucket ablation (`14` floats/board):** `d ∈ {FULL = 15, LOW = 5}` (`>= 15 → 15`, `< 15 → 5`, where `< 15` acts as `5`).
- **Fast Bitboard Move/Phantom Architecture (`das_rt_move_gen.hpp`):**
  - **Unified Human Input Limit (Independent Thumbs, 7.5 Hz):**
    - **D-pad thumb:** `>= 7` frames between any two D-pad button-down events (`>= 8f` period; bans quick `L → R` reversals; holding through ARE counts as an ARE button-down → `dpadCooldown = 0` at spawn; continuous DAS hold is one button-down).
    - **Rotate thumb (independent):** `>= 7` frames between any two `A`/`B` button-down events (`>= 8f` period; bans quick `A → B`). Replaces legacy `arrFrameIndex`/`isArrContinued` grid tracking with two tiny `0..7` cooldown counters `(dpadCooldown, rotCooldown)` carried across frame 18.
  - **Phase A — Precomputed `4 × uint64_t` (200-bit) Swept-Path Tracks (4 bitwise ANDs each):**
    - **First half (frames `0..17`):** Precomputed empty-board 18-frame adjustment-state table `S_adj(c, d)` when `maxH <= 9` (`topNonEmptyRow >= 11`, `~80%+` of boards cannot collide in frames `0..17`).
    - **Second half (frames `18..arrival`):** Precomputed swept-path masks per `(s_adj, goalRot, goalX)` across three clean, configurable tracks (`MoveSearchConfig`):
      1. **DAS track** (always on),
      2. **Native 7.5 Hz Slow-Tap track** (`enableSlowTaps = true`, always enabled for non-full DAS spawns `das < 15` and all `dasWillReset` adjustments — eliminates legacy "fake tuck" double-shifts),
      3. **Quicktap track** (`enableQuicktaps = false` for this pure-DAS training run, natively supported with `X.X` default = 1 release frame / `quicktapDelayFrames = 2` after the penultimate held DAS shift, `das → 0`).
  - **Pole-Vault Fallback (`enablePoleVaults = true`):** Only runs on a direction/track when its 4-AND swept-path mask collides. Tracks `(rot ∈ 0..3, rotCooldown ∈ 0..7)` as a **32-bit (`4 × uint8_t`) DP** along the track's fixed `(x(f), y(f))` schedule (~150 ns), natively finding arbitrary-frame pole vaults (e.g. `0 → 1 → 0` / `0 → 2 → 0` over tall peaks, delayed un-rotation at the edge) while strictly enforcing the 7-frame gap on `A`/`B`, plus stack backpointers for exact input reconstruction.
  - **Phase B — O(1) Drop + Exact Retroactive DAS + Single-Mask Wall-Charge:**
    - Column-height O(1) drop (with overhang fallback), closed-form hold-through-fall DAS (`framesUntilLock = 2*(lockY - yFirst) + ((frameFirst & 1) == 0 ? 1 : 0)`), and single-mask mid-fall wall-charge check (`touchesScreenWall || (board_rows[yWallStart..lockY] & sideShadowMask) != 0` → `16`), deduped per `(rot, x, lockY)` keeping max ending DAS.
  - **Phase C — Pure Real Overhang Tucks & Spins:** Overhang-driven reverse lookup (skipped 100% when the board has no overhangs!), using independent `dpadCooldown == 0` for shift tucks, `rotCooldown == 0` for spins, and both for combos, plus exact wall-charge tuck DAS (`10` + fall-hold).
  - **Standalone Milestone First:** Build, verify, and micro-benchmark `das_rt_move_gen.hpp` completely before starting any Phase 3 training or live-DAS games; later port back to `src/backend/full_move_search.cpp` once proven.
- **1× Leaf Scoring + 32-Bit Policy Words:**
  - Score unique lock placements `(rot, x, y)` **once** per `(b, c)` (shared across all `d` buckets & phantoms `φ`), so key encoding, `FlatBoardIndex` probes, and Soft-Boundary NNUE calls stay `1×`.
  - Pack `2`-bit ending bucket `d'_a` directly into existing `uint32_t` policy words (`1` terminal + `3` line-clear + `2` DAS-bucket + `26` `childId` bits = up to `67M` boards; `588 B/board` SSD cache for 3-bucket).
- **Leak-Free 3-vs-2 Bucket Ablation Protocol & Winner Selection Criteria:**
  - Start **both** 3-bucket and 2-bucket `5M` flywheels from the exact same neutral 0-frame Phase 2B base (`perline975_r3.tb.bin` + `perline975_r3_128x64_cal.nn.bin` + `perline975_r3.states.bin`, broadcast across `d` on Round 1). Neither branch ever reads the other's states, TB, or NNUE across independent on-policy Rounds 2–3 (`seed = 30,000,000`).
  - **Space/RAM-aware winner criterion (Occam's razor, `>= +10k` bar):** Evaluate `5M` 3-bucket (`21` floats/board) vs. `5M` 2-bucket (`14` floats/board) vs. the best **`5M` Phase 2B** baseline (`perline975_r3 + 128x64_cal`, `7` floats/board — *not* `20M` `perline975_20m`, which is reserved for the Step 3.4 scaled comparison) on **both** play strength (score, survival, TRT, latency) **and** space/RAM/compute cost. Require **`>= +10k`** improvement on `500g` (`seed = 30,000,000`) to justify stepping up to a heavier bucket count (since extra DAS buckets displace extra board states for the same RAM/disk budget); if the gap is `< +10k`, keep the simpler, more space-efficient model.
- **Final Scaling Target (`15M` vs. `20M`):** Deferred until after `5M` `V_RT` hit rates & 3-vs-2 ablation results are measured (`15M` = `1.5 GB` `.tb.bin` / `6.2 GB` RAM; `20M` = `2.0 GB` `.tb.bin` / `7.7 GB` RAM for 3-bucket; proportionally smaller for 2-bucket or Phase 2B).

### Staged Phase 3 Milestones
- [x] **3.0 — Fast Bitboard 18f RT + DAS Move/Phantom Generator (`das_rt_move_gen.hpp`)** *(complete before any Phase 3 training/games)*:
  - Implemented unified 7.5 Hz independent-thumb model (`dpadCooldown`, `rotCooldown`, `shiftCooldown`, ARE-hold `dpadCooldown = 0`), precomputed 18f prefix table (`maxH <= 9`, exact 0..16 DAS classes), SoA swept-path `BoardMask5` + 16 B `WalkMeta` CSR tracks (DAS + native slow-taps + configurable `X.X` quicktaps), 32-bit (`4 × uint8_t`) pole-vault DP on collisions, O(1) drop + exact retroactive DAS + bitmask wall-charges (`canFreshWallChargeFrom`), pure overhang-driven Phase C tucks/spins, and batch `searchRtMultiDas` with caller-owned `RtSearchScratch` (`53,504 B`) and fused 2 B `AdjEdge`.
  - Hard gate (ALL PASSED, `-Wall -Wextra -Werror`, ASan+UBSan clean, `15/15` `das_tb_test` / `11,424,881` assertions):
    1. **`0` mismatches across `23,000` boards (`393,738` moves, `301,126` phis, `133,175` early locks)** vs. brute-force frame-by-frame reference across all 7 suites (`pureDAS_rt18` 10k, `quicktap_rt18` 5k, `direct_rt18` 2k, `L29_grav1_rt18` 2k, `L18_grav3_rt18` 2k, `pureDAS_rt0` 1k, `zeroGap_rt18` 1k, full `das ∈ {0,4,5,10,11,15,16}` & `searchRtMultiDas` full payload parity),
    2. **Independent NES physics validator:** **`0` failures across `106,648` reconstructed spawn + adjustment sequences** (`3,000` boards across L19 `qt=0/1`, L29 `grav=1`, L18 `grav=3`),
    3. **Legacy 7.5 Hz (`"X......."`) audit (`2,000` boards, `32,946` legacy moves):** **`legOnly = 0`**, `legHigherDas = 6` (legacy 1-frame off-by-one wall-charge tuck gap cheat), `newExtra = 1,914` (`slowTap = 1,433`, `poleVault = 313`, `tuck = 168`, `other = 0`), `newHigherDas = 7,822`,
    4. **Throughput (`2,000` boards, `2.24 MB` tables):** `1T new_rt18 = 22.33 µs/call` (`2.8×` vs. `legacy_rt18 = 63.46 µs/call`), `multi3_rt18 = 15.83 µs/das` (`47.48 µs / 3 DAS`, `4.0×` per DAS; regimes: clean `43.39`, holey `66.13`, tall `32.61`), `spawn_only = 1.92 µs/call`, `8T = 9.59 µs / 3 DAS` (`104.2k board-pieces/s`, `4.9×` scaling).
    5. **3-Reviewer Sign-off:** Parallel independent **Logic**, **Perf & Scale**, and **Readability** subagent reviews completed; all HIGH & MEDIUM findings fixed and re-verified (`0` mismatches, ASan/UBSan clean).
- [x] **3.1 — `V_RT(b, c, d)` Binary Format (`SRTBV004`) & Soft-Boundary MPI Solver (`solve20RtMPI`)**:
  - Implemented `SRTBV004` (`TablebaseHeaderV4` 104 B with `numDasBuckets ∈ {2, 3}`, reaction-time / move-search flags, `writeTablebaseV4`, `MappedDasRtTablebase`, `IndexedDasRtTablebase`, `peekFileMagic`, `loadInputBoardKeysAnyFormat` supporting `SRSTV002`/`SRTBV003`/`SRTBV004`) in `tablebase_io_20row.hpp`.
  - Implemented `solve20RtMPI`, `RtDecisionWorkspace` (deduplicating unique lock cells across starting DAS buckets so board transitions, key encoding, `FlatBoardIndex` probes, and Soft-Boundary NN evaluations run `1×` per cell), `26`-bit `childId` + `2`-bit `d'` packed 32-bit policy store (`49 * K` words/board in RAM or SSD chunks), warm-start from `SRTBV003` (broadcast) or `SRTBV004`, `!fitsKey → 0.0` Soft-Boundary guard, `evaluateSoftBoundaryMcRt20`, and `solve_rt_20row.cpp`.
  - Hard gate (ALL PASSED): small-board brute-force `V_RT` oracle parity across hard & soft boundaries (`K ∈ {2, 3}`, `rt ∈ {0, 18}`, `PerLine` & `PerPiece`), `1T == 4T`, `Ram == Disk`, warm-start == cold-start, `!fitsKey` guard (`nonFitCalls == 0`), exact live-RT Soft-MC (`Mapped == Indexed`, `z <= 3.5`), + parallel 3-reviewer sign-off (**Logic**, **Perf & Scale**, **Readability**; all HIGH/MEDIUM findings resolved).
- [x] **3.2 — Multi-Bucket NNUE (`outDim = 7 × numDasBuckets`) & Native `V_RT` Runtime Adapter**:
  - Generalized `TinyDasNet` (`outDim ∈ {7, 14, 21}` in `SRNNV001`) with `evalAllRt`, `evalPieceBucket`, strided `evalBucket` (computing only the 7 Layer-3 outputs for a target DAS bucket), `expandedToBuckets(K)`, per-candidate `outIdx`, `vStarDim` validation, 21-output central finite-difference gradient check, and `1T == 4T` training determinism in `nnue_20row.hpp`.
  - Implemented `NnSoftBoundaryEvalRt20`, `sampleDistillDatasetRt20` (`1T == 4T` on-policy live-RT + DAS-bucket trajectory sampler), multi-bucket `bestReplyHybrid2PlyRtMultiBucket20` / `bestReplyNn2PlyRt20` / `bestReplyNn3PlyTopKRt20` in `rt_nn_boundary_20row.hpp`, and `SRTBV004` support in `train_nn_20row.cpp`.
  - Implemented `chooseTbNnRtMove` in `tbnn_precompute_adapter.hpp` (O(1) direct `V_RT(b', n, d')` tablebase lookup with zero 2nd-piece `fullMoveSearch` penalty sweeps, `OUT → TB2` re-entry with exact 2nd-piece ending DAS bucket `d''`, selective/prescreened 3-ply Top-K, and zero-RT fast path) and multi-gravity `DasRtMoveTables` routing in `benchmark_20row.cpp`.
  - Hard gate (ALL PASSED): `18/18` `das_tb_test` (`11,438,368` assertions) + parallel 3-reviewer sign-off (**Logic**, **Perf & Scale**, **Readability**; all HIGH/MEDIUM findings resolved).
- [ ] **3.3 — Leak-Free `5M` Live-DAS Flywheel & 3-Bucket (`{15, 10, 5}`) vs. 2-Bucket (`{15, 5}`) Ablation**:
  - Run independent `5M` on-policy live-DAS rounds for both 3-bucket and 2-bucket from neutral `perline975_r3`, compare score/survival/TRT/latency/RAM/disk against each other and the best **`5M` Phase 2B** policy (`perline975_r3 + 128x64_cal`, `ply=3[allout]`; *not* `20M` `perline975_20m`) on `seed = 30,000,000` (`500g`), and test `128×64` vs. `256×64` for 21 outputs if needed.
  - **Winner criterion (`>= +10k` bar):** Factor in space/RAM/compute efficiency alongside play strength — adopt 3-bucket only if it beats 2-bucket by **`>= +10k`**, and adopt 2-bucket only if it beats the best `5M` Phase 2B policy by **`>= +10k`** (matching 3 rounds of `5M` if time permits for a low-noise apples-to-apples comparison). When gains are `< +10k`, select the simplest, most space-efficient model that delivers top-tier performance.
- [ ] **3.4 — Capacity Scaling (`15M` or `20M`) & Phase 3 Exit Gate**:
  - Scale winning `V_RT` config to `15M–20M` (size chosen from `5M` hit-rate data), benchmark vs. production `200 ms b = -2` (`338.3k`) and Phase 2B `20M` (`395.4k`), update retrospective, and complete final 3-reviewer sign-off.

---

## Horizon 3 (Long-Term Future Roadmap — Detailed Design Deferred Until Phases 2B & 3 Complete)

- [ ] **Phase 4 — Stationary Level 18 Flywheel (`gravity = 3`, `L = 0..129`) & `20M+` Scale**:
  - Apply the proven Phase 2B + Phase 3 pipeline (winning discount/DAS representation) to Stationary Level 18 and scale both regimes to `20M+` boards.
- [ ] **Phase 5 — Finite-Horizon Multi-Level Backward Induction (`L = 229 → 130` & `L = 129 → 0`)**:
  - Shared-key exact undiscounted (`γ = 1.0`) backward induction across Levels 19–28 (`20× → 29×` multipliers, terminal `0.0` at `L >= 230`) and Level 18 → 19 transition (`L = 129 → 0`, `19×` into `V(b, c, d, L = 130)`), plus unified line-count-aware NNUE `V_nn(b, c, d, L)`.
- [ ] **Phase 6 — Untouched Held-Out Evaluation (`seed = 700,000`) & Production Integration Proposal**:
  - Head-to-head full NTSC **18 → topout** pure 10 Hz DAS evaluation vs. production StackRabbit `200 ms b=-2` (`1,074,642 ± 13,120`) on untouched `seed = 700,000` + `docs/benchmark_hard_positions.md`.
  - Production integration proposal (Path B: mmap TB + NNUE in `live_game_server`; Path A: standalone WASM NNUE) with full `AGENTS.md` verification matrix, and final retrospective update.
