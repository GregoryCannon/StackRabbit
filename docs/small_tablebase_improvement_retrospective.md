# Small-Tablebase Improvement Retrospective — Closing the Gap Under a 2.5M-Board Cap

This retrospective explains how we took a small partial **Very Low Lineout** tablebase capped at **2.5 million boards** (`12.8%` of the full 19.54-million-board tablebase) from **67.59 lines** all the way to **102.17 lines** (pure supervised) and **103.26 lines** (with reinforcement learning) — beating the entire 19.54-million-board tablebase (**89.90 lines**) by **+12.3 lines** and reaching **98.2%–99.3%** of the 19.54-million-board oracle-plus-neural-network ceiling (**104.02 lines**), without ever touching a single value from any tablebase larger than 2.5 million boards during training or tuning.

Why did we impose this artificial 2.5M-board cap when we already had a 19.54M-board tablebase on 4×10? Because on a real **20-row Nintendo Entertainment System Tetris** board (Level 18 / Level 19 Delayed Auto Shift stacking), an exhaustive tablebase is impossible — any practical tablebase will always be a small partial slice of the full game. Testing every improvement under strict small-tablebase rules on our fast 4×10 testbed let us find out *which* ideas genuinely carry a partial tablebase, *why* they interact, and *which* complex ideas (like self-play reinforcement learning) are **not** worth bringing into the 20-row engine.

