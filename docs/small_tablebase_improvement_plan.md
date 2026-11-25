# Plan: Small-Tablebase Improvement (`<= 2.5M` Boards, Zero Full-TB Leakage)

Goal: Maximize real NES Very Low Lineout survival under a **hard `2,500,000`-board tablebase ceiling** (`~12.7%` of the full `19.54M` TB) without ever using the `19.54M` TB during training — directly prototyping the algorithms needed for a partial-coverage 20-row DAS 18/19 Tetris engine.

Companion docs:
- [docs/tablebase_distillation_plan.md](tablebase_distillation_plan.md) — completed Phases D-1–D-4.
- [docs/tablebase_distillation_retrospective.md](tablebase_distillation_retrospective.md) — Discovery 4 motivation (`2.49M` Medium TB `67.59` vs `19.54M` Full TB `104.02`).

---

## 1. Hard Constraints & Evaluation Contract

### Non-Negotiable Rules
1. **Board cap**: No generated `.states.bin` or solved `.tb.bin` used by any agent in this project may exceed **`2,500,000` boards** (`<= 2.5M`). Enforce this explicitly via `--max-boards=2500000` and verify `tb.size() <= 2500000` in every benchmark/test output.
2. **Zero leakage from the `19.54M` TB**:
   - **Forbidden during training/generation/tuning**: `nb_final_nes.tb.bin`, `nb_final_uniform.tb.bin`, `nb_round0_nes.tb.bin`, `round8.tb.bin`, `nb_nn_64x32.nn.bin`, `nb_nn_128x64.nn.bin`, or any state list / weight file derived from `> 2.5M` boards.
   - **Allowed starting point**: Either scratch (`nb_small_nes.tb.bin` / `nb_mid_nes.tb.bin`, `<= 2,487,192` boards, built solely from heuristic rollouts + small-TB self-play) or freshly generated `<= 2.5M` sets.
   - **Allowed oracle-only usage**: `nb_final_nes.tb.bin` may be read **only** for post-hoc diagnostics (out-of-distribution value MAE, bias/inflation checks, top-1 move agreement vs oracle) on already-frozen models — never to select states, label targets, or tune training gradients.
3. **Code isolation & dependencies**: All code stays in `experimental/tablebase/` (`src/` untouched), zero external dependencies (pure C++17), binary artifacts stay in gitignored `experimental/tablebase/data/small_tb/`.

### Fixed Benchmarks & Baselines
- **Validation split** (margin sweeps, hyperparameter selection): `2,000` NES games, `seed=30000000` (disjoint from train & test).
- **Held-out test benchmark** (final numbers only): **`10,000` NES games, `seed=700000`** (identical to all prior phases).
- **Starting baselines (`<= 2.5M` boards, zero leakage)**:
  - `nb_mid_nes.tb.bin` (`2,487,192` boards), standard TB (`all-OUT` heuristic): **`50.97 ± 0.48`** (`E[V(0)] = 43.07`)
  - `nb_mid_self_64x32.nn.bin` (`31.3 KB`, standalone 2-ply NN): **`56.09 ± 0.54`**
  - **Baseline to beat** — `nb_mid_nes.tb.bin` + `nb_mid_self_64x32.nn.bin` calibrated partial-OUT (`m=1.0`, 2-ply): **`67.59 ± 0.65`**
- **Reference oracle ceilings (for comparison only)**:
  - Full `19.54M` TB (`all-OUT` heuristic): `89.90 ± 0.88` (`E[V(0)] = 86.85`)
  - Full `19.54M` TB + `128×64` calibrated partial-OUT (`m=2.0`, 2-ply): `104.02 ± 1.02`

### Experimental Structure
Test each of the 4 levers **in isolation first** (holding everything else equal against `nb_mid_nes` = `67.59`), then combine the winners in a multi-round `<= 2.5M` flywheel:

