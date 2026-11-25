# Handoff: Scaling the Tablebase + NNUE Flywheel to Real 20-Row NES Tetris

**Audience:** Engineering agent starting the 20-row NES Tetris (Level 18 / Level 19 DAS) tablebase + NNUE project.
**Purpose:** Transfer every proven design decision, binary/algorithmic specification, negative result, production engine hook, and open 20-row design question from our 4×10 Very Low Lineout experiments so you can start Phase 0 immediately with zero lost context.

---

## 1. Required Reading (Read These First)

Do **not** repeat the experiments already settled on 4×10. Read these three retrospectives and repository guides for the full empirical evidence and reasoning:

1. [docs/tablebase_retrospective.md](tablebase_retrospective.md) — 4×10 No-Next-Box (`V(b)`) → exact 1-piece Next-Box (`V(b, c)`) with true 7×7 NES RNG, disk-streamed Modified Policy Iteration (MPI), warm-starting, Pareto state-space skew, timing formulas, and recurring review lessons.
2. [docs/tablebase_distillation_retrospective.md](tablebase_distillation_retrospective.md) — Linear residual error mining (`30.05 → 48.94` lines), compact Stockfish-style NNUE (`31–79 KB`, `71.79` lines standalone), calibrated partial-OUT hybrid fallback (`89.90 → 104.02` lines), and ARM NEON / cache optimization lessons.
3. [docs/small_tablebase_improvement_retrospective.md](small_tablebase_improvement_retrospective.md) — Pushing a strictly capped `<= 2.50M`-board partial tablebase (`12.8%` of full size, zero oracle leakage) from `67.59` to **`102.17` lines** (pure supervised, `98.2%` of the `19.54M` oracle+NN ceiling) via Soft-Boundary MPI, `nn-error` state selection, wider NNUE, and 3-ply Top-K — plus why self-play RL was cut.
4. [AGENTS.md](../AGENTS.md) — Working agreements, change-scoped C++ verification matrix, performance landmines, and doc style rules (agent-targeted plans vs. human-targeted retrospectives).
5. [docs/DAS_explainer.md](DAS_explainer.md) & [docs/engine_endpoints.md](engine_endpoints.md) — Frame-exact NES DAS charge model (`0..16`), move-search costs, and production engine contracts.

---

## 2. Proven Architecture: What to Port vs. What to Drop

### Port These 4 Winning Pillars (Pure Supervised + Bellman Flywheel)
Together, these four pieces carried a `2.50M`-board partial tablebase (`12.8%` of full coverage) to **98.2% of the `19.54M` oracle+NN ceiling** (`102.17` vs. `104.02` lines) and produced a `155 KB` standalone NNUE (`73.04` lines) that beat the `19.54M`-trained `128×64` NN (`71.79`):

1. **Native 1-Piece Next-Box `V(b, c)` with Exact 7×7 NES RNG**:
   - Store `7` floats per board (`V(b, c)` for `c ∈ {I, O, L, J, T, S, Z}` = expected future reward from `(b, c)` *before* seeing Next Box `n`).
   - Exact Bellman equation:
     $$V^*(b, c) = \sum_{n=0}^{6} P(n \mid c) \max_{b' \in \text{Moves}(b, c)} \Big[ r(b, b') + \gamma \cdot C(b', n) \Big]$$
     where $C(b', n) = V^*(b', n)$ if $b' \in \mathcal{S}$, else the frozen conservative boundary value $B(b', n)$.
   - Because `Moves(b, c)` does **not** depend on `n`, generate legal moves and hash child boards **once** per `(b, c)` and share across all 7 next pieces `n = 0..6`.
   - Exact NES RNG (`P(n | c) = transitionProbability[c][n] / 64`) is free once `c` is in the state (`+3.41` lines on 4×10).
2. **Soft-Boundary Value-Bootstrapped MPI (`B(b', n) = max(0, V_nn(b', n) - m)`)**:
   - Single largest lever on partial tablebases (`+22.4` lines on the exact same `2.49M` boards). Fixes hard-`V=0` boundary panic at the source and lifts both the TB and every NN trained from it.
   - **Critical speedup (`bestOutStore`)**: Since the boundary NN is frozen during a solve, precompute and store the single best out-of-TB packed terminal payoff per `(b, c, n)` once on outer iteration 1 and reuse it across outer iterations `2..K` and residual checks (`15×` speedup on 4×10; will save hours on 20-row DAS).
   - **Safety rails**: Conservative margin `m` (calibrated to `~2%–3%` of expected episode return; `m = 2.0` lines on 4×10 gave clean geometric convergence with zero value inflation), hard `maxValueDivergenceGuard`, optional discount $\gamma < 1$, and exact Soft-Boundary Monte Carlo verification (`<= 3.5σ` vs. `E[V(0)]`) on every solve.
3. **NN-Guided `nn-error` State Selection Under a Fixed Board Cap**:
   - Never enumerate boards structurally. Run TB+NN self-play rollouts with a **50/50 exploration split** (half TB runner-ups, half NN frontier probes excluding the greedy TB move) plus **core sibling expansion** on high-visit boards.
   - When candidates exceed `maxBoards`, always protect empty board `0` + top `protectedCore` most-visited boards (keeps core Bellman paths connected), then score remaining candidates by:
     - In-TB boards: `visits * (1.0 + errWeight * min(errCap, |E[V_tb(b)] - E[V_nn(b)]|))`
     - Newly discovered out-of-TB frontier boards: `visits * (1.0 + frontierBoost)`
   - **Interaction law**: Only evict low-error easy boards once Soft-Boundary MPI is active (evicting easy boards under hard `V = 0` creates cliffs and crashes performance).
