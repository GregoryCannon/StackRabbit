# DAS Move Search Spec (C++ port oracle)

This document specifies the frame-exact semantics of the Node.js DAS move search
(`src/server/move_search.ts` + `src/server/dfs.ts`) for the parts being ported to C++.
It is the oracle definition for the fuzz harness and the reference for the C++
implementation. Line references are to the current `cpp-das` branch.

Companion doc: `docs/DAS_explainer.md` (game-level DAS mechanics).

## 1. Scope

**In scope** (the `INITIAL_PLACEMENT` spawn path of `getPossibleMoves`):

| TS component | Role |
|---|---|
| `tryPlacement` (`move_search.ts:475`) | per-goal frame simulation of shifts/rotations/charge |
| `deDupeSortedList` (`move_search.ts:164`) | midair state dedup |
| `exploreLegalPlacementsUntilLock` (`move_search.ts:209`) | fall phase, wall charges, tuck-state registration |
| `searchForTucksOrSpins` / `tryInput` (`dfs.ts`) | depth-1 tuck/spin exploration |
| `getPossibilityFromSimState` (`move_search.ts:370`) | possibility assembly, `dasChargeAfter` |
| `deDupeLockPossibilities` (`move_search.ts:155`) | final lock dedup |
| `readyForTuckInputsDas` (`move_search.ts:328`) | slow-tap / quicktap input gating |

**Out of scope** (remains TS-only or unchanged in C++):
- Adjustment searches (`adjustmentState.isAdjustment`, `adjTimeSimState`,
  `preAdjInputSequence`, `arrFrameVoluntarilyMissed`, `reactionTime` snapshots).
  Confirmed dead code in C++ (`adjustmentSearch` is test-only).
- Playouts / game simulation: keep the tap-timeline approximation
  (`getDasEquivalentInputTimeline`) there.
- `PieceRangeContext` reachability tables (`piece_ranges.cpp`): stay tap-derived.
- Multi-piece DAS charge threading through playouts.

**Equivalence definition** (what "C++ matches TS" means): the multiset of
`(rotationIndex, x, y, dasChargeAfter)` over all returned possibilities must match
exactly. Input sequences are *not* part of the equivalence contract (C++ does not
emit them), but they are documented here because TS semantics are defined through
them and they are invaluable for fuzz debugging. Once C++ gains optional
sequence tracking (§13), the contract upgrades to character-exact sequence
equality.

**Confirmation status**: the charge freeze during the fall (§7), the max-charge
lock dedup (§8), and the post-ARE slow-tap gating (§11) were reviewed against
the updated `DAS_explainer.md` and confirmed as intended behavior. They are
oracle facts, not bugs to fix.

## 2. Locked design decisions

1. C++ DAS is a gated optional path: `dasCharge < 0` ⇒ tap mode, byte-for-byte
   unchanged behavior. `dasCharge` is in `0..16` when DAS mode is active.
2. `dasCharge` is appended to the C++ pipe string as a new field after
   `aggressionLevel` (field index 10). `module.cpp`, `wasm.cpp`,
   `getCppEncodedInputString`, and the golden tests change in lockstep.
3. `GET_LOCK_VALUE_LOOKUP_DAS` keeps its 3-bucket response shape
   (deep + partial-penalty + min-penalty maps, keyed by `lockPositionEncoded`),
   but the shallow second-piece searches start from **real DAS charges**
   (16 / 10 / 5) instead of `secondPieceDelay` proxies (0 / 5 / 10). Tier
   selection in TS (`precompute.ts:410`) is unchanged: `>=15` → no penalty,
   `>=10` → partial, else → min.
4. Full tuck/spin port under DAS (DAS move search runs a handful of times per
   request; playouts are the volume path and they keep the approximation).
5. Slow-tap limit stays a global constant: `DAS_SLOW_TAP_TIMELINE = "X......."`
   (7.5 Hz), `slowTapWaitFrames = 7`.
