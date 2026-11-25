# 20-Row NES Tetris (Level 18 / 19 DAS) Tablebase + Neural Network Retrospective

*Status: Living retrospective covering **Phases 0, 1, and 2** (through the 5-million-board Stationary Level 19 flywheel, production `200 ms` baselines, ply-2 tablebase re-entry, and the 18-frame reaction-time ablation). Sections for Phases 3–6 will be appended as those stages complete.*

This retrospective documents how we transferred our Hybrid Tablebase + Neural Network architecture from a 4-row survival testbed (`4×10` Very Low Lineout) onto a **full 20-row Nintendo Entertainment System (NES) Tetris board** under pure **10 Hz Delayed Auto Shift (DAS)** rules (no quicktaps, 18-frame reaction time), with the long-term goal of beating production StackRabbit on average score in full **Level 18 → Level 29 killscreen** games.

Everything so far lives in pure C++17 under `experimental/das_tablebase/`, leaving StackRabbit's production `src/` engine and our earlier `experimental/tablebase/` code completely untouched. Every training, validation, and held-out benchmark uses disjoint random seeds (`seed = 10,000,000+` for training/state generation, `seed = 30,000,000` for validation tuning and mid-project comparisons, and `seed = 700,000` reserved strictly for untouched final held-out scores).

Companion documents:
- [docs/tetris_20row_tablebase_plan.md](tetris_20row_tablebase_plan.md) — engineering plan, locked design decisions, and phase checklists.
- [docs/tetris_20row_tablebase_handoff.md](tetris_20row_tablebase_handoff.md) — transfer specification from the 4×10 tablebase project.
- [docs/small_tablebase_improvement_retrospective.md](small_tablebase_improvement_retrospective.md) — empirical 4×10 small-tablebase breakthroughs (Soft-Boundary Modified Policy Iteration, error-weighted state selection, and why we dropped self-play reinforcement learning).

---

## Core Glossary (Used Throughout This Document)

- **NES (Nintendo Entertainment System)**: Refers here to classic 60-frames-per-second NES Tetris — including its exact piece rotation rules, gravity speeds (3 frames per row on Level 18, 2 frames per row on Levels 19–28, and 1 frame per row on the Level 29 killscreen), scoring multipliers (`40 / 100 / 300 / 1200 × (level + 1)`), and non-uniform piece randomizer (which makes repeating the previous piece less likely than drawing a new one).
- **L18 / L19 (Level 18 / Level 19)**: The two primary gravity regimes in classic NES Tetris stacking — `L18` (`3` frames per row, lines `0–129`) and `L19` (`2` frames per row, lines `130–229`).
- **DAS (Delayed Auto Shift)**: NES Tetris's horizontal movement mechanic when holding Left or Right, charging up to a full 16-frame counter and then auto-shifting once every 6 frames (10 Hz). In this project we target pure **10 Hz DAS (no quicktaps)**, represented in StackRabbit by the input timeline `"X....."`.
- **TB (Tablebase)**: A precomputed lookup table mapping compact board keys to exact solved continuation values for all 7 possible next pieces.
- **NN (Neural Network)**: A small learned function made of matrix multiplications and simple nonlinear activations (`ReLU`) that predicts continuation values for *any* 20×10 board, especially boards outside the TB.
- **NNUE (Efficiently Updatable Neural Network)**: A Stockfish-style compact, cache-resident NN evaluated at the leaves of a fast tactical search (here `128×64` at `~175 KB` or `256×64` at `~345 KB`).
- **MPI (Modified Policy Iteration)**: Our iterative Bellman solver that alternates between choosing optimal moves for every board in the TB and propagating updated values across the TB until convergence.
- **MC (Monte Carlo)**: Running thousands of random simulated games under a fixed policy to independently measure its true average return and check solver correctness.
- **TRT (Tetris Rate)**: The percentage of all cleared lines that came from 4-line Tetrises (`4 × TetrisCount / TotalLines`). High TRT is essential for high NES Tetris scores because a Tetris pays `1200` base points (`300` per line) versus `40` for a single (`7.5×` more per line).
- **MAE (Mean Absolute Error)**: Average prediction mistake (`|predicted value − target value|`) of the NNUE on held-out validation positions.
- **SE (Standard Error)**: Statistical uncertainty (`± SE`) of a sample mean across `N` simulated games; differences smaller than `~2 SE` may just be random piece-sequence noise.
- **Ply**: One piece placement. A **1-ply** lookup scores `r₁ + γ · V_tb(b', nextPiece)` directly after placing the current piece; a **2-ply** search also enumerates all legal placements of the visible `nextPiece` before evaluating the resulting board `b''`.
- **OUT (Out-of-Tablebase) & `OUT → TB2` (Ply-2 TB Re-Entry)**: A move is `OUT` if its resulting board `b'` is not stored in the TB. Under `OUT → TB2`, we check whether placing the visible `nextPiece` from `b'` lands on a board `b''` that *is* back inside the TB; if so, we use the exact TB value of `b''` instead of the NN.
- **Out-of-TB Safety Margin (`m`)**: When a turn has at least one move grounded in the TB and some moves leaving the TB to rely on the NN, subtracting a small safety margin `m` from the NN's estimate (`max(0, V_nn − m)`) prevents small NN overestimation errors from luring the agent away from exact TB lines.

---

## 1. Executive Summary: Where We Stand After Phases 0–2

