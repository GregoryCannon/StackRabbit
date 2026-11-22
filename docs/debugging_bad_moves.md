# Debugging Suspicious Engine Placements

A repeatable playbook for investigating why StackRabbit chose placement **A** over a human-preferred placement **B**, separating search/budgeting bugs from eval/playout bugs from genuinely defensible moves.

The last section catalogs the recurring diagnostic views used across investigations so they can be integrated into a web debugging tool.

---

## Quick Reference: Coordinates & Inputs

- **HTTP / query format (`p=...`)**: comma-separated (`board,level,lines,curPiece,nextPiece,timeline,playoutCount,playoutLength,pruningBreadth,aggression,dasCharge,reactionTime,`).
- **C++ CLI / `mainProcess` pipe format**: `|`-separated for legacy CLI helpers or `,`-separated in `live_game_server` (see [engine_endpoints.md](engine_endpoints.md)).
- **Placement coordinate systems** (easy to mix up when translating `Default:` output into internal lookup map keys):
  - **Response format** (`Default:relRot,relX,relY` and `[rot, xOff, yOff]`): relative to piece spawn (`INITIAL_X = 3`, `initialY = -2` for `I` and `-1` for all other pieces).
  - **Lookup map key** (`encodeLockPosition` / `encodeLockCell`): `"rot|x|y"` in board coordinates, where `x = 3 + relX` and `y = initialY + relY`.
    - Example for `L` (`initialY = -1`): `Default:3,-1,12` corresponds to lookup key `"3|2|11"`, and `[0,-4,11]` corresponds to `"0|-1|10"`.
- **Piece indices (`PIECE_LIST`)**: `0=I, 1=O, 2=L, 3=J, 4=T, 5=S, 6=Z`.
- **Existing compile-time / code debug flags**:
  - `TMT_LOGGING_ENABLED` in `src/backend/tournament_search.cpp` — prints R1/R2/R3 windows, per-cont scores, and Cut 1 / Cut 2 decisions.
  - `PLAYOUT_LOGGING_ENABLED` / `PLAYOUT_RESULT_LOGGING_ENABLED` in `src/backend/playout.cpp` and `src/backend/high_level_search.cpp`.
  - `fastEval(..., &outExplanation)` — returns a semicolon-delimited feature breakdown (`surface=...;avgHeight=...;tetrisReady=...;builtOutLeft=...`).

---

## Diagnostic Decision Tree

When a placement looks wrong, work top-down through these five stages. Each stage narrows the root cause to one subsystem before diving into code.

```
Suspicious move A chosen over human move B
  │
  ├─► Stage 1: Fast parameter sweeps (budget, rt, depth, mode, DAS)
  │     ├─ Flips with higher budget? ────────► Likely search / tournament / candidate-filtering bug
  │     ├─ Flips at rt=0? ───────────────────► Phantom / adjustment reachability compromise (check Stage 2)
  │     ├─ Flips across depth 0 → 2 → 4 → 8? ► Playout horizon or reward-pricing effect (check Stage 5)
  │     └─ Flips across safe / aggro / hyper?► Mode weights or DIG/STANDARD transition (check Stage 5)
  │
  ├─► Stage 2: Phantom & adjustment collapse breakdown
  │     └─ Decompose each phantom's 7-piece score: lockValue + dasPenalty + inputCost
  │
  ├─► Stage 3: High-N ground truth vs. tournament lookup (per next piece)
  │     ├─ Ground truth prefers B, tournament picked A ──► Stage 4: Tournament funnel & budget trace
  │     └─ Ground truth ALSO prefers A ──────────────────► Stage 5: Eval, playout decomposition & sequence traces
```

---

## Stage 1: Fast Parameter Sweeps

Before writing any custom instrumentation, re-run the same position while varying **one axis at a time** (via `precompute_cli` or `/precompute-sync`):

