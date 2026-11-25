# Very Low Lineout Tablebase Retrospective — Lessons for Real DAS 18 / DAS 19 Stacking

This document captures the empirical findings, engineering lessons, and cost models from building the isolated 4×10 **Very Low Lineout** tablebase testbed (`experimental/tablebase/`), from No-Next-Box (`V(b)`) through exact 1-piece Next-Box (`V(b, c)`) with true 7×7 NES RNG.

Everything here is written from one perspective: **what directly transfers — and what changes — when we port this exact pipeline onto real NES Tetris stacking (Level 18 / Level 19 DAS).**

Companion docs:
- [docs/tablebase_plan.md](tablebase_plan.md) — v0 No-Next-Box ruleset, oracle checks, and Phases 1–6.
- [docs/tablebase_nextbox_plan.md](tablebase_nextbox_plan.md) — v1 1-Piece Next-Box format, disk-streamed solver, and Phases NB-1–NB-5.
- [docs/tablebase_distillation_plan.md](tablebase_distillation_plan.md) & [docs/tablebase_distillation_retrospective.md](tablebase_distillation_retrospective.md) — distilling the `670.9 MB` tablebase into V2 linear heuristics (`48.94` lines), a `78.6 KB` Tiny NN (`71.79` lines), and calibrated partial-OUT fallbacks (`104.02` lines on full TB; `+11–17` lines on partial TBs).
- [docs/small_tablebase_improvement_plan.md](small_tablebase_improvement_plan.md) & [docs/small_tablebase_improvement_retrospective.md](small_tablebase_improvement_retrospective.md) — pushing a strictly capped `<= 2.5M`-board tablebase (`12.8%` of full size, zero leakage) from `67.59` to **`102.17` lines** (pure supervised, beating the `19.54M` full TB `89.90` by `+12.3` lines) via Soft-Boundary MPI, `nn-error` state selection, and calibrated 3-ply Top-6 fallback.

---

## 1. Executive Summary: What the Toy Problem Proved

On 10,000 identical fixed-seed games (`seed=700000`), the progression from hand-crafted heuristics to our final `19.54M`-board Next-Box tablebase (`670.9 MB` on disk) was dramatic:

| Agent | NES RNG Mean | Uniform `1/7` Mean | Live Speed (10 cores) |
|---|---:|---:|---:|
| NNB 1-ply Heuristic | — | `8.08` | `> 500k mv/s` (1 core) |
| NNB 2-ply Expectimax | — | `10.55` | `> 20k mv/s` (1 core) |
| NNB `oracle_1overhang` (`31.20M` enumerated) | — | `10.79` (`V(0)=8.38`) | — |
| NNB `round8.tb.bin` (`15.95M` sampled) | — | `14.34 ± 0.13` (`V(0)=13.52`) | `~570k mv/s` |
| Next-Box 2-ply Heuristic | `30.05 ± 0.28` | `23.34 ± 0.22` | — |
| Hybrid `round8.tb` (NNB) + 2-ply Next-Box | `76.68 ± 0.73` | `51.42 ± 0.49` | — |
| Next-Box `nb_round0_nes` (`round8` boards reused) | `81.08 ± 0.78` | — | — |
| Next-Box `nb_final_uniform` (`19.54M`, trained `1/7`) | `86.49 ± 0.85` | **`58.32 ± 0.57`** | `2.43M mv/s` |
| **Next-Box `nb_final_nes` (`19.54M`, trained NES)** | **`89.90 ± 0.88`** | `56.67 ± 0.54` | **`2.47M mv/s`** |

Six headline conclusions for Level 18 / Level 19 DAS:
1. **Native Next-Box `V(b, c)` beats `V_NNB(b) + 2-ply` by a wide margin** (`+13.22` lines / `+17.2%` on NES; `+6.90` lines / `+13.4%` on Uniform), while running faster at inference time (1-ply lookup instead of 2-ply expansion).
2. **Exact NES RNG is a free `+3.41` lines** once `c` is in the state, because `c` *is* `prevPiece` for the next spawn `n`.
3. **Importance-sampled state spaces crush structural enumeration.** `15.95M` sampled NNB boards beat `31.20M` enumerated `≤1`-overhang boards by `+5.14` closed-world lines (`13.52` vs `8.38`), and `85.7%` of our final `19.54M` Next-Box boards have sealed holes that enumeration excluded.
4. **Disk-streamed policy caching + `double` working buffers work cleanly on a 16 GB laptop** (`2.59 GB` peak RSS for `15.95M` boards / `111.6M` states / `781M` policy actions, zero swap).
5. **Tuning `--inner-sweeps` to match episode length + warm-starting (`--init`) is mandatory** as episode lengths grow — it gave a **3× solve speedup** on 80-line games and will matter even more on 500–2,000-piece DAS 18/19 games.
6. **Solver math is 100% verifiable end-to-end**: every tablebase's theoretical `E[V(0)]` matched 20,000-game closed-world Monte Carlo within `0.27σ–1.52σ`.

