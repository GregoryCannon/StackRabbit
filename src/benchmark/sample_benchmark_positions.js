// Samples a diverse 200-position benchmark subset from position_log.jsonl.
// Stratifies by gravity-relevant level groups (18 / 19-28), spreads samples
// across board quality and situation type, and enforces a minimum move spacing
// within each game to avoid near-duplicate positions.
//
// Usage (from repo root): node src/benchmark/sample_benchmark_positions.js
const fs = require('fs');
const path = require('path');

const INPUT_FILE = path.join(__dirname, 'position_log.jsonl');
const OUTPUT_FILE = path.join(__dirname, 'benchmark_positions.jsonl');

// NB: the current benchmark_positions.jsonl was further edited by hand after sampling:
// slot 125 (game 1, move 516) was all-dead per the oracle (every pair rated -3000), so it
// was swapped for game 1, move 504 from the position log (shallowEval -532.9, survivable).
// Re-running this sampler regenerates the original 200 without that swap.

// Level strata. Level 29 play is not relevant here, and levels 19-28 are functionally
// equivalent (gravity 2), so they form one stratum balanced equally against level 18
// (gravity 3).
const STRATA = [
  { name: 'level18', test: (p) => p.level === 18, count: 100 },
  { name: 'levels19-28', test: (p) => p.level >= 19 && p.level <= 28, count: 100 },
];

const MIN_MOVE_GAP = 8; // Min moveIndex spacing between two sampled positions from the same game
const SEED = 20260305; // Fixed seed so the sample is reproducible

