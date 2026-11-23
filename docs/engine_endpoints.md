# Engine Endpoints & Wire Contracts

One-page reference for the C++ engine's pipe wire format, standalone HTTP server
routes, and DAS search/precompute interfaces. The authoritative input-format
table lives in [TESTING.md](../TESTING.md); the game-level DAS mechanics
(including the frame-exact charge model) live in
[DAS_explainer.md](DAS_explainer.md).

## Request wire format (the C++ pipe string)

```
[board1 (200 chars 0/1)] | [optional board2] | level | lines | currentPieceIdx |
nextPieceIdx | inputFrameTimeline | playoutCount | playoutLength | pruningBreadth |
aggressionLevel | dasCharge |
```

- Every non-board field is **mandatory and strictly validated** by the C++ parser
  (`main.cpp`) — see [TESTING.md](../TESTING.md) for the full contract.
  `secondBoard` is the one genuinely optional field (per request type).
- `PRECOMPUTE` and `PRECOMPUTE_BACKUP` append a mandatory 12th field,
  `reactionTime` (`0..60` frames), after `dasCharge` (with `nextPiece = -1`,
  since precompute searches all seven next pieces).
- `dasCharge`: `-1` = tap mode, `0..16` = the player is holding DAS with that
  much charge.
- The **real** input frame timeline is always sent. The C++ side derives its own
  tapper approximation for piece-range/eval/playout contexts (below); only the
  full move search consumes the real timeline.
- Encoders and callers that must stay in lockstep:
  `src/backend/live_game_server.cpp`,
  `src/tests/precompute_cli.cpp`, `entrypoint.cpp`,
  `src/benchmark/speed_benchmark.cpp`, `src/benchmark/run_agent.cpp`,
  `src/wasm/sample_client/wasmRabbit-worker.js`, `src/fceux/stackrabbit.lua`.

## C++ live game server surface

`src/backend/live_game_server.cpp` — standalone zero-dependency HTTP
server serving the live precompute and A/B test endpoints over native sockets
(POSIX and Winsock2; see the file's header comment for build commands and
design details). Default port `3000`, overridable via `argv[1]` or the `PORT`
env var (the verification script `live_server_check` uses port `3101`).
Responses are `text/plain` with `Access-Control-Allow-Origin: *` and
`Connection: close`.

Routes:

- `GET /ping` → `200` `pong`.
- `GET /precompute?p=<pipe>` → `200` `Request accepted.` and the request is
  computed asynchronously (backup tier first, then full 7-thread precompute);
  `503` `Busy` if an async request is still computing; `400` if `p` is absent.
- `GET /async-result` → `200` + full result once computed, else `200` + backup
  response (which also signals cancellation to halt the in-flight full
  precompute), else `504` `Still calculating` (the window before the backup
  lands), else `404` `No previous async request has been made`. A rejected or
  failed engine run surfaces as `500` + message and sticks until the next valid
  `/precompute` kick.
- `GET /precompute-sync?p=<pipe>` → `200` + full precompute result computed
  synchronously with startup-calibrated 7-thread timing; `503` `Busy` if an
  async `/precompute` job is currently running (avoids oversubscribing two
  concurrent 7-thread precomputes); `400` if `p` is absent; `500` on `|`
  delimiter, validation rejection, or engine exception.
- `GET /get-move-ab-test?p=<pipe>` → `200` + synchronous JSON A/B move
  comparison between main weights and test weights, formatted via
  `fullMoveSearch`:
  - Agree: `["agree", "<formatted>", {"explanationMain":"..."}]`
  - Disagree: `["disagree", "<mainFormatted>", "<testFormatted>", {"explanationMain":"...", "explanationTest":"..."}]`
  - Each `<formatted>` string has the shape
    `rot,x,y|inputSequence|boardAfter|level|lines|dasCharge`, or `"none"` if no
    legal move exists (topped-out board).
  - Accepts either the 11-field standard comma-delimited pipe or the 12-field
    precompute shape (an optional trailing `reactionTime` field is ignored).
  - Returns `400` if `p` is absent, and `500` on `|` delimiter, validation
    rejection, or engine exception.
- `GET /debug-precompute?p=<pipe>` → `200` + synchronous JSON phantom collapse
  breakdown (ranked phantoms, per-next-piece `rawLockValue` / `dasPenalty` /
  `inputCost` / `totalValue`, reachable adjustments, and exact `response`
  string; `503` `Busy` while an async `/precompute` job is running; `400` if
  `p` is absent; `500` on `|` delimiter or pipe validation error).
- `GET /debug-ground-truth?p=<pipe>[&keys=k1,k2][&n=2000][&conts=5]` → `200` +
  synchronous JSON ground-truth playout comparison across all 7 next pieces for
  selected first-piece lock keys (default: top 4 by `fastEval`) plus the full
  list of reachable placements (`503` `Busy` while an async `/precompute` job is
  running; `400` on missing `p` or invalid `n`/`conts`/`keys`; `500` on `|`
  delimiter or pipe validation error).
- `GET /debug-tournament?p=<pipe>&next=<0..6|I|O|L|J|T|S|Z>` → `200` +
  synchronous JSON round-by-round tournament funnel trace (`R1`/`Cut1`/`R2`/
  `Cut2`/`R3` sample counts, timings, per-candidate ranks & published LCBs, and
  per-continuation `samples`/`rawMean`/`se`/`lcbScore`/`deathPct`) for one next
  piece; requires tournament mode (`pruningBreadth == -1` or `-2`); `503` `Busy`
  while an async `/precompute` job is running; `400` on missing `p`/`next`;
  `500` on `|` delimiter, pipe error, or non-tournament `pruningBreadth`.

