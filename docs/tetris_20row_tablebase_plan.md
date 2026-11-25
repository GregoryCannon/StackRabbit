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

- [ ] **Core primitives (`experimental/das_tablebase/core_20row.hpp`, `board_key.hpp`)**:
  - Implement locked `BoardKey` encode/decode, sentinel `EMPTY_KEY`, strong uncorrelated hash/mixers (for both `FlatBoardIndex` slots and `ShardedVisitCounter` shard bits), and exact 7×7 NES RNG (`NES_TRANSITION_NUM`, `NES_STATIONARY_PROB`, `Rng` splitmix64).
  - Implement allocation-free stack `TransitionList` + fast 10 Hz `"X....."` move generator (supporting Level 18 `gravity = 3` and Level 19 `gravity = 2`, including tucks matching `CAN_TUCK = 1`), plus pre-reaction adjustment-zone helper (`PreAdj` / `Adj` for 18-frame `V_RT(b, c)` readiness).
  - Thin read-only `eval_adapter.hpp` bridging `BoardKey` ↔ `src/backend/eval.cpp` strictly for Round 0 seeding and baseline benchmarking.
- [ ] **Fixed-seed benchmark CLI (`experimental/das_tablebase/benchmark_20row.cpp`)**:
  - Supports Stationary Level 19 (normalized score & survival), Stationary Level 18, and full **NTSC 18 → 29** games (both 0-frame fast mode and **exact 18-frame reaction time + `fullMoveSearch` + `dasPenaltyLookupMap`** via `src/backend/precompute.cpp` harness).
  - Record official production StackRabbit baselines on validation (`seed=30000000`) and held-out test (`seed=700000`):
    - `eval.cpp` 2-ply (0-frame & 18-frame RT),
    - `precompute` with production playouts (`160×8`, `300×6`, and tournament `-2` real-time frame deadline at 100% emulation speed).
- [ ] **Verification & Review (Logic + Simplicity + Perf/Scale)**:
  - Unit tests (`experimental/das_tablebase/das_tb_test.cpp`): bit-exact encode/decode round-trips + sentinel non-collision, bit-exact parity of fast move gen + line clears against `src/backend/move_search.cpp` & `move_result.cpp` over `10,000` random/live boards, NES RNG table parity against `src/backend/piece_rng.cpp`, `1T == 4T` benchmark determinism.
  - Compile `-Wall -Wextra -Werror` and `-DNDEBUG -fsyntax-only`; run 3 subagent reviewers.

---

### Phase 2: Stationary Level 19 Flywheel (Sampler, Soft-Boundary MPI, NNUE, Inspector & RT Check)
Prove the full 4-pillar flywheel on Stationary Level 19 (`gravity = 2`, 10 Hz) and verify 18-frame reaction-time calibration (`V(b, c)` vs. `V_RT(b, c)`).

- [ ] **Binary formats & shared I/O (`tablebase_io_20row.hpp`)**:
  - Implement `SRSTV002` (header with `keyEncoding`, `keyBytes`, `maxRows`, `maxHoles`, `numBoards`), `SRTBV003` (header adding `gamma`, `rewardMode`, `level`, `lineCount`, `reactionTimeFrames`), and reuse `SRNNV001` (`featureVersion = 2`, transactional load).
  - Port all strict I/O guards: `uint64_t` byte-size overflow check, unsorted/duplicate key rejection, non-finite/negative value rejection, `--init == outPath` / `--tb == outPath` active-`mmap` truncation guard.
- [ ] **State sampler (`state_gen_20row.hpp`, `generate_20row.cpp`)**:
  - Port `ShardedVisitCounter` (verifying uncorrelated shard vs. slot hash bits), `collectCoreSiblings` (bounded per-worker dedup), `collectMergedBoardCandidates`, `PrioritizedBoard` with `visits` / `nn-error` eviction, `protectedCore`, 50/50 exploration split, and Round 0 `eval.cpp` seeding (`--seed-eval`).