6. `PieceRangeContext` stays tap-derived.
7. **Long-term: the TS precompute (reaction-time adjustment logic) is expected
   to move into C++.** The DAS move search must therefore be **adjustment-ready**
   — designed around a generic start state (position/rotation/frames/arrIndex,
   DAS charge, held direction, slow-tap counters seeded from pre-adjustment
   context) rather than hardwired to spawn. Spawn-path-only is the first
   milestone, not a scope boundary; adjustment support is additive afterwards.

## 3. Model constants and state

- **Coordinates**: `x` absolute column `0..9` (piece cells may extend past edges);
  spawn `x = 3`; `y` increases downward, rows `0..19`; piece `initialY` is `-2`
  for I, `-1` otherwise. A goal placement is `(goalRotationIndex, goalOffsetX)`
  with relative offset `-5..4` (absolute `x ∈ [-2, 7]`).
- **Gravity**: `gravity = GetGravity(level)` (frames per row); a "gravity frame"
  is `frameIndex % gravity == gravity - 1`; `doubleGravity` drops 2 rows per
  gravity frame (only active when `DOUBLE_KILLSCREEN_ENABLED` and level ≥ 39).
- **`inputFrameTimeline` in DAS mode** governs *manual taps only* — rotation
  inputs and (indirectly, via `readyForTuckInputsDas`) quicktaps. Shifts are
  driven by the DAS charge clock, not the timeline. `shouldPerformInputsThisFrame`
  (`utils.ts:192`): `timeline[frameNum % timeline.length] === 'X'`.
- **`arrFrameIndex`**: 0 at spawn (`isArrContinued` is false on the spawn path);
  advances 1/frame. Used only for tap-timeline lookups (rotations).
- **`dasCharge`**: `0..16`, `-1` sentinel = tap mode. Charge does **not** change during ARE or line clears — no input of any kind modifies charge during ARE (`DAS_explainer.md`: "DAS is frozen during ARE"; holding through ARE *preserves* charge into the next piece but never adds to it). More generally, charge only changes on frames where a d-pad arrow is down; rotation buttons never affect it.

### Input alphabet (per simulated frame)

| Char(s) | Meaning |
|---|---|
| `.` | no input |
| `A` / `B` | rotate CW / CCW (tap) |
| `L` / `R` | shift left / right |
| `E` `F` / `I` `G` | shift+rotate combos (L+A, L+B / R+A, R+B) |
| `l` `e` `f` / `r` `i` `g` | lowercase: DAS button held, no shift executed this frame (visual only; Lua uppercases) |
| `_` | sequence prefix: hold current direction during ARE (preserves charge) |
| `l`/`r` from `findWallCharge` | wall-charge attempt (shift would collide); sets charge to 16 |
| `*`, `^` | ARE frames / line-clear frames; appended at possibility assembly, never mid-simulation |

`getInputThisFrame` (`move_search.ts:443`): `effShift = (willShift || willHold) ? shiftIncrement : 0`,
`effRot = willRotate ? rotIncrement : 0`, looked up as shift × rot. The result is
lowercased iff `useDAS && shiftIncrement != 0 && !willShiftThisFrame`.

## 4. DAS charge lifecycle

| Event | Charge after | Notes |
|---|---|---|
| Piece spawn (entry) | incoming `dasCharge` | preserved from previous piece via `getSearchStateAfter` |
| Shift while `pendingDasReset` | **0** | the "first-frame tap" down-press resets charge |
| Normal DAS shift (charge ≥ 15) | **10** | standard post-shift charge |
| Held frame, no shift | **+1** | only while `willHoldDasButtonThisFrame` |
| Wall charge (`findWallCharge` hit) | **16** | shift into wall/stack blocked |
| Manual tuck tap (dfs, non-wall-charge) | **0** | fresh down-press |
| Wall-charge tuck (dfs) | **10**, then +1 per held fall frame | see §9 |
| During ARE / line clear | unchanged | no charge accrual in ARE |
| Next piece hand-off | `dasChargeAfter` of the chosen possibility | |