The comma-delimited URL contract: the `p` parameter is the request string
delimited by `,` so queries are valid unencoded in browsers and match the
server's `[compute] backup=<ms> full=<ms> input=<query>` replay logs (raw `,`
is preferred; percent-encoded `%2C` is also accepted and decoded, while `|` is
rejected with `500` — synchronously on `/precompute-sync` and
`/get-move-ab-test`, and via `/async-result` on async `/precompute`). Internally
`live_game_server.cpp` translates `,` to `|` and delegates to `mainProcess`'s
strict field validation in `main.cpp`. Non-GET or malformed requests return
`400`, header reads timing out return `408`, and unknown paths return `200`
with `"Unknown request type. Received: <path>"`.

Backup tier semantics: the worker computes the backup response first (one move
search plus cheap evals, a few ms) so a poll landing before the full result is
answered with it instead of a `504`; the full precompute supersedes it once
done. The backup is a full-format response (`Default:<placement>` + seven
`<piece>:continue` lines, or `none` when the piece has no legal placement) —
the same shape the full result can take when every next piece keeps the default.

## Lock position encoding (the join key)

`lockPositionEncoded = "rot|x|y"` — absolute coordinates at lock
(`encodeLockPosition` in `high_level_search.cpp`, `encodeLockCell` in
`precompute.cpp`). This exact string is the join key between the lock-value /
DAS-penalty maps and `precompute.cpp`'s adjustment placement lookups;
`precompute.cpp` skips placements whose key is missing from a map. **Do not
change the format.**

## DAS penalty pipeline (`GET_LOCK_VALUE_LOOKUP_DAS` & `precompute.cpp`)

Lookup map shape: `[partialChargePenalties, minChargePenalties, deepValues]` —
three maps keyed by lock position (returned in-process as `DasLockValueMaps`
for `PRECOMPUTE`, or serialized as JSON for `GET_LOCK_VALUE_LOOKUP_DAS`).

- **First-piece DAS charge**: When `dasCharge >= 0` (always true in DAS-mode
  `PRECOMPUTE`, and optional for `GET_LOCK_VALUE_LOOKUP_DAS`), first-piece
  reachability starts from that known spawn `dasCharge` so unreachable
  high-charge placements are excluded from both the bucket maps and the deep
  search. `GET_LOCK_VALUE_LOOKUP_DAS` with `dasCharge == -1` falls back to
  `DAS_CHARGE_MAX`.
- **Penalty maps**: The backend runs a shallow search with three possible DAS
  charges (`15`, `10`, `5`) to measure how much worse lower DAS charges are
  compared to full charge. These charges apply to the **second** piece, because
  the precompute pipeline computes one lock-value lookup per possible next
  piece and then redirects the current piece mid-air after the reaction-time
  frame. Mid-air adjustments often cannot keep full DAS, so the penalty maps
  capture the value lost by entering the second piece with partial or minimal
  DAS charge. Both the tournament and legacy lookup paths compute the same
  `15/10/5` buckets.
- **Deep map & collapse combination**: The deep map is the full-charge baseline
  with playouts. In `precompute.cpp` (`collapseAndFormatPrecompute`), each
  candidate placement is scored in `double` precision as:
  `value = lockValues[i][key] + inputCost + dasPenalty(useDAS, dasChargeAfter, key, partialPenalties[i], minPenalties[i])`
  where `dasPenalty` applies `dasChargeAfter >= 15` → `0`, `>= 10` →
  `partialPenalties[i][key]`, else → `minPenalties[i][key]` (and `0` in tap
  mode or when a key is absent). For adjustments, `inputCost` comes from
  `adjustmentInputCost` (symmetric frame-by-frame deviation from the phantom's
  post-reaction default move plus tuck/spin cost); for a phantom without
  adjustments, it uses the initial placement's tuck cost.
- **Tournament mode** (`pruningBreadth = -1` for fixed playout count,
  `pruningBreadth = -2` for millisecond wall-clock budget):
  `getLockValueLookupTournament` receives the same charges and threads them
  through its `searchDepth2` pair enumeration; the tournament rounds/playouts
  themselves stay tap-based. The eval-only bucket maps are identical between
  tournament and legacy paths; only the deep map differs (tournament publishes
  conservative lower-confidence-bound scores).

## Where DAS is modeled exactly vs approximated

- **Exact**: `full_move_search.cpp` — the full move search used for both spawn
  and mid-air adjustment searches in `precompute.cpp`, for `GET_MOVE_AB_TEST`
  formatting, and for both pieces of `GET_LOCK_VALUE_LOOKUP_DAS`'s 2-ply
  searches (per-goal frame simulation, midair dedup, fall phase with wall
  charges, reaction-time `adjTimeSimState` capture, and depth-1 tuck/spin DFS).
- **Approximated**: `PieceRangeContext`, eval, and playouts simulate future
  pieces as tappers on the **DAS-equivalent timeline**
  (`getDasEquivalentInputTimeline` in `utils.hpp`). `main.cpp` builds those
  contexts from the derived timeline when `dasCharge >= 0`; the real timeline
  reaches only `fullMoveSearch`.
- **Not modeled**: DAS charge threading through multi-piece playouts.

## Future work (design intent to preserve)

- **Multi-piece DAS charge threading through playouts** (replace the tapper
  approximation above).
- **Possible optimization**: fusing the DAS charge clock into the tap
  reachability walk. Measured costs for context: the per-goal DAS search is
  ~100x the tap walk per call (~129us vs ~1us for a T piece), making the DAS
  endpoint ~2.5x a tap-mode request end-to-end (~14ms vs ~5.8ms
  production-shaped).
