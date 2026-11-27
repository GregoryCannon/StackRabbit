# AGENTS.md

Working agreements for AI agent sessions (and humans) changing the StackRabbit engine.
Read alongside [TESTING.md](TESTING.md) (test layout, input-format contract) and
[docs/engine_endpoints.md](docs/engine_endpoints.md) (engine endpoints & wire contracts).

## Change-scoped verification matrix

Run the default checks before and after any C++ change, plus any triggered checks
matching the files or behaviors touched (full harness details in [TESTING.md](TESTING.md)):

| Change scope | Tests to run |
|---|---|
| **Always (default for any C++ change)** | `npm run format_cpp`, `npm run cpp_test_unit`, `npm run precompute_static_lint` |
| **HTTP server, routes, or Lua wire format** (`live_game_server.cpp`, `main.cpp` parsing, `stackrabbit.lua`) | + `npm run live_server_check` |
| **Behavior-preserving refactors / perf optimizations**, or **intentional eval / search / precompute changes** (inspect diffs & re-baseline) | + `npm run precompute_verify` (3,470/3,470 golden fixtures) |
| **Full move search / simulation** (`full_move_search.cpp`, `tetrominoes.hpp`, `utils.hpp`) | + `npm run fuzz_verify_sequences` (2,783 character-exact fixtures) |
| **Threading, shared state, or static-buffer / allocation changes** in `precompute.cpp` or lookup call tree (`high_level_search`, `tournament_search`, `playout`, `eval`, `move_search`, `piece_rng`, and the other files audited by `precompute_static_lint.js`) | + `npm run precompute_tsan` (zero TSan warnings required) |
| **Eval / search quality or throughput changes** | + `npm run benchmark_run_agent -- GET_MOVE 49 2 20 hard` and/or interleaved `npm run speed_benchmark` |
| **Shared engine changes or WASM glue** (`wasm.cpp`, `wasmRabbit-worker.js`, pipe format) | + `npm run wasm_build` |

NB: a running `build/live_game_server` process keeps the OLD compiled binary in memory —
rebuilding on disk does not update it. Rebuild and restart `live_game_server`
(`npm run live_server`) to test new C++ code.

## Change discipline

- Make changes in small, **independently revertible steps** (git checkout of individual
  files works — see `git status` before assuming). Run the applicable change-scoped
  checks after each step before proceeding.
- **Production signature changes are the checkpoint boundary.** If a step stalls, STOP
  AND REPORT — do not "fix forward" by widening the diff.
- **Every boundary validates its own inputs** (the HTTP layer in `live_game_server.cpp`,
  the C++ pipe parser in `main.cpp`). Never rely on the caller having validated; silent
  defaults and junk-tolerant parsing have each caused real bugs here.
- **Precompute lookup thread-safety is a tested contract.** Precompute runs
  `getLockValueLookupMapped` / `getLockValueLookupDasMaps` (see their contract comments in
  `high_level_search.cpp`) once per next piece on concurrent threads. No new non-const
  file-scope statics or thread-unsafe libc calls (`rand()`, `strtok`, ...) in code reachable
  from those entry points. Enforcement: `npm run precompute_static_lint` (always-run
  default) and `npm run precompute_tsan` (~3–4s; required whenever touching threading,
  shared state, or the lookup call tree). The lint is knowingly crude; a false positive is
  waived in `src/tests/thread_static_allowlist.txt` (adding a waiver is a
  deliberate, reviewable act), never by weakening the lint.
- When touching the pipe format or shared signatures, update **all** callers in
  lockstep: `src/backend/live_game_server.cpp`,
  `src/tests/precompute_cli.cpp`, `entrypoint.cpp`,
  `src/benchmark/speed_benchmark.cpp`, `src/benchmark/run_agent.cpp`,
  `src/wasm/sample_client/wasmRabbit-worker.js`, `src/fceux/stackrabbit.lua`
  — and rebuild the WASM artifacts (`npm run wasm_build`) when the engine changes.
- Design APIs so parameters mean one thing. If a call site needs different semantics,
  add a clearly-named parameter rather than overloading an existing one, and make
  mutually exclusive modes explicit (error out when both are passed).
- **Mandatory reviewer subagent gate on non-trivial code changes.** Before relying on
  new or modified solver, sampler, NNUE, or adapter logic for multi-round training or
  benchmarks — and before closing out any phase or iteration — launch independent
  read-only reviewer subagents (**Logic**, **Perf & Scale**, **Readability**) against
  the diff. Launch them *in parallel* while smoke tests or long training/solve jobs run
  so review adds zero wall-clock delay, fix HIGH/MEDIUM findings before launching the
  next iteration, and record review sign-off explicitly in the plan doc checklist.
- **Code comments describe end-state behavior, not process.** Never reference
  migration phases/steps or retired implementations in code comments — that
  context belongs in the docs (`TESTING.md`, `docs/*.md`). Unintuitive temporary
  behavior caused by an in-progress refactor may carry a `TODO` naming the
  follow-up.
- **Commit messages describe qualitative changes only.** Never put benchmark
  scores, speedups, or metric improvement numbers in commit messages — record
  quantitative results in retrospective/plan docs (`docs/*.md`) instead.

## Writing Docs

When taking on big code changes, you should make the following docs:

#### A plan doc 

The purpose of this doc is to pin down all major technical design decisions and scopes the project into distinct phases. Agents will check off the TODO items within each phase as they are completed.

#### A retrospective doc 

The purpose of this doc is to share any quantitative or qualitative results, as well as high-level takeaways from the project.

### Doc Style

The writing style of docs varies by what the intended audience is:

#### Planning docs
The target audience is agents, so make sure the doc is as clear and specific as possible, without being verbose. Human-readability, while important, is not as much of a priority.

#### Retrospective docs
The target audience is humans, so comprehensibility is the #1 priority.

- Explain both *what* was done and *why* certain decisions were made. These insights are often the most important part of the doc.
- Using established technical language is totally fine, but include a glossary in every major section to define (in simple language) any technical terms you use.
- Too many abbreviations can make docs unreadable, especially to those not familiar with the technical language. The following guidelines should be followed for abbreviations:
  - Only use very common abbreviations (e.g. NN for neural net), and make sure each one is defined in a glossary before use.
  - Abbreviations must be used at least 3 times in the doc to be worth shortening. You should scan the doc for abbreviation counts after the first draft and after major edits. The exception to this rule is for technical terms like ReLU where the abbreviated form is more used/recognizable than the expanded version.


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
  [docs/engine_endpoints.md](docs/engine_endpoints.md) for the current measurements.

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

## Working Autonomously

You will often to be asked to work autonomously. When doing so, you should always be following an existing plan doc, and your goal should be to make as much progress as you can through the stated plan while working autonomously. Other guidelines:

- Always run code review agents after every major code change
- Run a 15 minute timer and provide text updates on the current in-progress work, as well as an updated ETA for the current step and the overall project.
- If a job fails or produces unexpected results, pause and brainstorm why it failed; don't simply retry it and brute force through the step. Fix any related tooling and make sure the system is resilient against the root cause of the mistake.
- Never discard expensive computation results. Any stage that takes more than a few minutes and produces resumable/additive outputs (state sets, checkpoints, training datasets) must persist its output before exiting non-zero to halt downstream stages — never throw away completed work just to fail fast.