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

### Phase S-3: Self-Play Reinforcement Learning & Expert-Iteration Fine-Tuning (`rl_finetune.hpp`, `rl_finetune_nn.cpp`, `tiny_nn.hpp`)
Test whether fine-tuning a small-TB-initialized NN directly on **unbiased 2-ply self-play rollouts** (zero `V = 0` TB boundary!) improves out-of-TB survival and recovers the remaining gap on messy digs.

- [x] **Implemented Hybrid Expert-Iteration + `TD(λ)` Self-Play Fine-Tuner (`rl_finetune.hpp`, `rl_finetune_nn.cpp`)**:
  - Shared `tb::PersistentWorkerPool`, `tb::detail::accumulateCandidateMoveLossAndGrad`, and `tb::detail::addL2WeightDecay` in `tiny_nn.hpp`; templated `sampleDistillDataset` (`distill_data.hpp`) on `NextBoxTbType` so read-only `--ood-tb` samples directly from `MappedNextBoxTablebase` (saving `1.35 GB` peak RAM).
  - Implemented `Eval2PlyCache` (`32,768` slots, exact IEEE-754 `double` 2-ply memoization, `10×` faster rollouts with bit-for-bit `1T == MT` determinism), chunked bounded-RAM deterministic rollout collector (`collectRlSelfPlayDecisions`), exact backward `computeTdLambdaReturns`, single-head `mcWeight` + 2-ply ExIt `exitChildWeight` + `exitRankWeight` loss (`computeRlSliceLossAndGrad`), Polyak EMA `updateTargetNetEma`, optional `<= 2.5M` TB replay anchor (`RL_MAX_ANCHOR_TB_BOARDS = 2,500,000` enforced!), exact per-iter cosine LR, and validation checkpointing.
- [x] **Phase S-3 Held-Out Ablations (`10,000` NES games, `seed=700000`, zero leakage)**:
  - **Isolated on hard `nb_mid_nes` (`mid_self_128x64` baseline: standalone 2-ply `59.40 ± 0.58`, TB+NN 2-ply `69.00 ± 0.67`, oracle OOD MAE `42.43`)**:
    - Pure Expert Iteration (`mcW=0`, `ε=0.06`): standalone 2-ply **`63.12 ± 0.61`** (`+3.72` lines!).
    - Pure `TD(0.9)` (`exitW=0`, `ε=0.06`): standalone 2-ply **`61.30 ± 0.60`** (`+1.90` lines).
    - Pure MC `TD(1.0)`: at `ε=0.06` collapsed (`5.19` val) because exploration cut rollout survival to `21.4` lines; at `ε=0.0` stayed flat (`58.72` val, `V(0) = 38.20`) because Huber loss (`δ=2.0`) on heavy-tailed exponential survival times targets the **median (~40)** instead of the **mean (~59)**!
    - Combined ExIt + `TD(0.9)` + rank-only `2.49M` TB anchor (`ε=0.06`): standalone 2-ply **`63.21 ± 0.60`** (`+3.81` lines); paired with un-updated hard `nb_mid_nes` TB (`m=2.0`): `65.47 ± 0.62`.
    - Combined ExIt + `TD(0.95)` + rank-only `2.49M` TB anchor (`ε=0.0`, `τ=0.5`): lifted `E[V_hat(0)]` `43.05 → 55.02`, cut oracle OOD MAE **`42.43 → 30.65`**, standalone 2-ply **`62.87 ± 0.62`** (`+3.47` lines; `62.57` without anchor). **Key law discovered**: pairing this scale-lifted NN (`55.02`) directly with the un-updated hard `V=0` TB (`43.07`) at `m=1.0–2.0` crashed to `12.30–13.74` (`in-TB = 43.7%`) due to `+12`-line scale mismatch — proving RL value lifting MUST be paired with Soft-Boundary MPI (Phase S-2)!
  - **S-2 + S-3 Synergy (`mid_soft5_rl_128x64` fine-tuned from `mid_soft5_m2.0_128x64` with `mid_soft5_m2.0.tb.bin` anchor, `2.49M` boards, zero leakage)**:
    - Oracle OOD MAE dropped below 20 (`22.17 → 19.88`, bias `-19.37`), `E[V_hat(0)] = 65.88`.
    - Standalone 2-ply NN: **`68.26 ± 0.67`** (`med=48.0, p90=156, max=738`).
    - `2.49M` Soft-TB + RL-NN 2-ply (`m=2.0`): `88.41 ± 0.85` (`in-TB=92.49%`, slightly under-margined due to `+2.0` RL scale lift).
    - **`2.49M` Soft-TB + RL-NN 3-ply Top-6 (`m=2.0`)**: **`98.67 ± 0.95`** (`med=70.0, p90=224, max=740`, `12.7k mv/s`) — **`+31.08` lines over baseline `67.59`, `+8.77` lines above the full `19.54M` TB (`89.90`), and `94.9%` of the `19.54M` TB + `128×64` NN oracle (`104.02`) using only `12.7%` of the boards!**
