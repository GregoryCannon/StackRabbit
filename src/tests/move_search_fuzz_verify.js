/**
 * Verifies the C++ move search against the golden fixtures at the
 * strongest available level: character-exact input sequences, plus the
 * (rotationIndex, x, y, dasChargeAfter, inputCostX10) tuples.
 *
 * The fixture JSON is a hand-maintained characterization baseline; intentional
 * updates come from reviewed verifier diffs (see TESTING.md).
 *
 * Covers BOTH modes: DAS fixtures (dasCharge >= 0, the frame-exact charge
 * search) and tap fixtures (dasCharge == -1, fullMoveSearch's ARR-gated tap
 * path), plus the adjustment fixtures (mid-air entry states from the
 * ADJUSTMENT_FIXTURES section, dumped under "adjustment <idx>" headers by
 * fixture_dump) and the reaction-time fixtures (spawn searches run WITH a
 * reaction time, dumped under "reactiontime <idx>" headers; compared in
 * RESULT ORDER, snapshot included -- the result-to-snapshot mapping is part
 * of the golden).
 *
 * Complements `npm run cpp_test_unit` (which asserts the tuples for all
 * fixtures in-doctest but does not carry sequences in the fixture header):
 * this script runs the full fixture set through build/fixture_dump and
 * diffs every entry, sequences included, against the baseline JSON.
 *
 * Usage: npm run fuzz_verify_sequences   (exits nonzero on any mismatch)
 */
const fs = require("fs");
const path = require("path");
const { execFileSync } = require("child_process");

const FIXTURES_JSON = path.join(__dirname, "data/move_search_fixtures.json");
const DUMP_BIN = path.join(__dirname, "../../build/fixture_dump");

// dasButtonHeld names (JSON) <-> DAS_BUTTON_* codes (C++ dump output).
const HELD_NAME_TO_CODE = { LEFT: -1, RIGHT: 1, NONE: 0 };