Range notes: holding caps the charge at **15** (the sim exits when charge ≥ 15);
**16** is reachable only via wall charge. 15 and 16 are functionally equivalent
(`DAS_explainer.md`) — the distinction is provenance only (manual charge vs wall
charge), which the model preserves in the input sequence. TS tier `>=15` covers both.

## 5. Phase A — `tryPlacement` (frame loop)

Per goal `(goalRotationIndex, goalOffsetX)`:

**Setup:**
- `simState = { x: initialX, y: initialY, frameIndex: framesAlreadyElapsed,
  arrFrameIndex: 0, rotationIndex: existingRotation, dasCharge: incoming }`
- `inputSequence = "_"` iff `useDAS && !isAdjustment && !dasWillReset && goalOffsetX != 0`
  (hold through ARE). On the spawn path `dasWillReset` reduces to
  `useDAS && incomingDasCharge <= 5` (direction terms are adjustment-only);
  `goalOffsetX == 0` never holds.
- `shiftIncrement = sign(goalOffsetX)`; `rotIncrement`: 0 if already at goal,
  else `-1` if `(cur - 1) mod 4 == goal`, else `+1`.
- Abort immediately if the piece collides at spawn (no possibility registered).
- `pendingDasReset = dasWillReset`; `dasButtonHeld = NONE`.

**Per frame, in exact order** (all predicates computed before any action):

1. `tapShouldInputThisFrame = shouldPerformInputsThisFrame(timeline, arrFrameIndex)`
2. `canShiftThisFrame = useDAS ? (dasCharge >= 15 || pendingDasReset) : tapShouldInputThisFrame`
3. `willShiftThisFrame = canShiftThisFrame && x != initialX + goalOffsetX`
4. `willHoldDasButtonThisFrame = useDAS && (x != initialX + goalOffsetX ||
   (dasButtonHeld != NONE && dasCharge < 15))` — i.e. hold while shifts remain
   (even before any press: models holding from spawn) **or** keep an existing
   hold alive for bonus charge.
5. `willRotateThisFrame = tapShouldInputThisFrame && rotationIndex != goalRotationIndex`
6. `isGravityFrame = frameIndex % gravity == gravity - 1`

Actions:
- **a. Shift** (if `willShift`): `performSimulationShift` — collision at
  `(x + inc, y)` aborts the whole goal (return, nothing registered). On success:
  `x += inc`; if DAS: `dasCharge = pendingDasReset ? 0 : 10; pendingDasReset = false`.
- **b. Hold charge** (else if `willHold`): `dasCharge++`.
- **c. Rotate** (if `willRotate`): rotation index is updated *before* the
  collision check; collision aborts the whole goal. (Failure does not roll back,
  but the abort makes it moot.)
- **d. Append** `getInputThisFrame(...)` to `inputSequence`.
- **e. Track `dasButtonHeld`**: char (uppercased) in `{L,E,F}` → LEFT,
  `{R,I,G}` → RIGHT, else NONE (including `.` and rotation-only `A`/`B`).
- **f.** `arrFrameVoluntarilyMissed` bookkeeping (adjustment-only; C++ skips).
- **g. Gravity** (if gravity frame): up to 2 drop attempts; if
  `collision(x, y+1)` → `lockAfterThisFrame = true` (y unchanged — this is the
  resting position), else `y++`.

Then:
- `frameIndex++; arrFrameIndex++`
- (adjustment snapshot at `reactionTime` — C++ skips)
- `foundPlacement = (rotationIndex == goal && x == initialX + goalOffsetX)`;
  if true, **push a copy** of the state (with `hasAlreadyLocked = lockAfterThisFrame`).
  This repeats every frame while the piece sits at the goal, so a single
  `(rot, x)` accumulates multiple entries.
