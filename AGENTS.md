# AGENTS.md

Working agreements for AI agent sessions (and humans) changing the StackRabbit engine.
Read alongside [TESTING.md](TESTING.md) (test layout, input-format contract) and
[docs/engine_boundaries.md](docs/engine_boundaries.md) (TS↔C++ contracts).

## Verification battery (run before and after any engine change)

- `npm run cpp_test_unit` — doctest suite, fast
- `npm run fuzz_verify_sequences` — TS↔C++ DAS move-search equivalence (character-exact sequences)
- `npx tsc`
- `npm run move_test`
- `npm run precompute_verify` — C++ precompute vs golden fixtures, byte-identical (3470/3470)
- `npm run precompute_tsan` — ThreadSanitizer replay of the 42 production-param precompute
  requests; zero warnings required (the thread-safety gate, see Change discipline)
- `npm run precompute_static_lint` — file-scope statics tripwire over the thread-audited files
- `npm run start_check` — builds the addon + TS (the only battery item exercising the
  node-gyp addon build), starts the server, waits for "Listening on port 3000", kills
  the process tree, and verifies port 3000 is freed; fails loudly if a stale server
  already occupies the port (kill it manually — the trap below)

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
- **Precompute lookup thread-safety is a tested contract.** Precompute runs
  `getLockValueLookupMapped` / `getLockValueLookupDasMaps` (see their contract comments in
  `high_level_search.cpp`) once per next piece on concurrent threads. No new non-const
  file-scope statics or thread-unsafe libc calls (`rand()`, `strtok`, ...) in code reachable
  from those entry points. Enforcement: `npm run precompute_tsan` (battery item — ~18s,
  cheap enough for every run) and `npm run precompute_static_lint`. The lint is knowingly
  crude; a false positive is waived in `src/cpp_modules/tests/thread_static_allowlist.txt`
  (adding a waiver is a deliberate, reviewable act), never by weakening the lint.
- When touching the pipe format or shared signatures, update **all** callers in
  lockstep: `request_parser.ts`, `worker_thread.ts`, `cmodules.ts`, `entrypoint.cpp`,
  `speed_benchmark.cpp`, `run_agent.cpp`, `src/wasm/sample_client/wasmRabbit-worker.js`
  — and rebuild the WASM artifacts (`npm run wasm_build`) when the engine changes.
- Design APIs so parameters mean one thing. If a call site needs different semantics,
  add a clearly-named parameter rather than overloading an existing one, and make
  mutually exclusive modes explicit (error out when both are passed).
- **Code comments describe end-state behavior, not process.** Never reference TS
  parity, the TS implementation, or migration phases/steps in code comments — that
  context belongs in the docs (TESTING.md, docs/*.md). Two exceptions:
  (1) ported behavior that may itself be a legacy bug may be flagged as parity
  with "legacy TS code"; (2) unintuitive temporary behavior caused by a not-yet-
  completed migration step may carry a TODO naming that step.

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
- **Float-parity vs V8: disable FMA contraction on accumulation lines.** Clang/GCC
  default to `-ffp-contract=fast`, which fuses `acc += a * b` into a
  single-rounding `fma` — even at -O0 — while V8 rounds the multiply and add
  separately. A 1-ULP drift per accumulation silently broke exact phantom-value
  ties in the precompute port (18/3470 fixtures picked a different winner). When
  C++ must reproduce JS float arithmetic bit-exactly, split the multiply and the
  add into separate statements (or control `-ffp-contract`) and verify with
  hex-float dumps.
