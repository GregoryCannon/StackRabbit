# Oracle Data Format

Specification for the benchmark oracle file: deep-search reference ratings for the
200 benchmark positions, used to measure agent accuracy (benchmark step 2 → step 3).

- **File**: `src/benchmark/oracle_positions.jsonl`
- **Format**: JSONL — one JSON object per line (UTF-8)
- **Source positions**: line-per-position from `src/benchmark/benchmark_positions.jsonl`

## Header (line 0)

Records the provenance of the oracle run. Must be enough to reproduce or audit the run.

```json
{
  "type": "oracle",
  "source": "benchmark_positions.jsonl",
  "timeline": "X.....",
  "engineCommit": "...",
  "weights": "MAIN/SAFE",
  "round1": { "playouts": 1000, "playoutLength": 10, "sequenceOffset": 0 },
  "round3": { "playouts": 7000, "playoutLength": 10, "sequenceOffset": 1000, "selection": "top 10% of pairs by round-1 rating" },
  "date": "...",
  "notes": ""
}
```

| Field | Meaning |
|---|---|
| `source` | Benchmark file the positions were read from |
| `timeline` | Input frame timeline used for the search context (same as the position log header) |
| `engineCommit` | Git commit of the engine that produced the ratings |
| `weights` | Weight set + aggression level used (e.g. `MAIN/SAFE`) |
| `round1` | First playout round: every (first, second) pair gets this many playouts, starting at `sequenceOffset` rows into the canonical batch |
| `round3` | Second playout round: extra playouts (disjoint sequence rows, starting at `sequenceOffset`) for the top 10% of pairs by round-1 rating; stored values pool both rounds |
| `date`, `notes` | Free-form |

## Position lines (1–200)

One line per benchmark position, self-contained (copies the position fields so each
line is standalone; `index` references the explicit `index` field on the corresponding
line of the benchmark file).

```json
{
  "index": 42,
  "board": "14,FC,FC,4",
  "level": 18,
  "lines": 57,
  "curPiece": 4,
  "nextPiece": 2,
  "aiMode": 0,
  "shallowEval": -12.34,
  "best": ["2|4|18", "1|5|20"],
  "bestRating": 31.42,
  "bestStandardError": 0.18,
  "margin": 0.53,
  "ratings": [["2|4|18", "1|5|20", 31.42, 0.18, 8000], ["2|4|18", "0|-4|19", 30.89, 0.21, 1000], "..."]
}
```

### Position fields (copied from the benchmark file)

| Field | Meaning |
|---|---|
| `index` | Position index, matching the explicit `index` field on the corresponding line of `benchmark_positions.jsonl`. Indices are slot-stable (1..200 in output order) and survive position swaps/reorders, so benchmark and oracle files can't silently desync |
| `board` | `formatBoard()` compressed board string |
| `level`, `lines` | Game level and line count |
| `curPiece`, `nextPiece` | PIECE_LIST indices (I=0 O=1 L=2 J=3 T=4 S=5 Z=6) |
| `aiMode` | Mode at capture time (STANDARD=0 DIG=1 LINEOUT=2 NEAR_KILLSCREEN=3 DIRTY_NEAR_KILLSCREEN=4) |
| `shallowEval` | Static board eval in forced STANDARD mode (see position logging) |

### Oracle fields

| Field | Meaning |
|---|---|
| `ratings` | All (first placement, second placement) pairs with their deep-search rating, **sorted best-first**. Each entry is `[firstKey, secondKey, rating, standardError, n]`. `ratings[0]` is always the best move. |
| `best` | The best pair, repeated for convenience: `[firstKey, secondKey]` |
| `bestRating` | Rating of the best pair |
| `bestStandardError` | Standard error of the best pair's rating (duplicate of `ratings[0][3]`, for convenience) |
| `margin` | `bestRating` − best rating among the *remaining* pairs (tie-ness indicator). Compare against the combined noise `sqrt(SE_best² + SE_second²)` — margins below ~2× that are within playout variance and weak test cases; step 3 may weight accuracy by margin |

## Conventions

1. **Placement keys** use the engine's canonical lock-position encoding `"rotationIndex|x|y"`
   (same as `GET_LOCK_VALUE_LOOKUP` output, via `encodeLockPosition`). NB: this uses the raw
   internal `x`, **not** the `x - 3` offset that `GET_MOVE` returns, and raw `y`, not the
   `y - initialY` offset. Document any consumer-side translation.
2. **Ratings** are stored with 2 decimals (`%.2f`), matching the engine's own encoded-map
   precision. A rating is the full search value for that pair: immediate reward + deep
   playout score of the resulting state.
3. **Ordering**: `ratings` must be sorted by rating descending; ties broken by insertion
   order of the search (stable sort).
4. **Variable length**: the number of entries depends on the pieces — O×O ≈ 81 pairs
   (9 × 9), I×I = 289 (17 × 17), T/J/L/S/Z up to 1156 (34 × 34 = 4 rotations × 10 box-x
   positions). Entries are variable-length; do not assume a fixed count.
5. **Variance**: each `rating` is the pooled sample mean over `n` independent playouts
   (`n` = the round-1 count for most pairs, round-1 + round-3 for pairs selected into
   round 3), so ratings are estimates, not exact values. The accompanying `standardError`
   is the standard error of that pooled mean (`stdev(allPlayoutScores) / sqrt(n)`); a 95%
   confidence interval for the true score is roughly `rating ± 1.96 × standardError`.
   Consumers comparing an agent's score to an oracle score should treat differences
   smaller than ~2× the combined standard error as statistically indistinguishable.
   Playout sequences are drawn from the fixed canonical sequence batches, so a given
   oracle run is deterministic and reproducible; sequences are also shared across pairs
   within a position (common random numbers), which tightens margin comparisons.