### What We Proved So Far
1. **A 20-row DAS tablebase is genuinely viable — boards revisit far more often than intuition suggests.** On real 20-row Level 19 and Level 18 play (Phase 0), a `5M`-board tablebase already hits **~60%–64%** of out-of-sample positions from an independent policy, and **93%–94%** of positions under its own on-policy flywheel (and **83%+** on clean 0-hole boards).
2. **Five rounds of our `5M`-board Stationary Level 19 flywheel climbed smoothly from `155,505` to `361,994` average score (0-frame) — beating `eval2ply` by `+118,000` points and reaching `94.0%` 100-line survival at `0.102 ms/move`.** That is roughly **1,900× faster** than production StackRabbit's `200 ms` tournament search while cutting 100-line topouts by more than two-thirds (`18.8% → 6.0%`).
3. **Ply-2 TB re-entry (`OUT → TB2`) delivered a free win in both score and speed.** When a temporary tuck or overhang steps outside the TB on ply 1 (`b' ∉ TB`) but the visible `nextPiece` resolves it back into the TB on ply 2 (`b'' ∈ TB`), scoring `b''` with exact `V_tb` (and zero NN safety margin) added **`+5,279` points** (`356.7k → 362.0k`) while cutting 0-frame latency in **half** (`0.214 → 0.102 ms/move`), and added **`+8,228` points** (`305.1k → 313.3k`) under live 18-frame reaction time + DAS physics.
4. **The 18-frame reaction-time ablation pinpointed the exact remaining gap on Level 19.** Plugging our 0-frame `5M` TB into StackRabbit's real 18-frame mid-air adjustment + DAS precompute collapse drops score by **`-50.5k` (`-14.2%`, `355.6k → 305.1k`, recovered to `313.3k` with ply-2 TB re-entry)** — and the 2×2 ablation proves that **DAS charge physics alone costs almost nothing (`-0.9k`, `-0.3%`)**, while **18-frame reaction-time blindness** accounts for almost the entire drop (`-31.0k` alone, `-50.5k` combined with DAS). Even before fixing reaction time or finite-horizon scoring, our `5M` prototype already matches production `200 ms` StackRabbit's 100-line survival (`81%–82%` vs. `81.2%`) at **`~77×` faster speed (`2.53 ms/move` vs. `195.5 ms/move`)**.
5. **Production `200 ms` baselines give us crisp targets and validate our Phase 4 endgame assumption.** On full uncapped **Level 18 → agent topout** (`10 Hz` pure DAS, `18`-frame reaction time, `200 ms`), production StackRabbit averages **`1,074,642 ± 13,120`** (`100.0%` transition, `95.0%` killscreen, `82.5%` maxout) — and **`1,071,786`** of that score (`99.73%`) is already earned upon entering Level 29 at line 230 (`+2,856` average on killscreen). Treating `L >= 230` as terminal `0.0` in Phase 4 backward induction is therefore virtually exact.

### Headline Comparison Table (Validation Split, `seed = 30,000,000`, Pure 10 Hz DAS)

| Agent & Configuration | Mode | Reaction Time / DAS | Games | Mean Score ± SE | Goal / Survival | TRT | Wall Speed |
|---|---|---|---:|---:|---:|---:|---:|
| Production `eval2ply` (0-frame baseline) | L19 (`100` lines) | `0` frames / no DAS | 500 | `244,031 ± 5,774` | `45.8%` goal | — | fast |
| Production `precompute 0×0 b=12` | L19 (`100` lines) | `18` frames / DAS | 200 | `248,760 ± 10,496` | `36.0%` goal | — | `3.89 ms/move` |
| Production `precompute 49×2 b=20` | L19 (`100` lines) | `18` frames / DAS | 100 | `343,228 ± 13,114` | `66.0%` goal | — | `13.62 ms/move` |
| Production `precompute 160×8 b=20` | L19 (`100` lines) | `18` frames / DAS | 50 | `345,656 ± 18,217` | `80.0%` goal | `58.6%` | `125.96 ms/move` |
| **Production `precompute 200ms b=-2`** | **L19 (`100` lines)** | **`18` frames / DAS** | **80** | **`338,305 ± 12,534`** | **`81.2%` goal** | **`55.1%`** | **`195.50 ms/move`** |
| Our `5M` Round 5 TB + `128×64` NNUE (before `OUT→TB2`) | L19 (`100` lines) | `0` frames / no DAS | 500 | `356,715 ± 3,049` | `93.4%` goal | `53.3%` | `0.214 ms/move` |
| **Our `5M` Round 5 TB + `128×64` NNUE (`OUT→TB2`)** | **L19 (`100` lines)** | **`0` frames / no DAS** | **500** | **`361,994 ± 2,877`** | **`94.0%` goal** | **`54.0%`** | **`0.102 ms/move`** |
| Our `5M` Round 5 Standalone `128×64` NNUE (no TB) | L19 (`100` lines) | `0` frames / no DAS | 500 | `346,682 ± 3,095` | `92.2%` goal | — | fast |
| Our `5M` Round 5 TB + `128×64` NNUE (before `OUT→TB2`) | L19 (`100` lines) | `18` frames / DAS | 200 | `305,110 ± 6,756` | `82.0%` goal | `47.1%` | `2.81 ms/move` |
| **Our `5M` Round 5 TB + `128×64` NNUE (`OUT→TB2`)** | **L19 (`100` lines)** | **`18` frames / DAS** | **200** | **`313,338 ± 5,816`** | **`81.0%` goal** | **`47.6%`** | **`2.53 ms/move`** |
| Production `eval2ply` (0-frame baseline) | Full 18→29 | `0` frames / no DAS | 500 | `640,950 ± 13,570` | `72.6%` transition / `27.6%` killscreen | — | fast |
| Production `precompute 49×2 b=20` (capped `230` lines) | Full 18→29 | `18` frames / DAS | 50 | `1,042,307 ± 29,312` | `98.0%` transition / `76.0%` killscreen | `71.5%` | `9.34 ms/move` |
| **Production `precompute 200ms b=-2` (uncapped topout)** | **Full 18→topout** | **`18` frames / DAS** | **40** | **`1,074,642 ± 13,120`** | **`100%` transition / `95.0%` killscreen / `82.5%` maxout** | **`67.8%`** | **`214.28 ms/move`** |

---

