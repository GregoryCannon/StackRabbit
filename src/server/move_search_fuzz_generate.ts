import { getPossibleMoves } from "./move_search";
import { AdjustmentState, DasButtonHeld, INITIAL_PLACEMENT, PieceId, Possibility } from "./types";
import * as fs from "fs";

/**
 * Fuzz fixture generator for the move search (see TESTING.md).
 *
 * TODO(Phase 5 cutover): this generator runs the TS engine and cannot outlive
 * it; it is deleted at cutover.
 *
 * Runs the move search over a deterministic matrix of boards / pieces / levels /
 * timelines / DAS charges, records the resulting placements as the golden oracle,
 * and emits two artifacts:
 *   - src/server/move_search_fixtures.json           (oracle, full detail)
 *   - src/cpp_modules/tests/move_search_fixtures.hpp (C++ literal for the doctest runner)
 *
 * Regenerate with: npm run fuzz_generate
 * Output is deterministic (fixed PRNG seed): reruns must produce byte-identical files.
 *
 * Comparison surface: the FULL entry list per fixture in result order (the
 * order getPossibleMoves emits: basic entries then tuck/spin entries,
 * post-dedupe). Both the doctest suite and fuzz_verify_sequences compare
 * order-exactly -- the result order is load-bearing downstream
 * (possibleMoves[0] is the default placement, and the phantom sort is stable
 * over this order), so an order divergence is a real bug, not noise.
 * inputCost is scaled x10 for integer comparison. Tap-mode fixtures
 * (dasCharge = -1) must have dasChargeAfter == -1 everywhere.
 *
 * Adjustment-mode fixtures are appended in a separate section: each one starts
 * from the adjTimeSimState snapshot of an initial spawn search run with a
 * (the phantom-placement pipeline), then runs the adjustment move search
 * (no reactionTime). Existing spawn fixtures keep their indexes and bytes; the
 * adjustment fixtures live in their own JSON key and C++ pools.
 *
 * Reaction-time fixtures form a third section: spawn searches run WITH a
 * reaction time and the FULL possibility list is recorded in result order,
 * each possibility carrying its walk's adjTimeSimState snapshot (or null) --
 * the result-to-snapshot mapping is the behavior under test.
 */

const SEED = 0x5eedc0de;
// Independent seed for the adjustment section so its sampling never perturbs
// the spawn-fixture PRNG stream (and vice versa).
const ADJUSTMENT_SEED = 0xada07e5d;
// Independent seed for the reaction-time section (same isolation pattern).
const REACTION_TIME_SEED = 0x7ea71e07;
const PIECES: PieceId[] = ["I", "O", "L", "J", "T", "S", "Z"];
const TIMELINES = ["X..", "X...", "X....."];
const DAS_BOUNDARY_CHARGES = [0, 5, 6, 10, 14, 15, 16];
// Reaction times for adjustment-entry fixtures. 13 is the only value <= 20 at
// which a tap-mode search can hold a DAS button at the reaction-time frame for
// ANY of the
// three timelines (rt-1 must be an ARR input frame: 12 % 3 == 12 % 4 == 12 % 6 == 0).
const ADJUSTMENT_RT_VALUES = [8, 13, 20];
// Reaction times for the reaction-time snapshot fixtures: 0 (never snapshots),
// 1 (earliest possible frame), small mid-travel values, values near and past
// typical lock frames (level 18 falls take ~57 frames on a flat-3 board,
// ~15 on flat-16; level 29 falls are ~4x faster still).
const REACTION_TIME_VALUES = [0, 1, 2, 5, 8, 13, 20, 40];

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

/** Mid-air entry parameters + the AdjustmentState, exactly as recorded from the
 * initial search's adjTimeSimState (see _calculatePhantomPlacements). */
interface AdjustmentFixtureInput {
  board: string;
  pieceId: PieceId;
  level: number;
  timeline: string;
  dasCharge: number; // adjustment search's incoming charge (-1 = tap mode)
  reactionTime: number; // reaction time of the initial search that produced this state
  framesAlreadyElapsed: number;
  existingXOffset: number;
  existingYOffset: number;
  existingRotation: number;
  isArrContinued: boolean;
  dasButtonHeld: string; // "LEFT" | "RIGHT" | "NONE"
  preAdjInputSequence: string;
  tag: string;
}

interface AdjustmentFixtureRecord extends AdjustmentFixtureInput {
  entries: Entry[];
}

/** The adjTimeSimState snapshot of one result's goal walk, or null when the
 * walk's piece locked before the reaction-time frame or the result came from
 * a sim state registered before the reaction-time frame. */
interface AdjTimeCaptureRecord {
  frameIndex: number;
  x: number;
  y: number;
  rotationIndex: number;
  dasCharge: number;
  isArrContinued: boolean;
  dasButtonHeld: string; // "LEFT" | "RIGHT" | "NONE"
  preAdjInputSequence: string;
}

interface ReactionTimeEntry {
  rot: number;
  x: number;
  y: number;
  dasChargeAfter: number;
  inputCostX10: number;
  inputSequence: string;
  adjCapture: AdjTimeCaptureRecord | null;
}

interface ReactionTimeFixtureInput {
  board: string;
  pieceId: PieceId;
  level: number;
  timeline: string;
  dasCharge: number; // -1 = tap mode
  reactionTime: number;
  tag: string;
}

