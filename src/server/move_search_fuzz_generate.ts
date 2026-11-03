import { getPossibleMoves } from "./move_search";
import { INITIAL_PLACEMENT, PieceId } from "./types";
import * as fs from "fs";

/**
 * Fuzz fixture generator for the move search (see docs/DAS_move_search_spec.md §14).
 *
 * Runs the TS move search over a deterministic matrix of boards / pieces / levels /
 * timelines / DAS charges, records the resulting placements as the golden oracle,
 * and emits two artifacts:
 *   - src/server/move_search_fixtures.json           (TS-side oracle, full detail)
 *   - src/cpp_modules/tests/move_search_fixtures.hpp (C++ literal for the doctest runner)
 *
 * Regenerate with: npm run fuzz_generate
 * Output is deterministic (fixed PRNG seed): reruns must produce byte-identical files.
 *
 * Comparison surface (spec §1): the multiset of (rotationIndex, x, y, dasChargeAfter)
 * per fixture, split into basic (inputCost == 0) and tuck (inputCost != 0) entries.
 * inputCost is scaled x10 for integer comparison. Tap-mode fixtures (dasCharge = -1)
 * must have dasChargeAfter == -1 everywhere.
 */

const SEED = 0x5eedc0de;
const PIECES: PieceId[] = ["I", "O", "L", "J", "T", "S", "Z"];
const TIMELINES = ["X..", "X...", "X....."];
const DAS_BOUNDARY_CHARGES = [0, 5, 6, 10, 14, 15, 16];

interface FixtureInput {
  board: string;
  pieceId: PieceId;
  level: number;
  timeline: string;
  dasCharge: number; // -1 = tap mode
  tag: string; // coverage category, for reports
}

interface Entry {
  rot: number;
  x: number;
  y: number;
  dasChargeAfter: number;
  inputCostX10: number;
  inputSequence?: string; // only when SR_FUZZ_INCLUDE_SEQUENCES=1
}

interface FixtureRecord extends FixtureInput {
  entries: Entry[];
}

// ---------------------------------------------------------------------------
// Deterministic PRNG (mulberry32)
// ---------------------------------------------------------------------------

