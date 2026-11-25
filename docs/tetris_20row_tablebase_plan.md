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
  - **Round 5** (full **`5.0M`** `nn-error` `eps=0.10`, Soft MPI `m=0.25`): `E[V(0)] = 6.950` (`z=0.33`), `r5_128x64` (`MAE=0.152`, `top1=82.0%`, `regret=0.0347`) → Hybrid **`356,715 ± 3,049`** (`14.863` normT, **`93.4%` goal**, `53.3%` TRT, **`94.0%` TB hit**, **`0.214 ms/mv`**; `m=0.35..0.50` = `354.4k–355.0k`, `93.6%` goal); NN-only **`346,682 ± 3,095`** (`92.2%` goal); cold 30-ep `r5_256x64` (`339 KB`, `MAE=0.162`, `top1=81.2%`) → Hybrid `349,727 ± 3,445` (`m=0.50`, `90.0%` goal).
  - [ ] *Pending user decision*: Final expansion (`20M+` cap once `V_RT(b, c)` decision is approved) + warm-trained `256×64` NNUE.
- [x] **18-Frame Reaction-Time Calibration Check (`V(b, c)` vs. `V_RT(b, c)`, `seed=30000000`, `200g` `tbnn-precompute` 2×2 ablation on `r5.tb.bin + r5_128x64.nn.bin`, `m=0.40`, `ply=2`)**:
  - `rt=0, das=0`: **`356,570 ± 4,432`** (`95.5%` goal, `52.5%` TRT, `94.65%` hit, `0.77 ms/mv`) — exact parity with 0-frame `tbnn`.
  - `rt=0, das=1`: **`355,628 ± 4,421`** (`95.5%` goal, `52.4%` TRT, `94.24%` hit, `0.91 ms/mv`) — DAS charge physics costs only `-0.9k` (`-0.3%`).
  - `rt=18, das=0`: **`325,538 ± 5,496`** (`84.0%` goal, `49.1%` TRT, `91.41%` hit, `0.93 ms/mv`) — `-31.0k` from 18-frame reaction-time blindness alone.
  - `rt=18, das=1`: **`305,110 ± 6,756`** (`82.0%` goal, `47.1%` TRT, `89.07%` hit, **`2.81 ms/mv`**) — `-50.5k` (`-14.2%`) total drop vs `rt=0, das=1`; beats `precompute 0×0` (`248.8k`, `36.0%`) by `+56.4k` / `+46%` survival, beats `precompute 49×2` (`343.2k`, `66.0%`) by `+16%` survival and `4.8×` speed, but trails `49×2` score by `-38.1k` due to both 0-frame `V(b, c)` RT sensitivity and low stationary TRT (`47–53%`). Standalone NNUE (`rt=18, das=1`): `301,696 ± 6,818` (`77.5%` goal, `32.75 ms/mv`).
  - [ ] *STOPPED before implementing `V_RT(b, c)` or scaling to `20M` boards — awaiting user go-ahead.*
- [x] **Verification & Review (Logic + Simplicity + Perf/Scale)**:
  - `das_tb_test`: `10/10` suites (`11,424,416 / 11,424,416` assertions), `cpp_test_unit` (`80/80`), `precompute_static_lint` (0 warnings), `format_cpp`, `-O3 -Wall -Wextra -Werror`, `-DNDEBUG -fsyntax-only`, `git diff --stat -- src/` & `experimental/tablebase/` empty, and all 3 Phase 2 subagent reviews completed and addressed.

---

### Phase 3: Stationary Level 18 Flywheel
Apply the proven Phase 2 pipeline to Stationary Level 18 (`gravity = 3`, 10 Hz).

- [ ] **Run Stationary Level 18 Flywheel (3–4 Rounds, `5M` iteration → `20M+` final)**:
  - Seed Round 0 from `eval.cpp` on Level 18 (`L = 0..129`, `gravity = 3`), drop `eval.cpp` from Round 1 onward, run `nn-error` state selection + Soft-Boundary MPI (`γ ≈ 0.995`, normalized score, and `V_RT` if locked in Phase 2) + `128×64` NNUE at `5M`, then warm-start expand to `20M+` boards with final `256×64` NNUE.
  - Verify clear gain over production StackRabbit on validation Level 18 Tetris rate, average score over 130 lines, and survival (`seed=30000000`), with Soft-MC `<= 3.5σ` on every solve.
- [ ] **Verification & Review**:
  - Verify Level 18 gravity-3 parity tests, `1T == 4T`, and `Soft-MC <= 3.5σ`. (Run Logic + Simplicity reviews if any solver/sampler code changed.)

---

### Phase 4: Full Level 19–28 Backward Induction (`L = 229 → 130`, Exact Undiscounted NES Score)
Replace stationary discounting across Levels 19–28 with exact undiscounted ($\gamma = 1.0$) backward induction using true `(levelAfter + 1)` NES multipliers and exact Level 29 killscreen horizon (`L >= 230` → `0.0`).

