/**
 * Differential runner for the C++ live game server's HTTP surface.
 *
 * Reads the golden precompute fixtures (production tier), builds each
 * PRECOMPUTE pipe string exactly like precompute_verify.js does, drives the
 * RUNNING server over HTTP — one /precompute kick, then /async-result polls —
 * and byte-compares the full result with the recorded output. Also asserts the
 * error path once per run: a junk pipe must surface as a 500 with a message,
 * and the next valid kick must clear it.
 *
 * The poll loop waits for the FULL result: a 200 whose body is backup-shaped
 * (`Default:...` + exactly 7 `<piece>:continue` lines, or the whole-body
 * `none`) is recorded as a backup observation and the polling continues until
 * the body equals the golden (byte-identity is the gate; a full result can
 * share the backup's exact shape) or is otherwise not backup-shaped. The 504
 * "Still calculating" transient (the window before the backup lands) is
 * retried within the timeout; 500 and 404 are sticky states that can never
 * resolve by retrying, so they fail immediately.
 *
 * Usage: node src/server/live_server_verify.js [--host <h>] [--port <p>]
 * Host/port also come from LIVE_SERVER_HOST / LIVE_SERVER_PORT; default
 * 127.0.0.1:3001 (the battery passes --port 3101).
 */
const fs = require("fs");
const http = require("http");
const path = require("path");

const FIXTURES_PATH = path.join(__dirname, "data/precompute_fixtures.jsonl");
const PIECE_LOOKUP = ["I", "O", "L", "J", "T", "S", "Z"];
const MAX_DIFF_EXAMPLES = 5;
const POLL_INTERVAL_MS = 10;
const POLL_TIMEOUT_MS = 30000; // full results take tens of ms; this is ~1000x headroom
const REQUEST_TIMEOUT_MS = 10000;
const JUNK_PIPE = "junk|pipe|that|fails|validation|x";

function fail(message) {
  console.error("\n[live_server_verify] FAIL: " + message);
  process.exit(1);
}

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

// True when the body is the backup (partial-result) response shape rather
// than a full result carrying placements: either the whole-body `none` (no
// legal placement) or `Default:...` followed by exactly seven
// `<piece>:continue` markers. A full result can share this exact shape when
// every next piece keeps the default (fixture 41's golden does), so the poll
// loop treats golden-equality as terminal alongside this classification.
function isBackupShaped(body) {
  if (body === "none") {
    return true;
  }
  const lines = body.split("\n");
  if (lines.length !== 8 || !lines[0].startsWith("Default:")) {
    return false;
  }
  return lines.slice(1).every((line) => /^[IOLJTSZ]:continue$/.test(line));
}

/** Classifies the first line where the two outputs diverge. */
function categorize(goldenOutput, actualOutput) {
  if (/^Error:/.test(actualOutput)) {
    return "server error";
  }
  if (goldenOutput === "none" || actualOutput === "none") {
    return goldenOutput === actualOutput ? null : "whole-response none vs content";
  }

  const goldenLines = goldenOutput.split("\n");
  const actualLines = actualOutput.split("\n");
  if (goldenLines[0] !== actualLines[0]) {
    return "default line";
  }
  for (let i = 1; i < Math.max(goldenLines.length, actualLines.length); i++) {
    if (goldenLines[i] !== actualLines[i]) {
      const goldenLine = goldenLines[i] || "";
      const actualLine = actualLines[i] || "";
      const fallbackRe = /^([IOLJTSZ]):(continue|none)$/;
      if (fallbackRe.test(goldenLine) !== fallbackRe.test(actualLine)) {
        return `piece ${goldenLine.slice(0, 1)} fallback vs content`;
      }
      if (fallbackRe.test(goldenLine) && fallbackRe.test(actualLine)) {
        return `piece ${goldenLine.slice(0, 1)} fallback marker`;
      }
      return `piece ${goldenLine.slice(0, 1)} line`;
    }
  }
  return null;
}