4. **Compact Multi-Task NNUE (`128×64` or `256×64`) + Calibrated Partial-OUT Search**:
   - Train purely via supervised distillation on **on-policy gameplay trajectories** (`70%` greedy TB + `30%` `ε`-recovery rollouts — **never** uniform over `.tb.bin` rows, which over-represents rare scrambles).
   - **Mandatory 4-part loss** (candidate-child + terminal top-out supervision was decisive, `+13.2` lines on `64×32`):
     1. Parent 7-output Huber value loss (`valWeight = 1.0`, `δ = 2.0` scaled to reward units),
     2. **Candidate-child Huber value loss** (`childValWeight = 1.0`) supervising *all* legal in-TB children (good moves and trap moves),
     3. **Exact terminal ply-2 top-out supervision** (`includeTerminalOutMoves = true`: any move causing ply-2 top-out is explicitly supervised with `Q* = r_m + 0.0`),
     4. **Boltzmann move-ranking cross-entropy** (`rankWeight = 4.0`, `T = 1.0` scaled to reward units) over legal in-TB children.
   - **Calibrated Partial-OUT runtime selector**: Look up all legal children in pass 1; if all exit TB, rank directly with NN once (no margin clamping). If at least one stays in TB, price out-of-TB moves at `r_m + max(0, estCont(b'_m, n) - m)` (in-TB wins ties). Once the TB is soft-solved, runtime `m` should be lighter (`~0.5–1.0` lines on 4×10 vs. `2.0` during solving) to avoid double-penalizing boundary escapes.
   - **Depth-vs-width tradeoff**: On 4×10, `128×64` 3-ply Top-6 (with 2-ply pre-screening slack `6.0` for a `57×` speedup) scored `102.17` at `22.4k mv/s`, while `256×64` 2-ply scored `98.88` at `23.3k mv/s`. On 20-row DAS (where move gen is ~100× heavier), `256×64` (or `512×64`) at **2-ply** avoids ply-3 DAS move-gen branching and is likely the better real-time tradeoff.

### Explicitly Drop These (Settled Negative Results)
- **Self-play RL (`TD(λ)` / Monte Carlo / Expert Iteration)**: Do **not** build RL into the 20-row v1 pipeline. On 4×10, once margins were matched fairly against a Soft-Boundary TB, pure supervised `128×64` beat RL by `+5.21` lines in 2-ply (`97.00` vs. `91.79`), tied within `0.76σ` in 3-ply (`102.17` vs. `103.26`), had higher oracle top-1 agreement, and avoided RL's value-scale TB-fleeing (`69 → 12` lines), exploration death spirals (`5.19` lines), and Huber-median distortion on heavy-tailed survival lengths.
- **Structural enumeration**: Wasted 58%–97% of slots on unreachable/dead shapes while missing 85.7% sealed-hole recoveries.
- **No-Next-Box `V(b)` + 2-ply hybrid**: `-13.2` lines weaker and slower at inference than native `V(b, c)`.
- **Pure MSE / Ridge regression for linear evals**: Hurt move ordering (`30.05 → 27.89`); always use Boltzmann move-ranking loss if fitting linear weights.

---

## 3. Existing 4×10 Reference Codebase & Technical Specs (`experimental/tablebase/`)

Everything built so far lives cleanly under `experimental/tablebase/` (zero external dependencies, pure C++17, `src/` untouched). Treat this directory as your **reference implementation** — keep 4×10 passing and put new 20-row code in a separate folder (e.g., `experimental/das_tablebase/`), sharing or porting clean primitives rather than mutating the 4×10 testbed.

### File Map (`experimental/tablebase/`)

