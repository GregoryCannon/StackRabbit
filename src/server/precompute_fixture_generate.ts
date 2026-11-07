import { PreComputeManager } from "./precompute";
import { RequestHandler } from "./request_handler";
import { getSearchStateFromUrlArguments, parseUrlArguments } from "./request_parser";
import { PieceId } from "./types";
import * as fs from "fs";

/**
 * Golden fixture generator for the precompute pipeline (Phase 0 step 5 of the
 * precompute C++ migration, see docs/precompute_cpp_migration.md).
 *
 * Drives the REAL TS precompute path (parseUrlArguments -> SearchState ->
 * RequestHandler.handlePrecomputeRequestSync with a live PreComputeManager) over
 * positions sampled from src/benchmark/data/position_log.jsonl, and records the
 * exact returned string per fixture into a versioned JSONL artifact. That file is
 * the parity oracle the C++ port will be differentially tested against in Phase 3.
 *
 * Regenerate with: npm run precompute_fixture_generate [-- <outputPath>]
 * Output is deterministic (fixed PRNG seed, fixed position log): two runs must be
 * byte-identical. No timestamps are embedded.
 *
 * Two-tier design (measured on this machine: eval-only requests ~12-46ms,
 * production-param requests ~27-54ms; the TS move search + adjustment phase
 * dominates, playout params matter less):
 *   - eval_only tier: every logged position x 2 (rt/das) combos with
 *     playoutCount=0/playoutLength=0 (eval-only lock lookups in the C++ workers).
 *     Parity still holds because the C++ side replays the identical params.
 *   - production tier: a smaller subset with the default engine params
 *     (playoutCount=49, playoutLength=2, pruningBreadth=20, aggression=safe).
 *
 * The logged nextPiece is ignored (precompute searches all 7 next pieces); the
 * current piece is re-randomized over all 7. Coverage quotas are asserted at the
 * end (hard-fail). Categories unreachable from the log are covered by synthetic
 * fixtures appended below (see SYNTHETIC_FIXTURES and the quota report).
 */

const SEED = 0x5eedc0de;
const DEFAULT_OUTPUT = "src/server/data/precompute_fixtures.jsonl";
const POSITION_LOG = "src/benchmark/data/position_log.jsonl";
const PIECES: PieceId[] = ["I", "O", "L", "J", "T", "S", "Z"];
const REACTION_TIMES = [0, 8, 20];
const DAS_BUCKETS = [-1, 0, 5, 10, 15, 16];
// (rt, das) combos in a fixed order: index c -> das = DAS_BUCKETS[c/3], rt = RT[c%3]
const NUM_COMBOS = REACTION_TIMES.length * DAS_BUCKETS.length;
const PRODUCTION_STRIDE = 41; // every 41st log position gets a production-tier fixture

// Coverage quota floors (hard-fail if unmet after generation).
const MIN_PER_PIECE = 100;
const MIN_PER_RT_ZERO = 100;
const MIN_PER_DAS_BUCKET = 100;
const MIN_PER_COMBOS = 30; // each (rt, das) combo
const MIN_CONTINUE = 1;
const MIN_PIECE_NONE = 1;
const MIN_WHOLE_NONE = 1;
const MIN_DEFAULT_PLACEMENT = 100;

interface LogPosition {
  gameId: number;
  moveIndex: number;
  board: string; // compressed formatBoard string
  level: number;
  lines: number;
}

interface SyntheticFixture {
  tag: string;
  board: string; // compressed formatBoard string
  piece: PieceId;
  level: number;
  lines: number;
  reactionTime: number;
  dasCharge: number;
  useProductionParams: boolean;
}

/**
 * Synthetic fixtures for coverage categories unreachable from the position log.
 * Only appended when the natural run misses a quota (deterministic condition).
 * - no_moves_*: a fully right-well board ("0,,20": every column but the well
 *   full). Only the vertical I fits the 1-wide well, so the other pieces have no
 *   legal moves and the whole response is exactly "none".
 * - piece_none_* / continue_*: a 16-high flat stack ("4," + 16 full rows) where
 *   every placement locks well before the reaction time, so no phantom gets an
 *   adjustment search state. The per-piece fallback is then "continue" when the
 *   winning phantom's input sequence is longer than the reaction time (rt=8;
 *   sequences carry ~12 trailing lock frames) and "none" when it is shorter
 *   (rt=20). The overhang ("ledge") variants additionally exercise the tuck
 *   path: flat 6 with a 5-deep shaft carved at col 4 (same shape as the
 *   move-search fuzz ledgeBoard).
 */