function mulberry32(seed: number): () => number {
  let s = seed;
  return function () {
    s |= 0;
    s = (s + 0x6d2b79f5) | 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// ---------------------------------------------------------------------------
// Board builders (200-char strings, row 0 = top, 10 cols per row)
// ---------------------------------------------------------------------------

function boardFromHeights(heights: number[]): string {
  const rows: string[] = [];
  for (let r = 0; r < 20; r++) {
    let row = "";
    for (let c = 0; c < 10; c++) {
      row += r >= 20 - heights[c] ? "1" : "0";
    }
    rows.push(row);
  }
  return rows.join("");
}

function carveCell(board: string, row: number, col: number): string {
  const idx = row * 10 + col;
  return board.slice(0, idx) + "0" + board.slice(idx + 1);
}

function placeCell(board: string, row: number, col: number): string {
  const idx = row * 10 + col;
  return board.slice(0, idx) + "1" + board.slice(idx + 1);
}

const FLAT_HEIGHTS = [0, 3, 8, 12, 16];

// Ledge/overhang board: flat 6 with a 5-deep shaft carved at col 4, leaving the
// surface cell at (row 14, col 4) hanging over it -- a classic tuck setup.
function ledgeBoard(): string {
  let board = boardFromHeights([6, 6, 6, 6, 6, 6, 6, 6, 6, 6]);
  for (let row = 15; row < 20; row++) {
    board = carveCell(board, row, 4);
  }
  return board;
}

// Tall column at the edge (wall adjacency for wall-charge scenarios).
function wallBoard(): string {
  return boardFromHeights([14, 4, 4, 4, 4, 4, 4, 4, 4, 4]);
}

// Jagged surface (same profile as speed_benchmark's jaggedBoard).
function jaggedBoard(): string {
  return boardFromHeights([3, 5, 2, 4, 6, 1, 3, 5, 2, 4]);
}

const STRUCTURED_BOARDS: Array<[string, string]> = [
  ["flat0", boardFromHeights([0, 0, 0, 0, 0, 0, 0, 0, 0, 0])],
  ["flat3", boardFromHeights([3, 3, 3, 3, 3, 3, 3, 3, 3, 3])],
  ["flat8", boardFromHeights([8, 8, 8, 8, 8, 8, 8, 8, 8, 8])],
  ["flat12", boardFromHeights([12, 12, 12, 12, 12, 12, 12, 12, 12, 12])],
  ["flat16", boardFromHeights([16, 16, 16, 16, 16, 16, 16, 16, 16, 16])],
  ["ledge", ledgeBoard()],
  ["jagged", jaggedBoard()],
  ["wall", wallBoard()],
];

function boardByName(name: string): string {
  for (const [n, b] of STRUCTURED_BOARDS) {
    if (n === name) return b;
  }
  throw new Error("Unknown board: " + name);
}

// ---------------------------------------------------------------------------
// Fixture matrix
// ---------------------------------------------------------------------------

function buildFixtures(rand: () => number): FixtureInput[] {
  const fixtures: FixtureInput[] = [];
  const add = (f: FixtureInput) => fixtures.push(f);

  // Structured tap-mode set: pieces x levels x timelines x (varied boards)
  const tapBoards = ["flat3", "flat8", "ledge", "jagged", "wall"];
  for (const pieceId of PIECES) {
    for (const level of [18, 19, 29]) {
      for (const timeline of TIMELINES) {
        for (const boardName of tapBoards) {
          add({ board: boardByName(boardName), pieceId, level, timeline, dasCharge: -1, tag: "tap_structured" });
        }
      }
    }
  }

  // Structured DAS set: pieces x boundary charges x levels x (tuck + flat boards)
  for (const pieceId of PIECES) {
    for (const dasCharge of DAS_BOUNDARY_CHARGES) {
      for (const level of [18, 19, 29]) {
        for (const boardName of ["flat3", "ledge"]) {
          add({ board: boardByName(boardName), pieceId, level, timeline: "X...", dasCharge, tag: "das_structured" });
        }
      }
    }
  }

  // Random tap set
  for (let i = 0; i < 80; i++) {
    const board = STRUCTURED_BOARDS[Math.floor(rand() * STRUCTURED_BOARDS.length)][1];
    const timeline = TIMELINES[Math.floor(rand() * TIMELINES.length)];
    const level = [18, 19, 29][Math.floor(rand() * 3)];
    add({ board, pieceId: PIECES[Math.floor(rand() * 7)], level, timeline, dasCharge: -1, tag: "tap_random" });
  }

  // Random DAS set (uniform charge 0..16, random board + timeline)
  for (let i = 0; i < 100; i++) {
    const board = STRUCTURED_BOARDS[Math.floor(rand() * STRUCTURED_BOARDS.length)][1];
    const timeline = TIMELINES[Math.floor(rand() * TIMELINES.length)];
    const level = [18, 19, 29][Math.floor(rand() * 3)];
    const dasCharge = Math.floor(rand() * 17);
    add({ board, pieceId: PIECES[Math.floor(rand() * 7)], level, timeline, dasCharge, tag: "das_random" });
  }

  return fixtures;
}

// ---------------------------------------------------------------------------
// Golden computation + property checks
// ---------------------------------------------------------------------------

function boardStrToBoard(boardStr: string): number[][] {
  const board: number[][] = [];
  for (let r = 0; r < 20; r++) {
    board.push(boardStr.slice(r * 10, r * 10 + 10).split("").map(Number));
  }
  return board;
}

function runFixture(f: FixtureInput): Entry[] {
  const board = boardStrToBoard(f.board);
  const possibilities = getPossibleMoves(
    board,
    f.pieceId,
    f.level,
    0,
    0,
    0,
    f.timeline,
    0,
    INITIAL_PLACEMENT,
    f.dasCharge,
    -1
  );

  const useDas = f.dasCharge !== -1;
  const basicLockCells = new Set<string>();
  const entries: Entry[] = [];
  for (const p of possibilities) {
    const parts = p.lockPositionEncoded.split("|");
    const rot = parseInt(parts[0]);
    const x = parseInt(parts[1]);
    const y = parseInt(parts[2]);
    const dasChargeAfter = p.dasChargeAfter;

    // --- Property checks (spec §14) ---
    if (!useDas && dasChargeAfter !== -1) {
      throw new Error(`[${f.tag}] tap fixture produced dasChargeAfter=${dasChargeAfter}`);
    }
    if (useDas && (dasChargeAfter < 0 || dasChargeAfter > 16)) {
      throw new Error(`[${f.tag}] dasChargeAfter=${dasChargeAfter} out of range [0,16]`);
    }
    if (x < -2 || x > 7) {
      throw new Error(`[${f.tag}] lock x=${x} out of range [-2,7]`);
    }
    if (y < -2 || y > 19) {
      throw new Error(`[${f.tag}] lock y=${y} out of range [-2,19]`);
    }
    if (p.inputCost === 0) {
      if (basicLockCells.has(p.lockPositionEncoded)) {
        throw new Error(`[${f.tag}] duplicate basic lock cell ${p.lockPositionEncoded}`);
      }
      basicLockCells.add(p.lockPositionEncoded);
    }

    const entry: Entry = {
      rot,
      x,
      y,
      dasChargeAfter,
      inputCostX10: Math.round(p.inputCost * 10),
      // Sequences are always included: they are the strongest equivalence
      // signal (exact input plans) and the main data product of the DAS search.
      inputSequence: p.inputSequence,
    };
    entries.push(entry);
  }

  entries.sort((a, b) =>
    a.rot - b.rot || a.x - b.x || a.y - b.y ||
    a.dasChargeAfter - b.dasChargeAfter || a.inputCostX10 - b.inputCostX10
  );
  return entries;
}

// ---------------------------------------------------------------------------
// Artifact emission
// ---------------------------------------------------------------------------

// Pack one expected entry into a single int32 (18 bits used):
//   bits 0-1:  rot            (0..3)
//   bits 2-5:  x + 8          (-2..7  -> 0..15)
//   bits 6-10: y + 2          (-2..19 -> 0..21)
//   bits 11-15: dasChargeAfter + 1  (-1..16 -> 0..17)
//   bits 16-17: inputCostX10 + 3    (-3..0  -> 0..3)
function packEntry(e: Entry): number {
  return (
    (e.rot & 3) |
    ((e.x + 8) & 15) << 2 |
    ((e.y + 2) & 31) << 6 |
    ((e.dasChargeAfter + 1) & 31) << 11 |
    ((e.inputCostX10 + 3) & 3) << 16
  );
}

function unpackEntry(p: number): Entry {
  return {
    rot: p & 3,
    x: ((p >> 2) & 15) - 8,
    y: ((p >> 6) & 31) - 2,
    dasChargeAfter: ((p >> 11) & 31) - 1,
    inputCostX10: ((p >> 16) & 3) - 3,
  };
}

function emitCppHeader(records: FixtureRecord[]): string {
  const boardPool: string[] = [];
  const boardIdx = new Map<string, number>();
  const timelinePool: string[] = [];
  const timelineIdx = new Map<string, number>();
  const intern = (pool: string[], map: Map<string, number>, value: string): number => {
    if (!map.has(value)) {
      map.set(value, pool.length);
      pool.push(value);
    }
    return map.get(value)!;
  };

  const packedPool: number[] = [];
  const fixtures: string[] = [];
  for (const rec of records) {
    const bIdx = intern(boardPool, boardIdx, rec.board);
    const tIdx = intern(timelinePool, timelineIdx, rec.timeline);
    const offset = packedPool.length;
    for (const e of rec.entries) {
      packedPool.push(packEntry(e));
    }
    const pieceIndex = PIECES.indexOf(rec.pieceId);
    fixtures.push(
      `  {${bIdx}, ${pieceIndex}, ${rec.level}, ${tIdx}, ${rec.dasCharge}, ${offset}, ${rec.entries.length}}, // ${rec.tag}`
    );
  }

  // Round-trip check: a packing bug must never silently corrupt the oracle.
  let poolCursor = 0;
  for (const rec of records) {
    for (const e of rec.entries) {
      const u = unpackEntry(packedPool[poolCursor++]);
      if (u.rot !== e.rot || u.x !== e.x || u.y !== e.y ||
          u.dasChargeAfter !== e.dasChargeAfter || u.inputCostX10 !== e.inputCostX10) {
        throw new Error(`Entry pack round-trip mismatch: ${JSON.stringify(e)} -> ${JSON.stringify(u)}`);
      }
    }
  }

  const boardLines = boardPool.map((b) => `  "${b}",`).join("\n");
  const timelineLines = timelinePool.map((t) => `  "${t}",`).join("\n");
  const entryLines: string[] = [];
  const ENTRIES_PER_LINE = 10;
  for (let i = 0; i < packedPool.length; i += ENTRIES_PER_LINE) {
    entryLines.push("  " + packedPool.slice(i, i + ENTRIES_PER_LINE).join(", ") + ",");
  }

  return `// GENERATED by src/server/move_search_fuzz_generate.ts -- do not edit by hand.
// Regenerate with: npm run fuzz_generate  (seed ${SEED})
// Golden oracle for the move search fuzz harness (docs/DAS_move_search_spec.md §14).
// FIXTURE_ENTRY_POOL holds one packed int32 per expected entry (see packEntry in
// the generator for the bit layout); entries are sorted by
// (rot, x, y, dasChargeAfter, inputCostX10). Values come from the TS
// implementation. dasCharge -1 = tap mode.
#ifndef SR_MOVE_SEARCH_FIXTURES
#define SR_MOVE_SEARCH_FIXTURES

struct MoveSearchFixture {
  int boardIdx;    // FIXTURE_BOARD_POOL index
  int pieceIndex;  // PIECE_LIST index (I, O, L, J, T, S, Z)
  int level;
  int timelineIdx; // FIXTURE_TIMELINE_POOL index
  int dasCharge;   // -1 = tap mode
  int entryOffset; // offset into FIXTURE_ENTRY_POOL (in entries, not ints)
  int entryCount;
};

static const char *FIXTURE_BOARD_POOL[] = {
${boardLines}
};

static const char *FIXTURE_TIMELINE_POOL[] = {
${timelineLines}
};

static const int FIXTURE_ENTRY_POOL[] = {
${entryLines.join("\n")}
};

static const MoveSearchFixture MOVE_SEARCH_FIXTURES[] = {
${fixtures.join("\n")}
};

static const int MOVE_SEARCH_FIXTURE_COUNT = ${records.length};

#endif // SR_MOVE_SEARCH_FIXTURES
`;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

const rand = mulberry32(SEED);
const inputs = buildFixtures(rand);
const records: FixtureRecord[] = [];
for (const f of inputs) {
  records.push({ ...f, entries: runFixture(f) });
}

const jsonPath = "src/server/move_search_fixtures.json";
const headerPath = "src/cpp_modules/tests/move_search_fixtures.hpp";
fs.writeFileSync(jsonPath, JSON.stringify({ seed: SEED, fixtures: records }));
fs.writeFileSync(headerPath, emitCppHeader(records));

const tapCount = records.filter((r) => r.dasCharge === -1).length;
const dasCount = records.length - tapCount;
const totalEntries = records.reduce((sum, r) => sum + r.entries.length, 0);
console.log(`Generated ${records.length} fixtures (${tapCount} tap, ${dasCount} DAS), ${totalEntries} expected entries.`);
console.log(`Wrote ${jsonPath} and ${headerPath}.`);