- Break if `lockAfterThisFrame`.
- Break if `foundPlacement && !canKeepHoldingDpadForMoreDAS && doneWaitingForAdjustment`
  where `canKeepHoldingDpadForMoreDAS = useDAS && dasCharge < 15 && dasButtonHeld != NONE`.
  On the spawn path `doneWaitingForAdjustment` is effectively true (no
  reaction-time snapshot needed), so the C++ loop can exit as soon as the
  placement is found and charging is done.

**Consequences that must be replicated exactly:**
- A placement reached early keeps simulating (holding for charge) until charge
  hits 15 or the piece locks; each intermediate frame registers another entry.
- A placement that locks before charge reaches 15 registers with its
  at-lock charge (e.g. 10 after the last shift, or the incoming charge for
  zero-shift placements, which never hold).
- Shift collision aborts the direction permanently (a horizontally blocked
  piece stays blocked; gravity never opens columns).

## 6. Midair dedup — `deDupeSortedList`

Group the registered midair states by key `rotationIndex + "|" + x`, preserving
first-seen group order. Per group keep exactly:
- the **first** entry (earliest reach: highest y, lowest charge — the
  "adjustable" variant), and
- the **last** entry (final frame before exit: lowest y, max charge), when the
  group has ≥ 2 entries.

## 7. Phase B — fall phase (`exploreLegalPlacementsUntilLock`)

Per surviving midair state, until lock:

1. If `hasAlreadyLocked` or the piece collides at its current position:
   register the lock possibility immediately and stop.
2. Per frame:
   - `isGravityFrame` per `frameIndex % gravity`.
   - **Tuck-readiness**: `readyForInputs = readyForTuckInputsDas(preAdj + inputSequence, timeline)`
     (DAS) or `shouldPerformInputsThisFrame(timeline, arrFrameIndex)` (tap).
     Once true, `startedLookingForTuckSpins` latches true.
   - **Tuck-state registration**: if `startedLooking && y > highestRegisteredY`,
     push a copy to `potentialTuckSpinStates` and set `highestRegisteredY = y`.
     The first registration happens at the hover position (before any fall).
   - **Wall charge** (DAS only, and only `startedLooking && dasCharge < 16`):
     `findWallCharge` checks `collision(x-1, y)` → `'l'`, else `collision(x+1, y)`
     → `'r'` (left priority). If found: append the char, `dasCharge = 16`.
     Otherwise append `'.'`.
   - **Charge is otherwise frozen during the fall** — held frames are recorded
     as `'.'` and do not increment charge. This models the arrows being released
     once the goal is reached: per `DAS_explainer.md`, charge only changes on
     frames where an arrow is down, so a frame with no press leaves the charge
     untouched. The single exception is the deliberate wall-charge re-press
     (`findWallCharge`), which sets 16. Only `tryPlacement`'s hover phase and
     the dfs wall-charge-tuck hold accrue charge.
   - **Gravity** (if gravity frame): up to 2 attempts; `collision(x, y+1)` →
     register lock possibility (`lockHeightLookup[rot,x] = y`) and stop; else `y++`.
   - `frameIndex++; arrFrameIndex++`

`lockHeightLookup[rot, x]` = the y at which the piece rests. All midair variants
of the same `(rot, x)` rest at the same cell, so later registrations overwrite
with an identical value.

## 8. Lock dedup — `deDupeLockPossibilities`

Basic (non-tuck) possibilities are produced in midair-state order, i.e. per
`(rot, x)` group the early-reach variant's lock precedes the charged variant's
lock (same `lockPositionEncoded = "rot|x|y"`). Adjacent duplicates are resolved
by **removing the first** — the final basic set keeps one possibility per lock
cell: the **max-charge** variant. (Early reach matters only via
`potentialTuckSpinStates`, which is deduped *before* this step.)

## 9. Phase C — tucks & spins (`dfs.ts`)

Depth-1 only: inputs are tried solely from `potentialTuckSpinStates`; a tuck
state is never fed back into the input search. `arrFrameIndex` is not advanced
here (no timeline inputs occur after the tuck input).