- [ ] **Shared-Key Multi-Slice Backward-Induction Solver (`backward_solver.hpp`, `solve_backward.cpp`)**:
  - Build one shared `states_l19_28.states.bin` (union of Phase 2 Stationary 19 boards + late-game `L = 190..229` aggressive rollouts under fixed board cap).
  - Build `FlatBoardIndex` **once**; keep at most **5 value slices** (`L..L+4`, `double[7]` for active `L`, `float[7]` for frozen `L+1..L+4`) in RAM at once.
  - For each slice `L = 229` down to `130`:
    - Line clears (`r ∈ 1..4`) add exact NES points `{40, 100, 300, 1200} × (getLevelAfterLineClears(level(L), L, r) + 1)` (scaled cleanly in `1/1200`-Tetris or `1/1000`-point units with checked terminal fixed-point bounds) + frozen `V(childId, n, L + r)` (or `0.0` if `L + r >= 230`, or Soft-Boundary NNUE if `childId` is out-of-TB).
    - `r = 0` moves iterate inside slice `L` (converges in ~4–8 outer sweeps with $\gamma = 1.0$).
  - Save compact shared-key slice table (`SRTBS001`: one sorted key table + `100` contiguous `float[7]` value blocks, or per-slice files) and run exact end-to-end Soft-Boundary MC verification from `(board=0, L=130)` through `L=230` (`<= 3.5σ` vs `E[V(0, L=130)]` in real NES points!).
- [ ] **Endgame NNUE & Slice-Retention Study**:
  - Train line-count-aware Level 19–28 NNUE (`level`, `linesToKillscreen` populated) across `L = 130..229` on-policy trajectories; verify 1–2 Soft-Boundary refinement passes if needed.
  - Measure on validation (`seed=30000000`) how much real 100-line NES score (`L = 130 → 230`) improves over Phase 2 Stationary 19 — and test slice-retention options (all 100 slices mmap'd vs. dense endgame `L = 185..229` + strided `L = 130..184` + NNUE) for disk-size vs. strength tradeoff.
- [ ] **Verification & Review (Logic + Simplicity + Perf/Scale)**:
  - Unit tests: exact level-up multiplier boundary test (`L = 137..139`, `L = 226..229`), small-board brute-force finite-horizon backward-induction oracle (`L = 0..10`), `1T == 4T`, `L >= 230` zero-continuation check, overflow guards. Run all 3 subagent reviewers.

---

### Phase 5: Level 18 → 19 Transition (`L = 129 → 0`) & Unified 230-Line Engine
Connect Level 18 cleanly into Phase 4's exact `V(b, c, L = 130)` and unify all regimes into a single full-game Hybrid TB + NNUE engine.

- [ ] **Level 18 → 19 Backward Induction**:
  - Build shared `states_l18.states.bin` (Phase 3 Stationary 18 boards + `L = 110..129` transition rollouts).
  - Run undiscounted ($\gamma = 1.0$) backward induction from `L = 129` down to `L = 0` (or down to stationary convergence around `L ≈ 105–115` if `V(b, c, L) - V(b, c, L+1)` stabilizes), using exact `19×` NES scoring on `L < 130` and terminating at `L >= 130` into Phase 4's exact `V(b, c, L')` (with Soft-Boundary NNUE for out-of-TB cross-boundary hits).
  - Verify exact full-game Soft-Boundary MC from `(board=0, L=0)` to `L = 230` (`<= 3.5σ` in exact NES points!).
- [ ] **Unified Line-Count-Aware NNUE (`V_nn(b, c, L)`)**:
  - Distill all regimes (`L = 0..229`) into one unified `256×64` (`~345 KB`) NNUE (`featureVersion = 2`, inputs include `level`, `linesToTransition`, `linesToKillscreen`, and `gravityMode`).
  - Benchmark full **NTSC 18 → 29** games (18-frame reaction time, 10 Hz) on validation (`seed=30000000`) comparing:
    - Hybrid TB + Unified NNUE (2-ply, tuned runtime `m`),
    - Standalone Unified NNUE (2-ply, zero TB — preview of future WASM strength),
    - Production StackRabbit (`eval.cpp` + playouts / tournament `-2`).
- [ ] **Verification & Review (Logic + Simplicity)**:
  - Cross-boundary `L = 126..133` unit tests (exact `getLevelAfterLineClears` parity with `src/backend/utils.hpp`, gravity switch `3 → 2` at `L >= 130`, score-scale consistency across boundary), `1T == 4T`, Soft-MC `<= 3.5σ`.

---

### Phase 6: Final Held-Out Evaluation, Integration Proposal & Human Retrospective
Deliver clean, reproducible head-to-head numbers on untouched test seeds and prepare the production integration proposal.

- [ ] **Untouched Held-Out Evaluation (`seed=700000`, zero tuning on test)**:
  - Full NTSC **18 → 29 DAS** head-to-head (18-frame reaction time, 10 Hz `fullMoveSearch` + `dasPenaltyLookupMap`): average score ± SE, median, p90, maxout rate, Level 18→19 transition rate, mean/p99 per-move latency (`ms`), and TB hit rate vs. production StackRabbit.
  - Also evaluate on `docs/benchmark_hard_positions.md` (`standard` and `hard` suites) and inspect representative disagreements in the 20×10 browser inspector.
- [ ] **Production Integration Proposal (No `src/` edits until user approves)**:
  - Present concrete options for wiring into `src/backend/`:
    - **Path B (Primary)**: Memory-mapped TB + Unified NNUE inside `live_game_server` / `precompute.cpp` (with `dasPenaltyLookupMap` + 18-frame mid-air adjustment search).
    - **Path A (WASM follow-up)**: Standalone Unified NNUE replacing `eval.cpp` + playouts for zero-asset browser/WASM builds.
  - List the exact `AGENTS.md` verification matrix required upon integration (`cpp_test_unit`, `precompute_static_lint`, `precompute_tsan`, `fuzz_verify_sequences`, `precompute_verify`, `live_server_check`, `wasm_build`).
- [ ] **Write Human-Targeted Retrospective (`docs/tetris_20row_tablebase_retrospective.md`)**:
  - Follow `AGENTS.md` human-doc rules: explain *what* and *why*, glossary in every major section, abbreviation count audit (`>= 3` uses), quantitative progression tables, and takeaways.