function main() {
  const data = JSON.parse(fs.readFileSync(FIXTURES_JSON, "utf8"));

  // Derive the fixture indexes from the baseline itself (never assume layout).
  const allIndexes = data.fixtures.map((_, i) => i);
  if (allIndexes.length === 0) {
    console.error(`No fixtures found in ${FIXTURES_JSON}.`);
    process.exit(1);
  }

  const cppOutput = execFileSync(DUMP_BIN, ["--all"], { encoding: "utf8", maxBuffer: 1 << 28 });

  const cppByFixture = {};
  const cppByAdjustment = {};
  const cppByReactionTime = {};
  let current = null;
  let currentKind = null; // "fixture" | "adjustment" | "reactiontime" (index spaces are independent)
  for (const line of cppOutput.split("\n")) {
    const spawnHeader = line.match(/^fixture (\d+) /);
    const adjustmentHeader = line.match(/^adjustment (\d+) /);
    const reactionTimeHeader = line.match(/^reactiontime (\d+) /);
    if (spawnHeader) {
      current = parseInt(spawnHeader[1], 10);
      currentKind = "fixture";
      cppByFixture[current] = [];
      continue;
    }
    if (adjustmentHeader) {
      current = parseInt(adjustmentHeader[1], 10);
      currentKind = "adjustment";
      cppByAdjustment[current] = [];
      continue;
    }
    if (reactionTimeHeader) {
      current = parseInt(reactionTimeHeader[1], 10);
      currentKind = "reactiontime";
      cppByReactionTime[current] = [];
      continue;
    }
    // Reaction-time entries carry their snapshot after the sequence; matched
    // BEFORE the plain entry regex (which would otherwise swallow the
    // snapshot fields as part of the sequence).
    const rtEntry = line.match(
      /^  r(-?\d+) x(-?\d+) y(-?\d+) c(-?\d+) k(-?\d+) seq=(.*?) adj=(\d+) f(-?\d+) x(-?\d+) y(-?\d+) rot(-?\d+) c(-?\d+) arr(-?\d+) held(-?\d+) pre=(.*)$/
    );
    if (rtEntry && currentKind === "reactiontime") {
      cppByReactionTime[current].push({
        key: `${rtEntry[1]}|${rtEntry[2]}|${rtEntry[3]}|${rtEntry[4]}|${rtEntry[5]}`,
        seq: rtEntry[6],
        hasCapture: rtEntry[7] === "1",
        capture: rtEntry[7] === "1"
          ? {
              frameIndex: parseInt(rtEntry[8], 10),
              x: parseInt(rtEntry[9], 10),
              y: parseInt(rtEntry[10], 10),
              rotationIndex: parseInt(rtEntry[11], 10),
              dasCharge: parseInt(rtEntry[12], 10),
              isArrContinued: rtEntry[13] === "1",
              dasButtonHeld: parseInt(rtEntry[14], 10),
              preAdjInputSequence: rtEntry[15],
            }
          : null,
      });
      continue;
    }
    const entry = line.match(/^  r(-?\d+) x(-?\d+) y(-?\d+) c(-?\d+) k(-?\d+) seq=(.*)$/);
    if (entry && current !== null) {
      const parsed = {
        key: `${entry[1]}|${entry[2]}|${entry[3]}|${entry[4]}|${entry[5]}`,
        seq: entry[6],
      };
      if (currentKind === "adjustment") {
        cppByAdjustment[current].push(parsed);
      } else {
        cppByFixture[current].push(parsed);
      }
    }
  }

  let spawnMatch = 0;
  let adjustmentMatch = 0;
  let reactionTimeMatch = 0;
  const mismatchedFixtures = [];
  const mismatchedAdjustments = [];
  const mismatchedReactionTimes = [];
  let totalEntries = 0;
  let totalSeqChars = 0;
  let tapCount = 0;
  let dasCount = 0;
  let adjTapCount = 0;
  let adjDasCount = 0;
  let rtTapCount = 0;
  let rtDasCount = 0;
  let rtCapturedEntries = 0;

  const toComparable = (entries) =>
    entries.map((e) => ({
      key: `${e.rot}|${e.x}|${e.y}|${e.dasChargeAfter}|${e.inputCostX10}`,
      seq: e.inputSequence,
    }));

  for (const idx of allIndexes) {
    const f = data.fixtures[idx];
    if (f.dasCharge === -1) tapCount++;
    else dasCount++;
    const ts = toComparable(f.entries);
    const cpp = cppByFixture[idx] || [];
    totalEntries += ts.length;
    for (const e of ts) totalSeqChars += e.seq.length;

    const exact =
      ts.length === cpp.length && ts.every((v, i) => v.key === cpp[i].key && v.seq === cpp[i].seq);
    if (exact) {
      spawnMatch++;
    } else {
      mismatchedFixtures.push(idx);
    }
  }

  const adjustmentFixtures = data.adjustmentFixtures || [];
  for (let idx = 0; idx < adjustmentFixtures.length; idx++) {
    const f = adjustmentFixtures[idx];
    if (f.dasCharge === -1) adjTapCount++;
    else adjDasCount++;
    const ts = toComparable(f.entries);
    const cpp = cppByAdjustment[idx] || [];
    totalEntries += ts.length;
    for (const e of ts) totalSeqChars += e.seq.length;

    const exact =
      ts.length === cpp.length && ts.every((v, i) => v.key === cpp[i].key && v.seq === cpp[i].seq);
    if (exact) {
      adjustmentMatch++;
    } else {
      mismatchedAdjustments.push(idx);
    }
  }

  // Reaction-time fixtures: compared in RESULT ORDER (no sorting), snapshot
  // included. A snapshot is comparable as null or the full field set, with
  // dasButtonHeld mapped to the C++ DAS_BUTTON_* code.
  const toComparableRt = (entries) =>
    entries.map((e) => ({
      key: `${e.rot}|${e.x}|${e.y}|${e.dasChargeAfter}|${e.inputCostX10}`,
      seq: e.inputSequence,
      capture: e.adjCapture
        ? {
            frameIndex: e.adjCapture.frameIndex,
            x: e.adjCapture.x,
            y: e.adjCapture.y,
            rotationIndex: e.adjCapture.rotationIndex,
            dasCharge: e.adjCapture.dasCharge,
            isArrContinued: e.adjCapture.isArrContinued,
            dasButtonHeld: HELD_NAME_TO_CODE[e.adjCapture.dasButtonHeld],
            preAdjInputSequence: e.adjCapture.preAdjInputSequence,
          }
        : null,
    }));
  const captureEquals = (a, b) =>
    a === b ||
    (a !== null && b !== null &&
      a.frameIndex === b.frameIndex &&
      a.x === b.x &&
      a.y === b.y &&
      a.rotationIndex === b.rotationIndex &&
      a.dasCharge === b.dasCharge &&
      a.isArrContinued === b.isArrContinued &&
      a.dasButtonHeld === b.dasButtonHeld &&
      a.preAdjInputSequence === b.preAdjInputSequence);

  const reactionTimeFixtures = data.reactionTimeFixtures || [];
  for (let idx = 0; idx < reactionTimeFixtures.length; idx++) {
    const f = reactionTimeFixtures[idx];
    if (f.dasCharge === -1) rtTapCount++;
    else rtDasCount++;
    const ts = toComparableRt(f.entries);
    const cpp = cppByReactionTime[idx] || [];
    totalEntries += ts.length;
    for (const e of ts) totalSeqChars += e.seq.length;
    rtCapturedEntries += ts.filter((e) => e.capture !== null).length;

    const exact =
      ts.length === cpp.length &&
      ts.every(
        (v, i) =>
          v.key === cpp[i].key && v.seq === cpp[i].seq && captureEquals(v.capture, cpp[i].capture)
      );
    if (exact) {
      reactionTimeMatch++;
    } else {
      mismatchedReactionTimes.push(idx);
    }
  }

  console.log(
    `Sequence verification: ${spawnMatch + adjustmentMatch + reactionTimeMatch}/` +
      `${allIndexes.length + adjustmentFixtures.length + reactionTimeFixtures.length} ` +
      `fixtures character-exact (${tapCount} tap, ${dasCount} DAS, ` +
      `${adjustmentFixtures.length} adjustment [${adjTapCount} tap, ${adjDasCount} DAS], ` +
      `${reactionTimeFixtures.length} reaction-time [${rtTapCount} tap, ${rtDasCount} DAS]; ` +
      `${totalEntries} entries, ${totalSeqChars} sequence chars compared, ` +
      `${rtCapturedEntries} capture slots compared)`
  );

  if (mismatchedFixtures.length > 0 || mismatchedAdjustments.length > 0 ||
      mismatchedReactionTimes.length > 0) {
    console.error(
      `MISMATCHED fixtures: ${mismatchedFixtures.join(", ")}` +
        (mismatchedAdjustments.length
          ? ` | adjustments: ${mismatchedAdjustments.join(", ")}`
          : "") +
        (mismatchedReactionTimes.length
          ? ` | reaction-time: ${mismatchedReactionTimes.join(", ")}`
          : "")
    );
    // Show the first few diffs to make triage immediate.
    for (const idx of mismatchedFixtures.slice(0, 3)) {
      const f = data.fixtures[idx];
      const baseline = toComparable(f.entries);
      const cpp = cppByFixture[idx] || [];
      console.error(`  fixture ${idx} (${f.pieceId} L${f.level} ${f.timeline} c${f.dasCharge}):`);
      let shown = 0;
      for (let i = 0; i < Math.max(baseline.length, cpp.length) && shown < 4; i++) {
        if (!baseline[i] || !cpp[i] || baseline[i].key !== cpp[i].key || baseline[i].seq !== cpp[i].seq) {
          console.error(`    baseline: ${baseline[i] ? baseline[i].key + " seq=" + baseline[i].seq : "-"}`);
          console.error(`    C++:      ${cpp[i] ? cpp[i].key + " seq=" + cpp[i].seq : "-"}`);
          shown++;
        }
      }
    }
    for (const idx of mismatchedAdjustments.slice(0, 3)) {
      const f = adjustmentFixtures[idx];
      const baseline = toComparable(f.entries);
      const cpp = cppByAdjustment[idx] || [];
      console.error(
        `  adjustment ${idx} (${f.pieceId} L${f.level} ${f.timeline} c${f.dasCharge} ` +
          `f${f.framesAlreadyElapsed} x${f.existingXOffset} y${f.existingYOffset} ` +
          `r${f.existingRotation} arr${f.isArrContinued ? 1 : 0} held=${f.dasButtonHeld} ` +
          `pre=${f.preAdjInputSequence}):`
      );
      let shown = 0;
      for (let i = 0; i < Math.max(baseline.length, cpp.length) && shown < 4; i++) {
        if (!baseline[i] || !cpp[i] || baseline[i].key !== cpp[i].key || baseline[i].seq !== cpp[i].seq) {
          console.error(`    baseline: ${baseline[i] ? baseline[i].key + " seq=" + baseline[i].seq : "-"}`);
          console.error(`    C++:      ${cpp[i] ? cpp[i].key + " seq=" + cpp[i].seq : "-"}`);
          shown++;
        }
      }
    }
    for (const idx of mismatchedReactionTimes.slice(0, 3)) {
      const f = reactionTimeFixtures[idx];
      const baseline = toComparableRt(f.entries);
      const cpp = cppByReactionTime[idx] || [];
      console.error(
        `  reaction-time ${idx} (${f.pieceId} L${f.level} ${f.timeline} c${f.dasCharge} ` +
          `reactionTime=${f.reactionTime}):`
      );
      let shown = 0;
      for (let i = 0; i < Math.max(baseline.length, cpp.length) && shown < 4; i++) {
        const b = baseline[i];
        const c = cpp[i];
        if (!b || !c || b.key !== c.key || b.seq !== c.seq || !captureEquals(b.capture, c.capture)) {
          const capStr = (x) =>
            x.capture === null
              ? "cap=null"
              : `cap=f${x.capture.frameIndex},x${x.capture.x},y${x.capture.y},r${x.capture.rotationIndex},c${x.capture.dasCharge},arr${x.capture.isArrContinued ? 1 : 0},held${x.capture.dasButtonHeld},pre=${x.capture.preAdjInputSequence}`;
          console.error(`    baseline: ${b ? b.key + " seq=" + b.seq + " " + capStr(b) : "-"}`);
          console.error(`    C++:      ${c ? c.key + " seq=" + c.seq + " " + capStr(c) : "-"}`);
          shown++;
        }
      }
    }
    process.exit(1);
  }
  console.log("OK");
}

main();