**Input sets** by orientation count: `1 → "LR"` (O), `2 → "LRAEI"` (I, S, Z),
`4 → "LRABEIFG"` (L, J, T). Deltas: `L/E/F → x-1`, `R/I/G → x+1`;
`A/E/I → rot+1`, `B/F/G → rot-1` (mod orientation count; rotation skipped for O).

**Visited check** (per candidate input): skip if
`parentState.y <= (lockHeightLookup[newRot, newX] || 0)` — tucking from a hover
position at/below the natural lock height of the target cell adds nothing novel.

**`tryInput`** — from the parent state (already pre-advanced to its frame),
loop per frame until lock:

1. `isGravityFrame = frameIndex % gravity == gravity - 1`.
2. If the input hasn't been applied yet (first iteration):
   - **Shift** (if `newX != x`): `collision(newX, y, curRot)` ⇒ discard this
     input entirely. Otherwise, DAS charge for the tap:
     - `couldWallChargeLastFrame = collision(newX, y - 1, curRot)`
     - `lastFrameWasGravityFrame = (frameIndex - 1) % gravity == gravity - 1`
     - If both ⇒ **wall-charge tuck**: `isWallChargeTuck = true`, `dasCharge = 10`.
       Else ⇒ `dasCharge = 0` (fresh down-press).
     - `x = newX`.
   - **Rotate** (if `newRot != rot`): `collision(x, y, newRot)` ⇒ discard; else apply.
   - **Sequence**: wall-charge tuck replaces the trailing `'.'` with the
     lowercase wall-charge char, then appends the input char; plain tucks just
     append. Mark input done.
3. Every subsequent frame (falling):
   - Default `newInput = '.'`.
   - If DAS and `dasCharge < 15`:
     - wall-charge tuck: `newInput = lowercase wall-charge char`, `dasCharge++`
       (accrues toward 15 while falling).
     - else if `readyForTuckInputsDas(preAdj + sequence)`: probe
       `findWallCharge`; if found, `newInput = wall char`, `dasCharge = 16`.
   - Append `newInput`.
4. Gravity frame: `collision(x, y+1)` ⇒ **lock**: build a possibility keyed by
   `"x|y|rot"` in the novel map (see below); else `y++`.
5. `frameIndex++`.

**Novel-map scoring** (`searchForTucksOrSpins`): per lock key, keep the
possibility with the higher `inputCost` (on the spawn path `adjTimeSimState` is
always null, so the `-1000` adjustment term is constant and drops out).
`inputCost`: `E/F/I/G = -0.3` (spin-tuck), `A/B = -0.2` (spin), `L/R = -0.1` (tuck).

**Output**: tuck/spin possibilities are appended after basic possibilities with
**no cross-list dedup** — a tuck lock cell may legitimately duplicate a basic
lock cell (with different input cost / charge).

## 10. Possibility assembly (`getPossibilityFromSimState`)

- `lockPositionEncoded = "rot|x|y"` (absolute coordinates at lock). **This exact
  format is the join key** for the C++ penalty maps — do not change it.
- `placement = [(rot - existingRotation + numOrientations) % numOrientations,
  x - initialX, y - initialY]`.
- Entry delay: `lockHeight = 20 - (y - startingY)`;
  `entryDelay = min(18, 10 + floor((lockHeight + 1) / 4) * 2)`;
  append `*` × `(entryDelay - 5)`.
- Line clear: if `numLinesCleared > 0`, append `^` × **17** (fixed count
  regardless of lines cleared — preserved quirk), then `*` × 5.
- `dasChargeAfter = simState.dasCharge` at the lock frame.
- `boardAfter`, `numLinesCleared` from locking the piece in.

## 11. `readyForTuckInputsDas` — exact definition

Given the full sequence (spawn path: the sim sequence itself, `_` prefix included):

1. `framesSinceLastInput` = length of the trailing run of `'.'` (any other char —
   letter, `_`, later `*`/`^` — stops the count).
