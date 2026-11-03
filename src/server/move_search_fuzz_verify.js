/**
 * Verifies the C++ DAS move search against the TS golden fixtures at the
 * strongest available level: character-exact input sequences, plus the
 * (rotationIndex, x, y, dasChargeAfter, inputCostX10) tuples.
 *
 * Complements `npm run cpp_test_unit` (which asserts the tuples for all DAS
 * fixtures in-doctest but does not carry sequences in the fixture header):
 * this script runs the full DAS fixture set through build/fixture_dump and
 * diffs every entry, sequences included, against the TS oracle JSON.
 *
 * Workflow: after any TS move-search change, regenerate the oracle with
 * `npm run fuzz_generate`, then run `npm run fuzz_verify_sequences` to check
 * the C++ side against the new golden.
 *
 * Usage: npm run fuzz_verify_sequences   (exits nonzero on any mismatch)
 */
const fs = require("fs");
const { execFileSync } = require("child_process");

const FIXTURES_JSON = "src/server/move_search_fixtures.json";
const DUMP_BIN = "build/fixture_dump";

function main() {
  const data = JSON.parse(fs.readFileSync(FIXTURES_JSON, "utf8"));

  // Derive the DAS fixture indexes from the oracle itself (never assume layout).
  const dasIndexes = [];
  data.fixtures.forEach((f, i) => {
    if (f.dasCharge !== -1) dasIndexes.push(i);
  });
  if (dasIndexes.length === 0) {
    console.error("No DAS fixtures found in the oracle JSON -- regenerate with `npm run fuzz_generate`.");
    process.exit(1);
  }

  const cppOutput = execFileSync(DUMP_BIN, ["--das"], { encoding: "utf8", maxBuffer: 1 << 28 });

  const cppByFixture = {};
  let current = null;
  for (const line of cppOutput.split("\n")) {
    const header = line.match(/^fixture (\d+) /);
    if (header) {
      current = parseInt(header[1], 10);
      cppByFixture[current] = [];
      continue;
    }
    const entry = line.match(/^  r(-?\d+) x(-?\d+) y(-?\d+) c(-?\d+) k(-?\d+) seq=(.*)$/);
    if (entry && current !== null) {
      cppByFixture[current].push({
        key: `${entry[1]}|${entry[2]}|${entry[3]}|${entry[4]}|${entry[5]}`,
        seq: entry[6],
      });
    }
  }

  let fullMatch = 0;
  const mismatchedFixtures = [];
  let totalEntries = 0;
  let totalSeqChars = 0;

  for (const idx of dasIndexes) {
    const f = data.fixtures[idx];
    const ts = f.entries
      .map((e) => ({
        key: `${e.rot}|${e.x}|${e.y}|${e.dasChargeAfter}|${e.inputCostX10}`,
        seq: e.inputSequence,
      }))
      .sort((a, b) => a.key.localeCompare(b.key) || a.seq.localeCompare(b.seq));
    const cpp = (cppByFixture[idx] || []).sort(
      (a, b) => a.key.localeCompare(b.key) || a.seq.localeCompare(b.seq)
    );
    totalEntries += ts.length;
    for (const e of ts) totalSeqChars += e.seq.length;

    const exact =
      ts.length === cpp.length && ts.every((v, i) => v.key === cpp[i].key && v.seq === cpp[i].seq);
    if (exact) {
      fullMatch++;
    } else {
      mismatchedFixtures.push(idx);
    }
  }

  console.log(
    `Sequence verification: ${fullMatch}/${dasIndexes.length} DAS fixtures character-exact ` +
      `(${totalEntries} entries, ${totalSeqChars} sequence chars compared)`
  );

  if (mismatchedFixtures.length > 0) {
    console.error(`MISMATCHED fixtures: ${mismatchedFixtures.join(", ")}`);
    // Show the first few diffs to make triage immediate.
    for (const idx of mismatchedFixtures.slice(0, 3)) {
      const f = data.fixtures[idx];
      const ts = f.entries
        .map((e) => ({ key: `${e.rot}|${e.x}|${e.y}|${e.dasChargeAfter}|${e.inputCostX10}`, seq: e.inputSequence }))
        .sort((a, b) => a.key.localeCompare(b.key) || a.seq.localeCompare(b.seq));
      const cpp = (cppByFixture[idx] || []).sort(
        (a, b) => a.key.localeCompare(b.key) || a.seq.localeCompare(b.seq)
      );
      console.error(`  fixture ${idx} (${f.pieceId} L${f.level} ${f.timeline} c${f.dasCharge}):`);
      let shown = 0;
      for (let i = 0; i < Math.max(ts.length, cpp.length) && shown < 4; i++) {
        if (!ts[i] || !cpp[i] || ts[i].key !== cpp[i].key || ts[i].seq !== cpp[i].seq) {
          console.error(`    TS : ${ts[i] ? ts[i].key + " seq=" + ts[i].seq : "-"}`);
          console.error(`    C++: ${cpp[i] ? cpp[i].key + " seq=" + cpp[i].seq : "-"}`);
          shown++;
        }
      }
    }
    process.exit(1);
  }
  console.log("OK");
}

main();
