/**
 * Golden fixture generator for the C++ precompute pipeline.
 *
 * Drives `build/precompute_cli` over positions sampled from
 * `src/benchmark/data/position_log.jsonl`, and records the exact returned
 * string per fixture into a versioned JSONL artifact verified by
 * `npm run precompute_verify`.
 *
 * Regenerate with: npm run precompute_fixture_generate [-- <outputPath>]
 * Output is deterministic (fixed seed, fixed position log): two runs must be
 * byte-identical. No timestamps are embedded.
 *
 * Two-tier design:
 *   - eval_only tier: every logged position x 2 (rt/das) combos with
 *     playoutCount=0/playoutLength=0 (eval-only lock lookups).
 *   - production tier: every 41st logged position with the default engine
 *     params (playoutCount=49, playoutLength=2, pruningBreadth=20,
 *     aggression=safe).
 *
 * The logged nextPiece is ignored (precompute searches all 7 next pieces); the
 * current piece is deterministically cycled over all 7. Coverage quotas are
 * asserted at the end (hard-fail). Categories unreachable from the position log
 * are covered by synthetic fixtures appended below (see SYNTHETIC_FIXTURES).
 */
const fs = require("fs");
const path = require("path");
const {
  runPrecomputeBatch,
  buildPipeString,
  PIECE_LOOKUP,
} = require("./precompute_verify");

const SEED = 0x5eedc0de;
const DEFAULT_OUTPUT = "src/server/data/precompute_fixtures.jsonl";
const POSITION_LOG = "src/benchmark/data/position_log.jsonl";
const REACTION_TIMES = [0, 8, 20];
const DAS_BUCKETS = [-1, 0, 5, 10, 15, 16];
// (rt, das) combos in a fixed order: index c -> das = DAS_BUCKETS[c/3], rt = RT[c%3]
const NUM_COMBOS = REACTION_TIMES.length * DAS_BUCKETS.length;
const PRODUCTION_STRIDE = 41; // every 41st log position gets a production-tier fixture

const DEFAULT_PLAYOUT_COUNT = 49;
const DEFAULT_PLAYOUT_LENGTH = 2;
const DEFAULT_PRUNING_BREADTH = 20;
const DEFAULT_AGGRESSION_LEVEL = "safe";

// Coverage quota floors (hard-fail if unmet after generation).
const MIN_PER_PIECE = 100;
const MIN_PER_RT_ZERO = 100;
const MIN_PER_DAS_BUCKET = 100;
const MIN_PER_COMBOS = 30; // each (rt, das) combo
const MIN_CONTINUE = 1;
const MIN_PIECE_NONE = 1;
const MIN_WHOLE_NONE = 1;
const MIN_DEFAULT_PLACEMENT = 100;

const BOARD_ENCODING = "abcdefghijklmnopqrstuvwxyzABCDEF";

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
 *   path: flat 6 with a 5-deep shaft carved at col 4.
 */