---

## 2. No-Next-Box (`V(b)`) → Next-Box (`V(b, c)`)

### Why Hybrid `V_NNB(b) + 2-ply` Leaves Major Value on the Table
Before training `V(b, c)`, the big open question was whether storing a 3× smaller `V_NNB(b)` (`12 B/board`) and doing a live 2-ply search over `(current, next)` would be "close enough." It wasn't — for two distinct reasons:
1. **Value-function mismatch (`+4.40` lines on the exact same boards)**: Solving `V_nes(b, c)` directly on the existing `round8.states.bin` (`15.95M` NNB boards) jumped immediately from `76.68` → `81.08`. A No-Next-Box value function prices boards by how well they survive *blind* draws, so it systematically undervalues setups (e.g., temporary overhangs, specific well/burn geometries) that are safe and lucrative whenever you have 1-piece lookahead.
2. **State-space mismatch (`+8.82` additional lines from Next-Box expansion)**: Running 4 warm-started Next-Box self-play rounds (`15.95M` → `19.54M` boards) lifted live play from `81.08` → **`89.90`**. Next-Box play deliberately enters shapes that NNB play avoids — `91.7%` of the `3.59M` newly added boards contain sealed holes and `30.8%` contain overhangs (compared to `17.2%` overhangs in NNB `round8`).

### The Computational Trick That Makes `V(b, c)` Cheap
For a fixed `(board, curPiece)`, the legal move list `Moves(b, c)` does **not** depend on the Next Box piece `n`. Inside each outer policy-improvement sweep:
- Generate `Moves(b, c)` **once** (7 calls per board — identical to NNB!).
- Hash each resulting child `cid = index.find(b')` **once**.
- Read the contiguous 7-entry row `vCur[cid * 7 + 0..6]` in a single cache line and update `bestQ[0..6]` across all 7 next pieces simultaneously.

So widening the table from `V(b)` to `V(b, c)` multiplies the state count by `7×` at **~1× the move-generation and hash-lookup cost**.

### Porting to DAS 18 / DAS 19: Full-DAS Tablebase + 2-Ply Exact DAS Inference
In Level 18/19 DAS stacking, legality and placement cost depend on `dasCharge` entering the turn. Rather than adding `dasCharge` into the tablebase state, the **recommended design** mirrors StackRabbit's existing `precompute` / `dasPenaltyMaps` split ([engine_endpoints.md](engine_endpoints.md#L133-L180)):

