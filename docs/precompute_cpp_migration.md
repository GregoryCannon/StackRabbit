# Precompute C++ migration plan

Goal: replace the TS precompute pipeline (`src/server/precompute.ts`, reached via
`precompute` / `precompute-sync` routes) with a single synchronous C++ entry point
that produces the **byte-identical output string** for any given request. TS is
eventually deprecated; this is the blocking feature for that.

## Non-goals (explicitly out of scope)

- Threading (the 7-fork worker pool in `PreComputeManager`), object lifecycle
  (worker readiness, callbacks, partial results), and logging/timing output.
  The C++ method is fully synchronous and single-threaded.
- Preserving TS function/class structure. Only the output string must match.

## Current architecture (what the TS path actually does)

`handlePrecomputeRequestSync` (request_handler.ts:298) → `finessePrecompute`
(precompute.ts:81) → pipeline below. Note the sync path discards the partial
result and waits for the full result, so only the final string matters.

1. **Initial move search** — `getPossibleMoves` (move_search.ts:194) on the
   current piece, passing the real `inputFrameTimeline`, `dasCharge`, and
   `reactionTime`. Each result gets `searchStateAfterMove` attached
   (`toPossibilityChain`). `possibleMoves[0]` is the *default placement*; empty
   list ⇒ final result is exactly `"No legal moves A"`.
2. **Per-next-piece lock value lookups (×7)** — worker_thread.ts:12 encodes a
   pipe string (board + level + lines + curPiece + nextPiece + timeline +
   playoutCount + playoutLength + pruningBreadth + aggressionLevel + dasCharge)
   and calls `cModule.getLockValueLookup` (tap mode) or `getLockValueLookupDas`
   (DAS mode). DAS mode returns `[partialPenaltyMap, minPenaltyMap, lockValueMap]`.
   **All of this already lives in C++** (high_level_search.cpp:667-901).
3. **Phantom placements** — `_calculatePhantomPlacements` (precompute.ts:152):
   stable-sort possibilities by `countInputs(placement)`, dedupe by the first
   `reactionTime` chars of `inputSequence`, and for each surviving possibility
   capture the sim state at reaction time (`adjTimeSimState`) as an adjustment
   search state. Special case: `reactionTime === 0` ⇒ a single phantom with
   `initialPlacement = null` and `adjustmentSearchState = initialSearchState`.
4. **Adjustment move search** — `_precompileAdjustmentMoves` (precompute.ts:246):
   for each phantom with an adjustment search state, run `getPossibleMoves` from
   that intermediate state with `adjustmentState.isAdjustment = true`
   (`preAdjInputSequence`, `dasButtonHeld`, `isArrContinued`) and **no**
   reactionTime (defaults to -1). Phantoms whose piece locked before reaction
   time get no adjustments.