| File(s) | Role | Reuse for 20-Row? |
|---|---|---|
| `core.hpp`, `heuristic.hpp` | 40-bit 4×10 board bitops, 1-input tucks, exact 7×7 NES RNG (`NES_TRANSITION_NUM`, `NES_STATIONARY_PROB`), `Rng` (splitmix64), V1 heuristic | Port RNG tables & `Rng`; replace 40-bit board & move gen with 20-row DAS |
| `tablebase_io.hpp` | `SRSTV001`, `SRTBV001`, `SRTBV002` binary readers/writers, POSIX `mmap` wrapper `MappedRecordFile`, `loadNextBoxInputBoards` | Port pattern & validation invariants; widen board key from `uint64_t` |
| `vi_solver.hpp` | `FlatBoardIndex` (O(1) open-addressing hash map), `runWorkerPool`, `HoleStratifiedCounts`, distribution stats | Port `runWorkerPool`, stats, and adapt `FlatBoardIndex` for 20-row key + strong mixing hash |
| `nextbox_solver.hpp`, `solve_nextbox.cpp` | Disk-streamed `PolicyActionStore`, `bestOutStore`, packed 32-bit MPI action/terminal words, `solveNextBoxMPI`, `evaluateNextBoxSoftBoundaryMC`, `IndexedNextBoxTablebase` | **Core engine to port almost verbatim** (swap board key, move gen, and reward table) |
| `state_generator.hpp`, `nextbox_gen.hpp`, `generate_nextbox.cpp` | `ShardedVisitCounter`, `FlatVisitMap`, `collectCoreSiblings`, `collectMergedBoardCandidates`, `PrioritizedBoard` (24B), `nn-error` / `hole-boost` / `visits` eviction | **Core sampler to port almost verbatim** (adapt hash map key + high-entropy shard hash!) |
| `distill_data.hpp`, `tiny_nn.hpp`, `train_nn.cpp` | On-policy trajectory sampler (`70/30`), `TinyNextBoxNet` (`SRNNV001`), sparse `[in×h1]`/`[h1×h2]`/`[7×h2]` NEON-friendly MLP, `PersistentWorkerPool` (`alignas(128)`), multi-task Adam trainer, calibrated partial-OUT 2-ply & 3-ply Top-K | **Core NNUE engine to port** (replace `extractFeatures` with 20-row features + bump `featureVersion`) |
| `distill_linear.hpp`, `distill_linear.cpp` | V2 26-feature linear distillation & residual error mining | Reference for feature discovery & `src/backend/eval.cpp` improvements |
| `rl_finetune.hpp`, `rl_finetune_nn.cpp` | Self-play `TD(λ)` + Expert Iteration fine-tuner & `Eval2PlyCache` | **Reference only — do not port to v1** |
| `benchmark_cli.cpp` | Standardized multi-threaded benchmark CLI (`--seed=700000`, `10,000` games) | Port pattern for fixed-seed Level 18/19 benchmarks |
| `server.cpp`, `server_routes.hpp`, `index.html` | Zero-dep HTTP server + interactive browser move/value inspector (`V*` vs. `V_nn`, transactional `/reload`, Step to Disagreement) | Port early! Invaluable for visual debugging of 20-row setups & digs |
| `tablebase_test.cpp` | 30 doctest suites (`388,075` assertions) covering every invariant | Mirror every test class for 20-row |

### Key Low-Level Specifications (What Transfers Unchanged vs. What Needs Resizing for 20-Row)
- **Binary headers & strict I/O invariants**:
  - **States file — RESIZE (`SRSTV001` → `SRSTV002`)**: 4×10 used a 16 B header (`magic[8]`, `uint64_t numBoards`) + sorted `8 B` `uint64_t` keys. For 20-row, bump magic to `SRSTV002`, add `keyBytes`/`maxRows` to the header, and widen each board key to **`24 B`** (`uint64_t[3]` = 192 bits, sufficient for 19 rows) or **`32 B`** (`uint64_t[4]`, full 200-bit 20×10 board).
  - **TB file — RESIZE (`SRTBV002` → `SRTBV003`)**: 4×10 used a 40 B header + `36 B` records (`8 B key + 28 B float[7]`). For 20-row, bump magic to `SRTBV003`, add `gamma`, `rewardMode`, `level`/`lineCount`, and `keyBytes`/`maxRows` to the header, and widen per-board records to **`52 B`** (`24 B key + 28 B values`) or **`60 B`** (`32 B key + 28 B values`) — `10M` boards ≈ `520–600 MB` on disk. Keep all strict I/O guards: `uint64_t` overflow check on `numBoards * sizeof(Record)`, reject non-finite/negative values, reject unsorted keys, and reject truncating an actively mmap'd `--init` or `--tb` file!
  - **NNUE file — REUSE AS-IS (`SRNNV001`, 32 B header)**: The exact existing 32-byte header (`magic[8]`, `featureVersion`, `inDim`, `h1`, `h2`, `outDim`, `rngMode`) + raw `float32` blocks (`W1`, `b1`, `W2`, `b2`, `W3`, `b3`) already adapts dynamically to any input width — just bump `featureVersion = 2` and `inDim ≈ 265` (with transactional temp-object staging on load so corrupt files never clobber a live model).
- **32-bit Packed MPI Policy Word — STILL FITS IN `uint32_t` (`nextbox_solver.hpp`)**:
  - Bit 31 = `0` (in-TB move): bits `0..27` = `childId` (up to `268,435,455` boards; `0x0FFFFFFF` = top-out/miss), bits `28..30` = `linesCleared` (`0..4`, mapped through `REWARD_TABLE[linesCleared]` inside inner sweeps so NES scoring needs zero extra bits!).
  - Bit 31 = `1` (soft-boundary terminal exit, or line-clear transition into a frozen `L+1..L+4` slice): bits `0..30` = fixed-point quantized terminal payoff (`2^20` scale on 4×10 → `[0, 2047.0]`). Re-tune the fixed-point shift for 20-row reward units (with explicit overflow abort).
  - Total policy cache size stays **`49 × 4 B = 196 B/board`**: RAM below threshold (`<= 6M` boards = `1.18 GB`), sequential SSD `ScopedTempFile` in `<= 32 MB` chunks above (`0 B` RAM overhead).