```text
  Baseline: nb_mid_nes (2.49M, hard V=0) + 64x32 Self-NN -> 67.59 lines
     │
     ├──► Phase S-1: Larger NNUE (128x64 / 256x64) + Top-K 3-Ply Out-of-TB Search
     ├──► Phase S-2: Soft-Boundary (Value-Bootstrapped) MPI Solver
     ├──► Phase S-3: Self-Play RL / Expert-Iteration Fine-Tuning Beyond the TB
     └──► Phase S-4: NN-Guided & Error-Weighted State Selection (<= 2.5M Cap)
                │
                ▼
          Phase S-5: Combined Multi-Round <= 2.5M Flywheel + Full Ablation & Retrospective
```

*(Note on horizontal mirror state abstraction: intentionally excluded here because exact 7×7 NES piece RNG is asymmetric between `S` and `Z` (`P(S→S)=4/64` vs `P(Z→Z)=2/64`), so mirroring breaks exact NES parity. State abstraction remains a 20-row DAS-specific design axis.)*

---

## 2. Phased Implementation Plan

### Phase S-1: Larger NNUE Capacity & Deeper Out-of-TB Search (`tiny_nn.hpp`, `train_nn.cpp`, `benchmark_cli.cpp`)
Isolate how much of the `67.59 → 104.02` gap comes simply from **NN capacity** and **out-of-TB search depth** on the exact same `nb_mid_nes.tb.bin` (`2.49M`) board set.

- [x] **Raised stack fast-path & outlined large-model cold path (`tiny_nn.hpp`)**:
  - Raised `NN_FAST_STACK_SUM` to `384` (`256×64` = `320`) and outlined `evalAllLarge` / `evalPieceLarge` (`TB_NN_NOINLINE`) so hot `evalAll`/`evalPiece` stack frames stay `< 2 KB` (eliminating macOS `___chkstk_darwin` probes).
  - Self-distilled strictly from `nb_mid_nes.tb.bin` (`2,487,192` boards, `600k` decisions, `30` epochs):
    - `mid_self_128x64.nn.bin` (`78.6 KB`, `70.7s`): `valMAE = 1.687` (`0h=1.43`), `top1 = 82.57%`, `regret = 0.3992`; read-only oracle `OOD MAE = 38.521` vs `nb_final_nes` (confirming ~40-line `V = 0` teacher truncation).
    - `mid_self_256x64.nn.bin` (`155.1 KB`, `144.1s`): `valMAE = 1.468` (`0h=1.23`), `top1 = 84.74%`, `regret = 0.3043`; read-only oracle `OOD MAE = 38.620`.
- [x] **Implemented pruned Top-K 3-ply out-of-TB search with 2-ply pre-screening (`tiny_nn.hpp`, `benchmark_cli.cpp`)**:
  - Added `scoreNextBoxNn3PlyTopK`, unified `scoreNextBoxNn` / `chooseNextBoxNnMove`, `NnOutSearchConfig` (`ply1()`/`ply2()`/`ply3(k)`), `--mode=nextbox-nn3`, `--out-ply=1|2|3`, and `--out-topk=K`.
  - Added `NN_3PLY_PRESCREEN_SLACK = 6.0` 2-ply pre-screening inside `chooseNextBoxHybridFallbackMove` for `ply >= 3`, speeding up `p3k4` by **57×** (`1100.9s → 19.3s` for 2,000 games; `28.2k mv/s` on 10,000 games — matching 2-ply throughput!).