Everything was built in pure C++17 inside `experimental/tablebase/` (leaving StackRabbit's production `src/` engine completely untouched) and evaluated on the exact same **10,000 held-out games** (`seed=700000`) used in [docs/tablebase_retrospective.md](tablebase_retrospective.md) and [docs/tablebase_distillation_retrospective.md](tablebase_distillation_retrospective.md).

Companion docs:
- [docs/small_tablebase_improvement_plan.md](small_tablebase_improvement_plan.md) — technical design, phase checklists, and raw run logs for Phases S-1 through S-5.
- [docs/tablebase_distillation_retrospective.md](tablebase_distillation_retrospective.md) — prior work on linear heuristics, compact neural networks, and calibrated out-of-tablebase fallbacks.
- [docs/tablebase_retrospective.md](tablebase_retrospective.md) — original 4×10 Very Low Lineout tablebase pipeline and lessons for Level 18 / Level 19 Delayed Auto Shift stacking.

---

## Core Glossary (Used Throughout This Document)

- **TB (Tablebase)**: A precomputed lookup table storing the expected number of future lines cleared from every board position in its state set under optimal play.
- **NN (Neural Network / Neural Net)**: A compact learned function made of layers of weights and simple nonlinearities that estimates board values for any board — including boards not stored in the TB.
- **NNUE (Efficiently Updatable Neural Network)**: The architecture popularized by Stockfish chess: a tiny, CPU-cache-resident NN evaluated at the leaves of a fast tactical search.
- **MPI (Modified Policy Iteration)**: Our iterative Bellman solver that alternates between picking the best move for every board in the TB and propagating updated values through the TB until convergence.
- **RL (Reinforcement Learning)**: Training an NN from its own live self-play games and rewards rather than purely imitating a precomputed TB.
- **MC (Monte Carlo)**: Running thousands of random simulated games under a fixed policy to independently measure its true average return (used both to verify solver math and as an RL training target).
- **OOD (Out-of-Distribution / Out-of-Tablebase)**: Boards that do **not** exist in the small TB, where the agent must rely on its NN to generalize.
- **MAE (Mean Absolute Error)**: Average size of prediction mistakes (`|predicted value − oracle value|`), measured in lines.
- **NES (Nintendo Entertainment System)**: Here refers to the classic NES Tetris piece-randomizer rules, where repeating the previous piece is less likely than drawing a fresh piece.
- **DAS (Delayed Auto Shift)**: NES Tetris's horizontal piece-movement mechanic. Level 18 / Level 19 DAS stacking on a 20-row board is the real-game target motivating this project.
- **Ply**: One piece placement. A **2-ply** search places the current piece and the visible next piece; a **3-ply** search also averages over all 7 possible pieces that could appear after that.
- **Partial-OUT Scoring & Safety Margin (`m`)**: When a turn has *some* legal moves staying inside the TB and *some* leaving it, the agent compares in-TB moves directly against out-of-TB moves priced at `lines cleared + NN value − m`. Subtracting a small safety margin `m` stops the agent from leaving the TB just because of small NN prediction noise.
- **Held-Out Benchmark / Validation Split**: Every model's final score is measured on **10,000 held-out NES games (`seed=700000`)** never used during training or tuning. Hyperparameters (like `m` and search width) were tuned strictly on a separate **2,000-game validation split (`seed=30000000`)**.

---

## 1. Executive Summary: Rules, Results, and Five Headline Takeaways

### Strict Anti-Leakage Rules
To make sure every result transfers honestly to a 20-row game where no giant oracle exists, we enforced two hard rules across the entire project:
1. **Strict 2.5M board cap**: No state set or solved TB used by any agent could exceed **2,500,000 boards** (about `85.8 MB` on disk, `12.8%` of the 19.54M full TB).
2. **Zero leakage from the 19.54M TB**: Neither `nb_final_nes.tb.bin` nor any NN or TB derived from more than 2.5M boards was ever allowed inside state generation, solving, NN training, or hyperparameter tuning. We opened the 19.54M TB strictly as a **read-only diagnostic oracle** on already-frozen models to measure true calibration and verify there was no hidden value inflation.

### Master Ablation Progression (10,000 Held-Out NES Games, `seed=700000`)

Every row below obeys the `<= 2.50M` board cap and zero-leakage rules unless explicitly marked as a `19.54M` oracle reference.

| Stage & Configuration | TB Size | NN Size | Average Lines Cleared | Median / 90th %ile / Max | Speed (10 cores) | Gain vs. Baseline |
|---|---:|---:|---:|---|---:|---:|
| **Baseline** (`nb_mid_nes`, hard `V=0`, pure TB + 2-ply heur) | `2.49M` | — | `50.97 ± 0.48` | `35.0 / 114 / 494` | — | `-16.62` |
| **Baseline** (`nb_mid_nes`, standalone `64×32` self-NN 2-ply) | `0` | `31 KB` | `56.09 ± 0.54` | `39.0 / 127 / 603` | — | `-11.50` |
| **Baseline to Beat** (`nb_mid_nes` + `64×32` self-NN 2-ply, `m=1.0`) | `2.49M` | `31 KB` | **`67.59 ± 0.65`** | `47.0 / 151 / 623` | — | **`0.00`** |
| **Phase S-1**: Larger `128×64` NN (standalone 2-ply) | `0` | `79 KB` | `59.40 ± 0.58` | `42.0 / 136 / 579` | `17.9k mv/s` | — |
| **Phase S-1**: Larger `256×64` NN (standalone 2-ply) | `0` | `155 KB` | `61.25 ± 0.60` | `43.0 / 139 / 587` | `10.1k mv/s` | — |
| **Phase S-1**: `nb_mid_nes` + `256×64` NN 2-ply (`m=1.0`) | `2.49M` | `155 KB` | `69.24 ± 0.68` | `48.0 / 157 / 620` | `20.9k mv/s` | `+1.65` |
| **Phase S-1**: `nb_mid_nes` + `128×64` NN **3-ply Top-6** (`m=1.0`) | `2.49M` | `79 KB` | **`72.43 ± 0.71`** | `50.0 / 164 / 688` | `28.2k mv/s` | **`+4.84`** |
| **Phase S-2**: 5-Round **Soft-Boundary MPI** (`m=2.0`), standalone `128×64` 2-ply | `0` | `79 KB` | `68.28 ± 0.65` | `48.0 / 155 / 676` | `17.4k mv/s` | — |
| **Phase S-2**: 5-Round **Soft-Boundary MPI** + `128×64` 2-ply (`m=2.0`) | `2.49M` | `79 KB` | **`89.97 ± 0.88`** | `63.0 / 206 / 820` | `38.2k mv/s` | **`+22.38`** |
| **Phase S-2**: 5-Round **Soft-Boundary MPI** + `128×64` **3-ply Top-6** (`m=2.0`) | `2.49M` | `79 KB` | **`92.48 ± 0.90`** | `65.0 / 208 / 874` | `21.5k mv/s` | **`+24.89`** |
| **Phase S-3**: Hard `nb_mid_nes` + RL-tuned `128×64` (standalone 2-ply) | `0` | `79 KB` | `63.21 ± 0.60` | `45.0 / 142 / 573` | — | — |
| **Phase S-3**: Soft-5 TB + RL-tuned `128×64` 3-ply Top-6 (`m=2.0`) | `2.49M` | `79 KB` | `98.67 ± 0.95` | `70.0 / 224 / 740` | `12.7k mv/s` | `+31.08` |
| **Phase S-4**: `2.50M` `nn-error` Soft-TB + Soft-5 `128×64` 3-ply Top-6 (`m=2.0`) | `2.50M` | `79 KB` | `93.95 ± 0.92` | `65.0 / 214 / 844` | `27.9k mv/s` | `+26.36` |
| **Phase S-4**: `2.50M` `nn-error` Soft-TB + RL `128×64` 3-ply Top-6 (`m=2.0`) | `2.50M` | `79 KB` | `100.29 ± 1.00` | `70.0 / 230 / 1182` | `20.4k mv/s` | `+32.70` |
| **Phase S-5 (Final, NO RL)**: Standalone `128×64` NN 2-ply | `0` | `79 KB` | `69.89 ± 0.67` | `50.0 / 159 / 729` | `26.4k mv/s` | — |
| **Phase S-5 (Final, NO RL)**: Standalone `256×64` NN 2-ply | `0` | `155 KB` | **`73.04 ± 0.70`** | `51.0 / 165 / 723` | `13.7k mv/s` | — |
| **Phase S-5 (Final, NO RL)**: Standalone `128×64` NN 3-ply Top-6 (`2k` games) | `0` | `79 KB` | `86.49 ± 1.85` | `60.0 / 200 / 687` | `0.8k mv/s` | — |
| **Phase S-5 (Final, NO RL)**: `s5_final` + `128×64` 2-ply (`m=1.0`) | `2.50M` | `79 KB` | **`97.00 ± 0.95`** | `68.0 / 222 / 990` | **`48.2k mv/s`** | **`+29.41`** |
| **Phase S-5 (Final, NO RL)**: `s5_final` + `256×64` 2-ply (`m=0.5`) | `2.50M` | `155 KB` | **`98.88 ± 0.98`** | `69.0 / 226 / 936` | `23.3k mv/s` | **`+31.29`** |
| **Phase S-5 (Final, NO RL, Recommended)**: `s5_final` + `128×64` **3-ply Top-6** (`m=0.5`) | **`2.50M`** | **`79 KB`** | **`102.17 ± 1.01`** | **`71.0 / 232 / 903`** | **`22.4k mv/s`** | **`+34.58`** |
| **Phase S-5 (Final, +RL Comparison)**: `s5_final` + RL `128×64` 2-ply (`m=4.5`) | `2.50M` | `79 KB` | `91.79 ± 0.91` | `63.0 / 210 / 974` | `43.1k mv/s` | `+24.20` |
| **Phase S-5 (Final, +RL Comparison)**: `s5_final` + RL `128×64` 3-ply Top-6 (`m=3.5`) | `2.50M` | `79 KB` | `103.26 ± 1.01` | `71.0 / 237 / 988` | `18.4k mv/s` | `+35.67` |
| *Oracle Reference*: Full `19.54M` TB alone (all-OUT heur fallback) | `19.54M` | — | *`89.90 ± 0.88`* | *`63.0 / 203 / 921`* | *`2.47M mv/s`* | *`+22.31`* |
| *Oracle Reference*: Full `19.54M` TB + `19.54M`-trained `128×64` NN 2-ply | `19.54M` | `79 KB` | *`104.02 ± 1.02`* | *`74.0 / 235 / 993`* | — | *`+36.43`* |

### Five Headline Takeaways
1. **Soft-Boundary MPI (Phase S-2) is by far the single largest breakthrough (`67.59 → 89.97` lines on the exact same 2.49M boards).** Replacing the solver's fake "exiting the TB equals instant death (`V = 0`)" boundary with a conservative NN estimate (`V_nn − 2.0`) fixes the corrupted TB values *at the source*, lifting both the TB and every NN trained from it without adding a single board.
2. **A `2.50M`-board TB + `79 KB` pure-supervised NN reaches `102.17` lines — zero RL required.** Combining larger NNUE capacity + Soft-Boundary MPI + `nn-error` state selection + calibrated 3-ply Top-6 search beats the entire `19.54M` full TB (`89.90`) by **`+12.27` lines** and hits **`98.2%`** of the `19.54M` oracle+NN ceiling (`104.02`). Even without 3-ply, `s5_final` + `256×64` reaches **`98.88` lines in pure 2-ply**!
3. **Self-play RL (Phase S-3) is NOT worth the complexity for production.** While RL initially looked like a `+6`-line breakthrough (`93.95 → 100.29` at fixed `m = 2.0`), a controlled margin sweep proved almost all of that gain was a hidden **value-scale / margin confounder**: RL shifted NN predictions up by `+2` to `+4` lines, which lowered the effective margin at runtime. Once we matched margins fairly on `s5_final`, pure supervised `128×64` **beat RL by `+5.21` lines in 2-ply (`97.00` vs. `91.79`)** and tied it within noise in 3-ply (`102.17` vs. `103.26`, `0.76σ`), with none of RL's fragility or code complexity.
4. **Targeted state selection (`nn-error` and `hole-boost`, Phase S-4) ONLY works once you have Soft-Boundary MPI.** Under hard `V = 0` boundaries, evicting common 0-hole boards to store messy 2-hole boards caused a **`-14.54`-line collapse** (`49.72 → 35.18`). Under Soft-Boundary MPI, the NN smoothly patches the missing easy boards and `hole-boost` completely flipped to **`+1.8` lines over pure visits**, while **`nn-error`** won cleanly (`+1.6` to `+3.8` lines over pure visits) by surgically targeting the NN's blind spots across all hole counts.
5. **The small-TB flywheel even produced a stronger standalone NN than the 19.54M TB did (`73.04` vs. `71.79` lines in 2-ply)!** Trained strictly on our `2.50M`-board Soft-Boundary TB (`s5_final`), a `155 KB` `256×64` standalone NN reached **`73.04 ± 0.70` lines** with zero TB lookups at runtime — beating the `19.54M`-trained `128×64` NN (`71.79`) from our earlier project.

---

## 2. Phase S-1: Larger NNUE & 3-Ply Top-K Out-of-TB Search

### Section Glossary
- **Expectimax**: A game-tree search that alternates between *maximizing* over the player's legal moves and taking the *expected value* (probability-weighted average) over random future piece spawns.
- **Top-K Beam Pruning**: Instead of running an expensive deeper search on all ~25 legal moves on a turn, first score all moves with a fast 2-ply pass and only expand the top `K` candidates to 3-ply.
- **2-Ply Pre-Screening (Slack Pruning)**: Before running 3-ply on an out-of-TB move during hybrid TB+NN play, check whether its fast 2-ply score is even within `6.0` lines of the best in-TB move. If it is already way behind, skip 3-ply completely.

### Why We Started With Inference Capacity Before Touching Training
Before changing how tablebases are solved or generated, we wanted a clean baseline of how much strength was sitting on the table purely from giving the out-of-TB fallback a smarter brain and one extra ply of tactical vision — holding the existing legacy `2.49M` hard-`V=0` tablebase (`nb_mid_nes.tb.bin`) fixed.

1. **Wider NNUE (`64×32 → 128×64 → 256×64`)**:
   - On `nb_mid_nes`, widening the NN steadily improved standalone 2-ply play: **`56.09 → 59.40 → 61.25` lines** (`+5.16` lines), with 1-ply regret dropping `0.63 → 0.50 → 0.37`.
   - However, when paired with the hard-`V=0` TB in 2-ply (`m = 1.0`), the jump from `128×64` to `256×64` was only **`+0.24` lines** (`69.00 → 69.24`, well within `± 0.68` statistical noise) because the NN was still imitating `nb_mid_nes`'s distorted `V = 0` values (`E[V(0)] = 43.07` instead of `86.85`).
   - **Why we used `128×64` for Phases S-2 through S-4 (and brought `256×64` back in Phase S-5):** Doubling the first hidden layer (`128×64 → 256×64`) made both training (`~60s → ~115s`) and inference/rollouts nearly **2× slower** while adding virtually no extra TB+NN signal in Phase S-1. Standardizing on `128×64` kept our multi-round Soft-Boundary MPI sweeps, RL ablations, and 320,000-game state generations twice as fast and strictly comparable across phases. Once the final `2.50M` TB (`s5_final`) was ready in Phase S-5, we trained and benchmarked **`256×64`** head-to-head against **`128×64`** — revealing that `256×64` shines in **standalone play (`73.04` vs. `69.89`) and 2-ply hybrid play (`98.88` vs. `97.00`)**, while `128×64` at 3-ply Top-6 beats `256×64` at 2-ply (`102.17` vs. `98.88`) at the exact same runtime speed (`~22–23k moves/sec`).
2. **3-Ply Top-K Out-of-TB Expectimax (`K = 6`)**:
   - At 2-ply, the agent knows the current piece `c` and the visible next piece `n`, and evaluates the NN after placing `n`.
   - At 3-ply, for each of the top `K` placements of `c` (and each placement of `n`), the agent averages over all 7 possible pieces `n₂` that could spawn next (using the exact NES transition probabilities `P(n₂ | n)`), picking the best placement of `n₂` and calling the NN at ply 3.
   - Because 3-ply only runs when a turn actually considers out-of-TB moves — and our **2-ply pre-screening slack (`6.0` lines)** skips hopeless out-of-TB moves immediately — adding 3-ply Top-6 gave a **`57×` speedup** over naive 3-ply (`28.2k moves/sec` across 10 cores) while adding **`+3.43` lines** on `nb_mid_nes` (`69.00 → 72.43` lines, `+4.84` over the `67.59` baseline).

### Key Takeaway From Phase S-1
A bigger NN and 3-ply search help immediately (`+4.84` lines), **but they cannot cure a poisoned tablebase**. As long as the `2.49M` TB itself believes leaving the TB equals `0` lines, the TB's own internal move preferences remain badly warped — which set the stage for Phase S-2.

---

## 3. Phase S-2: Soft-Boundary Value-Bootstrapped MPI (The Big Breakthrough)

### Section Glossary
- **Hard Boundary (`V = 0`)**: The classic closed-world TB solver assumption: any move that lands on a board outside the TB receives continuation value `0.0` (as if the player topped out immediately).
- **Soft Boundary (`max(0, V_nn − m)`)**: Replacing `0.0` on out-of-TB moves with a conservative frozen NN estimate minus a safety margin `m`. Inside the Bellman equation, exiting the TB becomes a finite terminal payoff rather than instant death.
- **Bellman Backup**: One update step of dynamic programming: `V(b, c) = E_n [ max_move (lines cleared + continuation value) ]`.
- **Fixed-Point / Geometric Convergence**: Repeatedly feeding each round's NN back into the next round's Soft-Boundary MPI solve. If the margin `m` is too small, positive NN errors could feed on themselves and explode upward ("value inflation"); if `m` is properly chosen, each round's jump shrinks by a constant factor (e.g., `~0.72×`) and settles cleanly below true value.
- **Soft-Boundary MC Verification**: An independent simulation check where the solved policy plays thousands of random games and immediately terminates with the exact quantized NN boundary payoff whenever it exits the TB. If the solver has zero bugs, the simulated average must match `E[V(0)]` within statistical noise (`<= 3.5σ`).

### Why Hard `V = 0` Poisoned the 2.49M Tablebase
In our previous project, `nb_mid_nes.tb.bin` (`2.49M` boards) had a theoretical empty-board value of only **`E[V(0)] = 43.07`**, even though true optimal play on 4×10 is **`86.85` lines** — half the game's value had vanished!

Why? Because in a 2.49M partial TB, roughly `3%–5%` of moves in a messy dig cross outside the TB. Under hard `V = 0`, the solver treats every one of those exits as instant death. That creates two cascading distortions:
1. **Terrified internal play**: Near the boundary (especially on 1-hole and 2-hole boards), the TB makes terrible, panicked moves just to stay on a memorized board rather than taking a clean dig that steps onto an unmemorized board.
2. **Corrupted training targets**: Any NN distilled from `nb_mid_nes` learns that boards near the boundary are worth far less than they really are (`OOD MAE = 42.43` lines!).

### How Soft-Boundary MPI Fixed It (Without Adding a Single Board!)
Instead of assigning `0.0` when a move exits the TB, we modified `solveNextBoxMPI` to evaluate a frozen NN on out-of-TB child boards and assign the conservative terminal payoff:
$$B(b', n) = \max\big(0,\; \hat{V}_{\text{NN}}(b', n) - m\big)$$
with a safety margin `m = 2.0` lines (and a hard divergence guard `maxV <= 1000` plus optional discount factor `gamma` as safety rails).

