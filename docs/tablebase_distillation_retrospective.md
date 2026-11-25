# Tablebase-to-Heuristic Distillation & Partial-TB Fallback Retrospective

This retrospective explains what we learned from using our solved 4×10 **Very Low Lineout** tablebase (19.54 million boards, 136.8 million exact board-and-piece values) to train stronger standalone heuristics and smarter out-of-tablebase fallbacks.

Everything here was built in pure C++17 inside `experimental/tablebase/` (leaving StackRabbit's production `src/` engine untouched) and measured on the exact same **10,000 held-out NES Tetris games** (`seed=700000`) used in [docs/tablebase_retrospective.md](tablebase_retrospective.md).

For the follow-up project pushing a strictly capped `<= 2.5M`-board tablebase (zero leakage from the `19.54M` TB) from `67.59` to **`102.17` lines**, see [docs/small_tablebase_improvement_retrospective.md](small_tablebase_improvement_retrospective.md).

---

## Core Glossary (Used Throughout This Document)

- **TB (Tablebase)**: A precomputed lookup table storing the exact expected number of future lines cleared from every board position in its state set under optimal play.
- **NN (Neural Network / Neural Net)**: A learned function made of layers of weights and simple nonlinearities that predicts board quality without needing a giant lookup table.
- **NNUE (Efficiently Updatable Neural Network)**: The evaluation architecture popularized by Stockfish chess — a tiny, CPU-cache-resident NN evaluated at the leaves of a fast tactical search.
- **DAS (Delayed Auto Shift)**: NES Tetris's standard piece-movement mechanic where holding Left/Right charges horizontal auto-shift speed. Level 18 / Level 19 DAS stacking is the real-game target motivating these experiments.
- **Heuristic**: A fast mathematical formula (like a weighted sum of holes, bumpiness, and heights) that estimates how good a board is without looking it up in a TB.
- **Ply**: One piece placement. A **1-ply** search evaluates placing only the current piece; a **2-ply** search evaluates every combination of placing the current piece followed by the visible Next-Box piece.
- **Distillation**: Training a small, fast "student" model (a 26-weight linear formula or a tiny NN) to imitate the decisions and values of a much larger "teacher" (here, the 671 MB TB).
- **Q-value**: The total expected score of choosing a specific legal move on the current turn: immediate lines cleared plus the expected future lines from the resulting board.
- **Held-out benchmark**: A fixed set of 10,000 random games (`seed=700000`) never used during training or tuning, so every model is judged fairly on unseen games.

---

## 1. Executive Summary: Why We Did This and What We Found

### The Core Problem
Even on a tiny 4-row board, **27.14% of games** eventually step outside our 19.54-million-board TB at least once. On a full 20-row NES Tetris board (Level 18 / Level 19 DAS stacking), an exhaustive TB is completely impossible: any realistic TB will cover the common clean-stacking core (say 80%–95% of moves) and frequently cross the boundary into messy digs or rare shapes outside the TB.

That motivated four concrete questions:
1. **Can TB data dramatically improve our existing hand-crafted linear heuristic** — first with the exact same 10 features, and then by discovering new features from its mistakes?
2. **How close can a tiny, fast NN (30–80 KB) get to a 671 MB TB** with zero TB lookups at playtime?
3. **When a TB is incomplete, how should the agent choose between an in-TB move and an out-of-TB move?**
4. **Can a small partial TB train its own NN to help expand the TB faster** (without needing a larger TB first)?

### Complete Benchmark Progression (10,000 Held-Out NES Games)

| Policy | Model Size | Average Lines Cleared | Median / 90th %ile / Max | Speed (10 cores) |
|---|---:|---:|---|---:|
| Original hand-tuned 2-ply heuristic (10 features, no TB training) | ~100 bytes | `30.05 ± 0.28` | `22.0 / 67 / 237` | — |
| Original 10 features — trained with Ridge Regression (squared error) | ~100 bytes | `27.89 ± 0.26` | `20.0 / 62 / 262` | — |
| Original 10 features — trained with Boltzmann Move Ranking | ~100 bytes | `31.73 ± 0.30` | `23.0 / 71 / 261` | — |
| Expanded 26 features — trained with Ridge Regression (squared error) | ~224 bytes | `42.85 ± 0.41` | `30.0 / 96 / 374` | — |
| Expanded 26 features — trained with Joint Value + Ranking loss | ~224 bytes | `46.86 ± 0.46` | `32.0 / 108 / 429` | — |
| **Expanded 26 features — trained with Pure Boltzmann Move Ranking** | **~224 bytes** | **`48.94 ± 0.46`** (`+62.9%`) | `35.0 / 111 / 436` | `300,500 moves/sec` |
| Compact NN (`64×32` hidden units) — 1-ply lookahead | `31.3 KB` | `22.73 ± 0.22` | `16.0 / 51 / 234` | `383,200 moves/sec` |
| **Compact NN (`64×32` hidden units) — 2-ply lookahead** | **`31.3 KB`** | **`65.88 ± 0.64`** (`+119%`) | `47.0 / 146 / 577` | `36,100 moves/sec` |
| Compact NN (`128×64` hidden units) — 1-ply lookahead | `78.6 KB` | **`32.27 ± 0.31`** (beats 2-ply heuristic) | `23.0 / 73 / 322` | `258,400 moves/sec` |
| **Compact NN (`128×64` hidden units) — 2-ply lookahead** | **`78.6 KB`** | **`71.79 ± 0.68`** (`+139%`) | `52.0 / 161 / 958` | `15,600 moves/sec` |
| Previous No-Next-Box TB + 2-ply search | `182 MB` | `76.68 ± 0.73` | `54.0 / 174 / 651` | — |
| Full Next-Box TB (heuristic fallback only when all moves exit) | `670.9 MB` | `89.90 ± 0.88` | `63.0 / 203 / 921` | `2,071,800 moves/sec` |
| **Full Next-Box TB + Calibrated NN Out-of-TB Scoring** | `671 MB + 79 KB` | **`104.02 ± 1.02`** (`+15.7%`) | `74.0 / 235 / 993` | — |

### Four Headline Takeaways
1. **Smarter features + move-ranking loss lifted a 26-weight linear heuristic by `+62.9%` (`30.05 → 48.94` lines)** at 300,000 moves/sec — and directly uncovered specific missing Tetris concepts (such as counting distinct *rows* containing holes and how many empty cells remain in the covering row) that can be ported into StackRabbit's `src/backend/eval.cpp`.
2. **A `78.6 KB` NN (`8,535×` smaller than the `670.9 MB` TB) reaches `71.79` lines with 2-ply search**, closing **70% of the gap** between the hand-tuned heuristic (`30.05`) and the exact TB (`89.90`) with zero TB lookups.
3. **Scoring out-of-TB moves with a conservative safety margin beats "only fall back when forced" by `+11` to `+17` lines across every TB size** — and even pushes the converged 19.54-million-board full TB from **`89.90` to `104.02` lines** by letting it escape boundary traps on just `0.73%` of moves.
4. **An NN trained strictly on a small partial TB beats its own teacher AND discovers new TB states `2.5×`–`3.3×` faster**, creating a self-reinforcing loop ideally suited for 20-row DAS 18/19 Tetris.

---

## 2. Linear Distillation & Feature Discovery (Lessons for `src/backend/eval.cpp`)

### Section Glossary
- **MSE (Mean Squared Error)**: Average of the squared difference between predicted and true values. Penalizes large global errors heavily, which can hurt fine move-to-move ranking in linear models.
- **Ridge Regression**: Standard linear regression (minimizing MSE) with a small penalty on large weight magnitudes to prevent numerical instability when features are correlated.
- **$R^2$ (Coefficient of Determination)**: Fraction of variance in the true TB values explained by the model (`1.0` is perfect prediction; `0.0` is predicting the constant average).
- **Boltzmann Move Ranking (Cross-Entropy with Temperature)**: A training objective that turns the true TB Q-values on a turn into target probabilities using `softmax(Q / T)` (where `T` controls sharpness) and trains the model's move scores to match that distribution. Mistakes that lose many lines receive much larger gradients than near-ties.
- **Residual Error Mining**: Sorting examples by the largest prediction mistakes ("residuals") and inspecting them directly to discover what structural concepts a model is missing.

### Why We Sampled Gameplay Trajectories Instead of Uniform TB Rows
Before building NNs, we wanted to know how far a simple, interpretable **linear heuristic** (just a dot product of hand-crafted board features and weights — the exact same structure as StackRabbit's production `src/backend/eval.cpp`) could go when trained directly from exact TB ground truth.

A tempting shortcut would be to train on every board stored in the TB file with equal weight. We intentionally did **not** do that. From our earlier TB analysis ([docs/tablebase_retrospective.md](tablebase_retrospective.md)), **85.7%** of boards stored on disk have sealed holes, yet **86.0%** of moves in real strong games happen on 0-hole boards. Training uniformly over disk rows would waste 85% of the model's capacity on rare scramble boards and underfit the clean core that decides most games.

Instead, we generated training positions from fast self-play games of the solved TB:
- **70% greedy TB play** so the dataset automatically reflects real strong-play board frequencies.
- **30% exploratory/recovery play** (taking the TB's runner-up or a heuristic move on 8% of turns) so the model still sees plenty of 1-hole, 2-hole, and 3+-hole recoveries.
- For each sampled turn, we recorded both the board's exact TB value and the exact quality score (`lines cleared + next board's TB value`) of every legal move whose resulting board was in the TB.

### Why Move-Ranking Loss Beat MSE Regression
Our first test kept the **exact same 10 board features** from our original hand-tuned heuristic (`sealedHoles`, `overhangs`, `sumHeightSq`, `height3Cols`, `ceilingCols`, `bumpiness`, `wellDepthSq`, `parityImbalance`, `safeFlatPairs`, `rowFillSq`) and only changed how the 10 weights were trained:

- **Ridge Regression (MSE) actually made gameplay *worse* (`30.05 → 27.89` lines).**
  - *Why?* MSE cares about predicting whether a board is worth `85` lines vs `10` lines globally. Because a 10-feature linear formula cannot predict exact survival across wildly different board types, MSE spends its weights minimizing giant errors on hopeless 3-hole boards — at the expense of fine move-to-move ordering among two clean 0-hole placements that differ by only `0.5` lines!
- **Boltzmann Move Ranking lifted the exact same 10 features to `31.73` lines (`+3.84` over Ridge, `+1.68` over hand-tuning).**
  - *Why?* Move ranking ignores global line-count scale and compares *only* the legal candidate moves available on the same turn. It focuses its limited 10-weight budget on avoiding high-cost blunders (mistakes that lose many lines) while tolerating harmless `0.05`-line ties.

Because even optimal move ranking on the original 10 features only reached `31.73` lines, we now had proof that **missing features — not bad weights — were holding the linear heuristic back.**

### How Residual Error Mining Uncovered Missing Tetris Concepts
To find what the 10-feature formula was blind to, we sorted validation boards by the largest prediction errors (`predicted value minus true TB value`) and inspected the worst outliers by eye. Two striking patterns jumped out immediately:

1. **False optimism (predicted up to `+41` lines too high)**: Boards with a single deep pinhole on the bottom row buried underneath two or more messy covering rows. The original heuristic only saw `sealedHoles = 1`, so it thought the board was barely hurt — even though digging through two fractured covering rows is nearly fatal.
2. **False pessimism (predicted up to `-71` lines too low)**: Boards with 3 to 5 holes sitting side-by-side in a *single* row underneath one covering row that was only 1 or 2 cells away from clearing. Because the original heuristic simply multiplied `holeCount × -38`, it panicked and treated a 1-piece clean wipe as a disaster!

Guided directly by those two failure modes, we designed **16 additional interpretable features** (creating a **26-feature** linear model — a strict superset of the original 10). The results confirmed our diagnosis immediately:
- Validation $R^2$ jumped from **`0.6812` to `0.8139`**, and average value error on messy 3+-hole boards dropped by more than half (from **`21.86` lines down to `8.40` lines**).
- Live 10,000-game performance jumped from **`30.05` → `42.85` (Ridge MSE) → `46.86` (Joint Value + Ranking) → `48.94` lines (Pure Move Ranking)** — a **`+62.9%` gain** from the exact same fast linear architecture (`300,500 moves/sec`)!

### Actionable Features to Test in StackRabbit's Production `src/backend/eval.cpp`

| Feature | What It Measures | Why It Helped So Much |
|---|---|---|
| **`holeRows`** | Number of distinct rows containing at least one hole | Distinguishes "3 holes in 1 row" (one burn clears all 3) from "3 holes stacked in 3 rows" (requires 3 separate digs). |
| **`rowTransitions`** | Number of horizontal filled-to-empty or empty-to-filled switches across rows | Measures how jagged and fragmented covering rows are; directly predicts how many pieces are needed to clear down to a hole. |
| **`coveredMinoCount`** | Total filled cells sitting directly above any hole | Directly measures the exact digging workload sitting on top of existing holes. |
| **`buriedDepth2Plus`** | Count of holes with 2 or more filled cells directly above them | Captures the steep survival cliff between a shallow 1-deep hole and a deeply buried pinhole. |
| **`coverRowMissing`** | Empty cells still missing from the covering row right above the highest hole | Separates an imminent 1-piece clear (1–2 missing cells) from a stalled dig (5+ missing cells). |
| **`wellCount` / `maxWellDepth`** | Columns bordered on both sides by taller neighbors | Penalizes accidental double-wells while distinguishing clean single-well setups. |

---

## 3. Compact Stockfish-Style Neural Evaluator (`TinyNextBoxNet`)

### Section Glossary
- **MLP (Multi-Layer Perceptron)**: A classic feed-forward NN made of stacked matrix multiplications with a nonlinear activation function between layers.
- **ReLU (Rectified Linear Unit)**: The activation function `max(0, x)`. It sets negative intermediate values to zero, which both creates nonlinearity and makes many activations zero so subsequent math can skip them.
- **Huber Loss**: A smooth hybrid between MSE (for small mistakes under 2 lines) and absolute error (for large mistakes over 2 lines). It prevents rare outlier boards from producing huge destabilizing gradient spikes during training.
- **Supervision (Value Supervision)**: Giving the NN an explicit ground-truth target value for a specific board (for example, telling it *"this resulting board is worth `18.1` lines"* or *"this top-out board is worth `0.0` lines"*) and nudging its weights to match that number.
- **L1 Data Cache**: The smallest, fastest memory directly inside each CPU core (64–128 KB on Apple Silicon). Weights that stay inside L1 cache can be read roughly 20–50× faster than main RAM.
- **Mean Absolute Error**: The average absolute difference (`|predicted - true|`) in lines between the NN's value estimate and the true TB value.
- **Out-of-Distribution**: Evaluating a trained model on board states that come from outside the set of states it was trained on, to verify genuine generalization rather than memorization.

### Architecture: Why `31 KB`–`79 KB` and 7 Simultaneous Outputs
Linear models can only add up independent penalties — they cannot express interactions like *"a hole on column 9 is fine if the I-piece is next and row 2 has 8 filled cells."* To capture those interactions while staying fast enough for real-time search, we built a tiny C++ MLP modeled after **Stockfish's NNUE**: small enough to live permanently inside the CPU's L1 data cache and fast enough to evaluate tens of thousands of leaves per second.

Our NN takes **88 inputs** per board: the 40 raw binary cells of the 4×10 playfield, plus 10 column heights, 10 per-column hole counts, 4 row fill counts, and 24 normalized structural summary features from Section 2. It passes those 88 inputs through two small hidden layers with ReLU activations and outputs **7 values simultaneously** — one predicted expected-lines value $\hat{V}(b, c)$ for each possible current piece (`I, O, L, J, T, S, Z`).

- **Why output all 7 pieces at once?** During 2-ply lookahead, once you place piece 1 and piece 2 to reach a leaf board, you need the expected value averaged over all 7 possible 3rd pieces. Predicting all 7 outputs in a single forward pass means you pay for the input and hidden layers **only once per leaf board**.
- **Why keep the NN tiny?**
  - The **`64×32`** MLP has `8,007` parameters (**`31.3 KB`**).
  - The **`128×64`** MLP has `20,103` parameters (**`78.6 KB`** — **`8,535×` smaller** than the `670.9 MB` TB).
  - Both fit entirely inside the CPU's L1 data cache, avoiding slow main-memory stalls. We also organized the weight matrices so zero input cells (empty squares) and zero ReLU activations are skipped completely.

### Two Surprises That Made or Broke Performance

#### Surprise 1: Why Training Only on Good Boards Failed — and Candidate-Child Supervision Fixed It
Our very first `64×32` NN trained only on the parent board's 7 values (Huber loss) plus move ranking over in-TB moves. It scored a disappointing **`15.05` lines** at 1-ply and **`52.67` lines** at 2-ply.

*Why did it fail?* During gameplay, a 1-ply agent considers *all* ~25 legal moves — including terrible trap moves that create fatal overhangs or leave the next piece with zero legal placements (instant top-out). Because our training dataset came from strong TB games, those terrible trap boards **almost never appeared as parent boards** in the dataset! And because lethal ply-2 top-out boards aren't stored in the TB, our original filter skipped them entirely. As a result, the NN had never been told those trap boards were worth `0.0` lines — so it occasionally hallucinated a high score for a move that immediately died!

Adding two fixes to the training loss solved this completely:
1. **Candidate-child value supervision**: Directly supervise the NN's predicted value (Huber loss) on *every* legal child board of each decision (good moves *and* bad trap moves), not just the parent board.
2. **Explicit terminal top-out targets**: Whenever a candidate move leaves the Next-Box piece with zero legal moves, include it in training with an exact ground-truth value of **`0.0` lines** rather than filtering it out.

That single change jumped the exact same `31.3 KB` (`64×32`) NN from **`15.05 → 22.73` lines (1-ply)** and **`52.67 → 65.88` lines (2-ply)**, and pushed the `78.6 KB` (`128×64`) NN to **`32.27` lines (1-ply — beating the 2-ply hand-tuned heuristic!)** and **`71.79` lines (2-ply)**.

#### Surprise 2: Why 2-Ply Search + Tiny NN Crushed 1-Ply (`+39.5` Lines on the Same Weights)
Using the *exact same* `78.6 KB` trained weights:
- **1-ply search** (pick the move maximizing `lines cleared + NN value of resulting board`) averaged **`32.27` lines**.
- **2-ply search** (search all legal placements of current piece + Next-Box piece, then evaluate the resulting leaf boards with the NN) averaged **`71.79` lines** — a massive **`+39.52` line jump**!

*Why?* This is the exact same reason **Stockfish NNUE** works so well in chess: NNs excel at evaluating *smooth, medium-term strategic potential* (how clean, flexible, and diggable a board is), whereas they struggle with *sharp, discrete 1-move geometry* (e.g., whether an S-piece actually fits into a specific notch on the next turn or tops out). Pairing a shallow exact 2-ply search (which handles exact piece fit, tucks, line clears, and immediate top-outs) with a tiny NN at the leaves gets the best of both worlds. For real StackRabbit, this maps directly onto the existing 2-ply search in `high_level_search.cpp`.

### Does the NN Generalize to Unseen Boards?
A key worry with NN distillation is memorization: what if the NN only works on boards it saw during training? To test out-of-distribution generalization on genuinely unseen boards, we trained a `64×32` NN strictly on an earlier 15.95-million-board TB (`round8`) and tested its predictions on held-out boards that only existed in the later 19.54-million-board TB (`nb_final \ round8`).

Its out-of-distribution Mean Absolute Error was **`12.60` lines** (`9.42` on 0-hole boards). Crucially, the older TB's closed-world values are globally **`~7.5` lines lower** than the newer TB (`79.4` vs `86.85`) simply because it had fewer boards. Subtracting that global offset leaves only **`~2–5` lines** of relative prediction error on boards the NN had *never* seen — proving it smoothly interpolates across unseen Tetris shapes rather than memorizing lookup entries.

---

## 4. Partial TBs, Calibrated Fallbacks & The Self-Distillation Flywheel

### Section Glossary
- **Closed-World Value (`E[V(0)]`)**: The exact theoretical expected lines cleared from an empty board assuming that stepping outside the TB ends the game immediately with `0` future lines. It is a clean, noise-free progress bar for how strong the TB itself has become.
- **Boundary Cliff (`V = 0` trap)**: An artificial distortion caused by treating out-of-TB states as `0.0` lines, which tricks a TB agent into picking terrible in-TB moves just to avoid leaving the table.
- **Safety Margin**: A fixed line penalty (here `1.0`–`2.0` lines) subtracted from the NN's estimate for out-of-TB moves, so the agent trusts exact TB values on close calls and only exits when an out-of-TB move is clearly better.
- **Self-Distillation**: Training a student NN strictly on a partial TB's own values, without using any external or larger TB.
- **Frontier Exploration / Rollouts**: Playing thousands of simulated games with occasional exploratory moves (`ε`-greedy probing) to discover valuable boards sitting just outside the current TB boundary.
- **MPI (Modified Policy Iteration)**: Our exact iterative solver that alternates between finding the best policy across all boards and repeatedly evaluating that policy until values converge.
- **Warm-Start Solving**: Initializing MPI with the previous round's converged values instead of all zeros, reaching the exact same mathematical solution much faster.
- **Data Leakage**: Accidentally using information from a stronger/future dataset (such as the full TB or test games) when evaluating a weaker regime. Keeping Small and Medium NNs strictly self-distilled prevented leakage.
- **Soft Boundary (Value-Bootstrapped MPI)**: Replacing the solver's harsh `V = 0` boundary for out-of-TB moves with a frozen, conservative NN estimate during MPI so interior TB values stop collapsing toward zero near the boundary.
- **State Abstraction**: Mapping many similar full-board states onto a compact canonical key (for example, encoding only the top contour and shallow holes) so a fixed-size TB covers far more of real gameplay.
- **Temporal-Difference / Self-Play Reinforcement Learning**: Updating an NN's predictions using actual simulated game trajectories and future-step value estimates rather than solely imitating a static TB.

### Experimental Setup: Zero Data Leakage
This was the most important experiment for a future 20-row DAS 18/19 TB, because a 20-row TB will **never** be complete. We created three controlled TB coverage regimes and tested them all on the exact same 10,000 held-out NES games:
1. **Small Partial TB (`673,453` boards — 3.4% of full size, solved in 18.8 seconds)**: Represents an early, very incomplete TB.
2. **Medium Partial TB (`2,487,192` boards — 12.7% of full size, solved in 2.0 minutes)**: Represents a realistic mid-sized core TB (`98.5%` move hit rate, but `72%` of games still exit the TB at least once).
3. **Full TB (`19,540,639` boards — 100% of our converged set)**: `99.79%` move hit rate (`27%` of games still exit at least once).

To prevent data leakage, for the Small and Medium TBs we trained a fresh `31.3 KB` (`64×32`) NN **using only that small/medium TB itself** — never touching the full TB at all (**self-distillation**).

### Complete Partial-TB Results (10,000 Held-Out NES Games)

| TB Size | Closed-World `E[V(0)]` | Standard TB (heuristic fallback only when all moves exit) | TB + NN Fallback only when all moves exit | Standalone Self-Distilled NN (2-ply, no TB) | **TB + Calibrated NN Out-of-TB Scoring (2-ply)** | Gain Over Standard TB |
|---|---:|---:|---:|---:|---:|---:|
| **Small (`673k` boards, 3.4%)** | `19.56` | `33.92 ± 0.32` (`93.94%` in-TB) | `33.35 ± 0.32` | `45.95 ± 0.44` (`31 KB`) | **`45.14 ± 0.43`** (margin `2.0`, `92.65%` in-TB) | **`+11.22` (`+33.1%`)** |
| **Medium (`2.49M` boards, 12.7%)** | `43.07` | `50.97 ± 0.48` (`98.51%` in-TB) | `51.01 ± 0.49` | `56.09 ± 0.54` (`31 KB`) | **`67.59 ± 0.65`** (margin `1.0`, `96.14%` in-TB) | **`+16.62` (`+32.6%`)** |
| **Full (`19.54M` boards, 100%)** | `86.85` | `89.90 ± 0.88` (`99.79%` in-TB) | `90.02 ± 0.88` | `71.79 ± 0.68` (`79 KB`) | **`104.02 ± 1.02`** (margin `2.0`, `99.06%` in-TB) | **`+14.12` (`+15.7%`)** |

*(On the Full TB, using 1-ply NN out-of-TB scoring with margin `2.0` scored `92.00 ± 0.91`, compared to `104.02` with 2-ply.)*

### Discovery 1: Why "Fall Back Only When Forced" Fails — and Why Calibrated Out-of-TB Scoring Wins Big
Look closely at columns 3 and 4 above: when we plugged the strong NN into the TB's existing fallback rule (*"only call the fallback when EVERY legal move leaves the TB"*), performance barely budged (`33.92 → 33.35`, `50.97 → 51.01`, `89.90 → 90.02`).

*Why?* Suppose 24 legal moves on a turn leave the TB (including a clean move worth `65` lines), and **just 1 terrible move** stays inside the TB (with a stored value of `2.0` lines). Under the old rule, every out-of-TB move is valued at **`0.0`**, so the agent sees `2.0 > 0.0` and willingly ruins its board just to stay inside the TB boundary!

To fix this boundary cliff without letting an over-optimistic NN lure the agent out of the TB unnecessarily ("exit traps"), we score each move as:
- **If the resulting board is inside the TB**: use its exact TB score `lines cleared + V_TB(next board, next piece)`.
- **If the resulting board is outside the TB**: estimate its future continuation with a 2-ply NN search, and subtract a conservative **safety margin** (in lines):
  $$\text{Score}_{\text{out}}(m) = \text{lines cleared} + \max\!\big(0,\; \text{NN 2-ply continuation} - \text{safety margin}\big)$$
- **If *every* legal move is outside the TB**: rank all moves directly by the 2-ply NN with no margin clamping, so relative ordering among low-value recovery moves is preserved.

We swept the safety margin on a separate 2,000-game validation set (`seed=30000000`) to see how sensitive it is:

| Safety Margin (lines) | Small (`673k`) | Medium (`2.49M`) | Full (`19.54M`) | What Happens |
|---:|---:|---:|---:|---|
| `0.0` (no penalty) | `42.13` | `64.31` | `107.18` | Slight NN over-optimism causes occasional unnecessary exits |
| **`1.0`** | `44.23` | **`68.52`** | **`107.86`** | **Sweet spot** — trusts TB on close calls, escapes real cliffs |
| **`2.0`** | **`45.59`** | `65.75` | **`107.85`** | **Sweet spot** — safely conservative across all coverage levels |
| `5.0` | `42.75` | `66.02` | `102.93` | Too conservative; rejects legitimate out-of-TB saves |
| `10.0` | `36.42` | `60.14` | `96.91` | Starts behaving like a blind boundary wall again |
| `+infinity` (old rule) | `33.13` | `50.41` | `89.09` | Treats every out-of-TB move as `0.0` whenever any move is in-TB |

The punchline on the full 19.54-million-board TB was extraordinary: allowing calibrated NN exits on just **`0.73%`** of moves (in-TB rate shifted from `99.79%` to `99.06%`) lifted real survival from **`89.90` to `104.02` lines** (`+14.12` lines, breaking 100 expected lines!). Even our huge 671 MB TB still had hidden `V = 0` boundary cliffs on rare turns, and the 79 KB NN cleanly bridged across them.

### Discovery 2: Self-Distillation Beats Its Own Teacher TB
Look at the Small (`673k`) and Medium (`2.49M`) rows in the table above:
- The `673k` TB plays at **`33.92` lines** — yet the `31.3 KB` NN trained *exclusively* on that `673k` TB plays standalone at **`45.95` lines** (**`+12.03` lines stronger than its own teacher!**).
- The `2.49M` TB plays at **`50.97` lines** — yet its self-distilled `31.3 KB` NN plays standalone at **`56.09` lines** (`+5.12`), and combining the `2.49M` TB with its own self-distilled NN via calibrated out-of-TB scoring jumps to **`67.59` lines** (**`+16.62` above the TB and `+11.50` above the standalone NN!**).

*Why can a student NN beat the TB it learned from?* Because an incomplete TB has a brutal jagged boundary: a good board that happened to be visited during sampling has value `45.0`, while an almost identical neighbor board that wasn't sampled has value `0.0`. The small NN cannot memorize those jagged holes — instead, it learns a smooth function of board geometry that automatically fills in missing neighbors with sensible values. Pairing the exact core TB with its own smooth student NN gives exact play inside the core *and* smooth survival outside it.

### Discovery 3: Expanding a TB 2.5×–3.3× Faster Using Self-Distilled Frontier Exploration
Next we ran a controlled A/B test to see whether a self-distilled NN can also **speed up growing the TB itself**.

Both arms started from the exact same `673,453`-board small TB (`E[V(0)] = 19.56` closed-world, `33.92` live). To expand a TB by one round, we run **50,000 rollout games** where on 92% of turns the agent plays its normal move and on **8% of turns** (`ε = 0.08`) it intentionally explores an alternative move to probe unvisited neighbor boards sitting just outside the TB (the **frontier**). Frequently visited new boards are then merged into the TB, and MPI is re-run using **warm-start solving**.

The *only* difference between the two arms was what guided out-of-TB moves and frontier probes during those 50,000 games:
- **Arm A (Hand-tuned heuristic probe)**: Discovered `+49,317` new boards (`722,770` total).
- **Arm B (Self-distilled `31.3 KB` NN with `2.0`-line safety margin)**: Discovered `+60,603` new boards (`734,056` total — virtually the same TB size, within `1.5%`).

| Metric After 1 Expansion Round | Starting TB (`673k`) | Arm A: Heuristic Probe (`723k`) | Arm B: Self-Distilled NN Probe (`734k`) | Speedup |
|---|---:|---:|---:|---:|
| **Closed-world exact value `E[V(0)]`** | `19.56` | `20.49` (`+0.93`) | **`21.90` (`+2.34`)** | **`2.51×` more exact value gained** |
| **Live 10,000-game survival** | `33.92` | `34.78 ± 0.33` (`+0.86`) | **`36.76 ± 0.35` (`+2.84`)** | **`3.30×` bigger gameplay jump** |
| **Coverage of messy 3+-hole positions** | `6.40%` | `9.90%` | **`16.17%`** | Far better recovery shapes discovered |

*Why did Arm B win by 2.5×–3.3× with nearly the same board count?* The hand-tuned heuristic wanders into weaker shapes outside the TB (and gets trapped inside the boundary by the old fallback rule), so many of the 49k boards it adds don't actually help optimal play survive longer. By contrast, the self-distilled NN with a 2-line safety margin plays much closer to optimal outside the TB (`45.14` vs `33.92` lines!), so it naturally escapes into the exact high-value recovery lines (especially 3+-hole digs) that the TB was missing. When MPI locks those states in, the TB's real strength jumps over three times faster per round.

### Discovery 4: Why TB Size Still Matters — and How to Overcome It on a Full 20-Row Board
Even with our best calibrated NN fallback, **TB size still mattered enormously** in the results table above:
- **Small (`673k` boards, 3.4%) + Self-NN**: **`45.14` lines**
- **Medium (`2.49M` boards, 12.7%) + Self-NN**: **`67.59` lines**
- **Full (`19.54M` boards, 100%) + Full-NN**: **`104.02` lines** (`+36.4` lines ahead of Medium!)

That `36.4`-line gap between Medium and Full is a crucial warning sign for full 20-row NES Tetris, where we will *never* be able to build an exhaustive TB covering 99.8% of play. Understanding **why** Medium trailed Full points directly to what we should do differently on a 20-row board:

1. **The closed-world `V = 0` boundary doesn't just hurt out-of-TB moves — it corrupts *interior* values inside a small/medium TB.**
   - Look at the Medium TB's closed-world value: `E[V(0)] = 43.07`, literally *half* of the Full TB's `86.85`!
   - Even on a spotless 0-hole board deep inside the Medium TB, MPI knows there is a ~1.5% chance per move of accidentally exiting the 2.49M set into `V = 0` (instant death). That doesn't merely shrink all values by half — it **distorts the interior policy** toward overly passive moves that hug the 2.49M boundary instead of taking strong recovery shapes whose follow-up moves happen to sit outside the TB.
   - Worse, the Medium Self-NN was trained *only* on those `V = 0`-truncated values (`E[V(0)] = 43.07`), so the student NN inherited the exact same boundary distortion! Notice that the identical `64×32` (`31 KB`) NN architecture reached **`65.88` lines** standalone when trained on Full TB values, versus **`56.09` lines** when trained on Medium TB values — a **10-line gap purely from teacher-value distortion**.
2. **Survival in Tetris is multiplicative, so sparse recovery coverage compounds.**
   - A `98.5%` move hit rate (Medium TB) sounds high, yet `72%` of games still leave the TB at least once (compared to `27%` on Full). Because one misplayed dig ends a 100-line game immediately, having exact solutions for messy 1–3+ hole digs (`85.7%` of the Full TB's 19.54M boards) compounds massively over long games.
3. **Training diversity and NN capacity**:
   - The Medium NN saw only 2.49M labeled boards (far fewer rare multi-hole dig patterns) and used `64×32` (`31 KB`), whereas the Full TB trained a `128×64` (`79 KB`) NN across 19.54M exact boards.

#### Brainstorming: Five Ways to Break Past the "Partial-TB Ceiling" in Full 20-Row Tetris
Since a 20-row DAS 18/19 TB will always operate in a partial-coverage regime, here are five concrete levers to close that gap without needing an exhaustive TB:

1. **Soft-Boundary (Value-Bootstrapped) MPI inside the solver** *(highest-leverage idea to test next)*:
   - In our experiment, MPI always plugged **`0.0`** into out-of-TB children, which caused the Medium TB's interior values to collapse (`43.07` vs `86.85`).
   - Instead, after Round 0 builds an initial core TB and trains a self-distilled NN, Round 1+ MPI can evaluate out-of-TB children using a **frozen, conservative NN value** (`max(0, V_NN - safety_margin)`) instead of `0.0`!
   - Keeping the NN weights **frozen** during each MPI solve (plus a safety margin or a slight discount factor `γ < 1`) preserves mathematical convergence and prevents runaway self-inflation — while eliminating the `V = 0` boundary collapse so both the interior TB policy *and* the next round's self-distilled NN learn undistorted survival values!
2. **Fine-tune the NN past the TB via Self-Play Reinforcement Learning / Temporal-Difference Learning**:
   - In our Medium experiment, the NN stopped at pure imitation of the truncated Medium TB (`56.09` lines). Once a partial TB gives the NN a strong, low-variance warm start on clean stacking, we can fine-tune the NN directly on **real simulated game outcomes** (2-ply self-play rollouts with temporal-difference targets) — which have **zero `V = 0` boundary bias** on messy out-of-TB digs!
3. **State Abstraction to stretch a 25M–75M TB much further**:
   - Raw 20-row bitboards waste huge capacity on irrelevant bottom rows. Defining TB keys over a **canonical top surface / height-capped clean core** (e.g., relative heights across the active top rows + well depth, letting the NNUE handle deep/messy boards) lets a 50M-board TB cover a far higher fraction of real Level 18/19 stacking than raw 200-bit boards ever could.
4. **Value-Targeted (Error-Weighted) TB Expansion instead of pure visit-count expansion**:
   - A tiny NN already predicts clean 0-hole boards within `~1.5` lines. Instead of spending scarce TB slots on easy boards the NN already handles well, prioritize adding frontier states where **NN prediction error or move regret is highest** (tricky 1–2 hole transitions, high-stack burns, and low-DAS pinch points).
5. **Scale Up the NNUE + Deeper Out-of-TB Search**:
   - Going from `64×32` (`31 KB`) to `128×64` (`79 KB`) added `+5.9` lines (`65.88 → 71.79`) with no sign of plateauing. On a 20-row board, a larger incrementally-updated NNUE (`256×64` or `512×64`) paired with StackRabbit's existing 2-ply search + playouts will be far stronger whenever play leaves the TB.

### Recommended Blueprint for 20-Row DAS 18 / DAS 19
Instead of trying to enumerate or solve an impossibly huge 20-row TB upfront, combine these discoveries into a self-reinforcing flywheel:

```text
Offline Flywheel (repeat per round):

  ┌─────────────────────────────────────────────────────┐
  │  1. Core TB (2M–25M clean DAS boards)               │◄──────────────┐
  │     Solved with closed-world V=0 boundary (MPI)     │               │
  └──────────────────────────┬──────────────────────────┘               │
                             │  On-policy + recovery                    │
                             │  self-play sampling                      │  Warm-start MPI
                             ▼                                          │  (--init)
  ┌─────────────────────────────────────────────────────┐               │
  │  2. Self-Distilled Tiny NNUE (~40–80 KB)            │               │
  │     Parent + child Huber + Boltzmann move ranking   │               │
  └──────────────────────────┬──────────────────────────┘               │
                             │  Calibrated out-of-TB scoring (margin    │
                             │  1–2) + 3× faster frontier probing       │
                             ▼                                          │
  ┌─────────────────────────────────────────────────────┐               │
  │  3. Expand Next TB Round                            ├───────────────┘
  │     Add high-value recovery & frontier states       │
  └─────────────────────────────────────────────────────┘

Online Play (combining Core TB + Tiny NNUE):

  Core TB   ──┐
              ├──►  2-Ply Exact DAS Search
  Tiny NNUE ──┘       • In-TB leaf  : exact V*(leaf, next)
                      • Out-of-TB   : max(0, NN 2-ply - margin)
```

---

## 5. Interactive Browser Inspector

### Section Glossary
- **Transactional Reload**: Staging newly loaded TB and NN files in temporary memory first and swapping them in only if *both* files validate cleanly, so a corrupted file never breaks a running session.

### Usage & Capabilities
To make it easy to visually inspect *where* and *why* the compact NN agrees or disagrees with the 671 MB TB, we extended the local HTTP server ([experimental/tablebase/server.cpp](file:///Users/gregcannon/Documents/StackRabbit/experimental/tablebase/server.cpp), [experimental/tablebase/server_routes.hpp](file:///Users/gregcannon/Documents/StackRabbit/experimental/tablebase/server_routes.hpp)) and browser interface ([experimental/tablebase/index.html](file:///Users/gregcannon/Documents/StackRabbit/experimental/tablebase/index.html)) to load any NN evaluator via `--nn=PATH` — either alongside a TB or completely standalone:

```bash
# Side-by-side Full TB + 78.6 KB Tiny NN inspector:
./build/tablebase_server 3030 \
  experimental/tablebase/data/nb_final_nes.tb.bin \
  --nn=experimental/tablebase/data/nb_nn_128x64.nn.bin

# Standalone NN mode (no TB required):
./build/tablebase_server 3030 --nn=experimental/tablebase/data/nb_nn_128x64.nn.bin
```

In the browser (`http://127.0.0.1:3030/`), you can:
- Play any policy live: **Next-Box TB**, **TB + NN Partial-OUT (`margin = 2.0`)**, **Tiny NN (2-ply)**, **Tiny NN (1-ply)**, or **Heuristic**.
- Inspect the NN's predicted value $\hat{V}(b, c)$ side-by-side with the exact TB value $V^*(b, c)$ and color-coded signed error ($\hat{V} - V^*$) for the current piece and all 7 piece choices.
- Compare per-move **TB Q**, **NN Q (2-ply and 1-ply)**, and **Heuristic** scores in the legal-placements table.
- Click **Step to Disagreement** (with **`TB ≠ NN (2p)`** or **`TB ≠ Heur`**) to fast-forward straight to interesting turns where the 79 KB NN and the 671 MB TB disagree, and hot-reload updated files transactionally via **Reload**.

---

## 6. Engineering & Code Review Workflow Takeaways

### Section Glossary
- **SIMD / ARM NEON**: Single Instruction, Multiple Data — CPU instructions that perform the same arithmetic operation on 4 `float32` numbers simultaneously in a single clock cycle.
- **False Sharing**: A multithreading slowdown where two CPU cores modify *different* variables that happen to sit inside the same hardware cache line (128 bytes on Apple Silicon), forcing the cores to repeatedly invalidate and re-transfer that cache line.
- **Finite-Difference Gradient Check**: A unit test that nudges each NN weight by a tiny `±ε` and compares `(loss(w + ε) - loss(w - ε)) / (2ε)` against the analytical backpropagation gradient, proving the training math is bug-free.
- **Seam Bug**: A defect that occurs not inside a single isolated algorithm, but at the boundary between two components (for example, C++ tie-breaking vs. JavaScript table sorting, or file loading vs. live server state).

### Why Adding a 3rd Performance & Scale Reviewer on the NN Hot Path Paid Off
Following the workflow guidelines from [docs/tablebase_retrospective.md](tablebase_retrospective.md), we used 2 code reviewers (**Logic Reviewer** with the 4-point seam checklist + **Simplicity Reviewer**) on every phase, and added a **3rd Performance & Scale Reviewer** specifically for the NN training and inference engine.

By inspecting the actual compiled Apple Silicon (ARM64) assembly output (`clang++ -O3 -S`), that 3rd reviewer caught five subtle low-level bottlenecks that looked completely harmless in C++ source code:
1. **Branching ReLU prevented SIMD vectorization**: Writing `if (v > 0) a = v` generated conditional-store branches instead of single-instruction ARM NEON `fmax` vector ops.
2. **Serial floating-point dependency chain in output dot products**: Accumulating into a single scalar `sum += ...` forced a 16-deep serial latency chain; splitting into 4 independent accumulators let the CPU execute 4 vector lanes in parallel.
3. **Hidden macOS stack probes**: Declaring slightly over 4 KB of stack arrays inside `evalAll` / `evalPiece` forced the compiler to emit `___chkstk_darwin` stack-probe calls on every leaf evaluation; adding a `< 4 KB` fast-path stack frame for NNs with `h1 + h2 <= 256` eliminated them completely.
4. **Thread churn and cache-line false sharing**: Spawning new OS threads on every training mini-batch and placing per-worker accumulators adjacent in memory caused Apple Silicon's 128-byte cache lines to bounce between cores. Switching to a persistent thread pool with 128-byte-aligned worker states removed the contention.
5. **Over-generating legal moves just to check survival**: Checking whether a board had *any* legal placement for the next piece was generating and deduplicating all ~30 moves; an early-exit placement check returned `true` on the first legal placement found.

Fixing those five items delivered a **`2.0×` training speedup** (`36.8s → 18.6s` for `64×32` on 10 cores) and a **`+45%` 1-ply inference speedup** (`178,200 → 258,400 moves/sec` for `128×64`).

### Seam Bugs Caught Before Committing
Across all four phases, the 4-point Logic Reviewer checklist and Simplicity Reviewer caught real cross-component bugs before they ever landed in git:
- **Linear distillation**: Catching singular/NaN solver inputs, centering features properly for Ridge conditioning, and ensuring unit tests used a board size with real line clears (`2×10`) rather than a tiny `4×4` board where `V* = 0` everywhere.
- **NN training & inference**: Clamping each piece's predicted value to `>= 0` individually before averaging across the 7 next-piece probabilities, verifying all 6 parameter blocks (`W1, b1, W2, b2, W3, b3`) against finite-difference gradient checks (`< 1e-4` relative error), and guaranteeing corrupt `.nn.bin` files never overwrite an already-loaded NN.
- **Partial-TB hybrid move selection**: Eliminating duplicate NN evaluations when all moves exit the TB (via a fast 2-pass lookup), enforcing exact tie-breaking parity with the pure TB selector when `safetyMargin = +infinity`, and removing a debug-only `assert` that masked a runtime safety check.
- **Server & browser inspector**: Fixing JavaScript table sorting and `1e-12` tie-break parity with C++ for calibrated out-of-TB moves, preserving the user's selected AI policy across weight reloads, gating TB-vs-NN disagreement flags so all-out-of-TB boards aren't mislabeled as TB disagreements, and making dual TB + NN reloads strictly transactional.