1. **Compute budget sweep** (e.g. `150ms`, `300ms`, `1000ms`, `3000ms` at `-2`, or fixed `-1` budgets):
   - If **A** disappears at higher compute time, the engine's own playout model agrees **B** is better — the issue is in candidate generation, tournament cuts, sample allocation, or LCB calibration (jump to Stages 3 & 4).
   - If **A** stays (or grows stronger) at `3000ms+`, the preference is real under the current eval/playout model.
2. **Reaction-time sweep** (`rt = 0` vs actual `rt`, e.g. `18` or `60`):
   - `rt = 0` reveals the unconstrained best placement for each of the 7 next pieces.
   - If `rt = 0` picks **B** for some pieces and **A** for others, the `rt > 0` default is a **phantom compromise** (check Stage 2 to see how close the 7-piece average is and whether **A** has an adjustment advantage).
   - If `rt = 0` picks **A** for *all* 7 pieces (or for the exact single next piece in question), the issue is inside the per-piece lock value or DAS penalty, not phantom collapse.
3. **Playout depth sweep** (`playoutLength = 0, 2, 4, 8, 12`):
   - `depth = 0` isolates `fastEval` (2-ply).
   - Comparing `0 → 2 → 4 → 8 → 12` shows *which horizon* creates the preference (e.g. a bug in intermediate playout rewards won't appear at `depth = 0`, while a horizon/myopia effect often shrinks or flips by `depth = 12–16`).
4. **Mode & DAS sweep** (`safe` / `aggro` / `hyper`; `dasCharge = -1` vs `0..16`):
   - `safe` vs `hyper` immediately flags `burnCoef` / `DIG` vs `STANDARD` weight discrepancies.
   - `dasCharge = -1` (tap) vs actual `dasCharge` isolates DAS reachability and `partialPenalty` / `minPenalty` effects.

---

## Stage 2: Phantom & Adjustment Collapse Breakdown

When `PRECOMPUTE` picks a surprising `Default:` phantom or a surprising single-piece adjustment, inspect `PrecomputeState` right before `collapseAndFormatPrecompute`:

1. **Rank the top 5–10 phantoms** by their exact NES-weighted 7-piece average (`1/28` for repeating the current piece, `9/56` for each of the other 6 pieces).
2. **List every reachable adjustment** (`phantom.possibleAdjustments`) along with its `dasChargeAfter` for both **A**'s phantom and **B**'s phantom.
3. **Break down every next piece (`I, O, L, J, T, S, Z`)** into its four additive terms:
   $$\text{score}(p) = \text{lockValue}[p][\text{key}] + \text{dasPenalty}(\text{dasAfter}) + \text{adjustmentInputCost}$$
4. **Compare "with adjustments" vs "no adjustments"** for both phantoms.

**What this catches:**
- **Free mid-air adjustments**: e.g. a phantom that can rotate in place for one bad next piece (`Z`) without losing DAS, shifting the 7-piece average by several points.
- **Hidden DAS penalties**: an adjustment losing DAS (`dasAfter < 10`) that takes `-30` or `-100` from `minPenalty`, or conversely a move winning only because its competitor got hit by a DAS penalty.
- **Close calls**: seeing whether **A** beats **B** by `+5 pts` (close strategic trade-off) vs `+100 pts` (clear bug or major oversight).

---

## Stage 3: High-N Ground Truth vs. Tournament Scores

This is the single highest-signal check in the entire workflow. It definitively separates **search bugs** from **evaluation/model preferences**.

For the 2–5 candidate first placements under investigation:
1. Run `searchDepth2(gameState, curPiece, nextPiece, firstPieceDasCharge, 15, ...)` for each of the 7 next pieces.
2. For each candidate first placement, take its top ~5 continuations by `fastEval` (or all continuations if suspecting a missed-cont bug).
3. Run a large canonical batch (`N = 2000–4000` playouts via `getPlayoutScore(..., sequenceOffset = 0)`) on each continuation — **no tournament cuts, no LCB penalty**.
4. Record per candidate & per next piece:
   - **Raw mean** (`immediateReward + mean(playoutScores)`) and **standard error (`SE`)**
   - **Death rate (%)** (fraction of playouts hitting `deathCoef`)
   - **Best continuation key** (`secondPlacement`), its fast-eval rank (`cont#k`), and lines cleared on ply 2
   - **Weighted 7-piece average** (and `±SE` of the difference between **A** and **B**)
5. Put the **Tournament published score** (`lockValues[i][key]`) side-by-side with the **Ground-Truth raw mean**.

**How to read the table:**
- **Tournament disagrees with Ground Truth** (e.g. Ground Truth says **B** is `+100` better, or `+28` better on `next = J`, yet Tournament published **B** much lower) → **Search/tournament bug**. Go to **Stage 4**.
- **Ground Truth agrees with Tournament** (and the gap is statistically significant, `gap >> SE`) → **Eval/playout/geometry preference**. Go to **Stage 5**.
- **Ground Truth gap is small (`< ~10 pts`) with `0%` deaths on both sides** → Inspect the 1–2 highest-swing next pieces' resulting boards (Stage 5) to confirm whether it's a legitimate dependency trade-off rather than a bug.

---

## Stage 4: Tournament Funnel & Budget Trace

When Stage 3 shows the tournament mis-scored a move that Ground Truth likes, trace the exact 3-round funnel for the offending `nextPiece`:

1. **Candidate list & fast-eval ordering**:
   - How many first-piece candidates entered Round 1? Are any of them **unreachable** under the real `dasCharge` or timeline?
   - What fast-eval rank did **B** get (and therefore how many Round-1 continuations did it receive under tiered R1)?
2. **Round 1 (`n1`) & Cut 1**:
   - Print `n1` and whether tiered R1 fell back to 1 cont/candidate due to a tight budget.
   - Check whether **B** was eliminated at Cut 1 because its `cont#0` was a trap while `cont#1..4` (its true best continuation) hadn't been sampled yet.
3. **Round 2 (`n2`) & Cut 2**:
   - Print `n2`, the pooled sample count `n1 + n2` before Cut 2, and the exact top `TMT_R3_COUNT` (4) survivors vs who finished `#5+`.
   - Check whether **B** had high variance / an unlucky death cluster in `[0, n1+n2)` that knocked it just outside the top 4 (especially if another slot was taken by an irrelevant/unreachable candidate).
   - Check whether timed R2 hit `n2 = 0` (discovery skipped completely).
4. **Round 3 (`n3`) & LCB cliff**:
   - Print final sample counts `n` and `SE` for **A** vs **B**.
   - Compare **raw mean** vs **published LCB** (`mean - tCrit * SE`): if **A** reached Round 3 (`n ≈ 1500`, tiny LCB penalty) while **B** was cut at Cut 2 (`n ≈ 100`, huge LCB penalty, especially with high death variance), **B**'s published score can be depressed by 100+ points purely from unequal sample depth.

---

## Stage 5: Playout & Eval Decomposition + Sequence Traces

When Stage 3 shows Ground Truth itself prefers **A** over **B** (even at `N = 4000`), inspect *why* the playouts score **A** higher:

1. **Inspect boards & `fastEval` explanations**:
   - Print the board and `fastEval(..., &expl)` after ply 1 (**A** vs **B**) and after ply 2 (for the 1–2 highest-swing next pieces, e.g. `T`, `Z`, `S`, `O`).
   - Check `evalContext.aiMode` (`STANDARD`, `DIG`, `LINEOUT`) before and after ply 1–2.
2. **Aggregate playout metrics** over `N = 200–2000` playouts (compare **A → bestCont** vs **B → bestCont**):
   - Breakdown of average score: `ply1–2 immediateReward` + `intermediate playout line-clear reward` + `leaf fastEval` + `death penalty`.
   - Operational stats: `death%`, `avg burns`, `avg tetrises`, `% of playouts that cleared holes`, `% reaching an open well`.
   - Horizon check: repeat at `depth = 4, 8, 12, 16` to see whether **A** is genuinely healthier long-term or only winning a short-horizon artifact.
3. **Side-by-side canonical sequence trace**:
   - Pick 2–5 individual canonical sequences (`seqIdx`) where **A** beats **B** by a large margin (or where **B** unexpectedly dies/burns).
   - Run the *exact same* piece sequence on both starting boards and print every playout ply:
     `ply | piece | aiMode | chosen placement | lines cleared | step reward | board / holes / well state`
   - Side-by-side traces immediately expose subtle root causes that averages hide:
     - **Reward-pricing inconsistency**: e.g. intermediate playout plies using different `burnCoef` weights than plies 1–2 and leaf.
     - **1-ply playout myopia**: e.g. a greedy 1-ply `DIG` move covering the well on ply 3 because it reduces height/holes right now.
     - **Tapper-vs-DAS playout gap**: playouts using tapper move search to execute free tucks/spins that a DAS player wouldn't easily make.
     - **Legitimate Tetris geometry**: e.g. **B** leaving a 2-wide valley that causes `T` and `Z` to build 5-high spires while **A** leaves clean homes for every piece.

---

## Symptom → Root Cause Cheat Sheet (From Real Investigations)

| Observed Symptom | Root Cause Found | Key Diagnostic That Exposed It |
|---|---|---|
| Depth 0 strongly prefers digging (**B**), but depth ≥ 4 flips to covering the well (**A**) in `HYPER` `DIG` mode | `playout.cpp` priced intermediate playout burns with `STANDARD HYPER` (`burnCoef = -45`) instead of `DIG` (`-1`), while plies 1–2 and leaf used `DIG` | Depth sweep (`0, 3, 4, 8`) + `safe` vs `hyper` + playout score decomposition (`immReward` vs `intermediateReward` vs `leafEval`) |
| High-budget `4000ms` `DIG` playouts still prefer an overhang (**A**) over clean dig (**B**) | Genuine board geometry + 1-ply myopic `DIG` playouts blocking col 9 + tap-approximated free tucks in playouts | Depth `4 → 16` sweep + side-by-side canonical sequence traces tracking open-well % and hole clears |
| `161ms` `PRECOMPUTE` picks `[1,4,12]` over `[1,1,12]`, even though Ground Truth says `[1,1,12]` is `+109` better | `TMT_R1_MAX_CONTS = 1` eliminated `[1,1,12]` at Cut 1 on `O` and `S` because its fast-eval `cont#0` was a trap | Ground Truth vs Tournament comparison → R1/Cut 1 trace showing `cont#0` vs `cont#1` |
| `300ms` `PRECOMPUTE` at `dasCharge=0` makes a strange overhang adjustment for `next=J` only; disappears at `1000ms` | `precompute.cpp` built first moves with `DAS_CHARGE_MAX` instead of real `dasCharge=0`, injecting 4 unreachable placements that stole a `TMT_R3_COUNT` slot and dropped the best move into a `n=126` LCB cliff | Budget sweep (`300ms` vs `1000ms`) + candidate list inspection + Cut 2 / LCB sample-size comparison |
| `3000ms` `rt=18` places `L` in middle (`[3,-1,12]`) instead of left (`[0,-4,11]`), while `rt=0` uses left for `O/J/S` | Genuinely defensible `+5.8 ± 0.9` pt dependency-coverage edge (`T +36.5`, `Z +22.9` w/ free rotation adj vs `S -17.6`, `O -4.9`, `J -4.0`), `0%` deaths on both | `rt=18` vs `rt=0` sweep + 4000-playout per-next-piece table + board inspection for `T`, `Z`, `S` |

---

## Reusable Views for a Web Debugging Tool

The same five diagnostic queries came up in almost every investigation. Below are the views that would deliver the highest leverage if exposed by a debug endpoint (e.g. on `live_game_server` or WASM) and rendered in the web tool.

### 1. Phantom & Adjustment Collapse Panel *(cheap — reuses `PrecomputeState` directly)*
- **Inputs**: standard `PRECOMPUTE` request.
- **Data**:
  - Top N phantoms (e.g. top 10) sorted by weighted 7-piece average, showing `defaultPlacement`, `weightedAvg`, `noAdjAvg`, `dasChargeAfter`, and `numAdjustments`.
  - Clicking any phantom expands a **7-row (`I O L J T S Z`) table**: chosen placement, `rawLockValue`, `dasPenalty` (and bucket: full/partial/min), `inputCost`, `total`, `dasAfter`, and clickable board preview.
  - Toggle to compare **two phantoms side-by-side** (showing per-piece deltas `A − B` highlighted by magnitude).

### 2. Per-Next-Piece Head-to-Head + Ground Truth Panel *(medium cost — runs on demand for 2–4 selected placements)*
- **Inputs**: board/config + 2–4 user-selected first-piece lock keys (e.g. engine winner vs human move) + `N` (e.g. `1000` or `4000` playouts).
- **Data**:
  - 7-row (`I O L J T S Z`) table comparing each placement's **Tournament published score** vs **Ground-Truth raw mean ± SE**, **death %**, **best continuation (`secondPlacement`)**, ply-2 line clears, and resulting post-ply-2 board preview.
  - Summary footer: weighted 7-piece GT average, `A − B` gap, `±SE`, and automatic flag when **Tournament rank != Ground-Truth rank** (immediately tells you "this is a search bug, open the Tournament Funnel panel").

### 3. Parameter Sweep Strip *(on demand)*
- **Inputs**: base request + one selected sweep axis (`budget`, `reactionTime`, `playoutLength`, `aggressionLevel`, or `dasCharge`).
- **Data**: compact table of `(paramValue → Default move, 7-piece adjustments, score gap between A and B)`.
- Replaces manually editing URLs and re-pasting requests to check `rt=0`, `depth=0/4/8`, `safe/aggro/hyper`, or `300ms` vs `1000ms`.

### 4. Tournament Funnel & Budget Inspector *(zero hot-path overhead via optional trace struct)*
- **Inputs**: base request + one selected `nextPiece`.
- **Data**:
  - Setup summary: total candidates, tiered R1 active?, `n1 / n2 / n3`, elapsed ms per round, observed `moves/ms`.
  - Candidate table (sortable by fast-eval rank, Cut-1 rank, Cut-2 rank, or published LCB):
    - Fast-eval rank, R1 conts screened, Cut-1 status (kept / eliminated), Cut-2 status (kept / eliminated), final `n`, raw mean, `SE`, published LCB.
    - Expandable row showing each of the candidate's `cont#0..cont#4` (`secondPlacement`, `fastEval`, `n`, `mean`, `SE`, `death%`).
- Immediately surfaces deceptive `cont#0` Cut-1 traps, Cut-2 slot starvation, and unequal-`n` LCB cliffs.

### 5. Playout Score Decomposer & Sequence Replayer *(on demand for 1–2 selected `(first, second)` pairs)*
- **Inputs**: two `(firstPlacement, secondPlacement)` pairs + `playoutLength` + `N` (e.g. `200`).
- **Data**:
  - **Aggregate decomposition**: `ply1–2 immediateReward`, `avg intermediateReward`, `avg leafEval`, `death%`, `avg burns`, `avg tetrises`, `% holes cleared`, `% open well`, plus `fastEval` explanation string at ply 1 and ply 2.
  - **Sequence picker**: top 5 sequences where `A` beat `B` by the most, top 5 where `B` beat `A` by the most, plus manual `seqIdx` input.
  - **Side-by-side ply stepper**: for a chosen `seqIdx`, step ply-by-ply through the exact same piece sequence on both boards, showing `piece`, `aiMode`, placement, lines cleared, step reward, and board state.

### Implementation Note for Eventual Web Integration
- Views **1** and **3** require almost no new engine logic (`preparePrecomputeState` already computes the full `PrecomputeState` maps and phantom list).
- View **4** can mirror the existing `TMT_LOGGING_ENABLED` checkpoints into an optional `TournamentTrace *outTrace = nullptr` parameter so production calls pay zero overhead when `outTrace == nullptr`.
- Views **2** and **5** are pure read-only debug endpoints (`getPlayoutScore` already accepts `outPlayoutScores` and `playoutDataList`), so they can live in a separate debug route without touching hot-path structs or production endpoints.