- [x] **Phase S-1 Held-Out Results (`10,000` NES games, `seed=700000`, `nb_mid_nes` `2.49M`)**:
  - Standalone 2-ply NN: `64×32` `56.09 ± 0.54` → `128×64` **`59.40 ± 0.58`** (`18.2k mv/s`) → `256×64` **`61.25 ± 0.60`** (`8.8k mv/s`).
  - TB + NN Partial-OUT 2-ply (`m=1.0`): `64×32` `67.59 ± 0.65` → `128×64` **`69.00 ± 0.67`** (`28.9k mv/s`, `m=2.0`: `68.09`) → `256×64` **`69.24 ± 0.68`** (`13.7k mv/s`, `m=2.0`: `68.14`).
  - TB + NN Partial-OUT **3-ply Top-K** (`128×64`): `m=2.0, k=4` **`70.50 ± 0.68`** (`19.4k mv/s`), `m=1.0, k=4` **`72.37 ± 0.71`** (`25.4k mv/s`), **`m=1.0, k=6` `72.43 ± 0.71`** (`med=50.0, p90=168, max=685`, `28.2k mv/s`, **`+4.84` lines** over baseline!).
  - Key finding: Scaling NN capacity `64×32 → 256×64` helps standalone (`+5.16` lines) but saturates around `~69.2` on 2-ply TB+NN due to `nb_mid_nes`'s `V = 0` value truncation (`OOD MAE = 38.6`); 3-ply Top-K out-of-TB adds **`+3.43` additional lines** (`69.00 → 72.43`) at nearly zero throughput cost.
- [x] **Verification & Review**:
  - `tablebase_test.cpp` (`25/25`, `382,010` assertions): `256×64` fast-path + `320×128` slow-path parity, independent brute-force 3-ply & pruned `topK=2` oracle, dedicated ply-3 top-out trap check, `topK <= 0` / unloaded-net / out-of-range guards, and loaded-TB all-OUT & partial-OUT `ply=3` tests (including exact ply-3 `O+I+I` line clear). Passed `cpp_test_unit` (`80/80`), `precompute_static_lint`, and Logic + Simplicity reviews.

---

### Phase S-2: Soft-Boundary (Value-Bootstrapped) MPI Solver (`nextbox_solver.hpp`, `solve_nextbox.cpp`)
Fix the #1 structural weakness diagnosed in Discovery 4: interior value collapse (`E[V(0)] = 43.07` vs `86.85`) caused by MPI treating every out-of-TB child as `V = 0.0`.

- [x] **Zero-overhead `PolicyActionStore` encoding for terminal boundary payoffs (`nextbox_solver.hpp`)**:
  - Repacked the 32-bit policy word with zero RAM/disk increase:
    - **Bit 31 == `0` (in-TB / zero-payoff out-of-TB)**: bits `30..28` = `3-bit reward` (`0..4`), bits `27..0` = `28-bit childId` (`INVALID_BOARD_ID = 0x0FFFFFFFU`, up to `268,435,454` boards).
    - **Bit 31 == `1` (terminal fixed-point boundary payoff)**: bits `30..0` = `round(clamp(qFixed, 0.0, 2047.0) * 2^20)` (`9.54e-7` line resolution, exact `0` at `0.0`).
  - Added `packNextBoxTerminalPayoff`, `isNextBoxTerminalPayoff`, `unpackNextBoxTerminalPayoff`, `NoOpBoundaryEval`, `NnSoftBoundaryEval` (`tiny_nn.hpp`), and `loadNextBoxInputBoards` (`tablebase_io.hpp`, accepting `.states.bin` or `SRTBV002` directly).
- [x] **Extended `NextBoxSolveConfig`, `solveNextBoxMPI`, and `solve_nextbox.cpp` with Soft-Boundary + Discount + Performance optimizations**:
  - Added `gamma` in `(0, 1]`, `maxValueDivergenceGuard` (`1000.0`, enforced on both outer and inner sweeps), terminal-overflow abort above `NB_TERMINAL_MAX_PAYOFF` (`2047.0`), per-worker `SoftBoundaryCache` (`32,768` slots), iter-1 precomputed `bestOutStore` (evaluates frozen NN boundary **once** on outer iter 1 and reuses exact terminal words on iters `2..K` + final residual), single-pass RAM sweeps, and exact `evaluateNextBoxSoftBoundaryMC` verification (`<= 3.5σ` on every solve!).
  - Exposed `--boundary-nn=PATH`, `--boundary-margin=F`, `--boundary-ply=1|2`, `--gamma=F`, `--max-v-guard=F` in `solve_nextbox.cpp`.