// --- Seeded PRNG (mulberry32) for reproducible sampling ---
function mulberry32(seed) {
  return function () {
    seed |= 0;
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rand = mulberry32(SEED);
function shuffle(arr) {
  for (let i = arr.length - 1; i > 0; i--) {
    const j = Math.floor(rand() * (i + 1));
    [arr[i], arr[j]] = [arr[j], arr[i]];
  }
  return arr;
}

// --- I-piece tetris-ready detection ---
// An I piece on a tetris-ready board is a trivial tetris slam, so those positions get
// their current piece re-randomized after sampling.
const I_PIECE_INDEX = 0;
const FULL_ROW = 1023;
const BOARD_ENCODING = 'abcdefghijklmnopqrstuvwxyzABCDEF';
const PIECE_CHARS = 'IOLJTSZ';

/** Decodes a formatBoard() compressed string back into 20 rows of 10-bit occupancy. */
function decodeBoard(compressed) {
  const [emptyRowsStr, middle] = compressed.split(',');
  const board = new Array(20).fill(0);
  let row = parseInt(emptyRowsStr, 10);
  for (let i = 0; i < middle.length; i += 2, row++) {
    const leftIdx = BOARD_ENCODING.indexOf(middle[i]);
    const rightIdx = BOARD_ENCODING.indexOf(middle[i + 1]);
    board[row] = (leftIdx << 5) | rightIdx;
  }
  for (; row < 20; row++) {
    board[row] = FULL_ROW - 1;
  }
  return board;
}

/** Number of full right-well rows at the bottom (the trailing count in formatBoard output). */
function numBottomWellRows(compressed) {
  return parseInt(compressed.split(',').pop(), 10);
}

/** Replica of isTetrisReady() in eval.cpp for the right well (column 9). */
function isTetrisReady(board) {
  const wellMask = 1; // Column 9 = bit 0
  const idealRowMask = FULL_ROW & ~wellMask;
  let row = 0;
  while (row < 20 && !(board[row] & wellMask)) row++;
  const wellColHeight = 20 - row;
  if (wellColHeight > 16) return false;
  for (let r = 0; r < 4; r++) {
    if ((board[19 - wellColHeight - r] & FULL_ROW) !== idealRowMask) return false;
  }
  return true;
}

// --- Load ---
const lines = fs.readFileSync(INPUT_FILE, 'utf8').trim().split('\n');
const header = JSON.parse(lines[0]);
const positions = lines.slice(1).map(JSON.parse);

// Drop exact duplicate positions (same board/level/lines/pieces)
const seen = new Set();
const unique = positions.filter((p) => {
  const key = `${p.board}|${p.level}|${p.lines}|${p.curPiece}|${p.nextPiece}`;
  if (seen.has(key)) return false;
  seen.add(key);
  return true;
});

const modeClass = (p) => (p.aiMode === 0 ? 'standard' : p.aiMode === 1 ? 'dig' : 'other');

/**
 * Samples `target` positions from `candidates`.
 * Candidates are bucketed by (aiMode class, shallowEval quartile); sampling round-robins
 * across buckets so the result spreads across both situation type and board quality.
 * A min move-index gap is enforced within each game, relaxing in halves (8 -> 4 -> 2 -> 1)
 * if the quota can't otherwise be met.
 */
function sampleStratum(candidates, target) {
  const sortedByEval = [...candidates].sort((a, b) => a.shallowEval - b.shallowEval);
  const quartileSize = Math.ceil(sortedByEval.length / 4);
  const quartileOf = new Map();
  sortedByEval.forEach((p, i) => quartileOf.set(p, Math.min(3, Math.floor(i / quartileSize))));
  const bucketOf = (p) => `${modeClass(p)}_q${quartileOf.get(p)}`;

  const picked = [];
  const pickedMovesByGame = new Map(); // gameId -> sorted list of moveIndex
  const hasGapConflict = (p, gap) => {
    const moves = pickedMovesByGame.get(p.gameId);
    if (!moves) return false;
    return moves.some((m) => Math.abs(m - p.moveIndex) < gap);
  };
  const pick = (p) => {
    picked.push(p);
    const moves = pickedMovesByGame.get(p.gameId);
    if (moves) {
      let lo = 0;
      while (lo < moves.length && moves[lo] < p.moveIndex) lo++;
      moves.splice(lo, 0, p.moveIndex);
    } else {
      pickedMovesByGame.set(p.gameId, [p.moveIndex]);
    }
  };

  let pool = candidates.slice();
  for (let gap = MIN_MOVE_GAP; gap >= 1 && picked.length < target; gap = Math.floor(gap / 2)) {
    shuffle(pool);
    const buckets = new Map();
    for (const p of pool) {
      const key = bucketOf(p);
      (buckets.get(key) ?? buckets.set(key, []).get(key)).push(p);
    }
    const keys = shuffle([...buckets.keys()]);
    const leftover = [];
    let progress = true;
    while (picked.length < target && progress) {
      progress = false;
      for (const key of keys) {
        const bucket = buckets.get(key);
        while (bucket.length && picked.length < target) {
          const candidate = bucket.shift();
          if (!hasGapConflict(candidate, gap)) {
            pick(candidate);
            progress = true;
            break;
          }
          leftover.push(candidate);
        }
      }
    }
    pool = leftover;
  }

  return picked;
}

// --- Sample each stratum ---
const allPicked = [];
const stratumSummaries = [];
for (const stratum of STRATA) {
  const candidates = unique.filter(stratum.test);
  const picked = sampleStratum(candidates, Math.min(stratum.count, candidates.length));
  if (picked.length < stratum.count) {
    console.log(`Warning: stratum ${stratum.name} only had ${candidates.length} unique candidates (wanted ${stratum.count})`);
  }
  allPicked.push(...picked);
  const evals = picked.map((p) => p.shallowEval);
  stratumSummaries.push({
    name: stratum.name,
    sampled: picked.length,
    evalRange: [Math.min(...evals).toFixed(1), Math.max(...evals).toFixed(1)],
  });
}

allPicked.sort((a, b) => a.gameId - b.gameId || a.moveIndex - b.moveIndex);

// --- Re-randomize the current piece on I-piece tetris-ready positions ---
// An I piece on a tetris-ready board is a trivial tetris slam, so for those positions
// the current piece is re-rolled (uniformly among the non-I pieces) while everything
// else stays the same. Each change is recorded via origCurPiece on the position.
const pieceChanges = [];
for (const p of allPicked) {
  const isIPieceTetrisReady =
    p.curPiece === I_PIECE_INDEX &&
    p.aiMode !== 1 && // Not DIG
    numBottomWellRows(p.board) >= 4 &&
    isTetrisReady(decodeBoard(p.board));
  if (isIPieceTetrisReady) {
    p.origCurPiece = p.curPiece;
    p.curPiece = 1 + Math.floor(rand() * 6); // Uniform among the 6 non-I pieces
    pieceChanges.push(p);
  }
}

// --- Write output ---
const outHeader = {
  ...header,
  sampledFrom: positions.length,
  seed: SEED,
  strata: STRATA.map((s) => `${s.name}:${s.count}`).join(','),
  iPieceTetrisReadyRandomized: pieceChanges.length,
};
fs.writeFileSync(OUTPUT_FILE, JSON.stringify(outHeader) + '\n' + allPicked.map((p, i) => JSON.stringify({index: i + 1, ...p})).join('\n') + '\n');

// --- Summary ---
console.log(`Wrote ${allPicked.length} positions to ${OUTPUT_FILE}`);
for (const s of stratumSummaries) {
  console.log(`  ${s.name}: ${s.sampled} sampled (shallowEval ${s.evalRange[0]} to ${s.evalRange[1]})`);
}
const tally = (label, fn) => {
  const counts = {};
  for (const p of allPicked) {
    const k = fn(p);
    counts[k] = (counts[k] || 0) + 1;
  }
  console.log(`  ${label}: ${JSON.stringify(counts)}`);
};
tally('by game', (p) => p.gameId);
tally('by level', (p) => p.level);
tally('by aiMode', (p) => p.aiMode);
tally('with holes', (p) => (p.holes > 0 ? 'yes' : 'no'));

// --- I-piece re-randomization report ---
console.log(`I-piece tetris-ready re-randomizations: ${pieceChanges.length}`);
for (const c of pieceChanges) {
  console.log(`  game ${c.gameId} move ${c.moveIndex} (level ${c.level}, lines ${c.lines}): I -> ${PIECE_CHARS[c.curPiece]}`);
}
