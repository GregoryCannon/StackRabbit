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
- [ ] **2B.1 — On-Policy `PerLine γ = 0.98` Flywheel (`5.0M`, `--merge`)**:
  - Run Rounds 2–3 (`perline98_r2`, `perline98_r3`) from `perline98_r1` (`391.3k / 60.8% TRT / 90.2% goal / 89.6% hit`) with `--merge` so the `5.0M` TB pulls in taller `60%`-TRT recovery boards (`H = 8..12`) and lifts both TB hit rate (`89.6% → 93%+`) and survival (`90.2% → 93–95%`).
- [ ] **2B.2 — Per-Line Horizon Sweep (`γ ∈ {0.975, 0.98, 0.985}`)**:
  - Map the converged `(score, TRT, survival)` Pareto frontier on `5.0M` on-policy states to confirm the best stationary L19 horizon.
- [ ] **2B.3 — `5M` State-Set & Soft-Boundary Solver Tuning**:
  - Tune `generate_20row` (`epsilon`, `nn-error` weight/cap, `protectedCore`, `siblingMinVisits`) for taller `PerLine` stacks.
  - Test tighter `--boundary-margin` (`0.20–0.30`) and 2-ply Soft-Boundary MPI (`--boundary-ply=2`) once `valMAE < 0.10`.
- [ ] **2B.4 — NNUE Architecture, Features & High-Stack Calibration**:
  - Flatten the remaining positive `V_nn` tail at `H = 14..18` (`+0.6..+2.7` on `perline98_r1`) via `--danger-frac` / `--recover-frac` / `temperature` / `rankWeight` tuning.
  - Test warm-started `256×64` NNUE and candidate feature enrichments (well depth, overhang/hole depth, surface parity/roughness) aiming for standalone NNUE `>= 385k–395k`.
- [ ] **2B.5 — 0-Frame Runtime Search & Margin Tuning**:
  - Sweep `out-margin` (`0.15–0.40`), selective `--out-ply=3` (now `PerLine`-consistent) on all-OUT / high-risk boards, and ply-2 re-entry refinements.
- [ ] **2B.6 — Qualitative Browser Inspector Audits (`das_inspector`)**:
  - Audit `TB+NN` vs. `eval.cpp` / `NN-only` disagreements and topout replays after each major jump to uncover systematic blind spots.
- [ ] **2B.7 — Capacity Scaling (`5M → 15M–20M`)**:
  - Only after `5M` recipe plateaus: first land the Perf-review 20M–50M RAM fixes in `state_gen_20row.hpp` (SoA `rawEntries`, 64-shard sibling dedup, parallel sort) and `tablebase_io_20row.hpp` (single-pass `IndexedDasTablebase::buildFromMapped`, `MADV_RANDOM`), then expand L19 NB TB to `15M–20M`.
- [ ] **2B Exit Gate & 3-Reviewer Sign-Off**:
  - Pure L19 NB stacking meets target bar (`>= 410k+`, `60–65%` TRT, `>= 93%` goal), all tests pass (`das_tb_test`, `-Wall -Wextra -Werror`, `-DNDEBUG`), and 3-reviewer pass (Logic, Perf & Scale, Readability) signed off.

---

## Horizon 2 (Near-Term Future): Phase 3 — Live 18-Frame Reaction Time & DAS Integration (Stationary L19)

*Deferred until Phase 2B exit gate is met.* Goal: transfer our strong L19 NB engine to true live **18-frame reaction time + 10 Hz DAS** rules and decisively beat production StackRabbit `200 ms b=-2` (`338,305 ± 12,534`, `81.2%` goal, `55.1%` TRT, `195.5 ms/mv`) at `< 5 ms/mv`.

- [ ] **3.1 — 3-Bucket `V_RT(b, c, d)` (`d ∈ {15, 10, 5}`, `21` floats/board)**:
  - Bake exact 18-frame precompute commitment + DAS charge carryover directly into the Bellman backup and NNUE (explicit `d = 10` bucket because NES DAS reachability is a sharp step function, not linear).
  - Directly targets all three gaps from our 6-way diagnostic (`-21.4k` pure RT, `-9.1k` Effect 1 mid-air reversal, `-17.9k` Effect 2 cross-piece carryover).
- [ ] **3.2 — 3-Bucket vs. 2-Bucket (`d ∈ {15, 5}`) Ablation**:
  - Quantify the real score/survival vs. RAM/speed tradeoff of 3 buckets vs. 2 buckets.
- [ ] **3.3 — Live-DAS On-Policy Flywheel & Benchmark vs. Production `200 ms b=-2`**:
  - Generate on-policy live 18-frame DAS states (capturing mid-air adjustment and low-DAS recovery positions), run Soft-Boundary MPI + NNUE distillation, and verify clear win over `338.3k` (`81.2%` goal, `55.1%` TRT) on validation `seed=30000000`.
- [ ] **3.4 — Verification & 3-Reviewer Sign-Off**:
  - Small-board brute-force `V_RT` oracle parity, `1T == 4T`, Soft-MC `<= 3.5σ`, and full 3-reviewer pass.

---

## Horizon 3 (Long-Term Future Roadmap — Detailed Design Deferred Until Phases 2B & 3 Complete)

- [ ] **Phase 4 — Stationary Level 18 Flywheel (`gravity = 3`, `L = 0..129`) & `20M+` Scale**:
  - Apply the proven Phase 2B + Phase 3 pipeline (winning discount/DAS representation) to Stationary Level 18 and scale both regimes to `20M+` boards.
- [ ] **Phase 5 — Finite-Horizon Multi-Level Backward Induction (`L = 229 → 130` & `L = 129 → 0`)**:
  - Shared-key exact undiscounted (`γ = 1.0`) backward induction across Levels 19–28 (`20× → 29×` multipliers, terminal `0.0` at `L >= 230`) and Level 18 → 19 transition (`L = 129 → 0`, `19×` into `V(b, c, d, L = 130)`), plus unified line-count-aware NNUE `V_nn(b, c, d, L)`.
- [ ] **Phase 6 — Untouched Held-Out Evaluation (`seed = 700,000`) & Production Integration Proposal**:
  - Head-to-head full NTSC **18 → topout** pure 10 Hz DAS evaluation vs. production StackRabbit `200 ms b=-2` (`1,074,642 ± 13,120`) on untouched `seed = 700,000` + `docs/benchmark_hard_positions.md`.
  - Production integration proposal (Path B: mmap TB + NNUE in `live_game_server`; Path A: standalone WASM NNUE) with full `AGENTS.md` verification matrix, and final retrospective update.