- **Offline tablebase (`V_full(b, c)`)**: Solve **`V_full(b, c)` at full DAS charge** (or using StackRabbit's fast DAS-equivalent tapper timeline, ~50–100× faster than `fullMoveSearch`). This keeps the tablebase compact (`7` floats/board), keeps the policy cache at `49` actions/board, and keeps outer policy-improvement sweeps fast.
- **Online harness (2-ply exact DAS search over `(c, n)`)**:
  1. **Ply 1 (`c`)**: Run exact `fullMoveSearch(b, c, currentDasCharge)` from the real spawn charge (handling low-DAS reachability and mid-air `reactionTime` adjustments exactly) → yields each reachable `(b', d')`.
  2. **Ply 2 (`n`)**: Run exact `fullMoveSearch(b', n, d')` using the **actual post-lock charge `d'`** → yields each reachable `(b'', d'')`.
  3. **Leaf evaluation**: Look up `b''` in `V_full` (one `O(1)` lookup, averaging its 7 floats weighted by `NES_TRANSITION[n][p_3]`), plus a small `thirdPieceDasPenalty(d'')` when `d'' < 15` (matching [high_level_search.cpp:687](../src/backend/high_level_search.cpp#L687)).
- **Why 2-ply instead of 1-ply `argmax V_full(b', n)`**: Pure 1-ply `fullMoveSearch` handles *entering* low DAS, but is blind to *killing* your DAS charge on piece 1 (`dasChargeAfter`), because `V_full(b', n)` assumes piece `n` enters with full charge. Running exact Ply 2 from `d'` catches every DAS-killing move that would strand piece 2 — solving the exact problem `dasPenaltyMaps` solves today without bloating the tablebase.

---

## 3. Piece RNG Details (Uniform `1/7` vs. Exact 7×7 NES RNG)

### Why Exact NES RNG Is Free Once You Have a Next Box
NES Tetris generates each next piece using a 1-step Markov transition matrix conditioned on the previous piece ([src/backend/piece_rng.cpp](file:///Users/gregcannon/Documents/StackRabbit/src/backend/piece_rng.cpp), remapped to `I, O, L, J, T, S, Z` in [core.hpp](file:///Users/gregcannon/Documents/StackRabbit/experimental/tablebase/core.hpp)):
- Repeat probability drops from `14.3%` (`1/7`) down to `3.1%–6.25%` (`2/64` or `4/64`), while non-repeat pieces occur at `15.6%–18.75%` (`10/64` or `12/64`).
- In a **No-Next-Box** tablebase `V(b)`, modeling NES RNG would force you to widen the state to `V(b, prevPiece)` (`7×` larger).
- In a **Next-Box** tablebase `V(b, c)`, the current piece `c` **already IS** `prevPiece` for the next box spawn `n`! Conditioning on exact NES RNG costs **0 extra bytes** and **0 extra flops** — you literally swap `1.0 / 7.0` for `transProb[c][n]`.

### Measured Impact (Two-Way Cross-Evaluation on Identical `19.54M` Boards)
| Policy Trained For | Eval on NES Games | Eval on Uniform `1/7` Games |
|---|---:|---:|
| Uniform `1/7` (`nb_final_uniform.tb.bin`) | `86.49 ± 0.85` | **`58.32 ± 0.57`** |
| Exact NES (`nb_final_nes.tb.bin`) | **`89.90 ± 0.88`** | `56.67 ± 0.54` |
| **Delta (NES-trained − Uniform-trained)** | **`+3.41` lines** | **`-1.65` lines** |

Two huge takeaways:
1. **The NES RNG makes the game dramatically easier AND changes the optimal policy.** First, the environment itself is ~50% more forgiving (`58.32` → `89.90`) because back-to-back droughts/repeats (`S→S`, `Z→Z`, `O→O`) are 2.3×–4.6× rarer. Second, explicitly optimizing for the 7×7 matrix adds **`+3.41` pure strategy lines** on NES — and the symmetric `-1.65` drop when running the NES policy on uniform games proves the policy genuinely exploits the anti-repeat structure (e.g., trusting that an `S` or `Z` you just saw is very unlikely to repeat immediately).
2. **For DAS 18 / DAS 19**: Always train directly with exact 7×7 NES RNG (`RngMode::Nes`) from Round 0, and use the exact stationary distribution $\pi_{\text{NES}}$ (`NES_STATIONARY_NUM / 335916`) for opening-piece weighting and headline `E[V(0)]` reporting so closed-world Monte Carlo checks remain exact.

---

## 4. In-Memory vs. On-Disk Architecture & Precision Lessons

### 4.1 Binary Search over `mmap` vs. In-RAM `FlatBoardIndex`
- **Live serving (`tablebase_server`)**: `mmap` + binary search over sorted records (`36 B/board`) is ideal — instant startup, tiny RSS, ~500ns/lookup (completely invisible for single interactive queries).
- **Offline rollouts & solvers (`generate_nextbox`, `solve_nextbox`, `benchmark_cli`)**: binary search over a `550–670 MB` mmap causes `~24` random cache misses per lookup. Building an in-RAM `FlatBoardIndex` (`uint64_t -> uint32_t`, Fibonacci hashing, `<= 50%` load factor) takes only **`0.21s`** for `19.54M` boards and lifted rollout throughput from **`404k mv/s` → `1.8M–2.5M mv/s`** (a **4×–6×** net win once paired with `IndexedNextBoxTablebase`).

### 4.2 Disk-Streamed `PolicyActionStore` (Keeping RAM Flat)
In Modified Policy Iteration, the fixed policy table is the largest data structure by far:
- For Next-Box (`49` actions/board), packing `(childId, reward)` into a single `uint32_t` (`29-bit childId` supporting up to `536.8M` boards + `3-bit reward`) shrinks the policy table from `245 B/board` to `196 B/board` — still **`3.83 GB`** at `19.54M` boards (and **`19.6–39.2 GB`** at `100M–200M` boards!).
- Streaming the policy table through an auto-unlinked temporary SSD file (`mkstemp` + immediate `unlink`, `64-bit off_t`, `12.8 MB` chunk buffer = `65,536` boards/chunk) brought peak RAM on `15.95M` boards down to **`2.59 GB` total RSS (`0` swaps)**, while producing **bit-for-bit identical** results to RAM mode.
- **Hidden bonus on macOS/Linux**: when the policy file (`3–4 GB`) fits in unused OS page cache, sequential `pread`/`pwrite` runs at memory-bandwidth speeds (~`0.6s` per full inner sweep over `15.95M` boards!). When it exceeds RAM on a `100M+` board DAS solve, it degrades gracefully to NVMe sequential throughput (`~3–6 GB/s` on Apple Silicon SSD = `~10–20s` per sweep) instead of crashing with OOM.

### 4.3 `double` Working Buffers vs. `float32` Precision Stalls
- On disk, storing `float32` values (`4 B` each) is completely fine (`7` significant digits).
- **Inside the solver, always keep `vCur` and `vNext` in `double` (`112 B/board` for 7 pieces)** whenever expected values exceed `~15–20` (and certainly for DAS 18/19 scores in the thousands or survival counts in the hundreds!). At `V = 100`, `1 ULP` of `float32` is `7.63e-6`, which makes `< 1e-7` residual convergence literally impossible and accumulates rounding drift across hundreds of inner sweeps.
- **RAM hygiene lesson**: Scope solver arrays (`boards`, `FlatBoardIndex`, `vCur`, `vNext`, `PolicyActionStore`) so they are freed *before* building `IndexedNextBoxTablebase` for post-solve evaluation, and free `ShardedVisitCounter` + `IndexedNextBoxTablebase` *before* `mergeFilterAndCapBoards` in the generator. Also sort+deduplicate per-worker sibling vectors periodically (`LOCAL_DEDUP_THRESHOLD = 2M`) so sibling expansion never spikes RAM by 2–4 GB.

### 4.4 RAM & Disk Budget Projection for Real DAS 18 / DAS 19
A 20-row × 10-column Tetris board takes 200 bits (`uint64_t[4]` = `32 B` or packed `25–32 B`, or `16 B` if height-capped to 12 rows / top-profile representation). Assuming a **16-byte canonical board key** for full-DAS `V_full(b, c)` (`7` states/board = `49` actions/board):

| Component | Per Board (`16B key`) | At `25M` Boards | At `50M` Boards | At `100M` Boards |
|---|---:|---:|---:|---:|
| Sorted `boards` array | `16 B` | `0.40 GB` | `0.80 GB` | `1.60 GB` |
| `FlatBoardIndex` (~50% load) | `~40 B` | `1.00 GB` | `2.00 GB` | `4.00 GB` |
| `vCur` + `vNext` (`double[7] × 2`) | `112 B` | `2.80 GB` | `5.60 GB` | `11.20 GB` |
| Policy cache (`disk`, `49 × 4B`) | `0 B` RAM (`196 B` SSD) | `4.9 GB` SSD | `9.8 GB` SSD | `19.6 GB` SSD |
| **Peak Solver RAM** | **`~168 B/board`** | **`~4.2 GB`** | **`~8.4 GB`** | **`~16.8 GB`** |
| Final `.tb.bin` (`key + float[7]`) | `44 B/board` | `1.10 GB` | `2.20 GB` | `4.40 GB` |

*Takeaway*: With full-DAS `V_full(b, c)` and disk-streamed policy caching, even **`50M–75M` DAS boards** fit comfortably in this **16 GB RAM** laptop (`~8.4–12.6 GB` peak RSS)! Pushing past `100M` boards would require either a 32–64 GB machine or a single-buffer / Gauss-Seidel value array (halving `vCur+vNext` from `112 B` to `56 B` at the cost of thread-count determinism).

---

## 5. Modified Policy Iteration (MPI): Outer vs. Inner Sweeps

This experiment taught a textbook-clear lesson about tuning MPI for undiscounted ($\gamma = 1$) absorbing MDPs:

### Why Round 0 Took 35.7 Minutes While Round 1 Took 11.8 Minutes
For an undiscounted survival/line-clearing MDP with expected episode length $L$ pieces, the Bellman operator's contraction factor per sweep is roughly:
$$\rho \approx 1 - \frac{1}{L}$$

- In **NNB** (`V(0) ≈ 13.5`, $L \approx 35$ pieces): $\rho \approx 0.971$, so `25` inner sweeps contracted the error by $0.971^{25} \approx 0.48\times$ per outer step. `15.95M` boards solved in **4 minutes**.
- In **NES Next-Box** (`V(0) ≈ 86.9`, $L \approx 220$ pieces!): $\rho \approx 0.9955$.
  - With `--inner-sweeps=50` on Round 0: $0.9955^{50} \approx 0.79\times$ per outer step! Reaching `tol=1e-7` from cold start required **62 outer steps** (`3,027` inner sweeps = **`35m 40s`**), even though the policy had already completely stabilized by outer step ~20 and the last 18 outer steps only changed `E[V(0)]` by `0.0017` lines!
  - Switching to **`--inner-sweeps=150` + `--tol=1e-5` + `--init` warm-start** on Rounds 1–4 cut outer steps from **62 → 10–13** (`11m 51s` on Round 1, `19m–28m` on 16.8M–19.5M boards), because $0.9955^{150} \approx 0.51\times$ per outer step AND `tol=1e-5` already matches `float32` storage precision (`1 ULP` at `V=85` is `7.6e-6`).

### Crucial Rules for DAS 18 / DAS 19 Solves
1. **Set `innerEvalSweeps` proportional to expected episode length $L$** (roughly `0.5 × L` to `1.0 × L`, or even higher when move-generation is expensive!). Because DAS move search is **~100× slower** than 4×10 move gen (see [AGENTS.md](file:///Users/gregcannon/Documents/StackRabbit/AGENTS.md)), an outer sweep on DAS 19 will dominate runtime — you want **as few outer policy-improvement sweeps as possible** (`~8–15` outer steps) and **lots of cheap inner evaluation sweeps** (until `innerDiff <= innerStopRatio * outerDiff`!).
   - *Upgrade worth making for DAS*: Notice our inner early-stop currently uses a fixed threshold `cfg.tolerance * cfg.innerStopRatio` (`5e-7`), so early outer iterations (when `outerDiff = 20.0`) always run all `innerEvalSweeps`. Making inner early-stop **adaptive** (`std::max(cfg.tolerance * 0.05, outerDiff * 0.05)`) avoids wasting inner sweeps on early throwaway policies while still converging tightly on the final policy!
2. **Beware infinite-horizon $\gamma = 1$ divergence if episodes become arbitrarily long.** On 4×10 lineout, top-out is inevitable ($L \approx 220$), so $\gamma = 1$ always contracts. On **Level 18** (or very strong **Level 19**), an optimal stacking policy can survive **thousands of pieces** ($L > 2000 \implies \rho > 0.9995$)! For DAS 18/19, either:
   - Use a **finite target horizon** (e.g., "maximize score / survival probability over the next `K` lines" or "transition survival to 230 lines"), OR
   - Use a slight **discount factor** $\gamma \in [0.995, 0.999]$ (effective horizon $200–1000$ pieces, guaranteeing fast MPI contraction and bounded values), OR
   - **Maximize Score / Tetris Rate per piece** via Relative Value Iteration (average-reward MDP) if survival is near-100%.
3. **Always warm-start expansion rounds (`--init`)** — and verify monotonicity (`V_{k+1}(0, c) >= V_k(0, c)` for all `c`) after every round as a built-in bug detector.

---

## 6. Final State Space vs. Initial Estimates (Exact Hole & Overhang Breakdown)

Before building the generator, we enumerated two structural subsets as exact oracles:
- **0-hole pure surface** (`oracle_0hole`): **`4,358,525`** boards → `V_NNB(0) = 5.476`
- **0-sealed, ≤1 open overhang** (`oracle_1overhang`): **`31,197,715`** boards (`4.36M` 0-hole + `26.84M` 1-overhang) → `V_NNB(0) = 8.385` (live `10.79`)
- **Full canonical 4×10 universe**: `~500 billion` boards.

We directly measured the exact composition of our converged sampled sets (`round8.states.bin` and `nb_final.states.bin`) and the `3.59M` Next-Box-only additions (`nb_final \ round8`):

| Structural Category | NNB `round8` (`15.95M`) | Next-Box `nb_final` (`19.54M`) | Next-Box Additions (`3.59M`) | Share of Live NB Moves |
|---|---:|---:|---:|---:|
| **0 holes (pure surface)** | `1,727,500` (**`10.83%`**) | `1,839,403` (**`9.41%`**) | `111,903` (`3.11%`) | **`86.05%`** (`100.00%` hit!) |
| **1 open overhang, 0 sealed** | `690,322` (`4.33%`) | `834,799` (`4.27%`) | `144,477` (`4.02%`) | *(part of 1-hole `10.60%`)* |
| **2+ open overhangs, 0 sealed** | `83,545` (`0.52%`) | `126,510` (`0.65%`) | `42,965` (`1.20%`) | — |
| **≥ 1 sealed hole (any overhangs)** | `13,445,501` (**`84.31%`**) | `16,739,927` (**`85.67%`**) | `3,294,426` (**`91.67%`**) | — |

| Detailed Distribution (`nb_final.states.bin`, `19.54M`) | `0` | `1` | `2` | `3` | `4` | `5` | `6+` |
|---|---:|---:|---:|---:|---:|---:|---:|
| **By total holes** | `9.41%` | `23.32%` | `25.16%` | `20.65%` | `12.60%` | `5.80%` | `3.06%` |
| **By open overhangs** | `80.26%` | `17.21%` | `2.26%` | `0.25%` | `0.02%` | `0.00%` | `0.00%` |
| **By sealed holes** | `14.33%` | `24.45%` | `24.38%` | `18.97%` | `10.73%` | `4.72%` | `2.41%` |

And comparing against the enumerated oracles:
- Out of all **`4,358,525`** mathematically valid 0-hole boards, optimal Next-Box play only ever visited **`42.20%`** (`1,839,403`) — yet hits **`100.00%`** of live 0-hole moves (`1,999,410 / 1,999,446`, literally 36 misses in 2 million moves!).
- Out of all **`26,839,190`** 1-overhang-only boards, optimal Next-Box play only ever needed **`3.11%`** (`834,799`)!
- By max height: **`82.98%`** of `nb_final` boards touch row 4 (`h = 4`), `16.36%` have `h = 3`, and only `0.66%` have `h <= 2`.

### Massive Implications for DAS 18 / DAS 19 State Sampling
1. **Never use structural enumeration — always use iterative TB-guided self-play sampling.** Even on a tiny 4×10 board, enumerating 0-hole and 1-overhang boards wasted `58%` and `97%` of its budget on dead shapes while missing the `85.7%` sealed-hole recovery states that pushed NNB from `8.38` → `13.52` and Next-Box to `89.90`!
2. **Expect extreme Pareto skew in coverage vs. table size.** Only `9.4%` of stored boards handled `86.0%` of live moves (at `100.00%` hit rate), while `42.1%` of stored boards (`8.23M` boards with `3+` holes) covered the final `1.17%` scramble tail (`87.67%` hit rate). In DAS 18/19, **visit-count pruning (`--min-visits`) + `--max-boards` eviction by visit frequency** is essential so the rare deep-dig tail doesn't drown out core stacking shapes.
3. **Why our 3 sampling ingredients worked together**:
   - **50/50 exploration split** (half TB runner-ups, half 2-ply heuristic frontier probes with greedy TB move excluded): crucial! Pure TB runner-up exploration cannot discover strong moves outside the current tablebase because the TB values every out-of-set move at `0.0`.
   - **Core sibling expansion** (emitting all unique best TB replies + heuristic reply on high-visit boards): rescued `550k–750k` high-value interior gap boards per round.
   - **Annealed `ε` (`0.04 → 0.02 → 0.008 → 0.002`)**: early high `ε` discovers diverse shapes; late low `ε` plays long `80+`-line games that densely populate the true greedy trajectory.
   - **Zero-overestimation boundary rule (`V(b') = 0` outside $\mathcal{S}$)**: guaranteed monotone improvement round-over-round without value hallucination. On DAS 18/19, keep this exact closed-world boundary inside the solver (or a provably conservative lower bound) and only apply heuristic fallback at live play / rollout time.

---

## 7. Time Breakdown & Runtime Estimation Formula for Future Solves

### Actual Time Spent (Measured from Transcript)
- **Full conversation calendar span**: `27h 00m 59s` (`20h 11m 47s` human idle/breaks + **`6h 49m 12s`** active agent + compute time).
- **Autonomous Next-Box run (`NB-3` + `NB-4` + `NB-5`)**: **`3h 43m 11s`** total:
  - MPI solver compute (6 full solves on 15.95M–19.54M boards): **`2h 30m 34s`** (`67.5%`)
  - Coding, builds, unit tests, review fixes, commits, compaction: **`34m 05s`** (`15.3%`)
  - One silent LLM stream-interruption timeout (`NB-3`, step 640→647): **`17m 39s`** (`7.9%`)
  - Six parallel review subagent passes (`3 phases × 2 reviewers`): **`14m 41s`** (`6.6%`)
  - Next-Box self-play state generation (Rounds 1–4, `500M` moves): **`3m 52s`** (`1.7%`)
  - Post-solve MC/live evals, coverage, 4× 10k benchmarks, I/O: **`2m 20s`** (`1.0%`)

### Predictive Timing Formula for DAS 18 / DAS 19
Break any MPI tablebase solve into its two exact components so you can predict wall-clock time within ~20% **before** launching a multi-hour job:

$$T_{\text{solve}} \approx I_{\text{outer}} \cdot \Big( T_{\text{outer}} + K_{\text{inner}} \cdot T_{\text{inner}} \Big)$$

Where:
1. **Outer policy-improvement sweep ($T_{\text{outer}}$)**:
   $$T_{\text{outer}} \approx \frac{N \times 7 \times C_{\text{move\_gen}}}{\text{cores} \times \eta_{\text{parallel}}}$$
   - On 4×10 (`C_move_gen ≈ 250 ns`), `19.5M` boards took **`~4.5s`** per outer sweep on 10 cores.
   - **On real Level 19 DAS**:
     - With a fast **DAS-equivalent move generator** (StackRabbit's tapper approximation at DAS speed, `C_move_gen ≈ 0.5–1 µs`): `20M` boards takes only **`~8–16s` per outer sweep** on 10 cores — solving in **minutes**, just like 4×10!
     - By contrast, running frame-exact `fullMoveSearch` (`~10–25 µs/call`) inside the offline solver pushes $T_{\text{outer}}$ to **`~2.5–6 min` per outer sweep**.
   - **Key design insight for DAS 18/19**: Use the fast DAS-equivalent move generator in the offline solver and save frame-exact `fullMoveSearch` for the 2-ply online harness. If you ever do run frame-exact `fullMoveSearch` inside the solver, run **deep inner evaluation** (`K_inner = 200–500` or adaptive stopping) so you only pay for **`8–15` outer sweeps**.
2. **Inner fixed-policy evaluation sweep ($T_{\text{inner}}$)**:
   $$T_{\text{inner}} \approx \max\left(\frac{N \times 49 \times 4\text{ B}}{BW_{\text{stream}}},\; \frac{N \times 49 \times C_{\text{gather}}}{\text{cores}}\right)$$
   - On 4×10 (`19.5M` boards, `3.83 GB` policy), each inner sweep took **`~0.85s`** (dominated by random `vCur[childId]` gather lookups + OS-cached sequential reads).
   - Notice: **$T_{\text{inner}}$ does NOT depend on DAS move-gen complexity at all!** Even on Level 19 DAS, an inner sweep only unpacks `(childId, reward)` and gathers `vCur[childId]`, taking just **`~1–3s`** per sweep at `20M` boards!
3. **Number of outer iterations ($I_{\text{outer}}$)**:
   - Cold start: $I_{\text{outer}} \approx \frac{\ln(\text{tol} / \Delta_0)}{K_{\text{inner}} \cdot \ln(\rho)} + \text{3–5 policy-warmup steps}$.
   - Warm start (`--init` on expanded set): typically **`10–15` outer steps** if $K_{\text{inner}} \approx 0.7 \times L$ (so $\rho^{K_{\text{inner}}} \approx 0.5$).
4. **Always run a 10-second micro-benchmark first**: Measure $T_{\text{outer}}$ and $T_{\text{inner}}$ on a `500k`-board sample, multiply by $N / 500\text{k}$, and verify the math before starting a multi-hour solve!

---

## 8. Takeaways for Future Agent Sessions & Workflow

### What Worked Exceptionally Well (Keep Doing!)
- **Strict isolation (`experimental/tablebase/`)**: Keeping `src/` completely untouched eliminated merge risk and kept regression testing fast (`cpp_test_unit` + `precompute_static_lint` stayed green on every commit).
- **Phase-by-phase cadence + dual subagent reviews (`logic_reviewer` + `readability_reviewer`)**: Every single phase caught real bugs before they compounded — including subtle tie-breaking parity bugs, `float32` vs `double` precision issues, peak-RAM spikes in `expandNextBoxCoreSiblings`, silent `--rng` defaults in `benchmark_cli`, missing `anyLeafInTb` parity in hybrid `/state-info`, and misleading UI dropdown options. The `~5 min/phase` review cost paid for itself many times over.
- **Exact mathematical self-checks built into every CLI**:
  - `1T` vs `4T` bit-for-bit determinism tests (caught race conditions and non-determinism immediately).
  - `Ram` vs `Disk` bit-for-bit equality tests (proved the disk streamer had zero offset/chunk bugs).
  - Automatic closed-world Monte Carlo verification (`<= 3.5σ` vs `E[V(0)]`) printed on every solve — caught any mismatch between solver Bellman math, RNG tables, and move execution immediately.
  - `-Wall -Wextra -Werror` **plus** `-DNDEBUG -fsyntax-only` checks on every phase (caught multiple debug-only unused-variable warnings).
- **Independent browser inspector early**: Building `server.cpp` + `index.html` right in Phase 2 made debugging tucks, overhangs, and policy disagreements effortless and visual.

### What to Improve Next Time
1. **Calibrate `--inner-sweeps` and `--tol` from a small probe before launching a full solve.** Round 0 spent `~20` extra minutes converging from `1e-5` to `1e-7` at `inner=50` when `float32` output precision is `7.6e-6` anyway. A 30-second probe on `1M` boards would have immediately revealed the `inner=150, tol=1e-5` sweet spot.
2. **Watch out for editor auto-save races during long autonomous runs.** Halfway through Phase NB-4, an open IDE tab flushed stale pre-commit copies of 3 files to disk. Always check `git status -s` / `git diff --stat` right before committing (and close/save editor tabs before starting an unattended multi-hour run).
3. **Chain multi-round experiments into a single shell script when unattended**, rather than waiting for an agent turn between rounds, OR keep `NotificationTimeoutSeconds` well matched to expected round duration (`1800s`+) to minimize unnecessary wakeups.
4. **Budget for non-compute wall time in autonomous runs.** Expect `~15 min/phase` for coding + dual reviews + fixes + verification, and occasional transient stream hiccups — so a 3-phase autonomous run with 2.5h of compute naturally takes ~3.5h wall clock.
5. **For DAS 18 / DAS 19, start with a fast bitboard/state-representation design spike (Phase 0)** before writing the solver: the hardest new problems in DAS 18/19 are (a) **board canonicalization / height cap** (how many rows/bits define a state?), (b) **objective formulation** (line-clear survival vs. Tetris score vs. transition survival + discount $\gamma$), and (c) **move-gen throughput** (making DAS move search fast enough that $T_{\text{outer}}$ stays minutes, not hours). Everything else — `FlatBoardIndex`, `PolicyActionStore`, warm-started double-precision MPI, 50/50 frontier-probing self-play, exact 7×7 NES RNG, closed-world MC verification, and the HTTP/HTML inspector — can be ported almost verbatim!

### Recurring Review Findings & Reviewer Prompting Checklist
Across 22 review subagent reports (11 phases × 2 reviewers), local math (bitboards, move gen, Bellman backups) was almost always right on the first try; nearly every real bug lived at the **seams**. The six most frequent bug classes were:
1. **Cross-component tie-breaking & fallback drift** (7/11 phases): subtle differences in Q ties, precision (`%.6f` vs `%.17g`), or out-of-TB fallbacks between solver, rollout generator, `/moves`, `/state-info`, and `index.html`.
2. **v1 → v2 copy-paste duplication** (7/11 phases): cloning large mmap, generator, or route blocks instead of extracting shared `detail::` helpers upfront.
3. **Hostile-input, sentinel & overflow traps** (6/11 phases): `numBoards * sizeof(Record)` overflow, checking `k == board` before `k == EMPTY_KEY` (`~0ULL`), unsigned CLI underflow, silent `--rng` defaults, and truncating an active `mmap` when `--init == outPath`.
4. **Scale & peak-RAM traps** (4/11 phases): correlated hash bits between shard index and map slot ($\Theta(N^2)$ clustering), holding solver arrays alive during post-solve eval (`2×` RAM), and un-deduped sibling buffers.
5. **Ineffective ("green-washing") unit tests** (4/11 phases): tests that passed without actually exercising the claimed path (e.g., odd-popcount boards never hitting the TB, overflow constants that didn't wrap, or substring checks instead of numerical assertions).
6. **`-DNDEBUG` & async UI races** (4/11 phases): `assert`-only bounds checks or `-Werror` failures in release mode, and missing `stopAutoplay()` / stale-`fetch` guards in `index.html`.

**Prompting upgrades for future DAS 18/19 phases:**
- Default to **2 separate reviewers** (Logic vs. Simplicity) for CLI, server, and UI phases — splitting concerns prevented duplication fixes and deep correctness audits from crowding each other out, while avoiding the overlap and nitpick noise of a 3rd generic reviewer.
- On **heavy compute phases only** (move-gen, `50M`-board state sampler, MPI solver), add a **3rd specialized Performance & Scale Reviewer** tasked exclusively with auditing hot-path struct sizes, cache locality, peak simultaneous RAM lifetimes, thread contention, and disk streaming bandwidth under `N = 50M` scale.
- Give the **Logic Reviewer** an explicit 4-point seam checklist: (1) *cross-component parity* on ties/fallbacks/RNG, (2) *`N = 50M` peak-RAM & hash-correlation audit*, (3) *adversarial test audit* ("would each test actually fail if the feature were broken?"), and (4) *`-DNDEBUG` & hostile-input audit* (`NaN`, negative, corrupt, duplicate flags, `--in == --out`).
- Give the **Implementer** one upfront rule before coding: *when adding a variant beside an existing module, extract shared `detail::` primitives first instead of duplicating and refactoring after review.*