- [x] **Phase S-2 Ablations on Fixed `nb_mid_nes` (`2,487,192` boards, zero leakage) + 5-Round Stability Test**:
  - **Round 1 1-ply margin sweep** (frozen `mid_self_128x64`, `~80s` solve each, all Soft-MC `0.04–0.82σ`):
    - `m=1.0`: `E[V(0)] = 51.68` | `m=2.0`: `E[V(0)] = 50.94` | `m=4.0`: `E[V(0)] = 49.62` | `m=6.0`: `E[V(0)] = 48.50` (vs hard `43.07`).
  - **Multi-Round Fixed-Set Bootstrap (`Soft-MPI → Retrain 128×64 NN`, exact same `2.49M` boards, zero runaway inflation)**:
    - `m=4.0` (3 rounds): `E[V(0)] = 43.07 → 49.62 → 53.27 → 55.85` (oracle OOD MAE `38.52 → 26.67`); held-out 10k NES (`seed=700000`): standalone NN **`67.51 ± 0.65`**, TB+NN 2-ply (`m=2.0`) **`84.62 ± 0.82`**.
    - `m=2.0` (5 rounds): `E[V(0)] = 43.07 → 50.94 (+7.87) → 55.89 (+4.95) → 59.33 (+3.44) → 61.94 (+2.61) → 63.85 (+1.91)` — clean geometric damping (`~0.72×`/round) with zero inflation; oracle OOD MAE halved **`38.52 → 20.12`**; Soft-MC `0.07–1.62σ`.
  - **Phase S-2 Held-Out Results (`10,000` NES games, `seed=700000`, `mid_soft5_m2.0` `2.49M` boards)**:
    - Pure Soft-TB (`all-OUT` heur): `51.12 ± 0.49` (vs `50.97` hard — confirming Soft-TB + NN partial-OUT is a co-designed pair).
    - Standalone `128×64` 2-ply NN: **`68.28 ± 0.65`** (`+8.88` over S-1 `59.40`, `+12.19` over baseline `56.09`!).
    - `2.49M` Soft-TB + NN Partial-OUT 2-ply (`m=2.0`): **`89.97 ± 0.88`** (`med=63.0, p90=206, max=820`, `38.2k mv/s`, **`+22.38` lines** over baseline — matching the entire `19.54M` full TB `89.90` on `12.7%` of the boards!).
    - `2.49M` Soft-TB + NN Partial-OUT **3-ply Top-6 (`m=2.0`)**: **`92.48 ± 0.90`** (`med=65.0, p90=208, max=874`, `21.5k mv/s`, **`+24.89` lines** over baseline `67.59`, beating the full `19.54M` TB by `+2.58` lines!).
- [x] **Verification & Review**:
  - `tablebase_test.cpp` (`28/28`, `383,201` assertions): terminal packing bounds, `NnSoftBoundaryEval::evalRow` edge cases (margin `0`/neg/`NaN`, `nullptr`/unloaded, negative NN pred, 1-ply top-out), adversarial non-index-0 tie-breaking in both Bellman backup and soft-boundary MC, `loadNextBoxInputBoards`, 10-column line-clear VI parity oracle across all 4 modes, `1T==4T` & `Ram==Disk` determinism, combined `gamma<1` + soft-NN MC check, outer/inner divergence & terminal-overflow guards. Passed `cpp_test_unit` (`80/80`), `precompute_static_lint`, and all 3 reviews (Logic, Simplicity, Performance & Scale).

---