interface ReactionTimeFixtureRecord extends ReactionTimeFixtureInput {
  // The FULL possibility list in result order (no sorting) -- the
  // result-to-snapshot mapping is part of the golden.
  entries: ReactionTimeEntry[];
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

// Jagged overhang board: the only structured board whose tap-mode search
// produces tuck/spin/spin-tuck possibilities (exercises dfs.tryInput's tap
// path -- the flat/ledge/wall tap boards yield none). Deliberately NOT in
// STRUCTURED_BOARDS: adding an entry there would shift every random-set pick
// (rand() * STRUCTURED_BOARDS.length) and churn existing fixtures.
const TUCK_COVERAGE_BOARD = boardFromHeights([10, 0, 11, 4, 16, 17, 17, 7, 14, 11]);

// Tall-stack boards: stacks reaching rows 0-6, where pieces must lock in the
// top rows. Flat near-topped lids (flat17/flat18: only the top 3/2 rows open),
// an edge wall with a low shelf (tallWall), a shaft carved through a near-top
// lid (tallShaft), and a topped-out lid (flat19: every placement collides at
// spawn -- empty result). Deliberately NOT in STRUCTURED_BOARDS (same reason
// as TUCK_COVERAGE_BOARD): these boards get their own appended sections so
// every existing fixture keeps its index and the random draws stay untouched.
const TALL_BOARDS: Array<[string, string]> = [
  ["flat17", boardFromHeights([17, 17, 17, 17, 17, 17, 17, 17, 17, 17])],
  ["flat18", boardFromHeights([18, 18, 18, 18, 18, 18, 18, 18, 18, 18])],
  ["flat19", boardFromHeights([19, 19, 19, 19, 19, 19, 19, 19, 19, 19])],
  ["tallWall", boardFromHeights([18, 13, 13, 13, 13, 13, 13, 13, 13, 13])],
  ["tallShaft", boardFromHeights([17, 17, 17, 17, 12, 17, 17, 17, 17, 17])],
];

function tallBoardByName(name: string): string {
  for (const [n, b] of TALL_BOARDS) {
    if (n === name) return b;
  }
  throw new Error("Unknown tall board: " + name);
}

const TALL_TOPPED_OUT_BOARD = tallBoardByName("flat19");

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

  // Tap tuck/spin-coverage set: the overhang board above makes the tuck/spin DFS
  // reachable in tap mode. Appended last so every pre-existing fixture keeps its
  // index and the packed entry pool stays prefix-stable. NB: the doctest tap
  // guard runs fastMoveSearch (the hot-path search, which has no spin search),
  // so it skips these; the fullMoveSearch tap path is what they verify.
  for (const pieceId of PIECES) {
    for (const level of [18, 19, 29]) {
      for (const timeline of TIMELINES) {
        add({ board: TUCK_COVERAGE_BOARD, pieceId, level, timeline, dasCharge: -1, tag: "tap_tuck" });
      }
    }
  }