## 2. Phase 0: Revisit-Rate Spike, Board Key Encoding, and Scale Planning

### Section Glossary
- **Revisit Rate (Out-of-Sample TB Hit Rate)**: The fraction of board positions encountered during fresh unseen games that already appeared in a separate training set of earlier games. If revisit rate is near zero, a TB cannot help; if it is high, a TB can memorize the core of the game.
- **Board Key Encoding**: A compact fixed-size bit representation of a 20×10 Tetris board (200 binary cells). A full 200-bit board needs 25 bytes, whereas encoding only the column heights and hole coordinates can fit into 8 or 16 bytes.
- **Surface Array**: The 10 column heights `surface[0..9]` (each `0..20`), describing the top skyline of the stack.
- **Hole**: Any empty cell that has at least one filled cell somewhere directly above it in the same column (including empty cells sitting underneath a covered right-well overhang).
- **Well Column**: The column intentionally left open (typically column 9, the rightmost column) for dropping `I` pieces to score Tetrises.

### Why We Ran a Measurement Spike Before Writing Any Solver
A 20×10 board has `2²⁰⁰` theoretical states — vastly larger than our `4×10` testbed (`2⁴⁰`). Before locking binary file formats or writing a multi-threaded solver, we needed hard answers to three questions:
1. Do real Level 18 and Level 19 DAS games actually revisit the same boards often enough for a `5M–50M`-board TB to matter?
2. How many bytes does a board key really need (`8 bytes` vs. `16 bytes`), and do high stacks (`height > 16`) or multi-hole overhangs actually matter?
3. How fast is 20-row 10 Hz move generation, and how long will a `5M` or `20M`-board solve take?

We built a read-only diagnostic (`experimental/das_tablebase/phase0_spike.cpp`) driving StackRabbit's `eval.cpp` (2-ply lookahead, standard aggressive weights, `"X....."` 10 Hz timeline, exact 7×7 NES piece randomizer) and collected **`10.0M` Level 19 moves (`50,000` games in `121 s`)** and **`10.7M` Level 18 moves (`35,000` games in `142 s`)**, evaluated out-of-sample on disjoint `500`-game test splits.

### Out-of-Sample Revisit Rates (Disjoint 500-Game Test Split)

| Training Table Size (Top-K Most Visited Boards) | Level 19 Out-of-Sample Hit Rate | Level 18 Out-of-Sample Hit Rate |
|---|---:|---:|
| `100,000` boards | `34.7%` | `35.2%` |
| `500,000` boards | `49.1%` | `51.9%` |
| `1,000,000` boards | `53.2%` | `57.0%` |
| `2,500,000` boards | `56.5%` | `60.7%` |
| **All unique training boards (`~4.8M`)** | **`59.5%`** (`83.4%` on 0-hole boards) | **`63.7%`** (`83.0%` on 0-hole boards) |

That gave an immediate green light: even under a *different* heuristic policy (`eval.cpp`) with zero error-guided state selection, ~`60%–64%` of all moves in unseen 20-row games landed on memorized boards — and over **`83%`** of clean 0-hole positions were exact hits!

### Height and Hole Stratification (Why Height > 16 Barely Matters, But 5–9 Holes Do)

| Stack Height Band | L19 Share of Moves | L19 Hit @ All | L18 Share of Moves | L18 Hit @ All |
|---|---:|---:|---:|---:|
| `H <= 8` | `76.1%` | `70.6%` | `62.3%` | `74.9%` |
| `H = 9..12` | `19.5%` | `29.7%` | `33.7%` | `50.0%` |
| `H = 13..15` | `2.4%` | `0.3%` | `3.3%` | `5.4%` |
| `H = 16` | `0.5%` | `0.00%` | `0.3%` | `0.23%` |
| `H >= 17` | `1.5%` | `0.00%` | `0.5%` | `0.00%` |

| Hole Count Band | L19 Share of Moves | L19 Hit @ All | L18 Share of Moves | L18 Hit @ All |
|---|---:|---:|---:|---:|
| `0` holes | `64.7%` | `83.4%` | `71.8%` | `83.0%` |
| `1` hole | `9.5%` | `23.8%` | `7.5%` | `23.5%` |
| `2–4` holes | `13.4%` | `12%–19%` | `8.4%` | `8%–17%` |
| **`5–9` holes (incl. covered right-well overhangs)** | **`10.8%`** | **`10.4%`** | **`10.7%`** | **`11.8%`** |
| `10+` holes | `1.7%` | `0.4%` | `1.7%` | `1.0%` |

Two major design insights jumped straight out of those tables:
1. **Human Tetris intuition about row 16 was confirmed:** stacks above row 16 are indeed vanishingly rare (`0.5%–1.5%` of moves) with **`0.00%` revisit rate** — they are near-topout survival emergencies best handled by the NNUE, not memorized in the TB. However, saving 1 bit per column (`0..15` vs. `0..20`) only saved 10 bits total, which did **not** squeeze a 9-hole board into 8 bytes anyway.
2. **Covered right-well overhangs are a huge surprise (`10.8%` of moves, `10%–12%` revisit rate).** Whenever a piece hangs over an empty right well (e.g., an `L` or `T` overhang creating a 5-to-9-cell vertical pocket), every empty cell underneath counts as a "hole" under the surface. An 8-byte key capped at `<= 4` holes drops **~12.5% of all live moves**!

### Candidate Board Key Encodings Compared