const TALL_STACK_BOARD = "4,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF,0"; // 16-high flat stack
const LEDGE_BOARD = "14,FFEpEpEpEpEp,0"; // flat 6, 5-deep shaft at col 4
const SYNTHETIC_FIXTURES: SyntheticFixture[] = [
  { tag: "no_moves_O", board: "0,,20", piece: "O", level: 18, lines: 0, reactionTime: 8, dasCharge: -1, useProductionParams: false },
  { tag: "no_moves_S", board: "0,,20", piece: "S", level: 18, lines: 0, reactionTime: 8, dasCharge: 10, useProductionParams: false },
  { tag: "no_moves_T_prod", board: "0,,20", piece: "T", level: 18, lines: 0, reactionTime: 8, dasCharge: -1, useProductionParams: true },
  { tag: "piece_none_tall_rt20", board: TALL_STACK_BOARD, piece: "O", level: 18, lines: 100, reactionTime: 20, dasCharge: -1, useProductionParams: false },
  { tag: "piece_none_tall_rt20_das", board: TALL_STACK_BOARD, piece: "T", level: 18, lines: 100, reactionTime: 20, dasCharge: 16, useProductionParams: false },
  { tag: "continue_tall_rt8", board: TALL_STACK_BOARD, piece: "L", level: 18, lines: 100, reactionTime: 8, dasCharge: -1, useProductionParams: false },
  { tag: "continue_tall_rt8_das", board: TALL_STACK_BOARD, piece: "J", level: 18, lines: 100, reactionTime: 8, dasCharge: 5, useProductionParams: false },
  { tag: "continue_ledge", board: LEDGE_BOARD, piece: "L", level: 18, lines: 40, reactionTime: 8, dasCharge: -1, useProductionParams: false },
  { tag: "continue_ledge_das", board: LEDGE_BOARD, piece: "J", level: 18, lines: 40, reactionTime: 20, dasCharge: 5, useProductionParams: false },
];

// ---------------------------------------------------------------------------

function mulberry32(seed: number): () => number {
  let s = seed;
  return function () {
    s |= 0;
    s = (s + 0x6d2b79f5) | 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (s >>> 7), 61 | t)) ^ t;
    return ((t ^ (s >>> 14)) >>> 0) / 4294967296;
  };
}

function comboFor(index: number): { reactionTime: number; dasCharge: number } {
  const c = ((index % NUM_COMBOS) + NUM_COMBOS) % NUM_COMBOS;
  return {
    reactionTime: REACTION_TIMES[c % REACTION_TIMES.length],
    dasCharge: DAS_BUCKETS[Math.floor(c / REACTION_TIMES.length)],
  };
}

function loadPositions(): { timeline: string; positions: LogPosition[] } {
  const raw = fs.readFileSync(POSITION_LOG, "utf8").trim().split("\n");
  const header = JSON.parse(raw[0]);
  if (header.type !== "header") {
    throw new Error("position_log.jsonl missing header line");
  }
  const positions: LogPosition[] = [];
  for (let i = 1; i < raw.length; i++) {
    const d = JSON.parse(raw[i]);
    positions.push({
      gameId: d.gameId,
      moveIndex: d.moveIndex,
      board: d.board,
      level: d.level,
      lines: d.lines,
    });
  }
  return { timeline: header.timeline, positions };
}

interface FixtureSpec {
  tier: "eval_only" | "production";
  source: { gameId?: number; moveIndex?: number; boardCompressed: string; syntheticTag?: string };
  boardCompressed: string;
  piece: PieceId;
  level: number;
  lines: number;
  reactionTime: number;
  dasCharge: number;
}

