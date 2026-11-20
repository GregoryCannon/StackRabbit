# Plan: Harder Agent-Accuracy Benchmark Positions

Date: 2026-11-06
Status: Formal plan (agreed design; no code changes made yet)

## 1. Objective

Create a fork of the agent accuracy benchmark in `src/benchmark` whose position set is
"hard" — positions where the agent's move choice is genuinely hard to get right, rather than
positions where a shallow agent happens to agree with the deep oracle.

## 2. Background

- **Oracle**: `src/benchmark/generate_oracle.cpp` runs a deep search (depth-2 over all
  first/second placement pairs, playout-rated) per benchmark position; results land in
  `src/benchmark/data/oracle_positions.jsonl` (format: `src/benchmark/oracle_format.md`).
- **Agent harness**: `src/benchmark/run_agent.cpp` runs the engine's move selector
  (`mainProcess`) at configurable depth (`playoutCount`, `playoutLength`, `pruningBreadth`)
  and scores the chosen move against the oracle.
- **Depth-0 probe**: `npm run benchmark_run_agent -- GET_LOCK_VALUE_LOOKUP 0 0 10`
  (`playoutCount=0`, `playoutLength=0` → eval-only, no playouts).
  Result: **77.5% match rate** (155/200), 147 exact matches, 8 within-margin matches,
  45 genuine errors.

## 3. Method (agreed)

1. Run the depth-0 agent over the benchmark positions and select the "easy" set:
   positions where the agent matched (exact rank-1/delta-0 or within-margin).
2. For each easy position, randomize the current + next piece (seeded RNG; skip degenerate
   positions).
3. Run the difficulty gauge via `GET_LOCK_VALUE_LOOKUP`:
   - shallow: `playoutCount=0`, `playoutLength=0` → eval argmax
   - deep: `playoutCount=2000`, `playoutLength=10`, `pruningBreadth=20` → playout argmax
4. If the two agent keys **agree** → position too easy; re-randomize and retry.
5. If they **disagree** → position is hard; add to the hard set.

## 4. Decisions (confirmed)

1. Redo exact matches **and** within-margin matches when identifying easy positions.
2. Use `GET_LOCK_VALUE_LOOKUP` (not `GET_MOVE`) as the difficulty gauge.
3. The deep gauge search is intended as a **preliminary** search; the disagreement criterion is
   what matters.
4. Skip degenerate positions (and randomized positions where the move search finds no legal
   move / goes all-dead).
5. Store the final hard position list in `src/benchmark/data/hard_benchmark_positions.jsonl`, which
   the oracle will later read to evaluate the hard positions.

## 5. Open Items / Next Steps Before Implementation

- Parameterize the benchmark position input path for `generate_oracle` and `run_agent`
  (optional CLI path argument, consistent with the existing `[startPos] [endPos]` argument
  pattern) so the hard set can be consumed without replacing the curated benchmark file.
- Define the schema and per-position provenance (original slot/index + re-randomized
  cur/next piece + RNG seed) for `src/benchmark/data/hard_benchmark_positions.jsonl`.
- Regenerate oracle data later over the confirmed hard position list.

## 6. Verification

After implementation lands, run the relevant verification checks (`npm run cpp_test_unit`,
`npm run precompute_static_lint`, `npm run fuzz_verify_sequences`), plus
`npm run benchmark_run_agent` on the new hard set.