function get(host, port, requestPath) {
  return new Promise((resolve, reject) => {
    const req = http.get(
      { host, port, path: requestPath, agent: false },
      (res) => {
        let body = "";
        res.setEncoding("utf8");
        res.on("data", (chunk) => (body += chunk));
        res.on("end", () => resolve({ status: res.statusCode, body }));
      }
    );
    req.setTimeout(REQUEST_TIMEOUT_MS, () => {
      req.destroy(new Error(`request timed out after ${REQUEST_TIMEOUT_MS}ms`));
    });
    req.on("error", reject);
  });
}

// Kicks one request and polls /async-result until the full result is in:
// the loop exits when the body equals the golden (byte-identity is the gate —
// note a full result can share the backup's exact shape, e.g. an
// all-seven-`continue` response) or when it is no longer backup-shaped.
// Returns the final response plus how many backup-shaped 200s were seen.
async function kickAndAwaitFull(host, port, pipe, golden) {
  const kick = await get(host, port, "/precompute?p=" + encodeURIComponent(pipe));
  if (kick.status !== 200 || kick.body !== "Request accepted.") {
    return {
      error: `kick returned ${kick.status} ${JSON.stringify(kick.body)} (expected 200 "Request accepted.")`,
    };
  }

  const deadline = Date.now() + POLL_TIMEOUT_MS;
  let backupObservations = 0;
  let lastBackupBody = null;
  for (;;) {
    let response;
    try {
      response = await get(host, port, "/async-result");
    } catch (err) {
      return { error: `poll failed: ${err.message}` };
    }
    if (response.status === 504) {
      // Transient "Still calculating": the backup has not landed yet. Keep
      // polling within the timeout — never past it.
      if (Date.now() > deadline) {
        return { error: `timed out waiting for the full result (last status 504)` };
      }
      await new Promise((resolve) => setTimeout(resolve, POLL_INTERVAL_MS));
      continue;
    }
    if (response.status !== 200) {
      // 500 and 404 are sticky slot states; retrying cannot change them.
      return { error: `poll returned ${response.status}: ${JSON.stringify(response.body)}` };
    }
    if (response.body === golden) {
      return { response, backupObservations, lastBackupBody };
    }
    if (isBackupShaped(response.body)) {
      backupObservations++;
      lastBackupBody = response.body;
      if (Date.now() > deadline) {
        return {
          error: `timed out waiting for the full result; still backup-shaped: ${JSON.stringify(response.body)}`,
        };
      }
      await new Promise((resolve) => setTimeout(resolve, POLL_INTERVAL_MS));
      continue;
    }
    return { response, backupObservations, lastBackupBody };
  }
}

// The error path: validation happens on the worker thread, after the kick has
// been answered, so a junk pipe is accepted at /precompute and must surface as
// a 500 + message at /async-result (the slot's sticky error state).
async function expectEngineRejection(host, port, junkPipe) {
  const kick = await get(host, port, "/precompute?p=" + encodeURIComponent(junkPipe));
  if (kick.status !== 200 || kick.body !== "Request accepted.") {
    fail(`junk-pipe kick returned ${kick.status} ${JSON.stringify(kick.body)} (expected 200 "Request accepted.")`);
  }

  const deadline = Date.now() + POLL_TIMEOUT_MS;
  for (;;) {
    const response = await get(host, port, "/async-result");
    if (response.status === 504) {
      if (Date.now() > deadline) {
        fail("junk pipe: timed out waiting for the 500 (last status 504)");
      }
      await new Promise((resolve) => setTimeout(resolve, POLL_INTERVAL_MS));
      continue;
    }
    if (response.status !== 500) {
      fail(
        `junk pipe: expected /async-result to answer 500 with a message, got ` +
          `${response.status} ${JSON.stringify(response.body)}`
      );
    }
    if (response.body.length === 0) {
      fail("junk pipe: the 500 response carried no message");
    }
    return response.body;
  }
}

