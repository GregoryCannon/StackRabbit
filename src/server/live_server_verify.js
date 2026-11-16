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
 * The poll loop waits for the FULL result: because serving a backup response at
 * /async-result halts the in-flight computation on the server, kickAndAwaitFull
 * waits INITIAL_FULL_WAIT_MS before polling (re-kicking with exponential backoff
 * if a slow fixture was still in its backup tier when polled). A dedicated
 * backup-and-halt check verifies that polling /async-result early returns a
 * backup-shaped response and halts a 500ms computation within milliseconds so
 * the slot stays on the backup and immediately accepts the next /precompute.
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
const INITIAL_FULL_WAIT_MS = 25; // production fixtures finish in ~6-14ms; avoid triggering backup halt
const POLL_INTERVAL_MS = 10;
const POLL_TIMEOUT_MS = 30000; // full results take tens of ms; this is ~1000x headroom
const REQUEST_TIMEOUT_MS = 10000;
const JUNK_PIPE = "junk,pipe,that,fails,validation,x";

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function fail(message) {
  console.error("\n[live_server_verify] FAIL: " + message);
  process.exit(1);
}

function buildPipeString(record, delimiter = ",") {
  const curPieceIndex = PIECE_LOOKUP.indexOf(record.currentPiece);
  return (
    [
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
    ].join(delimiter) + delimiter
  );
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

// Kicks one request and waits for the full result before polling /async-result.
// Because serving a backup response at /async-result halts the in-flight full
// computation on the server, we wait `waitMs` before polling; if a CPU pause
// causes the poll to hit the backup tier anyway (halting that attempt), we let
// the worker finish unwinding and re-kick with a doubled wait.
async function kickAndAwaitFull(host, port, pipe, golden, { encodeQuery = false } = {}) {
  const queryValue = encodeQuery ? encodeURIComponent(pipe) : pipe;
  const deadline = Date.now() + POLL_TIMEOUT_MS;
  let waitMs = INITIAL_FULL_WAIT_MS;
  let backupObservations = 0;
  let lastBackupBody = null;

  for (;;) {
    const kick = await get(host, port, "/precompute?p=" + queryValue);
    if (kick.status !== 200 || kick.body !== "Request accepted.") {
      return {
        error: `kick returned ${kick.status} ${JSON.stringify(kick.body)} (expected 200 "Request accepted.")`,
      };
    }

    await sleep(waitMs);

    for (;;) {
      let response;
      try {
        response = await get(host, port, "/async-result");
      } catch (err) {
        return { error: `poll failed: ${err.message}` };
      }
      if (response.status === 504) {
        if (Date.now() > deadline) {
          return { error: `timed out waiting for the full result (last status 504)` };
        }
        await sleep(POLL_INTERVAL_MS);
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
        // Polling while still in the backup tier halted the in-flight search;
        // wait briefly for the worker to mark DONE and re-kick with a longer wait.
        backupObservations++;
        lastBackupBody = response.body;
        if (Date.now() > deadline) {
          return {
            error: `timed out waiting for the full result; still backup-shaped: ${JSON.stringify(response.body)}`,
          };
        }
        await sleep(5);
        waitMs = Math.min(waitMs * 2, 1000);
        break;
      }
      return { response, backupObservations, lastBackupBody };
    }
  }
}

// Verifies the backup tier and its computation-halt contract: kicks a heavy
// timed precompute (500ms deadline), polls /async-result immediately to trigger
// the backup response, and checks that (1) the response is backup-shaped and
// (2) the 500ms computation halts within 15ms so the slot stays on the backup
// and the subsequent kick is immediately accepted rather than 503 "Busy".
async function verifyBackupHaltsComputation(host, port, fixture) {
  const heavyRecord = {
    ...fixture,
    playoutCount: 500,
    playoutLength: 8,
    pruningBreadth: -2,
  };
  const pipe = buildPipeString(heavyRecord);
  const kick = await get(host, port, "/precompute?p=" + pipe);
  if (kick.status !== 200 || kick.body !== "Request accepted.") {
    fail(
      `backup-halt check: kick returned ${kick.status} ${JSON.stringify(kick.body)} ` +
        `(expected 200 "Request accepted.")`
    );
  }

  const deadline = Date.now() + POLL_TIMEOUT_MS;
  let backupBody = null;
  for (;;) {
    const response = await get(host, port, "/async-result");
    if (response.status === 504) {
      if (Date.now() > deadline) {
        fail("backup-halt check: timed out waiting for backup response (last status 504)");
      }
      await sleep(1);
      continue;
    }
    if (response.status !== 200) {
      fail(
        `backup-halt check: expected 200 backup response, got ${response.status} ` +
          `${JSON.stringify(response.body)}`
      );
    }
    if (!isBackupShaped(response.body)) {
      fail(
        `backup-halt check: expected backup-shaped response on early poll, got ` +
          `${JSON.stringify(response.body)}`
      );
    }
    backupBody = response.body;
    break;
  }

  // Wait 15ms (far short of the 500ms deadline): the worker must have halted
  // and left the backup in place without overwriting it with a full result.
  await sleep(15);
  const afterHalt = await get(host, port, "/async-result");
  if (afterHalt.status !== 200 || afterHalt.body !== backupBody) {
    fail(
      `backup-halt check: expected slot to retain backup response after halt, got ` +
        `${afterHalt.status} ${JSON.stringify(afterHalt.body)}`
    );
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

  // Error paths first: a junk pipe and a legacy `|`-delimited pipe must each
  // surface as a 500 with a message; the next valid kick (fixture 0's `%2C`
  // check and the 42 raw-comma fixtures below) must clear the error state.
  console.log("Error-path check: junk pipe ...");
  const errorMessage = await expectEngineRejection(host, port, JUNK_PIPE);
  console.log("  500 with message: " + JSON.stringify(errorMessage));

  console.log("Error-path check: legacy '|' delimiter rejected ...");
  const pipeDelimiterError = await expectEngineRejection(
    host,
    port,
    buildPipeString(fixtures[0], "|")
  );
  console.log("  500 with message: " + JSON.stringify(pipeDelimiterError));

  console.log("Backup-and-halt check (500ms timed request halted on backup poll) ...");
  await verifyBackupHaltsComputation(host, port, fixtures[0]);
  console.log("  200 backup served and in-flight search halted.");

  console.log("Percent-encoded comma check (fixture 0) ...");
  const encodedResult = await kickAndAwaitFull(
    host,
    port,
    buildPipeString(fixtures[0]),
    fixtures[0].output,
    { encodeQuery: true }
  );
  if (encodedResult.error || encodedResult.response.body !== fixtures[0].output) {
    fail(
      encodedResult.error ||
        `percent-encoded comma check mismatched golden on fixture 0`
    );
  }
  console.log("  200 byte-identical.\n");

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
