/**
 * Verification runner for the C++ precompute pipeline.
 *
 * Reads the golden fixtures, derives each PRECOMPUTE pipe string (the 11
 * standard fields plus the reaction time; nextPiece is -1 because precompute
 * searches all seven next pieces), runs all requests through
 * `build/precompute_cli` in a single batch invocation, and compares each result
 * byte-for-byte with the recorded golden string. The summary is reported as-is:
 * nothing is sorted, re-ordered or normalised before the comparison.
 *
 * Usage: npm run precompute_verify
 *
 * `--dump-inputs [path]` (default build/precompute_tsan_inputs.txt) writes a
 * representative 6-fixture production-tier subset covering every combination of
 * (mode in {tap, DAS}) x (reactionTime in {0, 8, 20}) for the TSan replay harness instead
 * of running the full verification: a count line, then two lines per request —
 * the PRECOMPUTE pipe string, then the recorded golden output with every
 * newline escaped to a \x01 sentinel (pipe strings never contain newlines or
 * \x01). This mode does not invoke `build/precompute_cli`.
 */
const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const FIXTURES_PATH = path.join(__dirname, "data/precompute_fixtures.jsonl");
const PRECOMPUTE_CLI_BIN = path.join(__dirname, "../../../build/precompute_cli");
const DEFAULT_DUMP_PATH = path.join(__dirname, "../../../build/precompute_tsan_inputs.txt");
const PIECE_LOOKUP = ["I", "O", "L", "J", "T", "S", "Z"];
const MAX_DIFF_EXAMPLES = 5;
const MAX_STDOUT_BUFFER = 1 << 28; // 256 MiB

function buildPipeString(record) {
  const curPieceIndex = PIECE_LOOKUP.indexOf(record.currentPiece);
  if (curPieceIndex === -1) {
    throw new Error(`Unknown piece: ${record.currentPiece}`);
  }
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

/** Classifies the first line where the expected and actual outputs diverge. */
function categorize(expectedOutput, actualOutput) {
  if (/^Error:/.test(actualOutput)) {
    return "cpp error";
  }
  if (expectedOutput === "none" || actualOutput === "none") {
    return expectedOutput === actualOutput ? null : "whole-response none vs content";
  }

  const expectedLines = expectedOutput.split("\n");
  const actualLines = actualOutput.split("\n");
  if (expectedLines[0] !== actualLines[0]) {
    return "default line";
  }
  for (let i = 1; i < Math.max(expectedLines.length, actualLines.length); i++) {
    if (expectedLines[i] !== actualLines[i]) {
      const expectedLine = expectedLines[i] || "";
      const actualLine = actualLines[i] || "";
      const fallbackRe = /^([IOLJTSZ]):(continue|none)$/;
      if (fallbackRe.test(expectedLine) !== fallbackRe.test(actualLine)) {
        return `piece ${expectedLine.slice(0, 1)} fallback vs content`;
      }
      if (fallbackRe.test(expectedLine) && fallbackRe.test(actualLine)) {
        return `piece ${expectedLine.slice(0, 1)} fallback marker`;
      }
      return `piece ${expectedLine.slice(0, 1)} line`;
    }
  }
  return null;
}

function loadFixtures() {
  const raw = fs.readFileSync(FIXTURES_PATH, "utf8").trim().split("\n");
  return raw.slice(1).map((line) => JSON.parse(line));
}

function runPrecomputeBatch(pipeStrings) {
  if (pipeStrings.length === 0) {
    return [];
  }
  for (let i = 0; i < pipeStrings.length; i++) {
    const pipe = pipeStrings[i];
    if (typeof pipe !== "string" || !pipe || /[\r\n\x01]/.test(pipe)) {
      throw new Error(`Invalid precompute pipe string at index ${i}`);
    }
  }
  const stdout = execFileSync(PRECOMPUTE_CLI_BIN, [], {
    input: pipeStrings.join("\n") + "\n",
    encoding: "utf8",
    maxBuffer: MAX_STDOUT_BUFFER,
  });
  if (!stdout.endsWith("\n")) {
    throw new Error("precompute_cli returned truncated stdout (missing trailing newline)");
  }
  const lines = stdout
    .slice(0, -1)
    .split("\n")
    .map((line) => (line.endsWith("\r") ? line.slice(0, -1) : line));
  if (lines.length !== pipeStrings.length) {
    throw new Error(
      `precompute_cli returned ${lines.length} lines for ${pipeStrings.length} requests`
    );
  }
  return lines.map((line) => line.replace(/\x01/g, "\n"));
}

/**
 * Writes a diverse 6-fixture production-tier subset covering all
 * (mode in {tap, DAS}) x (reactionTime in {0, 8, 20}) buckets for the TSan replay harness.
 */
function dumpInputs(outputPath) {
  const production = loadFixtures().filter((fixture) => fixture.tier === "production");
  const selected = [];
  for (const mode of ["tap", "das"]) {
    for (const reactionTime of [0, 8, 20]) {
      const match = production.find(
        (fixture) =>
          (mode === "tap" ? fixture.dasCharge === -1 : fixture.dasCharge >= 0) &&
          fixture.reactionTime === reactionTime &&
          fixture.output !== "none"
      );
      if (!match) {
        throw new Error(
          `dumpInputs: missing production fixture for mode=${mode}, reactionTime=${reactionTime}`
        );
      }
      selected.push(match);
    }
  }
  const lines = [String(selected.length)];
  for (const fixture of selected) {
    lines.push(buildPipeString(fixture));
    lines.push(fixture.output.replace(/\n/g, "\x01"));
  }
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, lines.join("\n") + "\n");
  console.log(
    `Dumped ${selected.length} representative production-tier precompute requests to ${outputPath}`
  );
}

function main() {
  const dumpArg = process.argv.indexOf("--dump-inputs");
  if (dumpArg !== -1) {
    dumpInputs(process.argv[dumpArg + 1] || DEFAULT_DUMP_PATH);
    return;
  }

  const fixtures = loadFixtures();
  console.log(`Precompute verify: ${fixtures.length} fixtures\n`);

  const started = Date.now();
  const pipeStrings = fixtures.map(buildPipeString);
  const actualOutputs = runPrecomputeBatch(pipeStrings);

  let matches = 0;
  const categories = new Map();
  const examples = [];

  fixtures.forEach((fixture, index) => {
    const actualOutput = actualOutputs[index];
    const expectedOutput = fixture.output;

    if (actualOutput === expectedOutput) {
      matches++;
      return;
    }

    const category = categorize(expectedOutput, actualOutput) || "unclassified";
    categories.set(category, (categories.get(category) || 0) + 1);
    if (examples.length < MAX_DIFF_EXAMPLES) {
      examples.push({ index, fixture, expectedOutput, actualOutput, category });
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
      console.log("--- Golden output ---");
      console.log(example.expectedOutput);
      console.log("--- Actual output ---");
      console.log(example.actualOutput);
      console.log("");
    }
  } else {
    console.log("All fixtures byte-identical.");
  }

  process.exitCode = mismatches === 0 ? 0 : 1;
}

if (require.main === module) {
  main();
}

module.exports = { runPrecomputeBatch, buildPipeString, PIECE_LOOKUP };