- [x] **Verification & Review**:
  - `tablebase_test.cpp` (`29/29`, `387,986` assertions): exact `TD(λ)` backward recurrence (`λ = 0, 0.5, 1.0`, top-out vs truncation, negative/`NaN`/`+inf` clamps), ExIt ply-2 top-out = `0.0`, `1T == 4T` rollout determinism, `Eval2PlyCache` exactness, `maxDecisions` cap & `collectMoveTargets=false` & `maxStepsPerGame=1`, `updateTargetNetEma` safety guards (`tau<=0`/`NaN`/size mismatch), `PersistentWorkerPool`, 25-param finite-difference gradient check, `NaN` target guard, and SGD loss descent. Passed `cpp_test_unit` (`80/80`), `precompute_static_lint`, and all 3 reviews (Logic, Simplicity, Performance & Scale).

---

### Phase S-4: NN-Guided & Value/Error-Targeted State Selection (`<= 2.5M` Cap) (`nextbox_gen.hpp`, `generate_nextbox.cpp`)
Test how much better a **`2,500,000`-board** state set becomes when we choose *which* 2.5M boards belong in the tablebase using our NN instead of heuristic-seeded raw visit counts.

- [x] **Implemented Value/Error-Weighted Eviction Under `maxBoards <= 2.5M` (`core.hpp`, `distill_data.hpp`, `state_generator.hpp`, `nextbox_gen.hpp`, `generate_nextbox.cpp`)**:
  - Moved `expectedValueFromRow` to `core.hpp` and extracted shared `collectMergedBoardCandidates` / `finalizeMergedBoards` in `state_generator.hpp` (tight pre-counted reserve + immediate `rawEntries` release).
  - Implemented `BoardPriorityMode` (`Visits`, `NnError`, `HoleBoost`) with tightly packed 24-byte `PrioritizedBoard` (`tier` 0=empty `0ULL`, 1=merged `priorBoards`, 2=top `protectedCore` by visits, 3=scored), single-meaning CLI flags `--priority-mode=visits|nn-error|hole-boost`, `--nn-error-weight=F`, `--hole-boost-scale=F`, `--protected-core=K`, strict boundary validation (rejecting `--max-boards=0`, zero priority slots, mismatched mode knobs, or `--tb` == output path), early `indexedTb` cleanup for non-`NnError` modes, and dynamic-chunk parallel scoring (`1T == MT`).
  - Upgraded `--merge=PATH` to use `loadNextBoxInputBoards` (accepting both `.states.bin` and `SRTBV002` `.tb.bin`).