2. `didStartHoldingDpad`: scan the pairs `(i-1, i)` for
   `i` from `max(1, len - 7)` to `len - 1`. Map each char to a direction:
   `{L,E,F,l,e,f} → -1`, `{R,I,G,r,i,g} → +1`, anything else (incl. `_`, `*`, `^`)
   → undefined. If `thisFr` is truthy and `lastFr !== thisFr` ⇒ a down-press
   transition occurred ⇒ `didStartHoldingDpad = true`.
   - `.` → direction counts as a fresh down-press (relevant for hold resumes).
   - Uppercase↔lowercase of the same direction does **not** count (same value).
3. `minFramesWait = inputFrameTimeline.length - 1`.
4. Return `!didStartHoldingDpad && framesSinceLastInput >= minFramesWait`.

**Intended behavior (confirmed)**: with a `_` prefix followed by an early first shift
(e.g. `"_R..."`), the pair `('_', 'R')` is inside the 7-frame window for the
first 8 frames and blocks tuck inputs until frame 9. This is correct: the
player's down-press happened on the `_` frame (during ARE), so another
down-press a few frames after spawn would be a suspiciously fast tap — exactly
what the slow-tap constraint exists to prevent.

**C++ replacement**: this is string scanning in TS; in C++ track two counters in
`SimState` — `framesSinceLastInput` (0 on any non-`'.'` append, else ++) and
`framesSinceLastDpadDown` (0 on any appended char whose direction differs from
the previous char's direction and is nonzero, else ++), plus the previous
char's direction. `didStartHoldingDpad ⟺ framesSinceLastDpadDown <= 7`.
Initialize as if the sequence began with `_` when the hold marker is used
(previous direction = none, `framesSinceLastDpadDown = 0` — matching the
confirmed behavior above, since the first direction char will register as a
fresh transition).

## 12. `dasChargeAfter` hand-off summary

- Basic placements: charge at lock — typically `15` (held at goal), `16`
  (wall-charged during hover or fall), `10` (locked right after the last DAS
  shift), `0` (locked right after a reset/first-frame tap shift), or the
  *incoming* charge (zero-shift placements never hold).
- Tuck placements: `10..14` (wall-charge tuck still holding), `16` (wall-charged
  during tuck fall), or `0` (plain tuck).
- `getSearchStateAfter` hands `dasChargeAfter || 0` to the next piece. No ARE
  accrual is added anywhere (see §3).

## 13. C++ port notes (structure already agreed)

- **Gate**: `dasCharge < 0` ⇒ existing tap path, untouched. Keep DAS-only state
  out of the tap-mode hot loop (separate fields/struct passed alongside).
- **Sequences over counters (revised)**: the §11 counter encoding was dropped
  during implementation. `readyForTuckInputsDas` is ported as a **direct scan
  of the sequence string** (exactly like TS). Rationale: the DAS search is
  volume-light (no hot loop), the tap path never calls the readiness check, and
  the counters had two exactness hazards — the `_` position-0 down-press rule,
  and dfs's retroactive sequence surgery on wall-charge tucks (the last two
  chars are rewritten, which would desynchronize any counter state). In DAS
  mode the sequence string is therefore always tracked internally (it gates
  tuck readiness); the runtime flag only controls whether it is *copied* into
  the output `LockPlacement`.
- **Fast drop** (`getLockPlacementsFast`): the fall remains analytic, but must
  additionally determine, per state: whether a wall-charge opportunity occurs
  during the fall (collision at `x±1` for any traversed y, left priority, only
  once `startedLooking`, only while charge < 16) and the resulting
  `dasChargeAfter`. Per-frame equivalence here is exactly what the fuzz harness
  must prove.
- **Tuck tables under DAS**: `minTuckYValsByNumPrevInputs` / reachability bounds
  are tap-timeline-derived today. Under DAS the y-at-input-time depends on the
  charge clock. Expected approach: compute DAS variants of these tables per
  charge bucket (and for the incoming-charge distribution of registered states);
  final design settled during implementation, validated by fuzz equivalence.