### Phase S-3: Self-Play Reinforcement Learning & Expert-Iteration Fine-Tuning (`rl_finetune_nn.cpp`, `tiny_nn.hpp`)
Test whether fine-tuning a small-TB-initialized NN directly on **unbiased 2-ply self-play rollouts** (zero `V = 0` TB boundary!) improves out-of-TB survival and recovers the remaining gap on messy digs.

- [ ] **Design Hybrid Expert-Iteration + `TD(λ)` Self-Play Fine-Tuner (`experimental/tablebase/rl_finetune_nn.cpp`)**:
  - Start from a self-distilled NN (strictly from `<= 2.5M` TB). Maintain a **lagged target network** `targetNet` (updated every iteration or via Polyak EMA `tau`) so value targets remain stationary during each gradient round.
  - **Self-play rollout collector** (multi-threaded, deterministic per-game seeds `40,000,000+`, zero overlap with train/val/test):
    - Play `N` full NES games to real top-out using **2-ply NN** (or `2.49M TB + calibrated NN partial-OUT`) with mild `ε`-exploration (`ε = 0.05`–`0.08`, 50% runner-up / 50% random top-4) to cover messy digs.
    - For each game trajectory `(b_0, c_0, n_0, r_0), ..., (b_{T-1}, c_{T-1}, n_{T-1}, r_{T-1})`:
      - Compute **exact `TD(λ)` return targets** $G_t^{(\lambda)}$ backward from terminal top-out ($V_T = 0$):
        $$G_t^{(\lambda)} = r_t + (1 - \lambda)\,\hat{V}_{\text{target}}(b_{t+1}, n_t) + \lambda\,G_{t+1}^{(\lambda)}$$
        (supports pure unbiased Monte Carlo at `λ = 1.0` and low-variance bootstrapped `TD(λ)` at `λ = 0.85–0.95`).
      - Compute **2-ply Expert-Iteration move Q-targets** $Q_{\text{2ply}}(b_t, c_t, n_t, m) = r_m + \max_{m_2}(r_{m_2} + \mathbb{E}_{p_3|n_t}[\max(0, \hat{V}_{\text{target}}(b''_{m_2}, p_3))])$ for all legal moves `m` (with exact `0.0` for ply-2 top-outs!). This directly distills **2-ply tactical search** back into the 1-ply NN values and rankings on *every* state visited — including messy out-of-TB boards!
  - **Anti-forgetting TB anchor**: Mix `20–30%` labeled decisions sampled from the `<= 2.5M` TB alongside `70–80%` self-play decisions in each iteration, using a low cosine learning rate (`1e-4 → 1e-5`) and AdamW so the NN never drifts off-scale or forgets sharp core geometry.
  - **CLI flags**: `--init-nn=PATH`, `--anchor-tb=PATH` (optional, enforces `<= 2.5M` check!), `--out=PATH`, `--iters=I`, `--games-per-iter=G`, `--lambda=L`, `--mc-weight=W`, `--exit-rank-weight=W`, `--exit-child-weight=W`, `--anchor-frac=F`, `--lr=LR`, `--threads=T`, `--seed=S`, `--ood-tb=PATH` (read-only diagnostic only).
- [ ] **Phase S-3 Ablations (`seed=30000000` val → `seed=700000` 10k NES games)**:
  - Compare on the `nb_mid_nes` (`2.49M`) starting point:
    1. Pure **Expert Iteration** (2-ply search distillation on self-play trajectories, `mc-weight=0`).
    2. Pure **`TD(λ)` / MC return** fine-tuning (`λ ∈ {0.9, 1.0}`).
    3. **Combined ExIt + `TD(λ)` + TB anchor** across 5–10 outer iterations.
  - Evaluate both **standalone 2-ply NN** (can it beat `56.09` and approach `71.79` without ever seeing the `19.54M` TB?) and **`2.49M` TB + RL-NN calibrated partial-OUT**.