function buildFixtureSpecs(positions: LogPosition[]): FixtureSpec[] {
  const specs: FixtureSpec[] = [];
  const rand = mulberry32(SEED);
  void rand; // seed reserved for future randomized variation

  // Bulk tier: 2 fixtures per logged position, cycling all (rt, das) combos
  // (i*2+k walks the 18-combo space uniformly), and cycling pieces.
  for (let i = 0; i < positions.length; i++) {
    for (let k = 0; k < 2; k++) {
      const combo = comboFor(i * 2 + k);
      specs.push({
        tier: "eval_only",
        source: {
          gameId: positions[i].gameId,
          moveIndex: positions[i].moveIndex,
          boardCompressed: positions[i].board,
        },
        boardCompressed: positions[i].board,
        piece: PIECES[(i + k * 3) % 7],
        level: positions[i].level,
        lines: positions[i].lines,
        reactionTime: combo.reactionTime,
        dasCharge: combo.dasCharge,
      });
    }
  }

  // Production tier: every PRODUCTION_STRIDE-th logged position, cycling combos.
  let j = 0;
  for (let i = 0; i < positions.length; i += PRODUCTION_STRIDE) {
    const combo = comboFor(j);
    specs.push({
      tier: "production",
      source: {
        gameId: positions[i].gameId,
        moveIndex: positions[i].moveIndex,
        boardCompressed: positions[i].board,
      },
      boardCompressed: positions[i].board,
      piece: PIECES[j % 7],
      level: positions[i].level,
      lines: positions[i].lines,
      reactionTime: combo.reactionTime,
      dasCharge: combo.dasCharge,
    });
    j++;
  }

  return specs;
}

// ---------------------------------------------------------------------------
// Golden computation via the real TS precompute path
// ---------------------------------------------------------------------------

const REQUEST_TIMEOUT_MS = 120000;

function requestHasContinueLine(output: string): boolean {
  return /\n[IOLJTSZ]:continue(\n|$)/.test(output);
}

function requestHasPieceNoneLine(output: string): boolean {
  return /\n[IOLJTSZ]:none(\n|$)/.test(output);
}

function requestIsWholeNone(output: string): boolean {
  return output === "none";
}

function requestHasDefaultPlacement(output: string): boolean {
  return output.startsWith("Default:") && !output.startsWith("Default:N/A");
}