**The Engineering Trick That Made It 15× Faster:**
Evaluating an NN on every out-of-TB child inside every Bellman sweep would have made a 2.5M-board solve take 20+ minutes. But the boundary NN is **frozen** during a solve — so for every `(board, curPiece, nextPiece)`, the best out-of-TB move and its terminal payoff **never change from iteration 1 to iteration 25**! By precomputing and storing that single best out-of-TB packed 32-bit word (`bestOutStore`, using 20 fractional bits in `[0, 2047.0]`) on outer iteration 1 and reusing it on every subsequent iteration and residual check, a full `2.49M`-board Soft-Boundary MPI solve dropped to **~80 seconds** on 10 cores — with bit-exact parity against naive recomputation!

### What Happened Across 5 Fixed-Set Bootstrap Rounds (`nb_mid_nes`, Exact Same `2.49M` Boards)
We ran 5 rounds of **Solve Soft-Boundary MPI (`m = 2.0`) → Retrain `128×64` NN on the new TB → Repeat**, never changing a single board in `nb_mid_nes` and never touching the 19.54M TB:

| Bootstrap Round (`m = 2.0`, Fixed `2.49M` Boards) | `E[V(0)]` | Round Gain | Soft-MC Check | Oracle OOD MAE (Read-Only) | Standalone `128×64` 2-Ply | TB + NN 2-Ply (`m=2.0`) | TB + NN 3-Ply Top-6 (`m=2.0`) |
|---|---:|---:|---:|---:|---:|---:|---:|
| Round 0 (Hard `V = 0` `nb_mid_nes`) | `43.07` | — | `0.21σ` | `38.52` | `59.40 ± 0.58` | `69.00 ± 0.67` (`m=1`) | `72.43 ± 0.71` (`m=1`) |
| Soft Round 1 | `50.94` | `+7.87` | `0.07σ` | — | — | — | — |
| Soft Round 2 | `55.89` | `+4.95` | `1.62σ` | — | — | — | — |
| Soft Round 3 | `59.33` | `+3.44` | `0.87σ` | — | — | — | — |
| Soft Round 4 | `61.94` | `+2.61` | `1.04σ` | — | — | — | — |
| **Soft Round 5 (`mid_soft5_m2.0`)** | **`63.85`** | **`+1.91`** | **`0.61σ`** | **`20.12`** | **`68.28 ± 0.65`** | **`89.97 ± 0.88`** | **`92.48 ± 0.90`** |