- **NNUE Hot-Path Performance Tricks (`tiny_nn.hpp`)**:
  - Store `W1[inDim × h1]` and `W2[h1 × h2]` row-major by *input* index so zero binary cells (~70–85% on Tetris boards!) and ReLU-zero activations (~50%) are skipped completely with contiguous SIMD multiply-adds over `h1`/`h2`.
  - Store `W3[7 × h2]` row-major by *output* piece so single-piece `evalPiece(b, c)` computes only 1 row instead of 7, using 4 independent dot-product accumulators to break serial FP latency chains.
  - Use branchless `v > 0.0f ? v : 0.0f` (`fmax`) for ARM NEON auto-vectorization, keep fast-path stack buffers `< 4 KB` (`NN_FAST_STACK_SUM`) to avoid macOS `___chkstk_darwin` stack probes on every leaf, and pad per-worker training buffers to `alignas(128)` (Apple Silicon cache line size) inside a persistent thread pool.
- **Proven Structural Features to Carry Into the 20-Row NNUE**:
  - 4×10's **NNUE input vector** (`88` floats) combined: raw binary cells (`40`) + normalized column heights (`10`) + per-column hole counts (`10`) + row fill fractions (`4`) + `24` global summaries. (The standalone V2 linear model used only `26` hand-crafted features and zero raw cells.)
  - Residual error mining against the `19.54M` exact TB proved these structural concepts carry the highest signal beyond raw hole count: **`holeRows`** (count of distinct rows containing holes — far stronger than raw hole count!), **`coveredMinoCount`** (cells stacked on top of holes), **`buriedDepth2Plus`** (holes buried under `>= 2` rows), **`coverRowMissing`** (empty cells remaining in the lowest covering row — separates easy 1-clear digs from dead traps), **`rowTransitions`**, **`colTransitions`**, **`wellCount`/`maxWellDepth`**, and **`parityImbalance`**.

---

## 4. Production 20-Row StackRabbit Engine Hooks (`src/backend/`)

StackRabbit already has battle-tested, fuzz-verified 20-row NES Tetris mechanics in `src/backend/`. **Read these before writing a single line of 20-row simulation:**

- **`src/backend/types.hpp`**:
  - `GameState`: `unsigned int board[20]` (each row is a 32-bit bitfield: `c 0 tttttttttt hhhhhhhhhh bbbbbbbbbb` where bits `0..9` = filled cells `b`, bits `10..19` = hole flags `h`, bits `20..29` = tuck setups `t`, bit `31` = guaranteed-burn `c`), `int surfaceArray[10]`, `int numTrueHoles`, `float numPartialHoles`, `int lines`, `int level`.
  - `Piece`, `LockPlacement` (DO NOT widen `LockPlacement` — `+40 B` regressed production playouts by `~9%`), `AiMode` (`STANDARD`, `DIG`, `LINEOUT`).
- **`src/backend/move_search.hpp` (`fastMoveSearch`)**:
  - Fast ARR-timeline tap move search (`~0.5–1 µs/call`). Uses `inputFrameTimeline` (e.g., `"X...."` for 12 Hz, `"X....."` for 10 Hz DAS-equivalent).
- **`src/backend/full_move_search.hpp` (`fullMoveSearch`)**:
  - Frame-exact DAS charge (`0..16`, `dasCharge == -1` for tap mode) with hold-through-ARE, wall charges, quicktaps, and depth-1 tuck/spin DFS (`~10–25 µs/call` — ~100× slower than `fastMoveSearch`, fuzz-tested against 2,783 golden fixtures). Returns `FullMovePossibility { x, y, rotationIndex, dasChargeAfter, tuckInput, numLinesCleared, inputSequence }`.
- **`src/backend/precompute.cpp` & `src/backend/high_level_search.cpp` (`getLockValueLookupDasMaps`)**:
  - Already implements `dasPenaltyLookupMap` (pricing the exact cost of losing DAS charge across 2-ply frame-exact `fullMoveSearch`) AND reaction-time mid-air adjustment search (`adjTimeSimState`, choosing the best pre-reaction placement maximizing expected post-adjustment value over next pieces `n`).
- **`src/backend/move_result.hpp`**:
  - `getNewBoardAndLinesCleared(board, lockPlacement, newBoard)` and `advanceGameState(gameState, lockPlacement, evalContext)`.
- **`src/backend/piece_rng.cpp`**:
  - Exact NES `transitionProbability[7][7]` (denominator `64`). **Watch the index trap**: `transitionProbability` is in `T, J, Z, O, S, L, I` order while `PIECE_LIST` is in `I, O, L, J, T, S, Z` order — always remap via `pieceIndexToTableIndex` / `tableIndexToPieceIndex` (already verified in `experimental/tablebase/core.hpp`!).
- **`src/backend/eval.cpp`, `src/backend/params.hpp`, `src/backend/config.hpp`**:
  - Production linear weights (`MAIN_WEIGHTS`, `DIG_WEIGHTS`, `LINEOUT_WEIGHTS`, `WELL_COLUMN = 9`, `USE_BASE_7_RANKS = 1`).
  - **How to use `eval.cpp` (Round 0 seed ONLY — then drop it!)**: Do **not** build a weak toy linear heuristic from scratch for Round 0, and do **not** keep `eval.cpp` inside Rounds 1–5. Instead, call `eval.cpp` through a single thin read-only adapter **strictly for Round 0 state seeding and baseline benchmarking**, so Round 0 immediately visits grandmaster Level 18/19 shapes. From Round 1 onward, drop `eval.cpp` completely and run pure `TB + NNUE` (`nn-error` + Soft-Boundary MPI) so the flywheel breaks free of `eval.cpp`'s bespoke assumptions (`WELL_COLUMN = 9`, mode switches, hand-tuned burn penalties).