| Candidate Key | Size | Constraints | Coverage of All Moves (L19 / L18) | Coverage of `Hit@2.5M` (L19 / L18) | Coverage of `Hit@All` (L19 / L18) |
|---|---:|---|---:|---:|---:|
| `K8-Opt2a` (`8 B`) | `8 B` | `H <= 15`, `<= 4` holes | `87.4% / 87.6%` | `98.60% / 98.61%` | `98.11% / 98.00%` |
| `K8-Opt2b` (`8 B`) | `8 B` | `H <= 16`, `<= 3` holes | `83.0% / 85.2%` | `97.54% / 98.32%` | `96.67% / 97.70%` |
| `K8-Opt1a` (`8 B`) | `8 B` | right well + `<= 6` active rows | `82.2% / 83.1%` | `98.06% / 97.60%` | `97.62% / 96.78%` |
| **`K16-Opt2c` (Locked)** | **`16 B`** | **`H <= 20`, `<= 9` holes** | **`98.32% / 98.35%`** | **`99.99% / 99.98%`** | **`99.99% / 99.97%`** |
| `K16-Opt1b` (`16 B`) | `16 B` | any well + `<= 12` active rows | `97.93% / 98.99%` | `100.00% / 100.00%` | `100.00% / 100.00%` |

**Why we locked `K16-Opt2c` (`16 bytes = uint64_t[2]`, 126 of 128 bits used):**
- `50 bits` for `surface[0..9]` (`5 bits × 10`, full height `0..20`), `4 bits` for `numHoles` (`0..9`), and `72 bits` for up to 9 sorted hole coordinates (`(row - 1) × 10 + col ∈ 0..189`, `8 bits` each).
- Captures **`99.98%–99.99%`** of revisited boards and **`98.3%`** of all live moves (including 5-to-9-hole covered-well overhangs), makes **zero assumption about which column holds the well** (supports center wells, left wells, and burns cleanly), and encodes/decodes via pure bit shifts with a collision-free empty-slot sentinel (`surface[0] = 31 > 20`).
- **Why we designed every data structure for `20M–50M` boards from day one:** Even with a 16-byte key, our solver uses only `~168 B/board` in RAM (`~0.84 GB` at `5M`, `~3.36 GB` at `20M`, `~8.4 GB` at `50M`) and `44 B/board` on disk (`220 MB` at `5M`, `880 MB` at `20M`, `2.20 GB` at `50M`), fitting cleanly inside 16 GB RAM. Enforcing 64-bit offsets, overflow guards, and disk-streamed policy caching from the start lets us iterate rapidly at `5M` boards now and scale straight to `20M–50M` later with zero rewrites.

---

## 3. Phase 1: Fast 20-Row Core, Parity Tests, and Production Baselines

### Section Glossary
- **Bit-Exact Parity**: Verifying that our new standalone 20-row move generator produces the *exact* same set of legal placements, line clears, tucks, and resulting boards as StackRabbit's production `src/backend/move_search.cpp` on every test position.
- **64-Bit Bitboard Packing**: Packing four 16-bit board rows into a single 64-bit integer so placement stamping and full-row detection happen in a handful of machine instructions instead of per-row loops.
- **Precompute (`b = -2` / `49×2` / `160×8`)**: StackRabbit's production search engine, which evaluates all legal placements across possible next pieces using lookahead playouts. Fixed configurations like `49×2 b=20` run 49 playouts of length 2 with beam breadth 20; `200 ms b=-2` is StackRabbit's real-time tournament mode that dynamically allocates playout rounds to fit a `200 ms` wall-clock deadline per move.
- **7-Thread Startup Calibration**: Before running time-budgeted (`b = -2`) moves, StackRabbit runs a brief 7-thread benchmark on two reference boards (flat and overhang) to measure this machine's true search speed (`moves/ms`) so Round 1 of tournament search sizes itself accurately to the millisecond budget.
- **Normalized Tetris Units**: Rescaling NES line-clear points (`40 / 100 / 300 / 1200`) by dividing by `1200`, giving `{0, 1/30, 1/12, 1/4, 1.0}` per clear. Stationary solvers optimize discounted Normalized Tetris units (`γ = 0.995`), keeping value scales clean and independent of level multiplier.

### Fast Standalone 20-Row Engine (`3.5×` Faster Move Generation)
In `experimental/das_tablebase/core_20row.hpp` and `board_key.hpp`, we built an allocation-free 20-row engine (`alignas(64)` `Board20`, precomputed 64-bit placement masks, 64-bit bitboard `applyPlacement20`, and a 6-frame-cycle `findPlacements10Hz`):
- **Single-thread speed**: dropped from Phase 0's `461 ns/call` (`src/backend/fastMoveSearch`) down to **`119–137 ns/call`** (`~3.5×` faster) and `~985–1,100 ns/call` for full transition generation (placements + line clears + `K16-Opt2c` key encoding).
- **Verification**: `11,415,688+` parity assertions covering 5,000 random key round-trips, 9 hostile/corrupt-key invariants, uncorrelated 128-bit hash tests, exact 7×7 NES piece randomizer and level-up transitions, and 10,500+ live and random board move-generation / tuck / line-clear checks across all three gravities (`1, 2, 3`).

### Production StackRabbit Baselines (`seed = 30,000,000` Val & `seed = 700,000` Held-Out Test)

All baselines use pure **10 Hz DAS (`"X....."`, no quicktaps)**, StackRabbit's standard aggressive weight profile, and the exact NES piece randomizer.

#### 0-Frame Heuristic Baseline (`eval2ply`, no playouts)

| Mode | Split & Games | Mean Score ± SE | Normalized Tetris | Mean Lines | Transition / Goal / Killscreen / Maxout |
|---|---|---:|---:|---:|---|
| Stationary L19 (`100l` goal) | Val (`500g`) | `244,031 ± 5,774` | `10.168` | `71.63` | `45.8%` goal |
| Stationary L19 (`100l` goal) | Test (`1000g`) | `248,574 ± 4,080` | `10.357` | `73.31` | `48.2%` goal |
| Stationary L18 (`130l` goal) | Val (`500g`) | `442,124 ± 6,889` | `19.391` | `114.72` | `75.2%` goal |
| Stationary L18 (`130l` goal) | Test (`1000g`) | `460,804 ± 4,518` | `20.211` | `118.31` | `80.0%` goal |
| Full 18 → 29 (`230l` cap) | Val (`500g`) | `640,950 ± 13,570` | — | `162.64` | `72.6%` tr / `27.6%` ks / `11.8%` maxout |
| Full 18 → 29 (`230l` cap) | Test (`1000g`) | `666,482 ± 9,134` | — | `168.53` | `78.2%` tr / `27.0%` ks / `12.7%` maxout |