- **Placement search strategy (DAS mode)**: a new per-goal frame simulation
  (`dasPlacementSearch`) mirroring TS `tryPlacement` — one independent sim per
  `(goalRotationIndex, goalOffsetX)` — rather than a DAS-aware variant of the
  reachability walk. Rationale: (a) the TS-mirror makes fuzz equivalence nearly
  automatic, (b) the long-term adjustment support (§2.7) reuses the exact
  structure TS already has, since all adjustment machinery lives in TS's
  per-goal function, (c) DAS-mode volume (hundreds of bucket searches per
  `GET_LOCK_VALUE_LOOKUP_DAS`) tolerates the cost, while tap mode keeps the
  walk untouched for playouts. The C++ duplication the TS refactor eliminated
  does not exist here (`explorePlacementsNearSpawn` already delegates to the
  generalized walk), so no reconciliation work is needed. Fusing DAS into the
  walk (charge clock + analytic charge post-pass) remains a possible later
  optimization if benchmarks demand it.
- **`preAdjInputSequence` never becomes a C++ string**: `readyForTuckInputsDas`
  reads only the trailing-dot count and down-transitions within the last 7
  chars, so two counters plus held direction — seeded by scanning the
  pre-adjustment tail (≤ 8 chars, with `_` mapping to "no direction") — fully
  capture it (see §11).
- **Optional input-sequence tracking** (prerequisite for the C++ precompute
  endgame, and for sequence-level fuzz equivalence):
  - Gated by a runtime `bool trackInputSequences` — one binary serves both
    playouts (off) and lookups/harness (on); loop-invariant branch, no
    compile-time split.
  - Sequences are built in the DAS search's **per-goal local scope** into a
    fixed-capacity stack buffer (no heap growth per frame); copied into
    `LockPlacement.inputSequence` only when enabled. Disabled ⇒ the field stays
    empty (SSO, no allocation) and the tap hot path is untouched structurally —
    the walk never handles sequences.
  - Counters/charge-clock remain the decision mechanism; the sequence is a
    parallel per-frame recording (one byte-store per frame when enabled). The
    alphabet and encoding must match TS exactly (§3, incl. lowercase holds,
    `l`/`r` wall charges, the `_` prefix, and `*`/`^` padding appended at
    assembly).
  - The recording supports a **prefix seed** (spawn path: `"_"` or `""`), so the
    future adjustment era can prepend pre-adjustment context without rework.
  - Once tracking exists, the fuzz harness's DAS comparison upgrades from
    cells+charge to character-exact sequence equality (see §14).

## Appendix A: C++ signature sketch

```cpp
// --- Wire format (locked): dasCharge appended as field 10 ---
// board|level|lines|cur|next|timeline|playoutCount|playoutLength|pruningBreadth|aggressionLevel|dasCharge|
//   dasCharge: -1 = tap mode (default, byte-identical behavior), 0..16 = DAS mode

// --- Adjustment-ready start state (near term: only the spawn instantiation is exercised) ---
struct DasStartState {
  int x, y, rotationIndex, frameIndex, arrIndex;
  int dasCharge;           // 0..16
  int heldDirection;       // -1/0/+1 at start (what the player was holding)
  bool isAdjustment, isArrContinued;
  int framesSinceLastInput, framesSinceLastDpadDown;  // seeded per §11
};

// --- DAS entry point (SEPARATE from moveSearch -- see note below) ---
struct DasPossibility {
  int x, y, rotationIndex;
  int dasChargeAfter;      // 0..16 charge at the lock frame
  int inputCostX10;        // 0 basic; -1 tuck; -2 spin; -3 spin-tuck
  char tuckInput;          // '.' for basic
  int numLinesCleared;
  std::string inputSequence;
};

// IMPLEMENTED (das_move_search.cpp): a 1:1 port of the TS spawn-path pipeline —
// per-goal tryPlacement, deDupeSortedList, exploreLegalPlacementsUntilLock,
// dfs tuck search, deDupeLockPossibilities. LockPlacement and moveSearch are
// UNCHANGED: an earlier draft added dasChargeAfter/inputSequence fields to
// LockPlacement and regressed the tap playout benchmark ~9% (struct grew
// 32 -> ~72 bytes, copied tens of thousands of times per request), so DAS
// results flow through DasPossibility instead. The fuzz harness calls
// dasMoveSearch directly for DAS fixtures.
int dasMoveSearch(GameState, const Piece*, char const* inputFrameTimeline,
                  int dasCharge,
                  bool trackInputSequences,   // sequences always tracked internally; flag gates copying
                  OUT std::vector<DasPossibility>& results);
```