- [ ] **Verification & Review**:
  - Add unit tests in `tablebase_test.cpp`: exact `TD(λ)` backward recurrence for `λ = 0.0`, `λ = 0.5`, `λ = 1.0` (verifying `λ = 1.0` equals exact suffix sum of lines cleared!), 2-ply ExIt target terminal top-out = `0.0`, `1T == 4T` rollout determinism, and gradient descent smoke test reducing loss cleanly.
  - Run `format_cpp`, `-Wall -Wextra -Werror`, `-DNDEBUG`, `tablebase_test`, `cpp_test_unit`, `precompute_static_lint`, **3 reviewers** (`logic_reviewer` + `readability_reviewer` + `Performance & Scale` reviewer on `rl_finetune_nn.cpp`), commit `Phase S-3 (self-play RL + Expert Iteration fine-tuning)`.

---

### Phase S-4: NN-Guided & Value/Error-Targeted State Selection (`<= 2.5M` Cap) (`nextbox_gen.hpp`, `generate_nextbox.cpp`)
Test how much better a **`2,500,000`-board** state set becomes when we choose *which* 2.5M boards belong in the tablebase using our NN instead of heuristic-seeded raw visit counts.

- [ ] **Implement Value/Error-Weighted Eviction Under `maxBoards <= 2.5M` (`nextbox_gen.hpp`, `generate_nextbox.cpp`)**:
  - Today, `mergeFilterAndCapBoards` ranks candidate boards purely by `visitCount`. Under a hard `2.5M` cap, add optional priority scoring when `--nn=PATH` and `--priority-mode=` are specified:
    - `visits` (existing baseline): pure visit count.
    - `nn-error` (requires prior TB + NN): score `b` by `visitCount(b) * (1.0 + alpha * min(errCap, |E[V_tb(b)] - E[V_nn(b)]|))` for in-TB boards, and `visitCount(b) * (1.0 + frontierBoost)` for newly discovered out-of-TB frontier boards (with a protected core fraction, e.g., top `protectedCore = 500,000` most-visited boards always kept so Bellman paths stay well-connected).
    - `hole-boost` (works even without prior TB error): boost `1–2`-hole high-visit recovery boards relative to ultra-common `0`-hole boards that the NN already predicts within `~1.5` lines (`visitCount(b) * holeWeight[bucket(b)]`), again with a protected `0`-hole core.
  - **Important interaction test**: Test `nn-error` / `hole-boost` eviction under **BOTH** hard `V = 0` MPI *and* **Soft-Boundary MPI** (Phase S-2)! Hypothesis: under hard `V = 0`, evicting any frequent 0-hole board creates potholes, whereas under Soft-Boundary MPI the NN smoothly patches easy 0-hole boards so TB slots can focus on hard tactical positions!
  - Also test **clean NN-bootstrapped `<= 2.5M` generation from scratch** (starting from `nb_small_nes` `673k` → NN-guided expansion up to `2.5M`, zero heuristic-biased round1–2 legacy boards).
  - Add CLI flags to `generate_nextbox.cpp`: `--priority-mode=visits|nn-error|hole-boost`, `--priority-alpha=A`, `--protected-core=K`.
- [ ] **Phase S-4 Ablations (`<= 2,500,000` boards strictly enforced, `seed=700000` 10k NES games)**:
  - Compare 3 `2.5M` board sets (all trained/solved identically):
    1. Legacy `nb_mid_nes` (`2.49M`, heuristic-seeded).
    2. Fresh `2.5M` NN-guided `visits` set (built from `673k` via NN flywheel).
    3. Fresh `2.5M` NN-guided `nn-error` / `hole-boost` priority set (evaluated with hard MPI and Soft-Boundary MPI).