function loadProductionFixtures() {
  const raw = fs.readFileSync(FIXTURES_PATH, "utf8").trim().split("\n");
  return raw.slice(1).map((line) => JSON.parse(line)).filter((f) => f.tier === "production");
}

function main() {
  const argv = process.argv.slice(2);
  let host = process.env.LIVE_SERVER_HOST || "127.0.0.1";
  let port = process.env.LIVE_SERVER_PORT || "3001";
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--host") {
      host = argv[++i];
    } else if (argv[i] === "--port") {
      port = argv[++i];
    } else {
      fail(`unknown argument: ${argv[i]} (usage: [--host <h>] [--port <p>])`);
    }
  }
  if (!host || !port) {
    fail("both --host and --port must be non-empty");
  }

  const fixtures = loadProductionFixtures();
  if (fixtures.length === 0) {
    fail("no production-tier fixtures found in " + FIXTURES_PATH);
  }
  console.log(
    `Live server differential verify: ${fixtures.length} production fixtures against ${host}:${port}\n`
  );

  run(host, port, fixtures).catch((err) => fail(err && err.stack ? err.stack : String(err)));
}

async function run(host, port, fixtures) {
  const mismatches = [];
  const hardFailures = [];
  const categories = new Map();
  const examples = [];
  let matches = 0;
  let backupObservations = 0;
  let fixturesThatSawBackup = 0;
  const started = Date.now();

  // Error path first: a junk pipe is accepted at the kick (validation happens
  // on the worker thread) and must surface as a 500 with a message; the next
  // valid kick — fixture 0's, verified below — must clear it.
  console.log("Error-path check: junk pipe ...");
  const errorMessage = await expectEngineRejection(host, port, JUNK_PIPE);
  console.log("  500 with message: " + JSON.stringify(errorMessage));
  console.log("");

  for (const [index, fixture] of fixtures.entries()) {
    const pipe = buildPipeString(fixture);
    const result = await kickAndAwaitFull(host, port, pipe, fixture.output);

    if (result.error) {
      hardFailures.push({ index, fixture, message: result.error });
      break; // a sticky slot state (500/404) would poison every later poll
    }
    backupObservations += result.backupObservations;
    if (result.backupObservations > 0) {
      fixturesThatSawBackup++;
    }

    if (result.response.body === fixture.output) {
      matches++;
      continue;
    }

    const category = categorize(fixture.output, result.response.body) || "unclassified";
    mismatches.push({ index, fixture, category, actual: result.response.body });
    categories.set(category, (categories.get(category) || 0) + 1);
    if (examples.length < MAX_DIFF_EXAMPLES) {
      examples.push({ index, fixture, category, actual: result.response.body });
    }
  }

  const total = fixtures.length;
  const verified = matches + mismatches.length;
  console.log(`Byte-identical: ${matches}/${total} (${((matches / total) * 100).toFixed(1)}%)`);
  console.log(`Mismatches:     ${mismatches.length}`);
  console.log(`Hard failures:  ${hardFailures.length}`);
  console.log(
    `Backup observations: ${backupObservations} backup-shaped polls across ` +
      `${fixturesThatSawBackup}/${total} fixtures (the backup tier answering before the full result)`
  );
  console.log(`Elapsed:        ${((Date.now() - started) / 1000).toFixed(1)}s\n`);

  if (mismatches.length > 0) {
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
        `Fixture ${example.index} [${example.category}] piece=${f.currentPiece}` +
          ` level=${f.level} lines=${f.lines} rt=${f.reactionTime} das=${f.dasCharge}`
      );
      console.log("--- Golden output ---");
      console.log(f.output);
      console.log("--- Server output ---");
      console.log(example.actual);
      console.log("");
    }
  } else if (verified === total) {
    console.log("All fixtures byte-identical over HTTP.");
  }

  for (const failure of hardFailures) {
    console.log("=".repeat(72));
    console.log(`Fixture ${failure.index}: ${failure.message}`);
  }

  process.exitCode = matches === total && hardFailures.length === 0 ? 0 : 1;
}

main();