#### Live Production `precompute` Baselines (`rt = 18`, `das = 1`, Val `seed = 30,000,000`)

| Mode & Search Config | Games | Mean Score ± SE | Median / p90 / Max | Norm. Tetris | Lines / Pieces | TRT | Avg Max H | Rates | Wall Speed |
|---|---:|---:|---|---:|---|---:|---:|---|---:|
| L19 `0×0 b=12` | 200 | `248,760 ± 10,496` | — | `10.365` | `63.77l` | — | — | `36.0%` goal | `3.89 ms/mv` |
| L19 `50ms b=-2` *(uncalibrated)* | 50 | `265,680 ± 18,160` | — | `11.070` | `75.38l` | `50.8%` | `6.87` | `54.0%` goal | `264.0 ms/mv`* |
| L19 `49×2 b=20` | 100 | `343,228 ± 13,114` | — | `14.301` | `82.91l` | — | — | `66.0%` goal | `13.62 ms/mv` |
| L19 `160×8 b=20` | 50 | `345,656 ± 18,217` | — | `14.402` | `88.16l` | `58.6%` | `6.87` | `80.0%` goal | `125.96 ms/mv` |
| **L19 `200ms b=-2` (calibrated)** | **80** | **`338,305 ± 12,534`** | `374k / 440k / 484k` | **`14.096`** | `90.49l / 238.0` | **`55.1%`** | **`6.66`** | **`81.2%` goal** | **`195.5 ms/mv`** |
| Full 18→29 `49×2 b=20` (`230l` cap) | 50 | `1,042,307 ± 29,312` | — | `41.204` | `216.32l` | `71.5%` | — | `98%` tr / `76%` ks / `74%` max | `9.34 ms/mv` |
| **Full 18→topout `200ms b=-2` (uncapped)** | **40** | **`1,074,642 ± 13,120`** (`1,071,786` @ 230l) | `1.08M / 1.17M / 1.27M` | **`42.293`** | `231.60l / 599.3` | **`67.8%`** | **`7.15`** | **`100%` tr / `95%` ks / `82.5%` max** | **`214.3 ms/mv`** |

### Two Crucial Discoveries From Benchmarking Production StackRabbit
1. **Why startup 7-thread calibration mattered for `b = -2`:** Our very first `50 ms b=-2` run did not pass a calibrated `SearchBudget`, so `tournament_search.cpp` fell back to its compiled default (`180.0 moves/ms`) instead of this machine's real `~131 moves/ms`. That caused Round 1 to overshoot its `50 ms` budget all the way to **`264 ms/move`** and starve Rounds 2 and 3, dragging score down to `265.7k`. Wiring `calibratePrecomputeSpeed7T()` into `eval_adapter.hpp` (mirroring `src/backend/live_game_server.cpp`) fixed wall-clock accuracy immediately (`195.5 ms/move` on L19, `214.3 ms/move` on uncapped Full 18).
2. **Diminishing returns on stationary Level 19, huge survival gains on full games:** On stationary L19, going from `49×2` (`13.6 ms/mv`) to `200 ms b=-2` (`195.5 ms/mv`, `14×` more compute) boosted 100-line survival sharply (`66.0% → 81.2%`) while leaving mean score virtually flat (`343.2k` vs. `338.3k`) because deeper playouts slightly lowered TRT (`58.6% → 55.1%`). On full 18 → topout games, that extra survival pays off hugely: `200 ms b=-2` reaches transition **`100.0%`** of the time, reaches Level 29 killscreen **`95.0%`** of the time (vs. `76.0%` for `49×2`), maxes out **`82.5%`** of the time, and averages **`1,074,642`** — with **`99.73%`** (`1,071,786`) earned before line 230!

---

## 4. Phase 2: The 5-Million-Board Stationary Level 19 Flywheel

### Section Glossary
- **Flywheel (Generate → Solve → Train → Repeat)**: Our iterative bootstrapping loop. Each round uses the previous round's TB + NNUE to explore reachable boards, prioritizes keeping the most important boards under a fixed cap (`5M`), solves the new TB with Soft-Boundary MPI, and trains a fresh NNUE on the newly solved TB.
- **Hard Boundary (`V = 0`) vs. Soft-Boundary MPI (`max(0, V_nn − m)`)**: A Hard Boundary treats stepping onto any board outside the TB as instant topout (`0.0`). Soft-Boundary MPI replaces that fake cliff with a conservative frozen NN estimate minus safety margin `m`, letting the solver price clean digs accurately even when they briefly leave the TB.
- **`bestOutStore`**: An optimization that caches the single best out-of-TB boundary payoff per `(board, curPiece, nextPiece)` on outer iteration 1 and reuses it on all subsequent iterations (since the boundary NN is frozen during a solve), speeding up outer iterations 2+ by **`20×–30×`**.
- **Error-Weighted State Selection (`nn-error`)**: Under a fixed board cap (`5M`), reserving a protected core of the highest-visit boards while prioritizing remaining slots by `visits × (1 + weight × |V_tb − V_nn|)` — evicting "easy" boards the NN already predicts accurately so the TB spends its capacity on subtle positions where the NN makes mistakes.
- **Soft-MC `z`-Score**: An independent Monte Carlo rollout check verifying that the solved policy's simulated return matches the solver's reported empty-board value `E[V(0)]` within statistical noise (`z <= 3.5` standard errors). Every single solve in Phase 2 passed with `z <= 0.79`!
- **Top-1 Agreement & Regret**: On held-out TB positions, `top1` is how often the NNUE's #1 move matches the TB's #1 move, and `regret` is the average value lost (in Normalized Tetris units) when picking the NNUE's #1 move instead of the TB's #1 move.