5. **Collapse** — `_compileResponseFinesse` (precompute.ts:278): for each
   phantom × each next piece, compute
   `lockValue + adjustmentInputCost + dasPenalty` maximized over the phantom's
   adjustments (or the phantom's initial placement when it has none); weight by
   piece probability (NES PRNG transition table, precompute.ts:491); average
   over 7; pick the best phantom (strict `>`, so the first phantom wins ties).
   Iteration order: phantom array order, then `POSSIBLE_NEXT_PIECES` order
   `["I","O","L","J","T","S","Z"]`. Tie-break within a piece: `value >= maxValue`
   ⇒ the **later** adjustment wins.
6. **Format** — `formatPrecomputeResult` (precompute.ts:426):
   `Default:...` + one `\n<piece>:...` line per next piece, using
   `formatPossibility` / `formatDefaultPossibility` (utils.ts:129-184), with
   `continue` / `No legal moves C` fallbacks.

### C++ assets that already exist

| Asset | Location | Status |
|---|---|---|
| DAS move search (INITIAL_PLACEMENT, DAS mode) | `fullMoveSearch`, full_move_search.cpp | Fuzz-verified vs TS (rot/x/y/charge/cost; sequences via `fuzz_verify_sequences`) |
| Tap-mode move search | `fastMoveSearch`, move_search.cpp | Fuzz-verified on placement multiset only — **no sequences** |
| Tuck/spin DFS | ported inside full_move_search.cpp (DAS path) | Same coverage caveat |
| Lock value lookups | `getLockValueLookupEncoded` / `getLockValueLookupDas`, high_level_search.cpp | Production, fuzz-covered |

### Gaps

1. Tap mode in the full move search (sequences, `dasChargeAfter`, adj-time state).
2. Adjustment-state input path (`isAdjustment`, `preAdjInputSequence`,
   `dasButtonHeld`, `isArrContinued`) — affects DAS-reset logic in
   `tryPlacement` (move_search.ts:244-247, 641) and sequence construction
   (the `preAdjInputSequence` prefix at move_search.ts:402, and tuck gating in
   dfs.ts:207).
3. Reaction-time state capture (`adjTimeSimState`: frameIndex, x, y, rotation,
   dasCharge, derived AdjustmentState). Must ride in a side-channel struct, not
   in the hot-path result struct (AGENTS.md landmine).
4. Everything in steps 3-6 above (glue, collapse, formatting).

## Parity details checklist (each one is a real divergence risk)

- **`%.2f` rounding**: lock values and DAS penalties reach TS as JSON encoded
  with `%.2f` (high_level_search.cpp:754) and parsed as doubles. The collapse
  math in TS therefore operates on **2-decimal-rounded** values. The C++ glue
  must round identically (e.g. `snprintf("%.2f")` + `strtod`) before summing,
  or max-selection can diverge.
- **Double math**: sum/max in `double`, mirroring TS accumulation order
  (`lockValue + inputCost + penalty`), not `float`.
- **Stable sort**: phantom sort must be `std::stable_sort` (JS sort is stable);
  ties keep the pre-sort array order, which itself must match TS result order.
- **Order-exact results**: `getPossibleMoves` result order matters
  (`possibleMoves[0]` = default placement; de-dup order feeds the phantom
  sort). The existing fuzz compares sorted multisets — the new gate must be
  order- and sequence-exact.
- **Tie-breaking**: `value >= maxValue` ⇒ last adjustment wins; phantom
  comparison is strict `>` ⇒ first phantom wins.
- **Relative placement**: output `placement` is
  `[rot - existingRotation (mod numOrientations), x - initialX, y - initialY]`
  (move_search.ts:553-558). `numOrientations` varies per piece (O=1, I/S/Z=2,
  L/J/T=4). `fullMoveSearch` currently reports absolute rot/x/y.
- **`countInputs` quirk**: `placement[0] == 3 ⇒ 1 + |placement[1]|`, else
  `placement[0] + |placement[1]|` (precompute.ts:479) — operates on the
  *relative* placement.
- **DAS penalty rules** (precompute.ts:408): tap mode ⇒ 0; missing key ⇒ 0;
  `dasCharge >= 15` ⇒ 0; `>= 10` ⇒ partial map; else min map.
- **Adjustment input cost** (precompute.ts:378): per-char lookup over the
  **full** sequence including the `preAdjInputSequence` prefix; E/F/I/G = -0.3,
  A/B = -0.2, L/R = -0.1, lowercase e/f/i/g = -0.2, everything else 0.
- **`getSearchStateAfter` dasCharge**: `possibility.dasChargeAfter || 0` — note
  tap-mode possibilities carry `dasChargeAfter = -1`, which is truthy and
  therefore passes through as **-1** into the formatted output.
- **Level after line clears**: `getLevelAfterLineClears` (utils.ts:222) assumes
  18/19/29 starts; transition thresholds 126/136/196 lines.
- **Formatting quirks**: `inputSequence || "none"` (empty string ⇒ "none");
  `formatDefaultPossibility` slices at `reactionTime + 1` when the sequence
  contains `_` (the hold-DAS-during-ARE marker), else `reactionTime`
  (utils.ts:137); board rendered as 20 rows × 10 cols of 0/1 with no
  separators; `pushDown` applies only when `SHOULD_PUSHDOWN && level == 18`
  (currently `SHOULD_PUSHDOWN = false`, params.ts:13 — port the flag, not the
  hardcoded false).
- **Fallback strings** (decision: the "No legal moves A/B/C/D/E/F" debug
  distinctions collapse to the single string `none` (lowercase, to match
  `continue`) in the C++ path). Note the
  Lua agent checks these literals: stackrabbit.lua:280 matches
  `"No legal moves C"` and must be updated in lockstep. The per-piece
  **`continue` marker is load-bearing** (stackrabbit.lua:280/327/455 — it
  tells the agent to keep playing the default line) and is retained: per-piece
  fallback is `continue` when `defaultPlacement.inputSequence.length >
  reactionTime`, else `none` (precompute.ts:437-448).
- **`LOSS_DAS_PENALTY` is imported but unused** in precompute.ts — do not port.
- **Piece probability table**: NES PRNG transitions (precompute.ts:491-509),
  `lastSeenPiece = searchState.currentPieceId`, final average divides by 7.
- **The lock-lookup/adjustment intersection is deliberate — do not "fix" it.**
  The lookup values each lock cell once from a canonical spawn context; the
  adjustment search runs from per-phantom midair states. Many midair states
  converge on the same lock cell, so the lookup keeps the expensive valuation
  out of the collapse phase. The cost is that candidates are the intersection
  of the two reachable sets (missing keys are skipped, `CONTINUING!!!`).
  Parity means reproducing both reachable sets exactly. NB: the lookup only
  contains cells admitting at least one legal next-piece placement (it is built
  from (first, second) pairs) — on near-topped boards where every placement of
  the current piece blocks the next piece's spawn, the lookup is legitimately
  empty and the per-piece `none` fallback is the designed response.

## Plan

### Phase 0 — Renames + golden fixtures (no behavior change)

1. Rename `moveSearch` → `fastMoveSearch` (move_search.hpp/cpp and all call
   sites: high_level_search.cpp, tournament_search.cpp, main.cpp, benchmarks,
   fuzz harness). Own commit; full battery after; interleaved A/B benchmark
   against a same-session worktree baseline (unity-build layout luck).
2. Rename `dasMoveSearch` → `fullMoveSearch`; rename file
   `das_move_search.cpp/hpp` → `full_move_search.cpp/hpp`; update header
   comment (including the now-stale "tap mode never enters this file" note),
   docs/DAS_explainer.md, TESTING.md, fuzz harness, fixture_dump.cpp.
4. Lockstep fallback-string change (small, deliberate behavior change — flip
   characterization expectations with it): precompute.ts replaces
   `"No legal moves A"`, `"No legal moves B"`, and `"No legal moves C"` with
   `"none"`; stackrabbit.lua:280 checks `"none"` instead of
   `"No legal moves C"`. The `continue` marker is untouched. Shared formatters'
   "No legal moves E/F" null-guards are untouched (other routes consume them).
   Doing this before fixture capture means TS, C++, fixtures, and the agent
   share one convention from the start.
5. Golden fixture capture, sourced from `src/benchmark/data/position_log.jsonl`
   (~1700 recorded game positions: compressed board, level, lines, timeline in
   the header). For each position, re-randomize the current piece (precompute
   ignores the logged nextPiece — it searches all 7) and vary `reactionTime`
   (0 / small / large) and `dasCharge` (-1 for tap mode; 0, 5, 10, 15, 16 for
   DAS mode). Boards arrive in compressed form and must be decompressed
   (parseBoard) before encoding into the C++ pipe string. Target coverage:
   "no moves" boards, tucks, wall charges, the `continue` marker path, and
   line-clear level transitions. Run twice to confirm TS-path determinism.
   Store as a versioned fixture file consumable by both TS and C++ harnesses.

### Phase 1 — Extend `fullMoveSearch` to parity coverage

Each sub-step independently revertible, full battery after each:

1. Tap mode (`dasCharge == -1`): the `useDAS=false` branches of
   `tryPlacement` / `exploreLegalPlacementsUntilLock` (ARR-gated inputs via
   `shouldPerformInputsThisFrame`, no charge bookkeeping, no `_` marker).
2. Adjustment-state input path: `preAdjInputSequence` prefix on sequences,
   `isArrContinued`, `dasButtonHeld` → `dasWillResetLeft/Right`, and the
   "no reactionTime" mode. Includes the DFS awareness of
   `preAdjInputSequence` (dfs.ts:207).
3. Reaction-time capture: side-channel out-param carrying the sim state at
   `frameIndex == reactionTime` (only when not locked yet) plus the derived
   AdjustmentState (`getAdjustmentState(!arrFrameVoluntarilyMissed,
   dasButtonHeld, sequence)`, move_search.ts:791).
4. Fuzz generator + verifier extension: fixtures for tap mode with
   character-exact sequences, adjustment-mode entries, and reaction-time
   capture; comparison becomes **order-exact** (array equality, not multiset).
   This is the equivalence gate for Phase 2 — do not start Phase 2 red.

### Phase 2 — Port the glue (`precompute.cpp`)

New file; new request type `PRECOMPUTE` handled in `mainProcess` so
GameState/EvalContext setup is shared with existing paths. Input format: the
standard 11-field pipe string **plus a `reactionTime` field (field 11)** (new
layout, so existing callers are untouched; the C++ parser validates every
field itself). Every URL-arg validation currently in request_parser.ts that
affects this path must be mirrored in the C++ parser (house rule: each
boundary validates its own inputs).

Steps inside one synchronous function:

1. Initial `fullMoveSearch` (real timeline, `dasCharge`, `reactionTime`) →
   possibilities with sequences, relative placements, boards-after,
   `lockPositionEncoded`, `dasChargeAfter`, adj-time states. Empty ⇒ return
   `"No legal moves A"`.
2. Build GameState once; for each of the 7 next pieces call the lock-lookup
   internals **in-process** (no JSON round-trip). This requires splitting
   `getLockValueLookupDas` into a map-returning core (returns the partial /
   min / deep `unordered_map<string,float>` triple directly) + a thin JSON
   wrapper that the existing `GET_LOCK_VALUE_LOOKUP_DAS` path keeps using —
   pure refactor, wire output unchanged. Replicate `%.2f` rounding on all
   three maps so the collapse sees exactly the values TS saw.
3. Phantom placements: `std::stable_sort` by `countInputs`, prefix dedupe,
   adjustment search states (incl. the `reactionTime === 0` special case).
4. Adjustment `fullMoveSearch` per phantom.
5. Collapse: exact iteration order and tie-breaking, double math, rounded
   inputs, piece-probability table, `/7`.
6. Format the output string per `formatPrecomputeResult` + utils.ts, with the
   `none` fallback convention (see checklist) and the retained `continue`
   marker.

Performance note: the 7 lock lookups share one GameState; building contexts
once instead of 7× should make the C++ path strictly faster than the worker
path even before threading is considered.

### Phase 3 — Wiring, differential verification, cutover

1. Expose via module.cpp as `precompute(inputStr)`, mirroring the existing
   NAN_METHOD pattern with the std::runtime_error → JS error handling.
2. Differential harness: for every fixture, run TS `precompute-sync` and C++
   `precompute()`, require byte-identical strings. Wire as (a) a node script
   in the style of `fuzz_verify_sequences`, and (b) doctest coverage for the
   C++-internal pieces.
3. Serve the C++ path on a new `precompute-cpp` HTTP route (the `precompute`
   route keeps serving the TS path). Live cutover is explicitly **out of
   scope** for this migration — flipping routes, deleting
   `PreComputeManager`/worker_thread.ts, and retiring TS is a separate
   follow-up migration. The differential harness remains in place until then.

Each phase lands as small independently revertible commits; full battery
(`cpp_test_unit`, `fuzz_verify_sequences`, `tsc`, `move_test`, `cpp_test`)
before and after every step. Remember: a long-running dev server keeps the old
addon mapped — restart before trusting A/B results.

## Decisions

1. **Entry-point shape** — new `PRECOMPUTE` request type inside `mainProcess`
   (shares context setup, one parse path). Decided.
2. **Input field layout** — `reactionTime` is field 11 of the new request
   type's format. Decided.
3. **`getLockValueLookupDas` refactor** — split into a map-returning core
   (partial/min/deep `unordered_map<string,float>` triple) + a thin JSON
   wrapper that the existing request path keeps using; wire output unchanged.
   Required so the precompute glue can consume the maps in-process instead of
   parsing C++-generated JSON inside C++. Decided.
4. **Rounding** — replicate the `%.2f` rounding (snprintf+strtod) exactly.
   Decided.
5. **Fallback strings** — all "No legal moves A/B/C/D/E/F" variants become
   `none`. The per-piece **`continue` marker is retained** as its own separate
   response (load-bearing in the Lua agent, stackrabbit.lua:280/327/455).
   Scope note: only the precompute-path strings change (precompute.ts A/B/C);
   the shared `formatPossibility` / `formatDefaultPossibility` "No legal moves
   E/F" null-guards keep their text since other routes consume them. The TS
   strings and the Lua check (stackrabbit.lua:280) are updated in lockstep in
   Phase 0, so golden fixtures, TS, C++, and the agent all share one
   convention from the start. Cutover-time Lua churn is thereby avoided.
   Degenerate corner (reactionTime 0 + a piece whose adjustments all miss the
   lock value lookup): TS previously crashed here with a TypeError (null
   `defaultPlacement.inputSequence` in `formatPrecomputeResult`); it is now
   guarded to emit the normal `none` per-piece fallback, and C++ must reproduce
   that guarded behavior exactly (no error string).
6. **Fixture source** — `src/benchmark/data/position_log.jsonl` positions with
   re-randomized current pieces and varied reactionTime/dasCharge. Decided.
7. **WASM** — out of scope for this migration. Decided.
8. **Naming** — request type `PRECOMPUTE`, exported JS name `precompute()`,
   C++ file `precompute.cpp`, move searches `fastMoveSearch` / `fullMoveSearch`
   (see Phase 0). Decided.
9. **Cutover** — do **not** switch the live `precompute` routes to the C++
   path as part of this migration. The C++ path lands on its own route
   (e.g. `precompute-cpp`) and runs alongside TS; flipping live traffic,
   deleting `PreComputeManager`/worker_thread.ts, and retiring the TS path is
   a separate follow-up migration. Decided.