async function runFixture(
  handler: RequestHandler,
  timeline: string,
  spec: FixtureSpec
): Promise<any> {
  const params = `board=${spec.boardCompressed}&currentPiece=${spec.piece}&level=${spec.level}&lines=${spec.lines}&inputFrameTimeline=${timeline}&reactionTime=${spec.reactionTime}&dasCharge=${spec.dasCharge}`;
  const extraParams =
    spec.tier === "production" ? "" : "&playoutCount=0&playoutLength=0";

  const urlArgs = parseUrlArguments(params + extraParams, "precompute-sync");
  const searchState = getSearchStateFromUrlArguments(urlArgs);

  const output = await Promise.race([
    handler.handlePrecomputeRequestSync(searchState, urlArgs),
    new Promise<never>((_, reject) =>
      setTimeout(
        () => reject(new Error(`Request timed out: ${JSON.stringify(spec)}`)),
        REQUEST_TIMEOUT_MS
      )
    ),
  ]);

  // Recover the raw 200-char board from the parsed board (parseBoard already
  // decompressed the compressed log string).
  const boardRaw = urlArgs.board.map((row) => row.join("")).join("");
  if (boardRaw.length !== 200) {
    throw new Error(`Bad board length for fixture: ${JSON.stringify(spec)}`);
  }

  return {
    tier: spec.tier,
    source: spec.source,
    board: boardRaw,
    currentPiece: spec.piece,
    level: spec.level,
    lines: spec.lines,
    inputFrameTimeline: urlArgs.inputFrameTimeline,
    reactionTime: urlArgs.reactionTime,
    dasCharge: urlArgs.dasCharge,
    playoutCount: urlArgs.playoutCount,
    playoutLength: urlArgs.playoutLength,
    pruningBreadth: urlArgs.pruningBreadth,
    aggressionLevel: urlArgs.aggressionLevel,
    output,
  };
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

const outputPath = process.argv[2] || DEFAULT_OUTPUT;

const manager = new PreComputeManager();
manager.initialize(async () => {
  const handler = new RequestHandler(manager);
  const totalStart = Date.now();
  try {
    const { timeline, positions } = loadPositions();
    const specs = buildFixtureSpecs(positions);
    console.log(`Generating ${specs.length} fixtures (log positions: ${positions.length}, timeline: ${timeline})`);

    const records: any[] = [];
    const tierMs: Record<string, number> = { eval_only: 0, production: 0 };
    for (let i = 0; i < specs.length; i++) {
      const t0 = Date.now();
      const record = await runFixture(handler, timeline, specs[i]);
      tierMs[specs[i].tier] += Date.now() - t0;
      records.push(record);
      if ((i + 1) % 250 === 0) {
        console.log(`  ${i + 1}/${specs.length} fixtures done (${((Date.now() - totalStart) / 1000).toFixed(1)}s)`);
      }
    }

    // Synthetic coverage pass: only append what the natural set missed (the
    // condition is deterministic, so reruns stay byte-identical).
    const naturalContinue = records.filter((r) => requestHasContinueLine(r.output)).length;
    const naturalPieceNone = records.filter((r) => requestHasPieceNoneLine(r.output)).length;
    const naturalWholeNone = records.filter((r) => requestIsWholeNone(r.output)).length;
    const neededCategories: string[] = [];
    if (naturalContinue < MIN_CONTINUE) neededCategories.push("continue");
    if (naturalPieceNone < MIN_PIECE_NONE) neededCategories.push("piece_none");
    if (naturalWholeNone < MIN_WHOLE_NONE) neededCategories.push("no_moves");
    const synthetics = SYNTHETIC_FIXTURES.filter((s) => {
      if (s.tag.startsWith("no_moves")) return neededCategories.includes("no_moves");
      if (s.tag.startsWith("piece_none")) return neededCategories.includes("piece_none");
      if (s.tag.startsWith("continue")) return neededCategories.includes("continue");
      return false;
    });
    for (const s of synthetics) {
      specs.push({
        tier: s.useProductionParams ? "production" : "eval_only",
        source: { boardCompressed: s.board, syntheticTag: s.tag },
        boardCompressed: s.board,
        piece: s.piece,
        level: s.level,
        lines: s.lines,
        reactionTime: s.reactionTime,
        dasCharge: s.dasCharge,
      });
      const record = await runFixture(handler, timeline, specs[specs.length - 1]);
      records.push(record);
    }

    // Coverage quota assertions (hard-fail).
    const pieceCounts: Record<string, number> = {};
    const rtZeroCount = records.filter((r) => r.reactionTime === 0).length;
    const dasCounts: Record<string, number> = {};
    const comboCounts: Record<string, number> = {};
    const continueCount = records.filter((r) => requestHasContinueLine(r.output)).length;
    const pieceNoneCount = records.filter((r) => requestHasPieceNoneLine(r.output)).length;
    const wholeNoneCount = records.filter((r) => requestIsWholeNone(r.output)).length;
    const defaultPlacementCount = records.filter((r) => requestHasDefaultPlacement(r.output)).length;
    for (const r of records) {
      pieceCounts[r.currentPiece] = (pieceCounts[r.currentPiece] || 0) + 1;
      dasCounts[r.dasCharge] = (dasCounts[r.dasCharge] || 0) + 1;
      const key = `rt${r.reactionTime}_das${r.dasCharge}`;
      comboCounts[key] = (comboCounts[key] || 0) + 1;
    }

    const failures: string[] = [];
    for (const piece of PIECES) {
      if ((pieceCounts[piece] || 0) < MIN_PER_PIECE) {
        failures.push(`piece ${piece}: ${pieceCounts[piece] || 0} < ${MIN_PER_PIECE}`);
      }
    }
    if (rtZeroCount < MIN_PER_RT_ZERO) failures.push(`rt=0: ${rtZeroCount} < ${MIN_PER_RT_ZERO}`);
    for (const das of DAS_BUCKETS) {
      if ((dasCounts[das] || 0) < MIN_PER_DAS_BUCKET) {
        failures.push(`dasCharge ${das}: ${dasCounts[das] || 0} < ${MIN_PER_DAS_BUCKET}`);
      }
    }
    for (let c = 0; c < NUM_COMBOS; c++) {
      const combo = comboFor(c);
      const key = `rt${combo.reactionTime}_das${combo.dasCharge}`;
      if ((comboCounts[key] || 0) < MIN_PER_COMBOS) {
        failures.push(`combo ${key}: ${comboCounts[key] || 0} < ${MIN_PER_COMBOS}`);
      }
    }
    if (continueCount < MIN_CONTINUE) failures.push(`continue lines: ${continueCount} < ${MIN_CONTINUE}`);
    if (pieceNoneCount < MIN_PIECE_NONE) failures.push(`per-piece none lines: ${pieceNoneCount} < ${MIN_PIECE_NONE}`);
    if (wholeNoneCount < MIN_WHOLE_NONE) failures.push(`whole-response none: ${wholeNoneCount} < ${MIN_WHOLE_NONE}`);
    if (defaultPlacementCount < MIN_DEFAULT_PLACEMENT) {
      failures.push(`default placements: ${defaultPlacementCount} < ${MIN_DEFAULT_PLACEMENT}`);
    }

    // Emit artifact: header object then one JSON object per fixture.
    const header = {
      type: "precompute_fixtures",
      generator: "src/server/precompute_fixture_generate.ts (npm run precompute_fixture_generate)",
      seed: SEED,
      source: POSITION_LOG,
      timeline,
      engineParams: {
        defaultPlayoutCount: 49,
        defaultPlayoutLength: 2,
        pruningBreadth: 20,
        aggressionLevel: "safe",
        evalOnlyTierPlayouts: "playoutCount=0&playoutLength=0",
      },
      counts: {
        total: records.length,
        evalOnly: records.filter((r) => r.tier === "eval_only").length,
        production: records.filter((r) => r.tier === "production").length,
        synthetic: records.length - specs.filter((s) => !s.source.syntheticTag).length,
      },
      outputFormat: "formatPrecomputeResult string (TS precompute path, verbatim)",
    };
    const body = records.map((r) => JSON.stringify(r)).join("\n");
    fs.writeFileSync(outputPath, JSON.stringify(header) + "\n" + body + "\n");

    console.log(`\nCoverage quota table:`);
    console.log(`  pieces: ${JSON.stringify(pieceCounts)}`);
    console.log(`  reactionTime=0: ${rtZeroCount}`);
    console.log(`  dasCharge buckets: ${JSON.stringify(dasCounts)}`);
    console.log(`  (rt, das) combos: ${JSON.stringify(comboCounts)}`);
    console.log(`  continue lines: ${continueCount}`);
    console.log(`  per-piece none lines: ${pieceNoneCount}`);
    console.log(`  whole-response none: ${wholeNoneCount}`);
    console.log(`  default placement present: ${defaultPlacementCount}`);
    console.log(`  synthetic fixtures appended: ${synthetics.map((s) => s.tag).join(", ") || "none"}`);

    const elapsed = ((Date.now() - totalStart) / 1000).toFixed(1);
    console.log(`\nTier timing: eval_only ${(tierMs.eval_only / 1000).toFixed(1)}s, production ${(tierMs.production / 1000).toFixed(1)}s`);
    console.log(`\nGenerated ${records.length} fixtures in ${elapsed}s -> ${outputPath}`);

    if (failures.length > 0) {
      console.error(`\nCOVERAGE QUOTA FAILURES:\n  ${failures.join("\n  ")}`);
      process.exitCode = 1;
    }
  } catch (err) {
    console.error("Generation failed:", err);
    process.exitCode = 1;
  } finally {
    for (const worker of manager.workers) {
      try { worker.kill(); } catch (e) { /* already dead */ }
    }
    const totalMs = Date.now() - totalStart;
    console.log(`Total wall time: ${(totalMs / 1000).toFixed(1)}s`);
    process.exit(process.exitCode || 0);
  }
});