---

## 5. The 7 Hard New Design Decisions for 20-Row Tetris (Resolve in Phase 0!)

Do **not** jump straight into coding a 20-row solver. Every 4×10 → 20-row challenge lives in these 7 decisions — run a fast Phase 0 measurement spike on #2 and #6 first:

### Decision 1: Objective Function & Discount Factor $\gamma$ (Stationary Regimes)
On 4×10 lineout, episodes lasted $L \approx 220$ pieces, so undiscounted $\gamma = 1$ contracted cleanly ($\rho \approx 0.9955$). On stationary **Level 18** (or strong **Level 19**), survival can exceed **2,000+ pieces** ($\rho > 0.9995$, causing MPI to stall or diverge!), and competitive NES Tetris maximizes **Score (Tetrises)**, not raw lines:
- **Normalized Discounted NES Score (Recommended for stationary Level 19 & Level 18)**:
  - Normalize NES line-clear points by 1 Tetris (`1200 * (level + 1)`): `r(0..4) = {0.0, 1/30, 1/12, 1/4, 1.0}` (`40 / 100 / 300 / 1200` ratio — exact NES scoring, level-independent scale!).
  - Use discount $\gamma \in [0.992, 0.997]$ (effective horizon `125–333` pieces → fast MPI contraction in `~10–15` outer steps, bounded values `V <= ~30–100` Tetris-equivalents, and naturally balances Tetris efficiency vs. survival).
  - **Rescale every 4×10 value-unit hyperparameter to the new reward scale**: on 4×10 (`E[V(0)] ≈ 70–87` lines), `boundaryMargin = 2.0`, `huberDelta = 2.0`, `Boltzmann T = 1.0`, `errCap = 10.0`, and `prescreenSlack = 6.0` were tuned in raw line units. Under normalized Tetris-equivalents (`E[V(0)] ≈ 15–50`), leaving those defaults untouched would be `3×–5×` too large/blurry — scale them proportionally (e.g., `boundaryMargin ≈ 0.5–1.0`, `huberDelta ≈ 0.5–1.0`, `T ≈ 0.3–0.5`, `errCap ≈ 3.0`, `prescreenSlack ≈ 1.5–2.0`).
- **For finite-horizon transition bands (Decision 5 below)**: No discount ($\gamma = 1.0$) is needed because line count strictly terminates at the transition boundary!

### Decision 2: Board State Representation, `H_cap`, & Mandatory Phase 0 Revisit-Rate Spike
A full 20×10 occupancy grid is `200 bits` (`24 B` as `uint64_t[3]` for bottom 19 rows = 190 bits, or `32 B` for all 20 rows).
- **Exact bottom-`H_cap` boards (`H_cap = 16` or `18`, 24 B key)**: Zero aliasing, exact Bellman transitions inside `height <= H_cap`, and any board rising above `H_cap` cleanly exits to the Soft-Boundary NNUE! Total solver RAM: `key (24 B) + FlatBoardIndex (~32 B) + vCur/vNext (56 B) ≈ 112 B/board` (`10M` boards ≈ `1.12 GB` RAM). Do *not* apply height-shift canonicalization: NES gravity restricts horizontal reach at higher rows, so absolute height changes the legal move set.
- **Mandatory Phase 0 Diagnostic (Run BEFORE writing any solver!)**:
  - Run `5,000–10,000` games with StackRabbit's `eval.cpp` policy on Level 19 (and Level 18) and measure **exact board revisit rate** (fraction of live moves hitting a board seen `>= 2` times, and total hit rate at `N ∈ {1M, 2.5M, 5M, 10M, 20M}` boards, stratified by `maxHeight` and `holes`).
  - **Why this check is make-or-break**: If exact-board coverage at `5M–10M` boards is high (`~60%+`), the full Soft-Boundary TB + NNUE hybrid will shine directly. If exact-board coverage is `< 30%` because 20-row stacks rarely repeat identically, **pivot early**: either crop the TB to the high-collision core (e.g., `H_cap <= 10–12`, Soft-Boundary NNUE everywhere above) or treat the pipeline primarily as a **distillation engine to train a standalone 20-row NNUE** replacing `src/backend/eval.cpp`.

### Decision 3: DAS Charge — Keep It OUT of the TB, Price It Online via `dasPenaltyLookupMap`
- **Settled design**: Do **not** store DAS charge in the TB state. Keep the TB strictly **`V(b, c)` (`7` floats/board)**, solved offline with fast full-charge / DAS-equivalent move generation (`~0.5–1 µs/call`).
- At runtime, borrow StackRabbit's existing `precompute.cpp` **`dasPenaltyLookupMap` + 2–3 ply frame-exact `fullMoveSearch`** approach, using `V_tb(b, c)` / `V_nn(b, c)` as the leaf evaluator. Because `fullMoveSearch` tracks exact `dasChargeAfter` across ply 1 and ply 2–3, any move that loses DAS charge restricts subsequent reachability and automatically yields worse leaf boards and lower `V(b, c)` — zero state-space blowup in the TB!