  // Tall-stack structured sets, appended after every pre-existing section (same
  // index-stability pattern): pieces locking in the top rows on near-topped
  // boards, in both modes. The one-row-open lid (flat19) must come back empty
  // for every piece except the I (whose horizontal form hangs in the open top
  // row) -- hard-asserted after generation.
  for (const pieceId of PIECES) {
    for (const level of [18, 19, 29]) {
      for (const timeline of TIMELINES) {
        for (const boardName of ["flat17", "flat18", "flat19", "tallWall", "tallShaft"]) {
          add({ board: tallBoardByName(boardName), pieceId, level, timeline, dasCharge: -1, tag: "tap_tall" });
        }
      }
    }
  }
  for (const pieceId of PIECES) {
    for (const dasCharge of DAS_BOUNDARY_CHARGES) {
      for (const level of [18, 19, 29]) {
        for (const boardName of ["flat17", "flat18", "flat19", "tallShaft"]) {
          add({ board: tallBoardByName(boardName), pieceId, level, timeline: "X...", dasCharge, tag: "das_tall" });
        }
      }
    }
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
  return entriesFromPossibilities(f.tag, f.dasCharge, possibilities);
}

/** Shared entry extraction + property checks for spawn and adjustment fixtures.
 * Entries keep the search's result order (basic entries then tuck/spin
 * entries, post-dedupe): the comparison is order-exact downstream, and the
 * result order is what the phantom pipeline consumes. */
function entriesFromPossibilities(
  tag: string,
  dasCharge: number,
  possibilities: Possibility[]
): Entry[] {
  const useDas = dasCharge !== -1;
  const basicLockCells = new Set<string>();
  const entries: Entry[] = [];
  for (const p of possibilities) {
    const parts = p.lockPositionEncoded.split("|");
    const rot = parseInt(parts[0]);
    const x = parseInt(parts[1]);
    const y = parseInt(parts[2]);
    const dasChargeAfter = p.dasChargeAfter;

    // --- Property checks ---
    if (!useDas && dasChargeAfter !== -1) {
      throw new Error(`[${tag}] tap fixture produced dasChargeAfter=${dasChargeAfter}`);
    }
    if (useDas && (dasChargeAfter < 0 || dasChargeAfter > 16)) {
      throw new Error(`[${tag}] dasChargeAfter=${dasChargeAfter} out of range [0,16]`);
    }
    if (x < -2 || x > 7) {
      throw new Error(`[${tag}] lock x=${x} out of range [-2,7]`);
    }
    if (y < -2 || y > 19) {
      throw new Error(`[${tag}] lock y=${y} out of range [-2,19]`);
    }
    if (p.inputCost === 0) {
      if (basicLockCells.has(p.lockPositionEncoded)) {
        throw new Error(`[${tag}] duplicate basic lock cell ${p.lockPositionEncoded}`);
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

  return entries;
}

// ---------------------------------------------------------------------------
// Adjustment-mode fixtures
// ---------------------------------------------------------------------------

// DasButtonHeld enum order is LEFT=0, RIGHT=1, NONE=2 (const enum, so the
// runtime object does not exist and reverse mapping must be spelled out).
const DAS_BUTTON_HELD_NAMES = ["LEFT", "RIGHT", "NONE"];
// C++ FullMoveSearchContext encoding for the same values.
const DAS_BUTTON_HELD_CODES: Record<string, number> = { LEFT: -1, RIGHT: 1, NONE: 0 };

/** countInputs for phantom sorting (operates on the RELATIVE placement). */
function countInputsForPhantomSort(placement: [number, number, number]): number {
  if (placement[0] == 3) {
    return 1 + Math.abs(placement[1]);
  }
  return placement[0] + Math.abs(placement[1]);
}

interface AdjustmentInitialCondition {
  board: string;
  pieceId: PieceId;
  level: number;
  timeline: string;
  dasCharge: number;
  reactionTime: number;
}

function buildAdjustmentInitialConditions(): AdjustmentInitialCondition[] {
  const ics: AdjustmentInitialCondition[] = [];
  // Self-contained PRNG so the random pass never perturbs (or depends on) the
  // spawn-fixture stream.
  const adjRand = mulberry32(ADJUSTMENT_SEED);

  // Structured pass: rt=8 (mid-fall states) at level 18 across the charge
  // boundaries that drive dasWillReset (<= 5) and hold/wall-charge behavior;
  // rt=13 at level 18 (slow enough gravity that far-shift goals are still
  // moving at the reaction-time frame -- the only reachable tap-mode
  // dasButtonHeld-at-reaction-time setup in the rt set, since rt-1 must be an ARR
  // input frame) and level 29 (kill-screen); and rt=20 with the slowest ARR
  // timeline at level 19.
  for (const pieceId of PIECES) {
    for (const dasCharge of [-1, 0, 5, 16]) {
      for (const boardName of ["flat3", "ledge", "jagged", "wall"]) {
        ics.push({ board: boardByName(boardName), pieceId, level: 18, timeline: "X...", dasCharge, reactionTime: 8 });
      }
    }
    for (const boardName of ["flat3", "jagged"]) {
      ics.push({ board: boardByName(boardName), pieceId, level: 18, timeline: "X...", dasCharge: -1, reactionTime: 13 });
    }
    for (const dasCharge of [-1, 10]) {
      for (const boardName of ["flat8", "wall"]) {
        ics.push({ board: boardByName(boardName), pieceId, level: 29, timeline: "X..", dasCharge, reactionTime: 13 });
      }
    }
    for (const dasCharge of [-1, 5]) {
      ics.push({ board: boardByName("ledge"), pieceId, level: 19, timeline: "X.....", dasCharge, reactionTime: 20 });
    }
  }

  // Random pass: broad state variety (pieces/levels/timelines/charges/rts).
  for (let i = 0; i < 80; i++) {
    const board = STRUCTURED_BOARDS[Math.floor(adjRand() * STRUCTURED_BOARDS.length)][1];
    const timeline = TIMELINES[Math.floor(adjRand() * TIMELINES.length)];
    const level = [18, 19, 29][Math.floor(adjRand() * 3)];
    const dasCharge = Math.floor(adjRand() * 17) - 1; // -1..16
    const reactionTime = ADJUSTMENT_RT_VALUES[Math.floor(adjRand() * ADJUSTMENT_RT_VALUES.length)];
    ics.push({ board, pieceId: PIECES[Math.floor(adjRand() * 7)], level, timeline, dasCharge, reactionTime });
  }

  return ics;
}

/**
 * Runs each initial condition through the spawn search WITH a reaction
 * time, derives the phantom list exactly like PreComputeManager
 * ._calculatePhantomPlacements (stable countInputs sort, prefix dedupe), and
 * selects phantoms that captured an adjTimeSimState as adjustment fixture
 * inputs. The adjustment search itself runs in runAdjustmentFixture (with
 * reactionTime disabled, like production's _precompileAdjustmentMoves).
 *
 * Selection is deterministic and coverage-driven: always the first phantom
 * (minimum inputs), then the first phantom matching each still-uncovered
 * (isArrContinued, dasButtonHeld, rotation-class) bucket in a fixed
 * rare-category-first priority order (BUCKET_PRIORITY below), up to the
 * per-condition cap. Naive first/last/random sampling provably skews here:
 * countInputs ties keep goal-loop order, so the last phantom is always the
 * maximal-x goal (RIGHT holds) and the first is the no-shift goal (NONE),
 * starving LEFT holds; the multi-rotation goals that alone reach DAS
 * isArrContinued=true and existingRotation=3 sort last of all.
 */
const MAX_PHANTOMS_PER_CONDITION = 4;

function buildAdjustmentFixtures(): AdjustmentFixtureInput[] {
  const inputs: AdjustmentFixtureInput[] = [];

  for (const ic of buildAdjustmentInitialConditions()) {
    const board = boardStrToBoard(ic.board);
    const possibilities = getPossibleMoves(
      board,
      ic.pieceId,
      ic.level,
      0,
      0,
      0,
      ic.timeline,
      0,
      INITIAL_PLACEMENT,
      ic.dasCharge,
      ic.reactionTime
    );
    if (possibilities.length === 0) {
      continue;
    }

    // Mirror _calculatePhantomPlacements: stable sort by countInputs, then
    // dedupe by the first reactionTime chars of inputSequence (first wins).
    const sorted = [...possibilities].sort(
      (a, b) => countInputsForPhantomSort(a.placement) - countInputsForPhantomSort(b.placement)
    );
    const seenPrefixes = new Set<string>();
    const phantoms: Possibility[] = [];
    for (const p of sorted) {
      const prefix = p.inputSequence.substr(0, ic.reactionTime);
      if (seenPrefixes.has(prefix)) {
        continue;
      }
      seenPrefixes.add(prefix);
      phantoms.push(p);
    }

    // Phantoms whose piece locked before reaction time have no adjTimeSimState.
    const withAdjState = phantoms.filter((p) => p.adjTimeSimState);
    if (withAdjState.length === 0) {
      continue;
    }

    const picked: Possibility[] = [withAdjState[0]];
    for (const bucket of BUCKET_PRIORITY) {
      if (picked.length >= MAX_PHANTOMS_PER_CONDITION) {
        break;
      }
      const member = withAdjState.find(
        (p) => !picked.includes(p) && bucketMatches(bucket, bucketOf(p))
      );
      if (member) {
        picked.push(member);
      }
    }

    const initialY = ic.pieceId === "I" ? -2 : -1;
    for (const p of picked) {
      const at = p.adjTimeSimState!;
      inputs.push({
        board: ic.board,
        pieceId: ic.pieceId,
        level: ic.level,
        timeline: ic.timeline,
        dasCharge: at.dasCharge,
        reactionTime: ic.reactionTime,
        framesAlreadyElapsed: at.frameIndex,
        existingXOffset: at.x - 3,
        existingYOffset: at.y - initialY,
        existingRotation: at.rotationIndex,
        isArrContinued: at.adjustmentState.isArrContinued,
        dasButtonHeld: DAS_BUTTON_HELD_NAMES[at.adjustmentState.dasButtonHeld],
        preAdjInputSequence: at.adjustmentState.preAdjInputSequence,
        tag: "adjustment",
      });
    }
  }

  return inputs;
}

/**
 * Synthetic adjustment entries one row past the board's bottom edge: a piece
 * already past the bottom (existingYOffset >= 20) has no legal moves -- the
 * search returns empty before any board interaction. The mid-air pipeline can
 * produce existingYOffset = 20 for non-I pieces (a piece still airborne at
 * row 19 when the reaction-time frame arrives), so synthesize the entry state
 * directly and pin the empty-result path for every piece in both modes.
 */
function buildPastBottomAdjustmentFixtures(): AdjustmentFixtureInput[] {
  const inputs: AdjustmentFixtureInput[] = [];
  for (const pieceId of PIECES) {
    for (const dasCharge of [-1, 0]) {
      inputs.push({
        board: boardByName("flat3"),
        pieceId,
        level: 18,
        timeline: "X...",
        dasCharge,
        reactionTime: 13,
        framesAlreadyElapsed: 13,
        existingXOffset: 0,
        existingYOffset: 20,
        existingRotation: 0,
        isArrContinued: false,
        dasButtonHeld: "NONE",
        preAdjInputSequence: "",
        tag: "adjustment_past_bottom",
      });
    }
  }
  return inputs;
}

function bucketOf(p: Possibility): string {
  const at = p.adjTimeSimState!;
  const rotClass = at.rotationIndex === 3 ? "R3" : at.rotationIndex !== 0 ? "RN" : "R0";
  return `${at.adjustmentState.isArrContinued ? "T" : "F"}|${DAS_BUTTON_HELD_NAMES[at.adjustmentState.dasButtonHeld]}|${rotClass}`;
}

// Rare-category-first fill order ("any" = any rotation class).
//
// arrContinued=true in DAS mode requires every ARR frame before the capture to
// carry a real input, which only multi-rotation goals achieve -- and those sort
// LATE by countInputs, so list-order discovery starves them; the T|* buckets
// come first to counteract that. Goal-3 rotations (R3) sort last of all
// (relative placement rotation 3 = countInputs 3+|x|), so they get their own
// early slots. F|LEFT comes before F|RIGHT because countInputs ties keep goal
// loop order, making positive-x goals the default survivors of any tail pick.
const BUCKET_PRIORITY = [
  "T|LEFT|any", "T|RIGHT|any", "T|NONE|any",
  "F|LEFT|R3", "F|RIGHT|R3", "F|NONE|R3",
  "F|LEFT|RN", "F|RIGHT|RN", "F|NONE|RN",
  "F|LEFT|R0", "F|RIGHT|R0", "F|NONE|R0",
];

function bucketMatches(bucket: string, phantomBucket: string): boolean {
  const [arrC, held, rotClass] = bucket.split("|");
  const [pArrC, pHeld, pRotClass] = phantomBucket.split("|");
  return arrC === pArrC && held === pHeld && (rotClass === "any" || rotClass === pRotClass);
}

function runAdjustmentFixture(f: AdjustmentFixtureInput): Entry[] {
  const board = boardStrToBoard(f.board);
  const heldIndex = DAS_BUTTON_HELD_NAMES.indexOf(f.dasButtonHeld);
  if (heldIndex < 0) {
    throw new Error("Unknown dasButtonHeld name: " + f.dasButtonHeld);
  }
  const adjustmentState: AdjustmentState = {
    isAdjustment: true,
    isArrContinued: f.isArrContinued,
    dasButtonHeld: heldIndex as DasButtonHeld,
    preAdjInputSequence: f.preAdjInputSequence,
  };
  // The adjustment call passes NO reactionTime (defaults to -1): no adj-time
  // waiting, no capture.
  const possibilities = getPossibleMoves(
    board,
    f.pieceId,
    f.level,
    f.existingXOffset,
    f.existingYOffset,
    f.framesAlreadyElapsed,
    f.timeline,
    f.existingRotation,
    adjustmentState,
    f.dasCharge,
    -1
  );
  return entriesFromPossibilities(f.tag, f.dasCharge, possibilities);
}

// ---------------------------------------------------------------------------
// Reaction-time fixtures (spawn entry, capture behavior)
// ---------------------------------------------------------------------------

/**
 * Initial conditions for the reaction-time snapshot fixtures. The structured
 * pass pins the coverage axes per piece:
 *   - rt=0: the no-snapshot path (reaction-time frame never reached; walks exit as
 *     soon as the goal is reached and DAS charging is done).
 *   - rt=1/2: capture on the earliest frames, mid-travel (before most goals
 *     are reached -- every registered state of such a walk carries the
 *     capture).
 *   - rt=8/20 at level 18 (gravity 3): capture mid-fall at or after
 *     goal-reach for most goals.
 *   - rt=60 on the flat-3 board: around/past typical lock frames, so walks
 *     split between capturing and locking first.
 *   - level 29 tall boards (gravity 1, locks within ~5-15 frames): rt=3/13
 *     straddle the lock frames walk-by-walk.
 *   - the tuck-coverage board: the dfs novel-keeping comparison reads capture
 *     presence, so captures on a tuck-heavy board exercise that path.
 * The random pass broadens boards/levels/timelines/charges/reaction times,
 * including a board mix where some walks capture while others lock too early.
 */
const RT_TUCK_BOARD_LEVEL = 19;

function buildReactionTimeConditions(): ReactionTimeFixtureInput[] {
  const fixtures: ReactionTimeFixtureInput[] = [];
  const rtRand = mulberry32(REACTION_TIME_SEED);

  for (const pieceId of PIECES) {
    for (const dasCharge of [-1, 16]) {
      for (const rt of [0, 2, 8, 20]) {
        fixtures.push({
          board: boardByName("flat3"),
          pieceId,
          level: 18,
          timeline: "X...",
          dasCharge,
          reactionTime: rt,
          tag: "rt_flat3",
        });
      }
    }
    for (const rt of [8, 60]) {
      fixtures.push({
        board: boardByName("ledge"),
        pieceId,
        level: 18,
        timeline: "X...",
        dasCharge: -1,
        reactionTime: rt,
        tag: "rt_ledge",
      });
    }
    fixtures.push({
      board: boardByName("jagged"),
      pieceId,
      level: 18,
      timeline: "X...",
      dasCharge: 5,
      reactionTime: 8,
      tag: "rt_jagged",
    });
    for (const rt of [3, 13]) {
      fixtures.push({
        board: boardByName("flat16"),
        pieceId,
        level: 29,
        timeline: "X..",
        dasCharge: -1,
        reactionTime: rt,
        tag: "rt_killscreen_tall",
      });
    }
    fixtures.push({
      board: boardByName("wall"),
      pieceId,
      level: 29,
      timeline: "X..",
      dasCharge: 0,
      reactionTime: 13,
      tag: "rt_killscreen_wall",
    });
    for (const dasCharge of [-1, 10]) {
      for (const rt of [6, 20]) {
        fixtures.push({
          board: TUCK_COVERAGE_BOARD,
          pieceId,
          level: RT_TUCK_BOARD_LEVEL,
          timeline: "X...",
          dasCharge,
          reactionTime: rt,
          tag: "rt_tuck_board",
        });
      }
    }
  }

  // Random pass: TUCK_COVERAGE_BOARD is appended to the pick list LOCALLY (it
  // must never join STRUCTURED_BOARDS -- that would shift the other sections'
  // random draws).
  const rtBoards = [...STRUCTURED_BOARDS.map(([, b]) => b), TUCK_COVERAGE_BOARD];
  for (let i = 0; i < 70; i++) {
    const board = rtBoards[Math.floor(rtRand() * rtBoards.length)];
    const timeline = TIMELINES[Math.floor(rtRand() * TIMELINES.length)];
    const level = [18, 19, 29][Math.floor(rtRand() * 3)];
    const dasCharge = Math.floor(rtRand() * 17) - 1; // -1..16
    const reactionTime = REACTION_TIME_VALUES[Math.floor(rtRand() * REACTION_TIME_VALUES.length)];
    fixtures.push({
      board,
      pieceId: PIECES[Math.floor(rtRand() * 7)],
      level,
      timeline,
      dasCharge,
      reactionTime,
      tag: "rt_random",
    });
  }

  return fixtures;
}

function runReactionTimeFixture(f: ReactionTimeFixtureInput): ReactionTimeEntry[] {
  const board = boardStrToBoard(f.board);
  const useDas = f.dasCharge !== -1;
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
    f.reactionTime
  );

  const entries: ReactionTimeEntry[] = [];
  for (const p of possibilities) {
    const parts = p.lockPositionEncoded.split("|");
    const rot = parseInt(parts[0]);
    const x = parseInt(parts[1]);
    const y = parseInt(parts[2]);

    // --- Property checks (same core ranges as the shared entry checks) ---
    if (!useDas && p.dasChargeAfter !== -1) {
      throw new Error(`[${f.tag}] tap fixture produced dasChargeAfter=${p.dasChargeAfter}`);
    }
    if (useDas && (p.dasChargeAfter < 0 || p.dasChargeAfter > 16)) {
      throw new Error(`[${f.tag}] dasChargeAfter=${p.dasChargeAfter} out of range [0,16]`);
    }
    if (x < -2 || x > 7 || y < -2 || y > 19) {
      throw new Error(`[${f.tag}] lock (${x},${y}) out of range`);
    }

    const at = p.adjTimeSimState;
    if (at) {
      if (at.frameIndex !== f.reactionTime) {
        throw new Error(
          `[${f.tag}] capture frameIndex=${at.frameIndex} != reactionTime=${f.reactionTime}`
        );
      }
      if (at.x < -2 || at.x > 7 || at.y < -2 || at.y > 19) {
        throw new Error(`[${f.tag}] capture position (${at.x},${at.y}) out of range`);
      }
      if (at.adjustmentState.isAdjustment !== true) {
        throw new Error(`[${f.tag}] capture adjustmentState.isAdjustment is not true`);
      }
      if (useDas && (at.dasCharge < 0 || at.dasCharge > 16)) {
        throw new Error(`[${f.tag}] capture dasCharge=${at.dasCharge} out of range [0,16]`);
      }
      if (!useDas && at.dasCharge !== -1) {
        throw new Error(`[${f.tag}] tap capture dasCharge=${at.dasCharge} != -1`);
      }
    }

    entries.push({
      rot,
      x,
      y,
      dasChargeAfter: p.dasChargeAfter,
      inputCostX10: Math.round(p.inputCost * 10),
      inputSequence: p.inputSequence,
      adjCapture: at
        ? {
            frameIndex: at.frameIndex,
            x: at.x,
            y: at.y,
            rotationIndex: at.rotationIndex,
            dasCharge: at.dasCharge,
            isArrContinued: at.adjustmentState.isArrContinued,
            dasButtonHeld: DAS_BUTTON_HELD_NAMES[at.adjustmentState.dasButtonHeld],
            preAdjInputSequence: at.adjustmentState.preAdjInputSequence,
          }
        : null,
    });
  }
  // Result order preserved: the capture mapping is part of the golden.
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

function emitCppHeader(
  records: FixtureRecord[],
  adjustmentRecords: AdjustmentFixtureRecord[],
  reactionTimeRecords: ReactionTimeFixtureRecord[]
): string {
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

  // --- Adjustment section: reuses the pools above (lookup only; a missing
  // board/timeline would shift nothing but must be added consciously). ---
  const prefixPool: string[] = [];
  const prefixIdx = new Map<string, number>();
  const adjPackedPool: number[] = [];
  const adjFixtures: string[] = [];
  for (const rec of adjustmentRecords) {
    if (!boardIdx.has(rec.board) || !timelineIdx.has(rec.timeline)) {
      throw new Error(
        `Adjustment fixture uses a board/timeline missing from the spawn pools: ${rec.board.slice(0, 20)}... ${rec.timeline}`
      );
    }
    const bIdx = boardIdx.get(rec.board)!;
    const tIdx = timelineIdx.get(rec.timeline)!;
    const pIdx = intern(prefixPool, prefixIdx, rec.preAdjInputSequence);
    const offset = adjPackedPool.length;
    for (const e of rec.entries) {
      adjPackedPool.push(packEntry(e));
    }
    const pieceIndex = PIECES.indexOf(rec.pieceId);
    const heldCode = DAS_BUTTON_HELD_CODES[rec.dasButtonHeld];
    adjFixtures.push(
      `  {${bIdx}, ${pieceIndex}, ${rec.level}, ${tIdx}, ${rec.dasCharge}, ${rec.framesAlreadyElapsed}, `
      + `${rec.existingXOffset}, ${rec.existingYOffset}, ${rec.existingRotation}, `
      + `${rec.isArrContinued ? 1 : 0}, ${heldCode}, ${pIdx}, ${offset}, ${rec.entries.length}},`
    );
  }
  let adjCursor = 0;
  for (const rec of adjustmentRecords) {
    for (const e of rec.entries) {
      const u = unpackEntry(adjPackedPool[adjCursor++]);
      if (u.rot !== e.rot || u.x !== e.x || u.y !== e.y ||
          u.dasChargeAfter !== e.dasChargeAfter || u.inputCostX10 !== e.inputCostX10) {
        throw new Error(`Adjustment entry pack round-trip mismatch: ${JSON.stringify(e)} -> ${JSON.stringify(u)}`);
      }
    }
  }

  // --- Reaction-time section: full possibility list in RESULT ORDER (no
  // sorting), each entry with its walk's adjTimeSimState snapshot (or none). Entries
  // share the packEntry bit layout in their own pool; captures live in a
  // parallel struct pool, one per entry, with the preAdj sequence interned in
  // its own prefix pool. Boards/timelines intern into the shared pools above. ---
  const rtPrefixPool: string[] = [];
  const rtPrefixIdx = new Map<string, number>();
  const rtPackedPool: number[] = [];
  const rtCapturePool: string[] = [];
  const rtFixtures: string[] = [];
  for (const rec of reactionTimeRecords) {
    if (!boardIdx.has(rec.board) || !timelineIdx.has(rec.timeline)) {
      throw new Error(
        `Reaction-time fixture uses a board/timeline missing from the spawn pools: ${rec.board.slice(0, 20)}... ${rec.timeline}`
      );
    }
    const bIdx = boardIdx.get(rec.board)!;
    const tIdx = timelineIdx.get(rec.timeline)!;
    const entryOffset = rtPackedPool.length;
    const captureOffset = rtCapturePool.length;
    for (const e of rec.entries) {
      rtPackedPool.push(packEntry(e));
      if (e.adjCapture) {
        const at = e.adjCapture;
        const pIdx = intern(rtPrefixPool, rtPrefixIdx, at.preAdjInputSequence);
        rtCapturePool.push(
          `  {1, ${at.frameIndex}, ${at.x}, ${at.y}, ${at.rotationIndex}, ${at.dasCharge}, `
          + `${at.isArrContinued ? 1 : 0}, ${DAS_BUTTON_HELD_CODES[at.dasButtonHeld]}, ${pIdx}},`
        );
      } else {
        rtCapturePool.push(`  {0, 0, 0, 0, 0, 0, 0, 0, -1},`);
      }
    }
    const pieceIndex = PIECES.indexOf(rec.pieceId);
    rtFixtures.push(
      `  {${bIdx}, ${pieceIndex}, ${rec.level}, ${tIdx}, ${rec.dasCharge}, ${rec.reactionTime}, `
      + `${entryOffset}, ${rec.entries.length}, ${captureOffset}}, // ${rec.tag}`
    );
  }
  let rtCursor = 0;
  for (const rec of reactionTimeRecords) {
    for (const e of rec.entries) {
      const u = unpackEntry(rtPackedPool[rtCursor++]);
      if (u.rot !== e.rot || u.x !== e.x || u.y !== e.y ||
          u.dasChargeAfter !== e.dasChargeAfter || u.inputCostX10 !== e.inputCostX10) {
        throw new Error(`Reaction-time entry pack round-trip mismatch: ${JSON.stringify(e)} -> ${JSON.stringify(u)}`);
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
  const prefixLines = prefixPool.map((p) => `  "${p}",`).join("\n");
  const adjEntryLines: string[] = [];
  for (let i = 0; i < adjPackedPool.length; i += ENTRIES_PER_LINE) {
    adjEntryLines.push("  " + adjPackedPool.slice(i, i + ENTRIES_PER_LINE).join(", ") + ",");
  }
  const rtPrefixLines = rtPrefixPool.map((p) => `  "${p}",`).join("\n");
  const rtEntryLines: string[] = [];
  for (let i = 0; i < rtPackedPool.length; i += ENTRIES_PER_LINE) {
    rtEntryLines.push("  " + rtPackedPool.slice(i, i + ENTRIES_PER_LINE).join(", ") + ",");
  }
  const rtCaptureLines = rtCapturePool;

  return `// GENERATED by src/server/move_search_fuzz_generate.ts -- do not edit by hand.
// Regenerate with: npm run fuzz_generate  (seed ${SEED})
// Golden oracle for the move search fuzz harness (see TESTING.md).
// TODO(Phase 5 cutover): at cutover this fixture set becomes a hand-maintained
// characterization baseline; changes then come from reviewed verifier diffs,
// re-baselined from C++ itself.
// FIXTURE_ENTRY_POOL holds one packed int32 per expected entry (see packEntry in
// the generator for the bit layout); entries are in the reference search's
// result order (basic entries then tuck/spin entries, post-dedupe) and every
// fullMoveSearch-based comparison is order-exact. dasCharge -1 = tap mode.
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

// ---------------------------------------------------------------------------
// Adjustment-mode fixtures (seed ${ADJUSTMENT_SEED}). Each entry is a mid-air
// state saved as the adjTimeSimState snapshot by a spawn search run with a
// reaction time (the phantom-placement pipeline); the golden entries come from the adjustment move-search call (no
// reactionTime). dasButtonHeld: -1 left / 0 none / +1 right. FIXTURE_ENTRY_POOL
// and ADJUSTMENT_ENTRY_POOL share the packEntry bit layout.
struct AdjustmentSearchFixture {
  int boardIdx;             // FIXTURE_BOARD_POOL index (shared)
  int pieceIndex;           // PIECE_LIST index (I, O, L, J, T, S, Z)
  int level;
  int timelineIdx;          // FIXTURE_TIMELINE_POOL index (shared)
  int dasCharge;            // adjustment search's incoming charge (-1 = tap mode)
  int framesAlreadyElapsed;
  int existingXOffset;
  int existingYOffset;
  int existingRotation;
  int isArrContinued;       // 0/1
  int dasButtonHeld;        // -1 left / 0 none / +1 right
  int preAdjSeqIdx;         // FIXTURE_PREFIX_POOL index
  int entryOffset;          // offset into ADJUSTMENT_ENTRY_POOL (entries, not ints)
  int entryCount;
};

static const char *FIXTURE_PREFIX_POOL[] = {
${prefixLines}
};

static const int ADJUSTMENT_ENTRY_POOL[] = {
${adjEntryLines.join("\n")}
};

static const AdjustmentSearchFixture ADJUSTMENT_FIXTURES[] = {
${adjFixtures.join("\n")}
};

static const int ADJUSTMENT_FIXTURE_COUNT = ${adjustmentRecords.length};

// ---------------------------------------------------------------------------
// Reaction-time fixtures (seed ${REACTION_TIME_SEED}). Spawn searches run WITH
// a reaction time; the golden entries are the FULL possibility list in RESULT
// ORDER (no sorting), each carrying its walk's adjTimeSimState snapshot (or
// none) -- the result-to-snapshot mapping is part of the golden. REACTION_TIME_ENTRY_POOL
// shares the packEntry bit layout; REACTION_TIME_CAPTURE_POOL is index-aligned
// with each fixture's entries (hasCapture = 0 slots carry meaningless fields).
struct FixtureAdjTimeCapture {
  int hasCapture;      // 0/1
  int frameIndex;      // fields below are meaningful only when hasCapture != 0
  int x;
  int y;
  int rotationIndex;
  int dasCharge;       // -1 (tap) .. 16, as recorded at the reaction-time frame
  int isArrContinued;  // 0/1
  int dasButtonHeld;   // -1 left / 0 none / +1 right
  int preAdjSeqIdx;    // FIXTURE_RT_PREFIX_POOL index
};

struct ReactionTimeSearchFixture {
  int boardIdx;      // FIXTURE_BOARD_POOL index (shared)
  int pieceIndex;    // PIECE_LIST index (I, O, L, J, T, S, Z)
  int level;
  int timelineIdx;   // FIXTURE_TIMELINE_POOL index (shared)
  int dasCharge;     // -1 = tap mode
  int reactionTime;
  int entryOffset;   // offset into REACTION_TIME_ENTRY_POOL (entries, not ints)
  int entryCount;
  int captureOffset; // offset into REACTION_TIME_CAPTURE_POOL (one per entry)
};

static const char *FIXTURE_RT_PREFIX_POOL[] = {
${rtPrefixLines}
};

static const int REACTION_TIME_ENTRY_POOL[] = {
${rtEntryLines.join("\n")}
};

static const FixtureAdjTimeCapture REACTION_TIME_CAPTURE_POOL[] = {
${rtCaptureLines.join("\n")}
};

static const ReactionTimeSearchFixture REACTION_TIME_FIXTURES[] = {
${rtFixtures.join("\n")}
};

static const int REACTION_TIME_FIXTURE_COUNT = ${reactionTimeRecords.length};

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

const adjustmentInputs = buildAdjustmentFixtures().concat(buildPastBottomAdjustmentFixtures());
const adjustmentRecords: AdjustmentFixtureRecord[] = [];
for (const f of adjustmentInputs) {
  adjustmentRecords.push({ ...f, entries: runAdjustmentFixture(f) });
}

const reactionTimeInputs = buildReactionTimeConditions();
const reactionTimeRecords: ReactionTimeFixtureRecord[] = [];
for (const f of reactionTimeInputs) {
  reactionTimeRecords.push({ ...f, entries: runReactionTimeFixture(f) });
}

const jsonPath = "src/server/move_search_fixtures.json";
const headerPath = "src/cpp_modules/tests/move_search_fixtures.hpp";
fs.writeFileSync(
  jsonPath,
  JSON.stringify({
    seed: SEED,
    fixtures: records,
    adjustmentFixtures: adjustmentRecords,
    reactionTimeFixtures: reactionTimeRecords,
  })
);
fs.writeFileSync(headerPath, emitCppHeader(records, adjustmentRecords, reactionTimeRecords));

const tapCount = records.filter((r) => r.dasCharge === -1).length;
const dasCount = records.length - tapCount;
const totalEntries = records.reduce((sum, r) => sum + r.entries.length, 0);
const adjTapCount = adjustmentRecords.filter((r) => r.dasCharge === -1).length;
const adjDasCount = adjustmentRecords.length - adjTapCount;
const adjTotalEntries = adjustmentRecords.reduce((sum, r) => sum + r.entries.length, 0);
console.log(
  `Generated ${records.length} fixtures (${tapCount} tap, ${dasCount} DAS), ${totalEntries} expected entries.`
);
console.log(
  `Generated ${adjustmentRecords.length} adjustment fixtures (${adjTapCount} tap, ${adjDasCount} DAS), ${adjTotalEntries} expected entries.`
);
console.log(`Wrote ${jsonPath} and ${headerPath}.`);

// --- Adjustment coverage report + floors (see the prompt's coverage gate). ---
const coverage = new Map<string, number>();
for (const r of adjustmentRecords) {
  const mode = r.dasCharge === -1 ? "tap" : "das";
  for (const key of [
    `mode=${mode}`,
    `piece=${r.pieceId}`,
    `held=${r.dasButtonHeld}`,
    `arrContinued=${r.isArrContinued}`,
    `held=${r.dasButtonHeld},mode=${mode}`,
    `arrContinued=${r.isArrContinued},mode=${mode}`,
    `rot=${r.existingRotation}`,
  ]) {
    coverage.set(key, (coverage.get(key) || 0) + 1);
  }
}
const emptyAdj = adjustmentRecords.filter((r) => r.entries.length === 0).length;
const coverageKeys = [...coverage.keys()].sort();
console.log("Adjustment coverage:", coverageKeys.map((k) => `${k}=${coverage.get(k)}`).join(" "));
console.log(`Empty-result adjustment fixtures: ${emptyAdj}`);

// Coverage floors: hard-asserted so future generator edits cannot silently
// thin out the adjustment coverage. Calibrated with headroom below the counts
// observed at generation time.
const floors: Array<[string, number]> = [
  ["total", 150],
  ["mode=tap", 20],
  ["mode=das", 80],
  ["held=LEFT", 3],
  ["held=RIGHT", 3],
  ["arrContinued=true", 5],
  ["arrContinued=false", 50],
];
for (const piece of PIECES) {
  floors.push([`piece=${piece}`, 5]);
}
for (const [key, min] of floors) {
  const count = key === "total" ? adjustmentRecords.length : coverage.get(key) || 0;
  if (count < min) {
    throw new Error(`Adjustment coverage floor violated: ${key} = ${count} < ${min}`);
  }
}

// --- Tall-stack spawn coverage: the appended sections must exist at full
// width; every open near-topped board must produce placements, and the
// one-row-open lid (flat19) must be empty for every piece except the I (whose
// horizontal form hangs in the open top row) -- the near-lockout pin. ---
const tallRecords = records.filter((r) => r.tag === "tap_tall" || r.tag === "das_tall");
const tapTallCount = tallRecords.filter((r) => r.tag === "tap_tall").length;
const dasTallCount = tallRecords.filter((r) => r.tag === "das_tall").length;
if (tapTallCount !== 7 * 3 * 3 * 5 || dasTallCount !== 7 * 7 * 3 * 4) {
  throw new Error(
    `Tall-stack fixture counts drifted: tap_tall=${tapTallCount}, das_tall=${dasTallCount}`
  );
}
for (const r of tallRecords) {
  const nearLockout = r.board === TALL_TOPPED_OUT_BOARD;
  const expectEmpty = nearLockout && r.pieceId !== "I";
  if (expectEmpty !== (r.entries.length === 0)) {
    throw new Error(
      `Tall-stack result mismatch for ${r.pieceId} L${r.level} c${r.dasCharge} `
        + `(${nearLockout ? "one-row lid" : "open"} board): ${r.entries.length} entries`
    );
  }
}
console.log(
  `Tall-stack spawn fixtures: ${tallRecords.length} (tap ${tapTallCount}, DAS ${dasTallCount}); `
    + `one-row lid empty for every piece except I.`
);

// --- Past-bottom adjustment edge (existingYOffset >= 20): every synthesized
// fixture must come back empty (the early-return path, pinned per piece and
// mode). ---
const pastBottom = adjustmentRecords.filter((r) => r.tag === "adjustment_past_bottom");
if (pastBottom.length !== PIECES.length * 2) {
  throw new Error(`Past-bottom adjustment fixtures missing: ${pastBottom.length}`);
}
for (const r of pastBottom) {
  if (r.entries.length !== 0) {
    throw new Error(
      `Past-bottom adjustment fixture produced ${r.entries.length} entries for ${r.pieceId}`
    );
  }
}
console.log(`Past-bottom adjustment fixtures (existingYOffset=20, all empty): ${pastBottom.length}`);

// --- Reaction-time coverage report + floors (same hard-assert pattern). ---
const rtCoverage = new Map<string, number>();
for (const r of reactionTimeRecords) {
  const mode = r.dasCharge === -1 ? "tap" : "das";
  const captures = r.entries.filter((e) => e.adjCapture !== null).length;
  for (const key of [
    `mode=${mode}`,
    `piece=${r.pieceId}`,
    `rt=${r.reactionTime}`,
    `hasCaptures=${captures > 0}`,
    `mixed=${captures > 0 && captures < r.entries.length}`,
    `tag=${r.tag}`,
  ]) {
    rtCoverage.set(key, (rtCoverage.get(key) || 0) + 1);
  }
}
const rtTotalEntries = reactionTimeRecords.reduce((sum, r) => sum + r.entries.length, 0);
const rtCapturedEntries = reactionTimeRecords.reduce(
  (sum, r) => sum + r.entries.filter((e) => e.adjCapture !== null).length,
  0
);
console.log(
  `Reaction-time: ${reactionTimeRecords.length} fixtures, ${rtTotalEntries} entries `
    + `(${rtCapturedEntries} with captures).`
);
const rtCoverageKeys = [...rtCoverage.keys()].sort();
console.log("Reaction-time coverage:", rtCoverageKeys.map((k) => `${k}=${rtCoverage.get(k)}`).join(" "));

const rtFloors: Array<[string, number]> = [
  ["total", 150],
  ["mode=tap", 60],
  ["mode=das", 60],
  ["rt=0", 10],
  ["hasCaptures=true", 60],
  ["hasCaptures=false", 20],
  ["mixed=true", 15],
  ["tag=rt_tuck_board", 25],
];
for (const piece of PIECES) {
  rtFloors.push([`piece=${piece}`, 15]);
}
for (const [key, min] of rtFloors) {
  const count = key === "total" ? reactionTimeRecords.length : rtCoverage.get(key) || 0;
  if (count < min) {
    throw new Error(`Reaction-time coverage floor violated: ${key} = ${count} < ${min}`);
  }
}