- [ ] **Verification & Review**:
  - Add unit tests in `tablebase_test.cpp`: `<= maxBoards` hard cap enforcement, `0ULL` always preserved, protected-core invariant, `nn-error` priority ordering check, and `1T == 4T` bit-for-bit determinism for every priority mode.
  - Run `format_cpp`, `-Wall -Wextra -Werror`, `-DNDEBUG`, `tablebase_test`, `cpp_test_unit`, `precompute_static_lint`, dual reviews (`logic_reviewer` + `readability_reviewer`), commit `Phase S-4 (NN-guided + error-weighted 2.5M state selection)`.

---

### Phase S-5: Combined Multi-Round `<= 2.5M` Flywheel, Final Ablation & Retrospective (`docs/small_tablebase_improvement_retrospective.md`)
Combine every lever that won in Phases S-1–S-4 into a clean, reproducible end-to-end `<= 2.5M` pipeline and measure how close we get to the `19.54M` full tablebase!

- [ ] **Run End-to-End Combined `<= 2.5M` Flywheel (2–3 rounds, strictly zero leakage)**:
  - Round 0: `nb_small_nes` (`673k`, zero leakage) → Self-NN (`128×64` or `256×64`, whichever won S-1).
  - Round 1: Expand to `2.5M` using winning S-4 state selection → Solve with winning S-2 **Soft-Boundary MPI** → Retrain NN + apply winning S-3 **RL / ExIt fine-tuning**.
  - Round 2 (and Round 3 if still gaining): Refine `<= 2.5M` set (evicting low-value boards, adding new high-value frontier states while staying `<= 2,500,000`) → Re-solve with **Soft-Boundary MPI** → Final NN + RL/ExIt fine-tuning.
  - Evaluate on `10,000` held-out NES games (`seed=700000`):
    - Pure `2.5M` TB (`all-OUT` heur)
    - Standalone `<= 2.5M`-trained NN (2-ply & 3-ply)
    - **`<= 2.5M` TB + Final NN calibrated partial-OUT (`2-ply` & `3-ply`)**
  - Run **read-only oracle audit** against `nb_final_nes.tb.bin` (value MAE, mean bias/inflation, top-1 agreement across `0h / 1h / 2h / 3+h`) to verify true calibration and zero hidden inflation.
- [ ] **Write Retrospective Doc (`docs/small_tablebase_improvement_retrospective.md`)**:
  - Follow `AGENTS.md`'s human-readable retrospective style guide: Core Glossary + per-section Glossaries *before* use, abbreviation count audit (`>= 3` body uses), full "what & why" narrative, full individual-vs-combined ablation table, and concrete recommendations for the 20-row DAS 18/19 architecture.
  - Link from `docs/tablebase_retrospective.md` and `docs/tablebase_distillation_retrospective.md`.
  - Commit `Docs (small-tablebase improvement retrospective & ablation results)`.

---

## 3. Quality Gates & Review Protocol

- **Strict isolation**: Keep `src/` completely untouched (`git diff --stat -- src/` empty on every commit).
- **Anti-leakage check**: Every training/generation command logged in the plan/retrospective must explicitly list its `--tb` / `--init` / `--nn` inputs, and none may reference any `> 2.5M`-board artifact.
- **After every code phase (`S-1`–`S-4`)**:
  1. `npm run format_cpp`
  2. Compile all touched binaries with `-Wall -Wextra -Werror` and `-DNDEBUG -fsyntax-only`
  3. Run `./build/tablebase_test`, `npm run cpp_test_unit`, and `npm run precompute_static_lint`
  4. Run **Logic Reviewer** (4-point seam checklist: cross-component parity, scale/RAM audit, adversarial test audit, `-DNDEBUG`/hostile-input audit) + **Simplicity Reviewer** — plus **3rd Performance & Scale Reviewer** on Phases `S-2` (Soft-MPI hot loop) and `S-3` (RL rollout/training loop).
  5. Update checkboxes & results in this plan doc and commit cleanly per phase (`Phase S-1 (...)` through `Phase S-5` / `Docs (...)`).