- [ ] **Soft-Boundary MPI solver (`solver_20row.hpp`, `solve_20row.cpp`)**:
  - Port `FlatBoardIndex`, disk-streamed `PolicyActionStore` (32-bit packed word with retuned fixed-point terminal shift + explicit overflow abort), iter-1 `bestOutStore`, `double` `vCur`/`vNext`, adaptive inner early-stop, warm-start `--init`, `maxValueDivergenceGuard`, and automatic `evaluateSoftBoundaryMC` (`<= 3.5σ`).
  - Run a 10-second micro-benchmark on `250k–500k` boards before launching multi-million-board solves.
- [ ] **20-Row Multi-Task NNUE (`nnue_20row.hpp`, `train_nn_20row.cpp`)**:
  - Implement `featureVersion = 2` extractor (~265–275 floats: raw binary cells up to `H_feat` + 10 col heights + 10 col holes + row fills + V2 residual-mined burial/holeRows/coverRowMissing/transitions/wells/parity + Level 19 accessibility features `inaccessibleLeft/Right`, `col9WellDepth`, `tetrisReadyRows`, `builtOutLeft`; reserve 3 horizon slots zeroed in stationary mode).
  - Sparse `[inDim × h1]` / `[h1 × h2]` / `[7 × h2]` NEON-friendly MLP (`NN_FAST_STACK_SUM` `< 4 KB`), `alignas(128)` `PersistentWorkerPool`, 4-part multi-task loss (parent Huber + candidate-child Huber + explicit ply-2 top-out `0.0` + Boltzmann move-ranking CE), on-policy `70/30` trajectory sampler, calibrated partial-OUT 2-ply (and optional selective 3-ply Top-K with pre-screen slack).
- [ ] **20×10 Browser Inspector (`inspector_server.cpp`, `inspector.html`)**:
  - Port `experimental/tablebase/server.cpp` & `index.html` to 20×10 (`V_tb` vs `V_nn` side-by-side, `Step to Disagreement`, transactional `/reload`, visual comparison against `eval.cpp`).
- [ ] **Execute Stationary Level 19 Flywheel (3–5 Rounds, `5M` iteration → `20M+` final)**:
  - Round 0: Seed states from `eval.cpp` (10 Hz L19), solve Hard/Soft initial TB, train initial `128×64` NNUE.
  - Rounds 1–3 (`5M` cap for fast ~5 min/solve iteration): **Drop `eval.cpp`**; iterate `Generate (nn-error) → Soft-Boundary MPI (m ≈ 0.5–1.0, γ ≈ 0.995) → Retrain 128×64 NNUE`. Tune `γ` and runtime `m` strictly on validation (`seed=30000000`).
  - Final expansion (`20M+` cap once 18-frame `V(b, c)` vs. `V_RT(b, c)` is settled): Warm-start expand to `20M+` boards (`~3.4 GB` RAM, ~20–30 min solve) and train final `256×64` NNUE.
- [ ] **18-Frame Reaction-Time Calibration Check (`V(b, c)` vs. `V_RT(b, c)`)**:
  - Benchmark the converged Level 19 TB+NNUE inside the exact **18-frame reaction-time** harness against production StackRabbit on validation Level 19 games, and inspect disagreements in the 20×10 browser inspector.
  - **Conditional upgrade**: If 0-frame `V(b, c)` stacks too aggressively under 18-frame reaction time, implement exact `V_RT(b, c)` Bellman backup (`max_{a ∈ PreAdj} E_n[max_{m ∈ Adj(a)} (r + γ C(b'_m, n))]` — same `7` floats/board, zero TB bloat!) and A/B test on validation Level 19 18-frame games. Lock `V(b, c)` vs. `V_RT(b, c)` before Phase 3.
- [ ] **Verification & Review (Logic + Simplicity + Perf/Scale)**:
  - Unit tests: `1T == 4T` determinism (sampler, solver, NN trainer/inference), `Ram == Disk` & `Warm == Cold` MPI parity, small-board brute-force VI parity (Hard, Discounted, Soft-Boundary, and `V_RT` if enabled), 6-block central finite-difference gradient check (`< 1e-4`), hostile-input & overflow tests, Soft-MC `<= 3.5σ` on every solve. Run all 3 subagent reviewers.

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
