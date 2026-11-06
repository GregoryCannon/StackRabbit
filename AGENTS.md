# AGENTS.md

Working agreements for AI agent sessions (and humans) changing the StackRabbit engine.
Read alongside [TESTING.md](TESTING.md) (test layout, input-format contract) and
[docs/engine_boundaries.md](docs/engine_boundaries.md) (TS↔C++ contracts).

## Verification battery (run before and after any engine change)

- `npm run cpp_test_unit` — doctest suite, fast
- `npm run fuzz_verify_sequences` — TS↔C++ DAS move-search equivalence (character-exact sequences)
- `npx tsc`
- `npm run move_test`
- `npm run cpp_test` — node-gyp rebuild + live addon call

NB: a long-running dev server keeps the OLD addon mapped in memory — `node-gyp build`
on disk does not update it. Restart the server to test new code, and don't mistake
stale-server output for a code change.

## Change discipline

- Make changes in small, **independently revertible steps** (git checkout of individual
  files works — see `git status` before assuming). Run the full battery after each step
  before proceeding.
- **Production signature changes are the checkpoint boundary.** If a step stalls, STOP
  AND REPORT — do not "fix forward" by widening the diff.
- **Every boundary validates its own inputs** (TS URL args in `request_parser.ts`, the
  C++ pipe parser in `main.cpp`). Never rely on the caller having validated; silent
  defaults and junk-tolerant parsing have each caused real bugs here.
- When touching the pipe format or shared signatures, update **all** callers in
  lockstep: `request_parser.ts`, `worker_thread.ts`, `cmodules.ts`, `entrypoint.cpp`,
  `speed_benchmark.cpp`, `run_agent.cpp`, `src/wasm/sample_client/wasmRabbit-worker.js`
  — and rebuild the WASM artifacts (`npm run wasm_build`) when the engine changes.
- Design APIs so parameters mean one thing. If a call site needs different semantics,
  add a clearly-named parameter rather than overloading an existing one, and make
  mutually exclusive modes explicit (error out when both are passed).

## Performance landmines

- **Never widen hot-path structs to carry feature data.** Adding ~40 bytes to
  `LockPlacement` regressed the playout benchmark ~9% (the struct is copied tens of
  thousands of times per request). Flow new data through side channels instead (see
  `DasPossibility`).
- **Benchmark with an interleaved A/B against a same-session worktree baseline**
  (`git worktree add <tmp> HEAD`, build both, alternate runs). Cross-session
  comparisons produced a phantom +5.5% "regression" here; the same-session true cost
  was +0.2%. Build the same source twice first to establish the noise floor (~±0.7%
  per case on this machine) — deltas outside that band are real, and codegen/layout
  luck from touching a unity-build translation unit can swing either direction.
- Deep-dive costs: the DAS move search is ~100x the tap walk per call; see
  docs/engine_boundaries.md for the current measurements.

## Verification lessons

- **Verify on the FULL set, not a sample** — a 40-fixture sample once hid mislabeled
  results that only surfaced on the full run.
- **Never assume fixture ordering** when diffing generated JSON — derive indexes from
  the file itself.
- Pin new behavior with tests as you go (see TESTING.md's sections for the pattern);
  flip characterization tests deliberately and say why in the test comment.