### Decision 4: Human Reaction Time (Hybrid Between NNB and Next-Box)
Real StackRabbit plays with a **reaction-time delay** mimicking humans: on Level 19 especially, it must partially commit to a region of the board *before* reacting to Next Box piece `n` (via `precompute.cpp`'s mid-air adjustment search). This sits strictly between No-Next-Box (`reactionTime = inf`) and 0-frame Next-Box (`reactionTime = 0`).
- **Concern**: A 0-frame Next-Box TB (`V(b, c)`) assumes future turns can wait for `n` before committing left/right, so it may stack more aggressively than a reaction-time-limited Level 19 player can safely sustain.
- **Staged plan**:
  - **v0**: Train standard 0-frame `V(b, c)` (preserves exact 7×7 NES RNG and fast move gen). At runtime, `precompute.cpp` *already* enforces exact reaction-time reachability on the current turn (`max_{pre-adj} E_n[max_{reachable adj} V(b', n)]`). Measure empirically on Level 19 reaction-time games and in the visual inspector whether future-turn optimism causes risky placements.
  - **v1 upgrade (only if v0 proves too aggressive on Level 19) — Exact Reaction-Time Bellman `V_RT(b, c)` with ZERO extra TB storage**:
    $$V_{\text{RT}}(b, c) = \max_{a \in \text{PreAdj}(b, c)} \sum_{n=0}^{6} P(n \mid c) \max_{m \in \text{Adj}(a)} \Big[ r(m) + \gamma \cdot V_{\text{RT}}(b'_m, n) \Big]$$
    where `a` is the pre-reaction commitment (either exact `adjTimeSimState` clusters or a fast `{Left, Center, Right}` commitment-zone partition of `Moves(b, c)`). Keeps exact `7` floats/board and 7×7 NES RNG while teaching the TB to stay low and flexible under real reaction time.

### Decision 5: Line Count & Finite-Horizon 230-Line Staging (Backward Induction + Unified NNUE)
In DAS NES Tetris, the game effectively ends at Level 29 killscreen (`230` lines), so optimal play shifts dramatically near two transitions: **Level 18 → 19 (`L = 130`, lower/clean the stack)** and **Level 28 → 29 (`L = 230`, play aggressively since topping out leaves fewer points on the table)**.
- **Why backward induction by line count (`L → L-1`) works cleanly without exploding RAM or runtime**:
  - Because lines only increase on clears (`L' = L + r, r ∈ {1..4}`), slice `L` depends **only** on slice `L` itself (for `r = 0` placements) and the 4 already-solved slices `L+1..L+4`.
  - Only **5 slices** need to be in RAM at once, **no discount $\gamma < 1$ is needed** on transition bands, and each slice converges in a handful of sweeps because every line clear (`r >= 1`) acts as an instant frozen terminal payoff (identical to `bestOutStore`) and `r = 0` placements cannot run more than ~30–40 pieces without clearing or topping out!
  - **Share one `states.bin` across all slices of a transition band**: rather than building 45 separate state sets for `L = 229..185` (or 15 for `L = 129..115`), use one shared `states.bin` across the band (e.g., Stationary 19's boards unioned with late-game aggressive rollouts). That builds `FlatBoardIndex` **once**, lets every `L → L+1..L+4` lookup index directly by the **same `boardId`** (zero cross-slice hashing), and reuses identical policy-cache layout across slices!
  - Even better: you do **not** need 230 separate TBs at runtime! Solve the stationary cores + narrow transition bands (`~15` lines before `L = 130` and `~40–45` lines before `L = 230`, rather than all 230 lines), and **distill all solved slices into ONE single `~200–350 KB` line-count-aware NNUE `V_nn(b, c, L)`** (adding `level`, `linesToTransition`, and `linesToKillscreen` as 3 extra input floats).
- **Agreed 4-Step Solve Order (User-Specified, Phases 2–5 below)**:
  1. **Stationary Level 19 (Phase 2)** *(most important and likely most time-consuming; proves the full 20-row Soft-Boundary MPI + `nn-error` + NNUE flywheel on the primary speed)*
  2. **Stationary Level 18 (Phase 3)** *(clean steady-state Level 18 stacking)*
  3. **Level 28 → 29 transition / late-game endgame (Phase 4)** *(backward induction from `L = 229` down into Stationary Level 19, with terminal Level 29 killscreen residual at `L >= 230`)*
  4. **Level 18 → 19 transition (Phase 5)** *(backward induction from `L = 129` down into Stationary Level 18, terminating cleanly into Phase 2's Stationary Level 19 values at `L >= 130`)*

### Decision 6: Offline Move-Gen Throughput & Adaptive MPI
From [docs/tablebase_retrospective.md](tablebase_retrospective.md), total MPI solve time is:
$$T_{\text{solve}} \approx I_{\text{outer}} \cdot \Big( T_{\text{outer}} + K_{\text{inner}} \cdot T_{\text{inner}} \Big)$$
- $T_{\text{inner}}$ is independent of move-gen complexity (`~1–3s` per sweep at `10M–20M` boards on 10 cores); $T_{\text{outer}}$ scales linearly with `C_move_gen` (`~5–10s` at `10M` boards with fast `~0.5–1 µs` DAS-equivalent move gen).
- **Actionable optimizations**:
  1. Use an allocation-free stack `TransitionList` in the offline DAS-equivalent move generator (zero `std::string` or heap `std::vector` churn).
  2. Make inner early-stop **adaptive**: stop when `innerDiff <= std::max(cfg.tolerance * 0.05, outerDiff * 0.05)` with a generous cap (`K_inner = 150–400`) so stationary solves finish in `8–15` outer sweeps.
  3. Always run a **10-second micro-benchmark** on `250k–500k` boards before launching any multi-million-board solve.

### Decision 7: 20-Row NNUE Feature Vector & Production Integration Path
- **NNUE (`featureVersion = 2`)**: `200` raw binary cells (or `180` bottom rows, `~70%–85%` zeros skipped by sparse `[inDim × h1]` layout) + `10` col heights + `10` col holes + `20` row fills + `~25–30` structural summaries (all residual-mined V2 hole/burial/cover-row/transition features + `col9WellDepth`, `tetrisReadyRows`, `inaccessibleLeft/Right`, `avgHeight`, `maxHeight`, `builtOutLeft`) + (in Phases 4–5) `3` line-count horizon floats (`level`, `linesToTransition`, `linesToKillscreen`). Total `~270` floats; **`128×64`** (`~175 KB`) for iteration, **`256×64`** (`~345 KB`) for final 2-ply play (still `< 4 KB` stack buffer on Apple Silicon, avoiding `___chkstk_darwin`).
- **Do NOT use horizontal board-mirroring data augmentation**: NES Tetris is **not** left-right symmetric — piece spawn offsets (`INITIAL_X = 3`), rotation pivot/kick geometry, and right-well DAS wall-charge mechanics differ between left and right, so mirroring injects subtly invalid/mispriced training labels.
- **Integration into `src/backend/`** (only after beating production StackRabbit on held-out games and `docs/benchmark_hard_positions.md`):
  - **Path A (Standalone NNUE in `src/backend/eval.cpp`)**: Drop the `~175–345 KB` NNUE directly into StackRabbit's existing `precompute` / `dasPenaltyLookupMap` / `tournament_search` / `playout` / WASM pipeline (zero large TB asset to ship, works seamlessly with `fullMoveSearch` and reaction-time adjustments).
  - **Path B (Hybrid TB + NNUE)**: Memory-map a compact `SRTBV003` Soft-Boundary TB alongside the NNUE for desktop / `live_game_server` play while using Path A for WASM.

---

## 6. Recommended Phased Execution Roadmap

Follow this exact sequence (writing a concise agent-targeted plan doc first, per `AGENTS.md`):

- **Phase 0 — Scope Confirmation & Revisit-Rate Spike (1–2 hours)**:
  - Confirm exact discount $\gamma$ for Stationary Level 19/18, target `reactionTime` frames, and exact `inputFrameTimeline` / DAS-equivalent ruleset with the user.
  - Run a quick diagnostic on `5,000–10,000` StackRabbit `eval.cpp` Level 19 games measuring **exact 20-row board revisit rate** (at `1M / 5M / 10M` boards and `H_cap ∈ {10, 12, 16, 20}`) and **move-gen throughput (`ns/call`)** to lock in key size, `H_cap`, and Hybrid TB+NNUE vs. Standalone NNUE emphasis.
- **Phase 1 — Isolated 20-Row Core, Fast Move Gen & Fixed-Seed Benchmark Harness**:
  - Build zero-allocation 20-row board key + fast DAS-equivalent move gen + exact NES RNG + thin read-only `eval.cpp` adapter + fixed-seed held-out benchmark CLI (separate validation seed vs. held-out test seed; bit-exact parity checked against `src/backend/move_result.cpp` & `move_search.cpp`).
  - Record production StackRabbit (`eval.cpp` 2-ply + `precompute` reaction-time / `fullMoveSearch`) baselines on Level 19 and Level 18.
- **Phase 2 — Stationary Level 19 Flywheel (Sampler, Soft-Boundary MPI, NNUE & Inspector)**:
  - Port `ShardedVisitCounter` (uncorrelated shard/slot hash on 24B/32B keys), `FlatBoardIndex`, disk-streamed `PolicyActionStore`, `bestOutStore`, adaptive inner sweeps, warm-starting (`--init`), `SRSTV002`/`SRTBV003`, Soft-Boundary MC (`<= 3.5σ`), and 20-row `SRNNV001` (`featureVersion = 2`) multi-task trainer + calibrated partial-OUT search.
  - Seed Round 0 from `eval.cpp`, then drop `eval.cpp` and run 3–5 rounds of **Generate (`nn-error` eviction) → Soft-Boundary MPI → Retrain `128×64` / `256×64` NNUE** on Stationary Level 19.
  - Port the browser inspector (`server.cpp` / `index.html`) to 20×10 and test Level 19 inside `precompute.cpp` (`fullMoveSearch` + `dasPenaltyLookupMap` + reaction time) to verify whether 0-frame `V(b, c)` is well-calibrated or needs `V_RT(b, c)`.
- **Phase 3 — Stationary Level 18 Flywheel**:
  - Run the proven flywheel on Stationary Level 18 (discounted normalized score), verifying improvement over production StackRabbit on Level 18 Tetris rate and survival.
- **Phase 4 — Level 28 → 29 Endgame Transition (Backward Induction)**:
  - Run backward induction from `L = 229` down into Stationary Level 19 (keeping at most 5 slices `L..L+4` in RAM at once, terminating at Level 29 killscreen value `V_29` or `0` at `L >= 230`).
- **Phase 5 — Level 18 → 19 Transition (Backward Induction) & Unified Line-Count NNUE**:
  - Run backward induction from `L = 129` down into Stationary Level 18 (terminating cleanly into Phase 2's Stationary Level 19 values at `L >= 130`).
  - Distill all four regimes (Phases 2–5) into a single line-count-aware NNUE `V_nn(b, c, L)` (`~200–350 KB`) and benchmark full 230-line Level 18→19→29 games.
- **Phase 6 — Final Held-Out Evaluation, Integration Proposal & Human Retrospective**:
  - Head-to-head full-game evaluation against production StackRabbit (`run_agent`, `precompute` with reaction time, `docs/benchmark_hard_positions.md`) and write a human-readable retrospective following `AGENTS.md`'s doc style guide.

---

## 7. Non-Negotiable Quality Gates & Review Protocol

Carry over the exact discipline that kept 15+ consecutive phases bug-free on 4×10:

1. **Strict isolation**: Keep `src/` untouched (`git diff --stat -- src/` empty) throughout R&D. Only touch `src/` in a dedicated, user-approved integration phase, and when touching `src/` run the full change-scoped verification matrix in [AGENTS.md](../AGENTS.md) (`cpp_test_unit`, `precompute_static_lint`, `precompute_tsan`, `fuzz_verify_sequences`, `precompute_verify`, `wasm_build`).
2. **Per-phase compile & test gates**:
   - `npm run format_cpp` (always check `git status -s` before committing and revert any clang-format drift on untouched files).
   - Compile all new binaries with **`-Wall -Wextra -Werror`** AND **`-DNDEBUG -fsyntax-only`**.
   - Mandatory unit-test invariants:
     - Parity against `src/backend/` (board transitions, line clears, move lists, NES RNG).
     - `1T == 4T` bit-for-bit determinism (sampler, solver, NN training/inference).
     - `Ram == Disk` and `Warm == Cold` MPI solver parity; small-board brute-force VI parity across Hard, Discounted, and Soft-Boundary modes.
     - 6-block central finite-difference gradient check (`< 1e-4` relative error) on every NN parameter block.
     - Hostile-input & overflow tests (`NaN`/`±inf`, `numBoards * sizeof(Record)` overflow, unsorted/corrupt files, `--init == outPath` mmap truncation guard).
   - Automatic Soft-Boundary MC verification (`<= 3.5σ` vs. `E[V(0)]`) on every solve.
   - Strict validation/test split hygiene: tune margins/hyperparameters *only* on validation seeds; report final numbers *only* on untouched held-out test seeds.
3. **Subagent Review Cadence (After Every Code Phase)**:
   - Always run **2 reviewers in parallel**:
     - **`logic_reviewer`** with the **4-point seam checklist**: (1) cross-component tie-breaking/fallback parity, (2) scale & peak-RAM audit (`N = 10M–20M`), (3) adversarial/non-tautological unit-test audit, (4) `-DNDEBUG` & hostile-input audit.
     - **`readability_reviewer`** checking duplication, single-meaning API parameters, and end-state-only comments.
   - Add a **3rd Performance & Scale Reviewer** on every hot-path phase (20-row move gen, state sampler, Soft-Boundary MPI loop, NNUE training/inference — checking struct packing, ARM64 assembly/SIMD, stack probes, cache-line false sharing, and disk streaming bandwidth).
   - Watch for the **6 recurring seam-bug classes** documented in [docs/tablebase_retrospective.md](tablebase_retrospective.md#L239-L250) (tie-break drift, copy-paste duplication, overflow/sentinel/mmap truncation traps, hash-bit correlation / peak-RAM spikes, tautological unit tests, and `-DNDEBUG` / async UI races).

---

## 8. First-Turn Quick-Start Checklist

When you start the 20-row session:
1. Read the three retrospectives listed in Section 1 + [AGENTS.md](../AGENTS.md) + [docs/DAS_explainer.md](DAS_explainer.md).
2. Confirm the remaining Phase 0 parameters with the user (Stationary Level 19 is already locked in as the first solve target with DAS charge handled online via `dasPenaltyLookupMap`: confirm discount $\gamma$, target `reactionTime` frames for evaluation, and exact `inputFrameTimeline` / DAS-equivalent offline move rules).
3. Run the **Phase 0 revisit-rate & move-gen throughput spike** on `5,000–10,000` StackRabbit `eval.cpp` Level 19 games *before* designing the file format or solver — let real 20-row collision numbers decide `H_cap`, board budget, and Hybrid TB+NNUE vs. Standalone NNUE emphasis.
4. Write a concise, agent-targeted plan doc (`docs/tetris_20row_tablebase_plan.md`) pinning down the Phase 0 findings and Phases 1–6 (`Core → Stationary 19 → Stationary 18 → 28→29 transition → 18→19 transition + Unified NNUE → Final Eval & Retrospective`), then execute phase by phase with full quality gates and reviews.