Look at those round-over-round deltas: `+7.87 → +4.95 → +3.44 → +2.61 → +1.91`. Every round contracted cleanly by `~0.72×` with **zero value inflation** (every Soft-MC check passed within `0.07σ–1.62σ`, and `E[V(0)] = 63.85` stayed well below the true `86.85`). Meanwhile:
- The `2.49M` Soft-TB + `128×64` NN jumped **`+22.38` lines** (`67.59 → 89.97`) in 2-ply — **matching the entire `19.54M` full TB (`89.90`) on `12.7%` of the boards!**
- Adding 3-ply Top-6 pushed it to **`92.48 ± 0.90` lines** (`+24.89` over baseline).
- Even the standalone `128×64` NN (zero TB at playtime) jumped **`+8.88` lines (`59.40 → 68.28`)** purely because its `2.49M` teacher stopped giving panicked `V = 0` labels!
- (We also tested a more conservative `m = 4.0` for 3 rounds: it converged smoothly too, `43.07 → 49.62 → 53.27 → 55.85`, reaching `84.62` in 2-ply — proving `m = 2.0` is the sweet spot: tight enough for fast progress, conservative enough to prevent any inflation.)
- One crucial nuance: **pure Soft-TB with heuristic fallback** stayed at `51.12 ± 0.49` (essentially identical to hard TB's `50.97`). Why? Because the Soft-TB was deliberately solved assuming a smart NN would catch it when it steps off the edge — if you swap the NN out for a dumb heuristic at runtime, stepping off the edge fails. **Soft-Boundary TB and its NN are a co-designed pair.**

---

## 4. Phase S-3: Self-Play RL & Expert Iteration — And Why We Cut It

### Section Glossary
- **TD(λ) (Temporal-Difference Lambda)**: An RL algorithm that blends short-horizon bootstrapped value estimates (`λ = 0`) with full end-of-game Monte Carlo returns (`λ = 1`) using an exponential decay factor `λ` (e.g., `λ = 0.95`).
- **Expert Iteration**: Using a slow, strong search (here, 2-ply NN expectimax) as an on-the-fly "teacher" during self-play games, and training the fast 1-ply NN to imitate that 2-ply search's rankings and values.
- **Polyak Target Averaging**: Blending new NN weights slowly into a separate "target" network (`w_target ← (1 − τ)·w_target + τ·w`) using an exponential moving average to stabilize RL value updates.
- **Huber Loss**: A smooth training loss that acts like squared error for small mistakes (`|err| <= δ`) and like absolute error for large mistakes (`|err| > δ`), preventing outlier games from blowing up gradients.
- **TB Replay Anchor**: Mixing `25%` supervised move-ranking samples from the `<= 2.5M` TB into every RL mini-batch so the NN does not catastrophically forget clean stacking patterns while exploring.
- **Scale Mismatch / TB-Fleeing**: A failure mode where the NN predicts systematically higher values than the TB (`V_nn > V_tb`). Even after subtracting a safety margin, out-of-TB moves look artificially better than in-TB moves, causing the agent to abandon the TB unnecessarily.

### What We Built and What Worked vs. Failed in Isolation
We implemented a full, deterministic, multithreaded RL fine-tuner ([experimental/tablebase/rl_finetune.hpp](file:///Users/gregcannon/Documents/StackRabbit/experimental/tablebase/rl_finetune.hpp), [experimental/tablebase/rl_finetune_nn.cpp](file:///Users/gregcannon/Documents/StackRabbit/experimental/tablebase/rl_finetune_nn.cpp)) with an exact 2-ply memoization cache (`Eval2PlyCache`, `10×` faster rollouts), exact backward `TD(λ)` returns, 2-ply Expert Iteration child + ranking losses, Polyak target averaging, and an enforced `<= 2.5M` TB replay anchor.

Testing RL components in isolation on the weak hard-`V=0` baseline (`mid_self_128x64`, `59.40` standalone) uncovered three fascinating failure modes and lessons:

1. **Why Pure Monte Carlo (`TD(1.0)`) Failed Twice — For Two Totally Different Reasons**:
   - With `6%` random exploration (`ε = 0.06`), random exploratory mistakes cut average rollout survival from `57` lines down to `21` lines — which pulled the NN's value predictions down, which made play even worse, triggering an exploratory death spiral (**`5.19` lines** on validation!).
   - Removing exploration (`ε = 0.0`) stopped the collapse, yet the NN stayed completely flat (`58.72` lines, `E[V(0)] = 38.20` — *below* its starting `43.05`!). Why? **Tetris survival lengths follow a heavy-tailed exponential distribution** (median `~40`, mean `~59`). Huber loss (`δ = 2.0`) acts like absolute-error loss on large errors, and **absolute-error loss mathematically optimizes the median (`~40`), not the mean (`~59`)!**
2. **Combined Expert Iteration + `TD(0.95)` + Rank-Only TB Anchor Worked Cleanly Standalone**:
   - On the weak hard-`V=0` NN (`59.40`), combining 2-ply Expert Iteration with `TD(0.9)`–`TD(0.95)` and a rank-only `2.49M` TB anchor lifted standalone 2-ply play to **`63.21 ± 0.60` (`+3.81` lines)** and lifted `E[V(0)]` from `43.05 → 55.02` (cutting oracle OOD MAE `42.43 → 30.65`).
3. **The Scale-Mismatch Law (Why RL Cannot Be Paired With an Un-Updated Hard TB)**:
   - When we paired that scale-lifted RL NN (`E[V(0)] = 55.02`) directly with the un-updated hard `nb_mid_nes` TB (`E[V(0)] = 43.07`) at `m = 1.0–2.0`, gameplay **crashed from `69.00` down to `12.30–13.74` lines** (`in-TB` collapsed to `43.7%`)! Because the NN was `+12` lines higher than the TB, every out-of-TB move looked `+10` lines better than staying inside the TB, so the agent actively fled the TB.

### The Plot Twist: The Margin Confounder That Changed Our Mind on RL
When we first applied RL on top of our Phase S-2 Soft-5 model (`mid_soft5_m2.0_128x64` → `mid_soft5_rl_128x64`), we initially saw an exciting jump in **3-ply Top-6 at fixed `m = 2.0`**:
- On `mid_soft5_m2.0`: `92.48` (supervised) → **`98.67`** (RL, `+6.19` lines)
- On Phase S-4 `s4_cap_nnerr_soft`: `93.95` (supervised) → **`100.29`** (RL, `+6.34` lines)

Yet something looked suspicious: on that exact same Soft-5 model, RL gave **`0.0` lines** standalone (`68.28 → 68.26`) and **hurt** 2-ply (`89.97 → 88.41`). Why would RL suddenly add `+6` lines only in 3-ply?

We spotted the hidden confounder: **RL had lifted `V_nn` by `+2.0` lines (`63.85 → 65.88`), while the non-RL NN was still sitting `2.0` lines below `s4_cap_nnerr_soft`'s `65.89`!** Evaluating both at fixed `--out-margin=2.0` meant the supervised NN was carrying an effective `~4.0`-line penalty against out-of-TB moves, whereas the RL NN was effectively running at `margin ≈ 0.0`!

To test this cleanly in Phase S-5, we trained a fresh **pure-supervised `128×64` NN** directly on the same TB (`s5_final.tb.bin`) and swept `--out-margin` separately on the validation split (`seed=30000000`) for **both** pure supervised and RL, then evaluated each at its own optimal margin on the `10,000`-game held-out test (`seed=700000`):

| Model on `s5_final.tb.bin` (`2.50M` Boards) | Optimal Margin (Validation) | Standalone 2-Ply | TB + NN 2-Ply (10k) | TB + NN 3-Ply Top-6 (10k) | Margin Stability (Validation 2-Ply) |
|---|---:|---:|---:|---:|---|
| **Pure Supervised `128×64` (NO RL)** | `1.0` (2p) / `0.5` (3p) | **`69.89 ± 0.67`** | **`97.00 ± 0.95`** | **`102.17 ± 1.01`** | Rock-solid (`93.2–96.8` across `m=0.5..2.0`) |
| **Pure Supervised `256×64` (NO RL)** | `0.5` (2p) / `1.0` (3p) | **`73.04 ± 0.70`** | **`98.88 ± 0.98`** | `101.20 ± 0.99` | Rock-solid (`97.1–102.1` across `m=0.5..2.0`) |
| **RL-Finetuned `128×64`** | `4.5` (2p) / `3.5` (3p) | `69.55 ± 0.67` | `91.79 ± 0.91` (**`-5.21`**) | `103.26 ± 1.01` (`+1.09`, `0.76σ`) | Fragile (`72.5` at `m=1.5`, needs `m=3.5–4.5`) |

**Verdict on Self-Play RL:** Once Soft-Boundary MPI is already lifting TB and NN values cleanly, **self-play RL does not earn its engineering cost.** Pure supervised distillation beat RL by `+5.21` to `+7.09` lines in 2-ply, tied it within statistical noise (`0.76σ`) in 3-ply, achieved higher oracle top-1 agreement (`86.37%` vs. `85.76%`), and stayed naturally scale-aligned with the TB without fragile margin compensation. **We recommend omitting RL from the 20-row DAS 18/19 production pipeline.**

---

## 5. Phase S-4: Error-Weighted & Hole-Boosted `<= 2.5M` State Selection

### Section Glossary
- **Protected Core (`--protected-core`)**: Always reserving a fixed slice of the `2.5M` budget (here, `500,000` boards) for the highest-visit boards regardless of priority score, so the opening and core Bellman paths stay tightly connected.
- **NN-Error Priority (`--priority-mode=nn-error`)**: Ranking unprotected candidate boards by `visits × (1 + w · min(10, |E[V_tb] − E[V_nn]|))` for in-TB boards, and `visits × (1 + 2.0)` for newly discovered out-of-TB frontier boards. Easy boards the NN already nails get evicted first; boards where the NN is surprised get kept.
- **Hole-Boost Priority (`--priority-mode=hole-boost`)**: Ranking unprotected candidate boards by `visits × (1 + s · holeWeight)`, weighting 1-hole (`1.0`), 2-hole (`2.0`), and 3+-hole (`1.5`) boards heavily over 0-hole (`0.0`) boards.

### Why Raw Visit Counts Waste Precious Tablebase Memory
In a small `2.5M`-board TB, pure visit-count selection spends a huge share of its slots on common, clean 0-hole boards — the exact positions our NN already predicts within `~1.4` lines. Meanwhile, rare-but-critical 1-hole, 2-hole, and tricky 0-hole dependency positions get evicted, forcing the NN to improvise in the exact spots where it is weakest.

### The Hard-vs-Soft MPI Interaction Hypothesis — Proven 100%
We generated **three exact `2,500,000`-board sets** from the **exact same `320,000`-game Soft-5 rollout** (`seed=4000020`, `ε = 0.16`, `top-k = 8`, `~3.4M` candidate boards trimmed down to `2.50M` using `protectedCore = 500,000`) and solved every single set under **both Hard `V = 0` MPI and Soft-Boundary MPI (`m = 2.0`)**:

| Eviction Mode (`2.50M` Exact Cap, Identical Rollout) | Hole Split (`0h / 1h / 2h / 3+h`) | Hard `V=0` MPI `E[V(0)]` / Live Heur | Soft-MPI (`m=2.0`) `E[V(0)]` (`Soft-MC`) | Soft-TB + Soft-5 NN 2-Ply (`m=2.0`) | Soft-TB + Soft-5 NN 3-Ply (`m=2.0`) | Soft-TB + RL NN 3-Ply (`m=2.0`) |
|---|---|---:|---:|---:|---:|---:|
| **`visits`** (control) | `628k / 868k / 526k / 478k` | `35.27` / `49.72` | `66.44` (`1.93σ`) | `88.44 ± 0.87` | `91.65 ± 0.90` | `96.52 ± 0.94` |
| **`hole-boost`** (`scale=2.0`) | `237k / 502k / 769k / 992k` | **`22.18` / `35.18` (`-14.54`)** | `64.15` (`0.17σ`) | `88.35 ± 0.85` | **`93.43 ± 0.91` (`+1.78`)** | **`98.33 ± 0.95` (`+1.81`)** |
| **`nn-error`** (`w=2.0`) — **Winner** | `642k / 885k / 529k / 445k` | `34.83` / `49.41` | `65.89` (`1.49σ`) | **`90.08 ± 0.87` (`+1.64`)** | **`93.95 ± 0.92` (`+2.30`)** | **`100.29 ± 1.00` (`+3.77`)** |

Two crystal-clear discoveries jumped straight out of that table:
1. **Soft-Boundary MPI completely flips the economics of state selection.** Under Hard `V = 0` MPI, `hole-boost` threw away `~400k` 0-hole boards and immediately **collapsed by `-14.54` lines** (`49.72 → 35.18`) because every missing 0-hole board became an instant-death cliff. Under Soft-Boundary MPI + 3-ply NN, those missing easy 0-hole boards were smoothly patched by the NN, and `hole-boost` **reversed to beat `visits` by `+1.78` to `+1.81` lines!**
2. **`nn-error` beat both `visits` AND `hole-boost` across every single configuration.** Why? Because `hole-boost` blindly evicted useful 0-hole and 1-hole boards (dropping live TB hit rate from `95.9%` down to `92.6%` and slowing 3-ply from `27.9k` to `17.4k moves/sec`), whereas `nn-error` surgically kept high-`|V_tb − V_nn|` boards across **all** hole buckets — keeping tricky 0-hole dependencies while evicting easy ones! That preserved `96.0%` TB hit rate while gaining **`+1.64` (2-ply) / `+2.30` (3-ply) / `+3.77` (+RL 3-ply)** over pure visits.

*(We also verified clean bootstrapping from scratch starting strictly from the tiny `673k` `nb_small_nes` TB + `31 KB` `64×32` NN with zero legacy `nb_mid_nes` boards: `s4_fresh_visits` (`2.22M` boards) reached `90.83 ± 0.88` lines in 1 round of Soft-MPI + 3-ply, proving the pipeline needs no heuristic-seeded legacy data.)*

---

## 6. Phase S-5: Combined `<= 2.5M` Flywheel & Read-Only Oracle Calibration Audit

### Section Glossary
- **Calibration / Signed Bias (`predicted − oracle`)**: The average signed difference between our `<= 2.5M` model's predicted line count and the true `19.54M` oracle value. A negative bias means our model is *conservatively under-estimating* true value (safe); a positive bias would signal dangerous value inflation.
- **Top-1 Move Agreement**: Percentage of oracle gameplay turns on which our `<= 2.5M` model chooses the exact same placement as the `19.54M` full TB.

### The Clean End-to-End Zero-RL Combined Flywheel
Combining the three clean winners — **Soft-Boundary MPI (Phase S-2) + `nn-error` `2.50M` State Selection (Phase S-4) + Wider NNUE & 3-Ply Top-6 (Phase S-1)** — we ran two clean flywheel refinement rounds (strictly `<= 2,500,000` boards, zero leakage, zero RL):
1. Retrained `128×64` supervised NN (`s5_r1_128x64`) on `s4_cap_nnerr_soft` (`2.50M`) → solved `s5_r1.tb.bin` (`E[V(0)] = 67.26`, Soft-MC `1.19σ`).
2. Regenerated a refined `2,500,000`-board `nn-error` state set (`s5_r2_nnerr.states.bin`, `ε = 0.14`, `w = 2.0`, `protectedCore = 500k`) — pushing live messy-board coverage up sharply (`2h`: `55.6% → 62.6%`, `3+h`: `31.2% → 43.1%`).
3. Solved two rounds of Soft-Boundary MPI (`m = 2.0`) on `s5_r2_nnerr.states.bin` → **`s5_final.tb.bin`** (`2,500,000` boards, **`E[V(0)] = 70.37`**, 30,000-game Soft-MC `70.26 ± 0.17`, **`0.61σ`** — zero inflation!).
4. Trained final pure-supervised **`s5_final_128x64.nn.bin` (`78.6 KB`)** and **`s5_final_256x64.nn.bin` (`155.1 KB`)** on `s5_final.tb.bin`, tuned `--out-margin` strictly on the 2,000-game validation split (`seed=30000000`), and evaluated on the official `10,000`-game held-out NES test (`seed=700000`):
   - **Pure Supervised `128×64`, 2-ply (`m = 1.0`)**: **`97.00 ± 0.95` lines** (`48.2k moves/sec`, `95.02%` in-TB).
   - **Pure Supervised `256×64`, 2-ply (`m = 0.5`)**: **`98.88 ± 0.98` lines** (`23.3k moves/sec`, `94.98%` in-TB).
   - **Pure Supervised `128×64`, 3-ply Top-6 (`m = 0.5`)**: **`102.17 ± 1.01` lines** (`med = 71.0, p90 = 232, max = 903`, `22.4k moves/sec`, `95.37%` in-TB) — **`98.2%` of the `19.54M` oracle+NN ceiling (`104.02`) using `12.8%` of the boards!**
   - **Standalone `256×64` NN (zero TB at playtime!)**: **`73.04 ± 0.70` lines** in 2-ply (beating the `19.54M`-trained `128×64` NN `71.79`!), and standalone `128×64` reached **`86.49 ± 1.85` lines** in 3-ply Top-6 (`2k` games).

### Read-Only Oracle Calibration Audit (20,000 Held-Out `19.54M` Oracle Decisions, `seed=91000000`)
To verify that our `102.17`-line result came from genuinely improved accuracy rather than hidden value inflation, we audited our frozen models (read-only) against `nb_final_nes.tb.bin` (`19.54M` boards) across `20,000` held-out oracle game decisions:

| Frozen Model Audited (Read-Only vs. `19.54M` Oracle) | Decisions Checked | Overall MAE | Mean Signed Bias | Overall Top-1 Agreement | Top-1 by Holes (`0h / 1h / 2h / 3+h`) |
|---|---:|---:|---:|---:|---|
| Legacy `nb_mid_nes` TB (`2.49M`, hard `V=0`) | `18,995` (in-TB) | `43.29` | `-43.29` | `90.18%` | `90.3% / 88.3% / 93.1% / 96.4%` |
| Phase S-2 `mid_soft5_m2.0` TB (`2.49M`, Soft-5) | `18,995` (in-TB) | `22.55` | `-22.55` | `92.41%` | `92.6% / 90.0% / 92.5% / 96.4%` |
| **Phase S-5 `s5_final` TB (`2.50M`, Soft-MPI + `nn-error`)** | `19,042` (in-TB) | **`16.44`** | **`-16.44`** | **`93.75%`** | **`94.0% / 90.6% / 94.4% / 96.0%`** |
| Baseline `nb_mid_self_64x32` NN (standalone 2-ply) | `20,000` (all) | `44.06` | `-44.04` | `84.55%` | `84.0% / 87.7% / 90.6% / 92.2%` |
| **Phase S-5 `s5_final_128x64` NN (`79 KB`, pure supervised)** | `20,000` (all) | **`17.29`** | **`-17.14`** | **`86.37%`** | **`86.0% / 88.8% / 89.4% / 93.6%`** |
| **Phase S-5 `s5_final_256x64` NN (`155 KB`, pure supervised)** | `20,000` (all) | **`17.21`** | **`-17.12`** | **`86.69%`** | **`86.3% / 89.2% / 89.1% / 94.3%`** |
| Phase S-5 `s5_final_rl_128x64` NN (`79 KB`, RL) | `20,000` (all) | `13.09` | `-12.83` | `85.76%` | `85.3% / 88.4% / 90.0% / 95.0%` |
| **Hybrid `s5_final` + `128×64` (2-ply, `m=1.0`)** | `20,000` (all) | **`16.63`** | **`-16.58`** | **`94.62%`** | **`94.9% / 92.3% / 91.8% / 95.7%`** |
| **Hybrid `s5_final` + `256×64` (2-ply, `m=0.5`)** | `20,000` (all) | **`16.59`** | **`-16.55`** | **`94.82%`** | **`95.1% / 92.6% / 91.2% / 94.3%`** |

Every single number tells a coherent, rock-solid story:
- **Zero value inflation**: `s5_final` stays conservatively `-16.44` lines below the `19.54M` oracle on average (`-16.57` on 0h, `-15.42` on 1h, `-14.44` on 2h, `-15.58` on 3+h), yet cut value error by **`2.63×`** (`43.29 → 16.44`)!
- **Near-oracle move quality on `12.8%` of the boards**: Our `2.50M` hybrid agrees with the exact `19.54M` oracle on **`94.6%–94.8%`** of all moves (`95.1%` on 0-hole boards, `92.6%` on 1-hole digs, `95.7%` on 3+-hole scrambles) — and on the remaining `5%` where they disagree, many moves are actually *better* than the `19.54M` hard-`V=0` TB because our NN guides clean boundary escapes!

---

## 7. Blueprint for the Real 20-Row Level 18 / Level 19 DAS Engine

### Section Glossary
- **Runtime Latency Budget**: On NES Tetris (60 frames per second), a piece spawns and locks every ~0.3–0.8 seconds, so StackRabbit's move selector must evaluate a full turn well within a few milliseconds on a single CPU thread.
- **Value-Scale Coupling**: Keeping the TB's value scale and the NN's value scale naturally aligned by always training the NN directly on the current Soft-Boundary TB (rather than letting an independent RL process drift the NN's scale upward).

This project directly answers what our 20-row DAS 18/19 tablebase architecture should — and should **not** — look like:

1. **Build the flywheel around Soft-Boundary MPI + `nn-error` State Selection + Pure Supervised Distillation.**
   - Do **not** wait for a giant TB before training an NN. Start from a tiny seed TB, distill a `128×64` or `256×64` NNUE, plug that frozen NN directly into Soft-Boundary MPI (`m ≈ 2%–3%` of expected survival/score), evict easy low-`|V_tb − V_nn|` boards while keeping a protected high-visit core, and repeat for 4–6 rounds.
   - Precompute the frozen best-out-of-TB terminal payoff (`bestOutStore`) once on outer iteration 1 of each MPI solve — on 20-row DAS boards (where NN features are slightly heavier), avoiding redundant NN calls across inner sweeps turns an overnight solve into minutes.
2. **Skip self-play RL (at least for v1).**
   - Soft-Boundary MPI already achieves the entire theoretical purpose of RL (bootstrapping unbiased long-horizon values past a partial TB boundary) — **deterministically, without exploration death spirals, without Huber-median distortion, and without TB-fleeing scale drift**.
   - Keep `rl_finetune.hpp` in `experimental/tablebase/` as a reference benchmark, not a production dependency.
3. **Choose between `256×64` 2-ply vs. `128×64` 3-ply Top-K based on latency budget.**
   - In our 4×10 testbed, **`s5_final` + `128×64` 3-ply Top-6 (`m = 0.5`)** (`102.17` lines, `22.4k moves/sec`) and **`s5_final` + `256×64` 2-ply (`m = 0.5`)** (`98.88` lines, `23.3k moves/sec`) ran at almost the **exact same speed**, with `128×64` 2-ply (`m = 1.0`, `97.00` lines) running **`2.1×` faster (`48.2k moves/sec`)**!
   - On 20-row DAS, where each legal move requires a much more expensive DAS move search, **`256×64` 2-ply** (zero ply-3 branching!) or **`128×64` 2-ply with selective 3-ply Top-K only on high-risk/out-of-TB turns (via 2-ply pre-screening slack)** will fit comfortably inside real-time NES frame budgets while preserving nearly all the strength of an impossibly huge tablebase.
4. **Calibrate the runtime safety margin `m` lower once the TB is soft-solved.**
   - When the TB was solved with hard `V = 0`, we needed `m = 1.0–2.0` to prevent NN noise from pulling moves out of the TB. Once the TB is solved with Soft-Boundary MPI (`m = 2.0`), the TB's own values already incorporate conservative boundary pricing — so at runtime, a lighter margin (`m = 0.5–1.0`) lets the NN rescue boundary traps cleanly without double-penalizing out-of-TB escapes.

---

## 8. Engineering & Review Lessons

### Section Glossary
- **Tautological Unit Test**: A test that passes trivially because its setup never actually reaches the code branch it claims to verify (for example, testing a `NaN` guard when every item is skipped before the guard runs).
- **Thread-Count Determinism**: Guaranteeing that running a solver, generator, or trainer on 1 thread produces bit-for-bit identical results to running on 4 or 10 threads.

Across Phases S-1 through S-4, running our **Logic Reviewer** (4-point seam checklist) + **Simplicity Reviewer** (plus a **3rd Performance & Scale Reviewer** on Soft-Boundary MPI and RL) caught real defects before a single bad commit ever landed:
- **Phase S-1**: Fixed top-K tie-breaking parity, ply-3 top-out semantics, and 2-ply pre-screening slack (`57×` speedup with zero quality loss).
- **Phase S-2**: Caught a tautological tie-break test, validated `2^20`-scaled terminal payoff packing bounds and overflow guards, verified 10-column line-clear Value Iteration parity across all 4 modes (`Hard`, `Discounted`, `Soft-NN 1-ply`, `Soft-NN 2-ply`), and implemented `bestOutStore` (`15×` solve speedup) + single-pass in-memory inner sweeps.
- **Phase S-3**: Caught a critical release-build (`-DNDEBUG`) out-of-bounds/`NaN` hazard in Polyak target updates, exact cosine learning-rate scheduling, opening-piece expectation parity, and bounded-memory chunked rollout collection, plus `Eval2PlyCache` (`10×` faster rollouts with exact thread-count determinism).
- **Phase S-4**: Split overloaded `--priority-alpha` into single-meaning `--nn-error-weight` and `--hole-boost-scale` flags, eliminated 85 lines of duplicated 3-way merge code, packed `PrioritizedBoard` from 32 bytes to 24 bytes with early TB/buffer release, caught a silent `< 4096` static-slice worker bug and two tautological unit tests, and guarded against zero priority slots and same-path `--tb` truncation.
- **Verification**: All 30 tablebase unit tests (`388,075` assertions), all 80 StackRabbit C++ unit tests, `precompute_static_lint`, `-Wall -Wextra -Werror`, and `-DNDEBUG` passed cleanly, with `git diff --stat -- src/` remaining completely empty across the entire project.