## 14. Fuzz / oracle harness

- **Generator** (TS, fixed PRNG seed) emits fixtures as JSON *and* as a C++
  literal header (unity build has no JSON parser; mirrors the existing
  `testCases` pattern in `move_search.cpp`).
- **Fixture record**: `{ board, pieceId, level, inputFrameTimeline, dasCharge,
  existingXOffset/YOffset/Rotation, framesAlreadyElapsed }` → expected sorted
  list of `{ rotationIndex, x, y, dasChargeAfter }` (+ `inputSequence` /
  `inputCost` for debugging). The schema is intentionally adjustment-ready:
  held direction / `isArrContinued` / pre-adjustment counter seeds are added
  later without format changes.
- **C++ runner** (doctest, `npm run cpp_test_unit` style): parse spawn args,
  run `moveSearch`, compare multisets exactly.
- **Sequence verification** (`npm run fuzz_verify_sequences`): a persisted
  second gate covering the full DAS fixture set at character-exact sequence
  level. `src/cpp_modules/tests/fixture_dump.cpp` dumps C++ results for all DAS
  fixtures (via `dasMoveSearch` with sequence tracking enabled; tap fixtures
  legitimately emit no sequence), and `src/server/move_search_fuzz_verify.js`
  diffs every entry — tuple *and* sequence — against the oracle JSON, exiting
  nonzero on any mismatch. DAS fixture indexes are derived from the JSON itself
  (never from positional assumptions). Rationale for staying outside the doctest
  header: sequences would add ~400KB to the fixture header; this script covers
  the full set with zero header growth.
- **Tap-mode regression set**: `dasCharge = -1` fixtures must match current C++
  behavior byte-for-byte (non-regression guard), and TS-vs-C++ tap-mode
  equivalence is a prerequisite milestone before DAS cases are compared.
- **Coverage axes**: all 7 pieces; `dasCharge ∈ {0..16}` with emphasis on
  `{0, 5, 6, 10, 14, 15, 16}`; levels 18 / 19 / 29; timelines `"X.."`, `"X..."`,
  `"X....."`; boards: flat heights 0–16, overhang/tuck setups, edge/wall
  adjacency, near-ceiling spawns; zero-shift and max-shift placements.
- **Property checks** (TS runner): `dasChargeAfter ∈ [0, 16]`; no duplicate
  lock cells within the basic set; every placement collision-free on
  `boardAfter`; tuck possibilities never appear without a `potentialTuckSpinState` parent.
- **Input validation**: `existingRotation` must be `< numOrientations` (today
  TS crashes on I/O/S/Z with `existingRotation = 2`, O with `1` — pin the
  validated behavior at the fixture-generation boundary rather than porting the crash).

## 15. Out-of-scope reminders

- No `GET_MOVE` / `GET_TOP_MOVES` behavior changes (their playout-driven paths
  keep the tap-timeline approximation).
- No changes to eval, `PieceRangeContext`, or tournament search semantics beyond
  what `GET_LOCK_VALUE_LOOKUP_DAS` needs.
- `getDasEquivalentInputTimeline` survives for playout paths; it stops being
  used for `GET_LOCK_VALUE_LOOKUP_DAS` once the C++ DAS search lands.