### Engineering Highlights Built for `20M–50M` Scale
Before launching the `5M` flywheel, we validated every component on a `250k`-board micro-benchmark (`7.86 s` Hard MPI, `21.1 s` Soft iter 1 → `0.61–0.79 s` iters 2–7, `0.08–0.11 s/epoch` NNUE) and enforced strict `20M–50M` scale invariants:
- **Cache-line `FlatBoardIndex`** (`tablebase_io_20row.hpp`): 6-slot Array-of-Structures buckets (`128 B` per bucket, Lemire `fastrange`), costing just **`32.8 B/board`** (`164 MB` at `5M`, `1.64 GB` at `50M`) with a single cache-line fetch on hits.
- **Disk-streamed `PolicyActionStore20`** (`solver_20row.hpp`): automatically streams policy actions (`196 B/board`) via 64-bit `pread`/`pwrite` above `6M` boards so solver RAM stays capped at `~168 B/board`.
- **Vector-accelerated Multi-Task NNUE** (`nnue_20row.hpp`, `272` input features): specialized branchless `(128, 64)` and `(256, 64)` inference keeping stack buffers under `2 KB`, trained with our proven 4-part multi-task loss (parent 7-output Huber + candidate-child Huber + explicit ply-2 topout supervision + Boltzmann move-ranking cross-entropy) on `70/30` on-policy trajectories (`~3.5 s/epoch` for `300k` decisions on `128×64`).

### Round-by-Round Progression (`5M` Board Cap, Stationary L19, `γ = 0.995`, Val `500g`, 0-Frame)

Round 0 seeded states from `eval.cpp` (Hard MPI); **from Round 1 onward, `eval.cpp` was dropped completely** and the flywheel bootstrapped purely from its own TB + NNUE.

| Round | State Set & Solver Config | `E[V(0)]` | Soft-MC `z` | NNUE (`128×64`) Val MAE / Top-1 | Hybrid Score ± SE (`500g`) | Hybrid Goal (`100` lines) | Hybrid TRT / TB Hit | Standalone NNUE Score ± SE |
|---|---|---:|---:|---|---:|---:|---|---:|
| **Round 0** | `5.0M` `eval.cpp` seed, Hard `V=0` | `2.557` | `0.73` | `0.228` / `78.6%` | `155,505 ± 5,462` | `13.2%` | — / `82.6%` | — |
| **Round 1** | `5.0M` Soft MPI (`m = 0.60`), `eval.cpp` dropped | `3.497` | `0.06` | `0.202` / `80.5%` | `223,825 ± 5,996` | `35.4%` | — | `241,489 ± 6,295` (`47.2%`) |
| **Round 2** | `518k` high-churn (`ε = 0.14`), Soft (`m = 0.45`) | `3.547` | `0.79` | `0.108` / `82.1%` | `292,943 ± 4,330` | `74.6%` | — | `295,141 ± 4,490` (`77.0%`) |
| **Round 3** | `2.75M` `nn-error` (`ε = 0.05`), Soft (`m = 0.25`) | `5.115` | `0.29` | `0.176` / `81.4%` | `327,724 ± 3,833` | `84.2%` | — / `90.5%` | `329,159 ± 3,465` (`88.4%`) |
| **Round 4** | `3.51M` `nn-error` (`ε = 0.08`), Soft (`m = 0.25`) | `6.581` | `0.34` | `0.185` / `80.6%` | `347,786 ± 3,323` | `92.0%` | — / `94.2%` | `339,474 ± 3,360` (`90.0%`) |
| **Round 5** | **`5.0M` `nn-error` (`ε = 0.10`), Soft (`m = 0.25`)** | **`6.950`** | **`0.33`** | **`0.152` / `82.0%`** (`regret 0.0347`) | **`356,715 ± 3,049`** | **`93.4%`** | `53.3%` / `94.0%` | **`346,682 ± 3,095`** (`92.2%`) |
| **Round 5 (`256×64`)** | Cold 30-epoch `256×64` (`339 KB`) on `r5.tb.bin` | `6.950` | `0.33` | `0.162` / `81.2%` | `349,727 ± 3,445` (`m=0.50`) | `90.0%` | — | — |
| **Round 5 + `OUT→TB2`** | **`5.0M` Round 5 + `128×64` + Ply-2 TB Re-entry** | **`6.950`** | **`0.33`** | `0.152` / `82.0%` | **`361,994 ± 2,877`** | **`94.0%`** | **`54.0%` / `93.0%`*** | `346,682 ± 3,095` |

*\*Note: Ply-1 direct TB hit moved from `94.0%` to `93.02%` when ply-2 TB re-entry was enabled — directly confirming that ~`1%` of turns now intentionally step out of the TB on ply 1 to take a stronger temporary overhang/tuck that resolves straight back into the TB on ply 2.*