- [x] **Phase S-4 Held-Out Ablations (`<= 2,500,000` boards strictly enforced, `10,000` NES games `seed=700000`, zero leakage)**:
  - **Head-to-Head Eviction Priority & Hard-vs-Soft MPI Interaction (`2,500,000` exact cap, identical `320k`-game Soft-5 rollout `seed=4000020`, `eps=0.16`, `topK=8`, `siblingMin=2`, `protectedCore=500k`)**:
    - **`visits` (`2.50M`, `0h=628k/1h=868k/2h=526k/3+h=478k`)**: Hard MPI `E[V(0)]=35.27`, live heur `49.72 ± 0.48`. Soft-MPI (`m=2.0`, `mid_soft5_m2.0_128x64`): `E[V(0)]=66.44` (`1.93σ`), TB+NN 2-ply `88.44 ± 0.87`, 3-ply Top-6 `91.65 ± 0.90`, +RL-NN 3-ply Top-6 `96.52 ± 0.94`.
    - **`hole-boost` (`2.50M`, `holeScale=2.0`, `0h=237k/1h=502k/2h=769k/3+h=992k`)**: Hard MPI `E[V(0)]=22.18`, live heur `35.18 ± 0.33` (**`-14.54` collapse** from 0-hole cliffs!). Soft-MPI (`m=2.0`): `E[V(0)]=64.15` (`0.17σ`), TB+NN 2-ply `88.35 ± 0.85`, 3-ply Top-6 **`93.43 ± 0.91`** (`+1.78` over `visits`!), +RL-NN 3-ply Top-6 **`98.33 ± 0.95`** (`+1.81` over `visits`!) — **full reversal of Hard MPI**, proving Soft-Boundary MPI turns hole-prioritized eviction from catastrophic into a net win!
    - **`nn-error` (`2.50M`, `errWeight=2.0`, `0h=642k/1h=885k/2h=529k/3+h=445k`) — CLEAR WINNER**: Hard MPI `E[V(0)]=34.83`, live heur `49.41 ± 0.48`. Soft-MPI (`m=2.0`): `E[V(0)]=65.89` (`1.49σ`), TB+NN 2-ply **`90.08 ± 0.87`** (`+1.64` vs `visits`), 3-ply Top-6 **`93.95 ± 0.92`** (`+2.30` vs `visits`, `+1.47` vs legacy `mid_soft5_m2.0`), +RL-NN (`mid_soft5_rl_128x64`) 3-ply Top-6 **`100.29 ± 1.00`** (`med=70.0, p90=230, max=1182`, `20.4k mv/s`, **`+3.77` vs `visits`, `+1.62` vs legacy `mid_soft5_m2.0`, `+10.39` above the entire `19.54M` full TB `89.90`, `96.4%` of the `19.54M` TB + `128×64` NN oracle `104.02`!!**).
  - **Clean NN-Bootstrapped Sets (zero heuristic-seeded legacy `nb_mid_nes` boards)**:
    - `s4_fresh_visits` (`2.22M`, from `673k nb_small_nes` + `64×32` self-NN): Hard MPI `E[V(0)]=34.42`, live `48.59 ± 0.47`; Soft-MPI (`m=2.0`) `E[V(0)]=65.47` (`0.25σ`), 2-ply `87.69 ± 0.84`, 3-ply Top-6 `90.83 ± 0.88`.
    - `s4_soft5_visits` (`2.22M`, `eps=0.12`, `siblingMin=3`): Soft-MPI (`m=2.0`) `E[V(0)]=68.41` (`1.61σ`), live heur `58.42 ± 0.56`, 2-ply **`91.16 ± 0.89`** (`+1.19` vs legacy `mid_soft5_m2.0`), 3-ply Top-6 **`93.99 ± 0.91`** (`+1.51` vs legacy `mid_soft5_m2.0`), +RL-NN 3-ply Top-6 `96.44 ± 0.94`.
- [x] **Verification & Review**:
  - `tablebase_test.cpp` (`30/30`, `388,075` assertions): `parseBoardPriorityMode`/`boardPriorityModeName` round-trips, `NnError` `errCap`-decisive eviction, `HoleBoost` all-4-bucket ordering (`2h > 3+h > 1h > 0h`), `protectedCore > maxBoards` + `priorBoards > maxBoards` + sibling rescue + `0ULL` absent, non-tautological `NaN`/`±inf`/negative sanitization with `protectedCore=0` (`evalCalls == 3`), `fastTb == nullptr` frontier fallback, true multi-chunk (`20,001` unique boards, `500` in-TB) parallel `1T == 4T` determinism (`nnCalls1 == nnCalls4 > 400`), and end-to-end `1T == 4T` determinism. Passed `-Wall -Wextra -Werror` & `-DNDEBUG` on all 6 binaries, `cpp_test_unit` (`80/80`), `precompute_static_lint`, and dual reviews (Logic + Simplicity).

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
