# Engine Boundaries — TS ↔ C++ Contracts

One-page reference for the interfaces between the TypeScript server and the C++ engine.
The authoritative input-format table lives in [TESTING.md](../TESTING.md); the game-level
DAS mechanics (including the frame-exact charge model) live in
[DAS_explainer.md](DAS_explainer.md).

## Request wire format (the C++ pipe string)

```
[board1 (200 chars 0/1)] | [optional board2] | level | lines | currentPieceIdx |
nextPieceIdx | inputFrameTimeline | playoutCount | playoutLength | pruningBreadth |
aggressionLevel | dasCharge |
```

- Every non-board field is **mandatory and strictly validated** by the C++ parser
  (`main.cpp`) — see TESTING.md for the full contract. `secondBoard` is the one
  genuinely optional field (per request type).
- `dasCharge`: `-1` = tap mode (byte-identical legacy behavior), `0..16` = the player
  is holding DAS with that much charge.
- The **real** input frame timeline is always sent. The C++ side derives its own tapper
  approximation for piece-range/eval/playout contexts (below); only the DAS move search
  consumes the real timeline.
- Encoders that must stay in lockstep: `src/server/request_parser.ts`
  (`getCppEncodedInputString`), `src/server/worker_thread.ts`,
  `src/server/cmodules.ts`, `entrypoint.cpp`, `src/benchmark/speed_benchmark.cpp`,
  `src/benchmark/run_agent.cpp`, `src/wasm/sample_client/wasmRabbit-worker.js`,
  `src/fceux/stackrabbit.lua`.

## C++ live game server surface

`src/cpp_modules/src/live_game_server.cpp` — standalone zero-dependency HTTP
server serving the live precompute flow (the TS server's replacement; see
docs/live_game_server.md for the design). Default port 3001, overridable via
argv[1] or the `PORT` env var (the battery uses 3101). Responses are
`text/plain`; every response closes the connection.

Routes:

- `GET /ping` → 200 `pong`.
- `GET /precompute?p=<pipe>` → 200 `Request accepted.` and the request is
  computed asynchronously (503 `Busy` if one is still computing — defensive;
  the agent never overlaps; 400 if `p` is absent).
- `GET /async-result` → 200 + full result once computed, else 200 + backup
  response, else 504 `Still calculating` (the window before the backup lands),
  else 404 `No previous async request has been made`. A rejected/failed engine
  run surfaces as 500 + message and sticks until the next valid kick.

The pipe-direct contract: the `p` parameter is the raw 12-field engine pipe
string (percent-encoded by the client, decoded here) — there is **no URL-argument
layer**; `mainProcess`'s pipe parser is the single input boundary, so the
server inherits the engine's strict field validation verbatim.

Backup tier semantics: the worker computes the backup response first (one move
search plus cheap evals, a few ms) so a poll landing before the full result is
answered with it instead of a 504; the full precompute supersedes it once done.
The backup is a full-format response (`Default:<placement>` + seven
`<piece>:continue` lines, or `none` when the piece has no legal placement) —
the same shape the full result can take when every next piece keeps the default.

## Lock position encoding (the join key)

`lockPositionEncoded = "rot|x|y"` — absolute coordinates at lock (C++
`encodeLockPosition`, TS `Possibility.lockPositionEncoded`). This exact string is the
join key between the C++ lock-value maps and TS's placement lookups; TS skips
placements whose key is missing from a map. **Do not change the format.**

## DAS penalty pipeline (`GET_LOCK_VALUE_LOOKUP_DAS`)

Response shape: `[partialChargePenalties, minChargePenalties, deepValues]` — three maps
keyed by lock position.

- **Penalty Maps** The backend does a shallow search with three possible DAS charges
  (15, 10, 5), to compare how much worse the lower DAS charges are compared to the best moves.
  Note that these charges apply to the SECOND piece, due to how the "precompute"
  function calls will later consume this map. The precompute algorithm calculates which
  lock positions are favorable depending on which piece is coming next (one LOCK_VALUE_LOOKUP
  map for each next piece), and then once it knows which piece is coming next (halfway
  through placing the first piece), it redirects the piece trajectory. It often isn't
  able to keep full DAS during these "adjustments", so we need to calculate how much of
  a loss it is if we have a poor DAS charge going into the second piece. Note that
  both the tournament and legacy lookup paths run the same 15/10/5 buckets.
- **Deep map** = the full-charge baseline with playouts. TS combines:
  `value = deep[key] + inputCost + getDasPenalty(tier of placement.dasChargeAfter)`
  where `getDasPenalty` (precompute.ts) applies `>=15` → no penalty, `>=10` → partial
  penalty, else → min penalty. The algebra resolves to exactly `V_tier` for the tier the
  player's actual placement ends in.
- **Tournament mode** (`pruningBreadth = -1`): `getLockValueLookupTournament` receives
  the same charges and threads them through its `searchDepth2` pair enumeration; the
  tournament rounds/playouts themselves stay tap-based. The eval-only bucket maps are
  identical between tournament and legacy paths; only the deep map differs (tournament
  publishes conservative lower-confidence-bound scores).

## Where DAS is modeled exactly vs approximated

- **Exact**: `full_move_search.cpp` — the DAS move search used for both pieces of the DAS
  endpoint's 2-ply searches (per-goal frame simulation, midair dedup, fall phase with
  wall charges, depth-1 tuck/spin DFS).
- **Approximated**: `PieceRangeContext`, eval, and playouts simulate future pieces as
  tappers on the **DAS-equivalent timeline** (`getDasEquivalentInputTimeline`, ported to
  C++ in `utils.hpp`). `main.cpp` builds those contexts from the derived timeline when
  `dasCharge >= 0`; the real timeline reaches only the DAS move search.
- **Not modeled**: DAS charge threading through multi-piece playouts.

## Future work (design intent to preserve)

- **Adjustment-era support**: the DAS move search is deliberately adjustment-ready —
  designed around a generic start state (position/rotation/frames/arrIndex, DAS charge,
  held direction, slow-tap counters) and an input-sequence prefix seed — but only the
  spawn instantiation is exercised. The TS precompute (reaction-time adjustment logic)
  is expected to move into C++ long-term; input-sequence tracking (runtime-gated
  `trackInputSequences`, already implemented) is the prerequisite.
- **Multi-piece DAS charge threading through playouts** (replace the tapper
  approximation above).
- **Possible optimization**: fusing the DAS charge clock into the tap reachability walk.
  Measured costs for context: the per-goal DAS search is ~100x the tap walk per call
  (~129us vs ~1us for a T piece), making the DAS endpoint ~2.5x a tap-mode request
  end-to-end (~14ms vs ~5.8ms production-shaped).