const TALL_STACK_BOARD = "4,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF,0"; // 16-high flat stack
const LEDGE_BOARD = "14,FFEpEpEpEpEp,0"; // flat 6, 5-deep shaft at col 4
const SYNTHETIC_FIXTURES = [
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

function comboFor(index) {
  const c = ((index % NUM_COMBOS) + NUM_COMBOS) % NUM_COMBOS;
  return {
    reactionTime: REACTION_TIMES[c % REACTION_TIMES.length],
    dasCharge: DAS_BUCKETS[Math.floor(c / REACTION_TIMES.length)],
  };
}

/** Decodes a compressed or raw board string into a 200-char string of '0'/'1'. */
function parseBoard(boardStr) {
  if (boardStr.includes(",")) {
    const parts = boardStr.split(",");
    if (parts.length !== 3) {
      throw new Error(`Invalid compressed board format: ${boardStr}`);
    }
    const encodedRows = parts[1];
    if (!/^\d+$/.test(parts[0]) || !/^\d+$/.test(parts[2]) || encodedRows.length % 2 !== 0) {
      throw new Error(`Invalid compressed board fields: ${boardStr}`);
    }
    const numEmptyRows = parseInt(parts[0], 10);
    const numFullRightWellRows = parseInt(parts[2], 10);

    const rows = [];
    for (let i = 0; i < numEmptyRows; i++) {
      rows.push("0000000000");
    }
    for (let i = 0; i < encodedRows.length; i += 2) {
      const idx1 = BOARD_ENCODING.indexOf(encodedRows.charAt(i));
      const idx2 = BOARD_ENCODING.indexOf(encodedRows.charAt(i + 1));
      if (idx1 === -1 || idx2 === -1) {
        throw new Error(`Invalid compressed board character in: ${boardStr}`);
      }
      rows.push(idx1.toString(2).padStart(5, "0") + idx2.toString(2).padStart(5, "0"));
    }
    for (let i = 0; i < numFullRightWellRows; i++) {
      rows.push("1111111110");
    }
    if (rows.length !== 20) {
      throw new Error(
        `Invalid compressed board. Must contain 20 rows, but found: ${rows.length}`
      );
    }
    return rows.join("");
  }

  if (boardStr.length !== 200 || !/^[01]{200}$/.test(boardStr)) {
    throw new Error(
      `Invalid raw board (expected 200 chars of 0/1, got length ${boardStr.length})`
    );
  }
  return boardStr;
}

function normalizeTimeline(timeline) {
  if (!timeline || !/^[X.\-]+$/.test(timeline)) {
    throw new Error(`Invalid input frame timeline: ${timeline}`);
  }
  return timeline.replace(/-/g, ".");
}

function loadPositions() {
  const raw = fs.readFileSync(POSITION_LOG, "utf8").trim().split("\n");
  const header = JSON.parse(raw[0]);
  if (header.type !== "header") {
    throw new Error("position_log.jsonl missing header line");
  }
  const positions = [];
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
  return { timeline: normalizeTimeline(header.timeline), positions };
}

function buildFixtureSpecs(positions) {
  const specs = [];

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
        piece: PIECE_LOOKUP[(i + k * 3) % 7],
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
      piece: PIECE_LOOKUP[j % 7],
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
// Golden computation via build/precompute_cli
// ---------------------------------------------------------------------------

function requestHasContinueLine(output) {
  return /\n[IOLJTSZ]:continue(\n|$)/.test(output);
}

function requestHasPieceNoneLine(output) {
  return /\n[IOLJTSZ]:none(\n|$)/.test(output);
}

function requestIsWholeNone(output) {
  return output === "none";
}

function requestHasDefaultPlacement(output) {
  return output.startsWith("Default:") && !output.startsWith("Default:N/A");
}

function runFixtureBatch(timeline, specs) {
  const baseRecords = specs.map((spec) => ({
    tier: spec.tier,
    source: spec.source,
    board: parseBoard(spec.boardCompressed),
    currentPiece: spec.piece,
    level: spec.level,
    lines: spec.lines,
    inputFrameTimeline: timeline,
    reactionTime: spec.reactionTime,
    dasCharge: spec.dasCharge,
    playoutCount: spec.tier === "production" ? DEFAULT_PLAYOUT_COUNT : 0,
    playoutLength: spec.tier === "production" ? DEFAULT_PLAYOUT_LENGTH : 0,
    pruningBreadth: DEFAULT_PRUNING_BREADTH,
    aggressionLevel: DEFAULT_AGGRESSION_LEVEL,
  }));

  const outputs = runPrecomputeBatch(baseRecords.map(buildPipeString));
  return baseRecords.map((record, i) => {
    const output = outputs[i];
    if (output.startsWith("Error:")) {
      throw new Error(`Engine error for fixture ${JSON.stringify(specs[i])}: ${output}`);
    }
    return { ...record, output };
  });
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

function main() {
  const outputPath = process.argv[2] || DEFAULT_OUTPUT;
  const totalStart = Date.now();

  try {
    const { timeline, positions } = loadPositions();
    const specs = buildFixtureSpecs(positions);
    console.log(
      `Generating ${specs.length} fixtures (log positions: ${positions.length}, timeline: ${timeline})`
    );

    const records = runFixtureBatch(timeline, specs);

    // Synthetic coverage pass: only append what the natural set missed (the
    // condition is deterministic, so reruns stay byte-identical).
    const naturalContinue = records.filter((r) => requestHasContinueLine(r.output)).length;
    const naturalPieceNone = records.filter((r) => requestHasPieceNoneLine(r.output)).length;
    const naturalWholeNone = records.filter((r) => requestIsWholeNone(r.output)).length;
    const neededCategories = [];
    if (naturalContinue < MIN_CONTINUE) neededCategories.push("continue");
    if (naturalPieceNone < MIN_PIECE_NONE) neededCategories.push("piece_none");
    if (naturalWholeNone < MIN_WHOLE_NONE) neededCategories.push("no_moves");
    const synthetics = SYNTHETIC_FIXTURES.filter((s) => {
      if (s.tag.startsWith("no_moves")) return neededCategories.includes("no_moves");
      if (s.tag.startsWith("piece_none")) return neededCategories.includes("piece_none");
      if (s.tag.startsWith("continue")) return neededCategories.includes("continue");
      return false;
    });
    if (synthetics.length > 0) {
      const syntheticSpecs = synthetics.map((s) => ({
        tier: s.useProductionParams ? "production" : "eval_only",
        source: { boardCompressed: s.board, syntheticTag: s.tag },
        boardCompressed: s.board,
        piece: s.piece,
        level: s.level,
        lines: s.lines,
        reactionTime: s.reactionTime,
        dasCharge: s.dasCharge,
      }));
      records.push(...runFixtureBatch(timeline, syntheticSpecs));
    }

    // Coverage quota assertions (hard-fail).
    const pieceCounts = {};
    const rtZeroCount = records.filter((r) => r.reactionTime === 0).length;
    const dasCounts = {};
    const comboCounts = {};
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

    const failures = [];
    for (const piece of PIECE_LOOKUP) {
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
      generator: "src/server/precompute_fixture_generate.js (npm run precompute_fixture_generate)",
      seed: SEED,
      source: POSITION_LOG,
      timeline,
      engineParams: {
        defaultPlayoutCount: DEFAULT_PLAYOUT_COUNT,
        defaultPlayoutLength: DEFAULT_PLAYOUT_LENGTH,
        pruningBreadth: DEFAULT_PRUNING_BREADTH,
        aggressionLevel: DEFAULT_AGGRESSION_LEVEL,
        evalOnlyTierPlayouts: "playoutCount=0&playoutLength=0",
      },
      counts: {
        total: records.length,
        evalOnly: records.filter((r) => r.tier === "eval_only").length,
        production: records.filter((r) => r.tier === "production").length,
        synthetic: synthetics.length,
      },
      outputFormat: "precompute response string (verbatim)",
    };
    const body = records.map((r) => JSON.stringify(r)).join("\n");
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
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
    console.log(`\nGenerated ${records.length} fixtures in ${elapsed}s -> ${outputPath}`);

    if (failures.length > 0) {
      console.error(`\nCOVERAGE QUOTA FAILURES:\n  ${failures.join("\n  ")}`);
      process.exitCode = 1;
    }
  } catch (err) {
    console.error("Generation failed:", err);
    process.exitCode = 1;
  } finally {
    const totalMs = Date.now() - totalStart;
    console.log(`Total wall time: ${(totalMs / 1000).toFixed(1)}s`);
  }
}

main();