### Key Lessons From the Stationary Level 19 Flywheel
1. **Why Round 0 (`155.5k`) looked so weak — and why dropping `eval.cpp` immediately worked:** Round 0's boards came from `eval.cpp` (which prefers high right-well Tetris stacks with `45.8%` survival) and were solved under Hard `V = 0`. Once Soft-Boundary MPI replaced `V = 0` in Round 1 and on-policy generation replaced `eval.cpp`, every single round lifted `E[V(0)]` (`2.557 → 3.497 → 3.547 → 5.115 → 6.581 → 6.950`) and real score (`155.5k → 223.8k → 292.9k → 327.7k → 347.8k → 356.7k → 362.0k`) with zero value inflation (`Soft-MC z = 0.06–0.79`).
2. **Even the standalone `175 KB` `128×64` NNUE (zero TB at runtime!) reached `346,682 ± 3,095` (`92.2%` 100-line survival)** — beating production `eval2ply` (`244.0k`, `45.8%`) by **`+102,651` points** and doubling its 100-line survival rate! (Cold-starting a `256×64` network for only 30 epochs slightly underfit at `MAE = 0.162` vs `128×64`'s `0.152`, which is why we kept `128×64` for fast iteration and will give `256×64` a longer/warm schedule once the final `20M+` TB is built.)
3. **Why stationary Level 19 plays ultra-low and conservative (`avgMaxH = 6.15`, `TRT = 53%–54%`):** Under infinite-horizon discounting (`γ = 0.995`, effective horizon `~200` placements), a single early topout forfeits `~6.95` future Normalized Tetrises, whereas burning a single instead of waiting for a Tetris costs only a fraction of a Tetris. That explains why our `5M` stationary agent achieves an extraordinary **`94.0%`** 100-line survival rate (`avgMaxH = 6.15` vs. production StackRabbit's `6.66–6.87`) while scoring slightly fewer Tetrises per line than an aggressive finite-horizon player. In **Phase 4**, replacing stationary `γ = 0.995` with exact undiscounted backward induction (`L = 229 → 130`, true `20× → 29×` level-up multipliers, terminal `0.0` at killscreen) will naturally incentivize high-TRT play as the killscreen approaches!

---

## 5. Interactive 20×10 Inspector & The Ply-2 Tablebase Re-Entry Breakthrough

### Section Glossary
- **Ply-2 TB Re-Entry (`OUT → TB2`)**: When placing the current piece leaves an unmemorized board `b' ∉ TB`, expanding all legal placements of the visible `nextPiece` and checking whether any resulting board `b''` is **back inside the TB** (`b'' ∈ TB`). If so, `b''` is scored directly using exact expected `V_tb` (`r₂ + γ · E[V_tb(b'', p₃)]`) with zero NN evaluation and zero safety-margin penalty.

### How Visual Inspection Surfaced an Immediate Engine Upgrade
To audit the tablebase qualitatively, we built a zero-dependency local web inspector (`./build/das_inspector`, [inspector_server.cpp](file:///Users/gregcannon/Documents/StackRabbit/experimental/das_tablebase/inspector_server.cpp), [inspector.html](file:///Users/gregcannon/Documents/StackRabbit/experimental/das_tablebase/inspector.html)) with interactive board painting, undo/redo history, NES-style visual current/next piece boxes, a Disagreement Finder (`TB+NN` vs. `eval.cpp` / `NN-only`), and live hover previews showing both Hybrid's and `eval.cpp`'s implied 2nd-piece placements.

While inspecting next-box positions in the tool, a classic NES Tetris pattern stood out: **it is very common to set up a weird temporary overhang or tuck with the current piece (landing on an `OUT` board `b'`) that is immediately resolved by the visible `nextPiece` back onto a clean, memorized board `b'' ∈ TB`.** Previously, our hybrid selector only probed the TB at ply 1; for any ply-1 `OUT` move, it evaluated *all* ply-2 leaves using only the NNUE (and subtracted the `Out Margin` penalty).

Updating `chooseHybridMove20`, `chooseTbNnPrecomputeMove`, and the inspector (`/moves`) to probe the TB on every ply-2 leaf of an `OUT` move (`bestReplyHybrid2Ply20`, zero margin penalty when `b'' ∈ TB`) produced an immediate A/B win across **both** 0-frame and 18-frame play — and actually **cut latency in half** because ply-2 TB hits skip the NNUE forward pass entirely:

| Evaluation Mode (`r5.tb.bin + r5_128x64.nn.bin`, L19, `seed = 30,000,000`) | Before (NN-only ply 2) | After (`OUT → TB2` ply-2 re-entry) | Delta |
|---|---:|---:|---:|
| **`tbnn` 0-frame** (`500g`, `m = 0.25`) | `356,715 ± 3,049` (`93.4%` goal, `53.3%` TRT, `0.214 ms/mv`) | **`361,994 ± 2,877`** (`94.0%` goal, `54.0%` TRT, **`0.102 ms/mv`**) | **`+5,279` score**, **`2.1×` faster** |
| **`tbnn-precompute` 18f DAS** (`200g`, `rt = 18, das = 1, m = 0.40`) | `305,110 ± 6,756` (`82.0%` goal, `47.1%` TRT, `2.81 ms/mv`) | **`313,338 ± 5,816`** (`81.0%` goal, `47.6%` TRT, **`2.53 ms/mv`**) | **`+8,228` score**, **`10%` faster** |

---

## 6. The 18-Frame Reaction-Time Ablation (`V(b, c)` vs. `V_RT(b, c)`)

### Section Glossary
- **RT (Reaction Time, `18` frames)**: In real NES Tetris (and StackRabbit's `precompute` engine), the player does not see the next piece until the current piece spawns, and needs `18` frames (`0.30 s`) to react. On Level 19 (`gravity = 2`), the current piece falls **9 rows** and executes up to **3 horizontal shifts** during those 18 frames before the player can adjust mid-air for the newly revealed `nextPiece`.
- **Precompute Collapse**: StackRabbit's exact decision procedure under RT: it chooses an initial 18-frame input prefix (a "phantom" commitment) that maximizes the expected value across all 7 possible next pieces, where for each next piece `n` the player chooses the best legal mid-air adjustment reachable from that 18-frame state.
- **DAS Charge Penalty**: When placing the current piece ends without full DAS charge (`< 15` frames), the *next* piece starts with less horizontal reach. StackRabbit accounts for this by computing the exact value drop between full DAS (`15`), partial DAS (`10`), and minimum DAS (`5`).
- **`V(b, c)` vs. `V_RT(b, c)`**: Our Phase 2 tablebase `V(b, c)` assumes 0-frame RT (full choice of any legal placement of `c` after seeing `n`). A reaction-time-aware tablebase `V_RT(b, c)` instead puts the exact 18-frame precompute commitment choice inside the Bellman equation: choosing the best 18-frame initial prefix *before* averaging over `n`.

### The 2×2 (`Reaction Time × DAS`) Ablation Results
To measure how much our 0-frame `V(b, c)` tablebase loses under true live NES constraints — and isolate *why* — we built `tbnn_precompute_adapter.hpp` (calling StackRabbit's exact `fullMoveSearch`, `analyzePrecomputeCollapse`, and DAS penalty buckets) and ran a controlled **2×2 ablation** on `r5.tb.bin + r5_128x64.nn.bin` (`200` validation games, `seed = 30,000,000`, `m = 0.40`, `ply = 2`):

| Condition (`r5.tb.bin + r5_128x64`, `200g`) | Mean Score ± SE | Delta vs. `rt=0, das=0` | Goal (`100l`) | TRT | Ply-1 TB Hit | Wall Speed |
|---|---:|---:|---:|---:|---:|---:|
| `rt = 0, das = 0` (0-frame, no DAS) | `356,570 ± 4,432` | `0` | `95.5%` | `52.5%` | `94.65%` | `0.77 ms/mv` |
| `rt = 0, das = 1` (0-frame + full DAS charge physics) | `355,628 ± 4,421` | **`-942` (`-0.3%`)** | `95.5%` | `52.4%` | `94.24%` | `0.91 ms/mv` |
| `rt = 18, das = 0` (18-frame RT, no DAS) | `325,538 ± 5,496` | **`-31,032` (`-8.7%`)** | `84.0%` | `49.1%` | `91.41%` | `0.93 ms/mv` |
| `rt = 18, das = 1` (full live 18f RT + DAS, before `OUT→TB2`) | `305,110 ± 6,756` | **`-51,460` (`-14.4%`)** | `82.0%` | `47.1%` | `89.07%` | `2.81 ms/mv` |
| **`rt = 18, das = 1` (full live 18f RT + DAS, with `OUT→TB2`)** | **`313,338 ± 5,816`** | **`-43,232` (`-12.1%`)** | **`81.0%`** | **`47.6%`** | **`88.12%`** | **`2.53 ms/mv`** |
| *Standalone `128×64` NNUE (`rt = 18, das = 1`, no TB)* | *`301,696 ± 6,818`* | *`-54,874`* | *`77.5%`* | — | *`0%`* | *`32.75 ms/mv`* |
| *Production StackRabbit `200ms b=-2` (`rt = 18, das = 1`, `80g`)* | *`338,305 ± 12,534`* | — | *`81.2%`* | *`55.1%`* | — | *`195.50 ms/mv`* |

### What the 2×2 Ablation Tells Us
1. **DAS charge physics alone is essentially a non-issue (`-0.9k`, `-0.3%`)**: Pure 10 Hz `"X....."` move generation already models the vast majority of DAS reachability; StackRabbit's runtime DAS-charge penalty buckets cleanly handle the occasional low-charge move without needing DAS charge baked into the board key!
2. **18-frame reaction time is a major real effect (`-31.0k` alone, `-43.2k` to `-51.5k` combined with DAS):** Under 0-frame assumptions, `V(b, c)` happily builds shapes that rely on waiting until frame 0 to commit left vs. right once `nextPiece` is known. Under 18-frame Level 19 gravity (9 rows of drop before reacting), high stacks and sharp two-sided dependencies become unadjustable in mid-air — forcing awkward compromises that both lower TRT (`52.4% → 47.6%`) and increase topouts (`4.5% → 19.0%`).
3. **Why we are already strikingly close to production StackRabbit `200 ms` — and how Phases 3–5 close the gap:** Even with a `5M`-board 0-frame stationary TB, our `r5 + 128×64` (`OUT→TB2`) already matches production `200 ms b=-2` on 100-line survival (`81.0%` vs. `81.2%`) while running **`77×` faster (`2.53 ms/mv` vs. `195.5 ms/mv`)**, and trails `200 ms b=-2` on Level 19 score by only **`-25.0k`** (`313.3k` vs. `338.3k`). Three large, orthogonal upgrades remain ahead of us:
   - **Reaction-time-aware solving (`V_RT(b, c)`)**: Baking 18-frame mid-air commitment directly into the Bellman backup on Level 19 so the tablebase stops valuing unadjustable shapes (directly attacking the `-43k` RT drop).
   - **Finite-horizon backward induction (Phases 4–5)**: Replacing stationary `γ = 0.995` with exact undiscounted NES scoring (`L = 229 → 130` and `L = 129 → 0`) so the agent plays aggressively for `20× → 29×` Tetrises (`65%–75%+` TRT instead of stationary `47%–54%` TRT) while smoothly managing the Level 18 → 19 transition and killscreen horizon.
   - **Scaling from `5M` → `20M+` boards + full `256×64` NNUE**: Lifting both TB coverage (`88%–93% → 96%+`) and out-of-TB accuracy.

---

## 7. Upcoming Phases (To Be Appended)

- **Phase 2 Decision / Phase 3**: Implement `V_RT(b, c)` (if approved) and scale Stationary Level 19 + Level 18 flywheels (`5M → 20M+`).
- **Phase 4**: Shared-key multi-slice backward induction across Levels 19–28 (`L = 229 → 130`, exact undiscounted NES points, terminal `0.0` at `L >= 230`).
- **Phase 5**: Level 18 → 19 transition backward induction (`L = 129 → 0`) and unified line-count-aware NNUE `V_nn(b, c, L)`.
- **Phase 6**: Final untouched held-out evaluation (`seed = 700,000`, full Level 18 → topout pure 10 Hz DAS) against production StackRabbit `200 ms b=-2`, plus production integration proposal.
