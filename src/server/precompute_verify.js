/**
 * Differential runner for the C++ precompute pipeline.
 *
 * Reads the golden fixtures, derives each PRECOMPUTE pipe string (the 11
 * standard fields plus the reaction time; nextPiece is -1 because precompute
 * searches all seven next pieces), calls the addon, and compares the result
 * byte-for-byte with the recorded string. The summary is reported as-is:
 * nothing is sorted, re-ordered or normalised before the comparison.
 *
 * Usage: npm run precompute_verify
 *
 * `--dump-inputs [path]` (default build/precompute_tsan_inputs.txt) writes the
 * production-tier requests for the TSan replay harness instead of running the
 * differential verify: a count line, then two lines per request — the
 * PRECOMPUTE pipe string, then the recorded golden output with every newline
 * escaped to a \x01 sentinel (pipe strings never contain newlines or \x01).
 * This mode does not load the addon.
 */
const fs = require("fs");
const path = require("path");

const FIXTURES_PATH = path.join(__dirname, "data/precompute_fixtures.jsonl");
const DEFAULT_DUMP_PATH = path.join(__dirname, "../../build/precompute_tsan_inputs.txt");
const PIECE_LOOKUP = ["I", "O", "L", "J", "T", "S", "Z"];
const MAX_DIFF_EXAMPLES = 5;

function buildPipeString(record) {
  const curPieceIndex = PIECE_LOOKUP.indexOf(record.currentPiece);
  return [
    record.board,
    record.level,
    record.lines,
    curPieceIndex,
    -1, // nextPiece: precompute searches all seven next pieces
    record.inputFrameTimeline,
    record.playoutCount,
    record.playoutLength,
    record.pruningBreadth,
    record.aggressionLevel,
    record.dasCharge,
    record.reactionTime,
  ].join("|") + "|";
}

/** Classifies the first line where the two outputs diverge. */
function categorize(tsOutput, cppOutput) {
  if (/^Error:/.test(cppOutput)) {
    return "cpp error";
  }
  if (tsOutput === "none" || cppOutput === "none") {
    return tsOutput === cppOutput ? null : "whole-response none vs content";
  }

  const tsLines = tsOutput.split("\n");
  const cppLines = cppOutput.split("\n");
  if (tsLines[0] !== cppLines[0]) {
    return "default line";
  }
  for (let i = 1; i < Math.max(tsLines.length, cppLines.length); i++) {
    if (tsLines[i] !== cppLines[i]) {
      const tsLine = tsLines[i] || "";
      const cppLine = cppLines[i] || "";
      const fallbackRe = /^([IOLJTSZ]):(continue|none)$/;
      if (fallbackRe.test(tsLine) !== fallbackRe.test(cppLine)) {
        return `piece ${tsLine.slice(0, 1)} fallback vs content`;
      }
      if (fallbackRe.test(tsLine) && fallbackRe.test(cppLine)) {
        return `piece ${tsLine.slice(0, 1)} fallback marker`;
      }
      return `piece ${tsLine.slice(0, 1)} line`;
    }
  }
  return null;
}

function loadFixtures() {
  const raw = fs.readFileSync(FIXTURES_PATH, "utf8").trim().split("\n");
  return raw.slice(1).map((line) => JSON.parse(line));
}

/** Writes the production-tier requests in the TSan replay harness's input format. */
function dumpInputs(outputPath) {
  const production = loadFixtures().filter((fixture) => fixture.tier === "production");
  const lines = [String(production.length)];
  for (const fixture of production) {
    lines.push(buildPipeString(fixture));
    lines.push(fixture.output.replace(/\n/g, "\x01"));
  }
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, lines.join("\n") + "\n");
  console.log(`Dumped ${production.length} production-tier precompute requests to ${outputPath}`);
}

function main() {
  const dumpArg = process.argv.indexOf("--dump-inputs");
  if (dumpArg !== -1) {
    dumpInputs(process.argv[dumpArg + 1] || DEFAULT_DUMP_PATH);
    return;
  }

  const cModule = require(path.join(__dirname, "../../build/Release/cRabbit"));
  const fixtures = loadFixtures();
  console.log(`Precompute differential verify: ${fixtures.length} fixtures\n`);

  let matches = 0;
  const categories = new Map();
  const examples = [];
  const started = Date.now();

  fixtures.forEach((fixture, index) => {
    let cppOutput;
    try {
      cppOutput = cModule.precompute(buildPipeString(fixture));
    } catch (err) {
      cppOutput = `Error: ${err.message}`;
    }
    const tsOutput = fixture.output;

    if (cppOutput === tsOutput) {
      matches++;
      return;
    }

    const category = categorize(tsOutput, cppOutput) || "unclassified";
    categories.set(category, (categories.get(category) || 0) + 1);
    if (examples.length < MAX_DIFF_EXAMPLES) {
      examples.push({ index, fixture, tsOutput, cppOutput, category });
    }
  });

  const total = fixtures.length;
  const mismatches = total - matches;
  console.log(`Byte-identical: ${matches}/${total} (${((matches / total) * 100).toFixed(1)}%)`);
  console.log(`Mismatches:     ${mismatches}`);
  console.log(`Elapsed:        ${((Date.now() - started) / 1000).toFixed(1)}s\n`);

  if (mismatches > 0) {
    console.log("Mismatch categories (first divergence per fixture):");
    const ordered = [...categories.entries()].sort((a, b) => b[1] - a[1]);
    for (const [category, count] of ordered) {
      console.log(`  ${String(count).padStart(5)}  ${category}`);
    }
    console.log("");

    for (const example of examples) {
      const f = example.fixture;
      console.log("=".repeat(72));
      console.log(
        `Fixture ${example.index} [${example.category}] tier=${f.tier}` +
          ` piece=${f.currentPiece} level=${f.level} lines=${f.lines}` +
          ` rt=${f.reactionTime} das=${f.dasCharge}` +
          ` playouts=${f.playoutCount}/${f.playoutLength}` +
          (f.source && f.source.syntheticTag ? ` tag=${f.source.syntheticTag}` : "")
      );
      console.log("--- TS output ---");
      console.log(example.tsOutput);
      console.log("--- C++ output ---");
      console.log(example.cppOutput);
      console.log("");
    }
  } else {
    console.log("All fixtures byte-identical.");
  }

  process.exitCode = mismatches === 0 ? 0 : 1;
}

main();
