/**
 * Differential runner for the C++ live game server's HTTP surface.
 *
 * Selects 3 representative golden precompute fixtures dynamically from
 * precompute_fixtures.jsonl (production tap, production DAS, and whole-response
 * `none`), drives the RUNNING server over HTTP — one /precompute kick, then
 * /async-result polls — and byte-compares each result with its recorded golden.
 * Also verifies:
 *   - error paths (junk pipe and `|` delimiter -> 500, cleared by next valid kick),
 *   - backup-and-halt cancellation (early /async-result poll serves backup,
 *     halts a 500ms computation quickly, and /precompute-sync returns 503 Busy
 *     while the async job is running),
 *   - /precompute-sync (byte-identical to golden, 400 on missing p, 500 on junk/`|`),
 *   - /get-move-ab-test (valid JSON with formatted placement + explanations for
 *     11- and 12-field pipes, 400 on missing p, 500 on junk/`|`).
 *
 * The poll loop waits for the FULL result: because serving a backup response at
 * /async-result halts the in-flight computation on the server, kickAndAwaitFull
 * waits INITIAL_FULL_WAIT_MS before polling (re-kicking with exponential backoff
 * if a slow fixture was still in its backup tier when polled). A dedicated
 * backup-and-halt check verifies that polling /async-result early returns a
 * backup-shaped response and halts a 500ms computation within milliseconds so
 * the slot stays on the backup and immediately accepts the next /precompute.
 *
 * Usage: node src/tests/live_server_verify.js [--host <h>] [--port <p>]
 * Host/port also come from LIVE_SERVER_HOST / LIVE_SERVER_PORT; default
 * 127.0.0.1:3000 (the battery passes --port 3101).
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

function buildPipeString(record, delimiter = ",", { includeReactionTime = true } = {}) {
  const curPieceIndex = PIECE_LOOKUP.indexOf(record.currentPiece);
  const fields = [
    record.board,
    record.level,
    record.lines,
    curPieceIndex,
    record.nextPiece !== undefined ? PIECE_LOOKUP.indexOf(record.nextPiece) : -1,
    record.inputFrameTimeline,
    record.playoutCount,
    record.playoutLength,
    record.pruningBreadth,
    record.aggressionLevel,
    record.dasCharge,
  ];
  if (includeReactionTime) {
    fields.push(record.reactionTime);
  }
  return fields.join(delimiter) + delimiter;
}

// True when the body is the backup (partial-result) response shape rather
// than a full result carrying placements: either the whole-body `none` (no
// legal placement) or `Default:...` followed by exactly seven
// `<piece>:continue` markers. A full result can share this exact shape when
// every next piece keeps the default, so the poll loop treats golden-equality
// as terminal alongside this classification.
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

  // While the async precompute is RUNNING, /precompute-sync must return 503 Busy
  // so two 7-thread precomputes never oversubscribe the machine.
  const syncBusy = await get(host, port, "/precompute-sync?p=" + buildPipeString(fixture));
  if (syncBusy.status !== 503 || syncBusy.body !== "Busy") {
    fail(
      `backup-halt check: expected /precompute-sync to return 503 "Busy" while async job runs, ` +
        `got ${syncBusy.status} ${JSON.stringify(syncBusy.body)}`
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

  // Wait up to 200ms (still well short of the 500ms deadline) for all worker
  // threads to finish unwinding and clear the RUNNING state so the next
  // synchronous or async request never flakes with 503 Busy.
  const unwindDeadline = Date.now() + 200;
  const probePipe = buildPipeString(fixture);
  for (;;) {
    const probe = await get(host, port, "/precompute-sync?p=" + probePipe);
    if (probe.status === 200 && probe.body === fixture.output) {
      break;
    }
    if (probe.status !== 503) {
      fail(
        `backup-halt check: expected /precompute-sync to return 200 (or transient 503) after ` +
          `halt, got ${probe.status} ${JSON.stringify(probe.body)}`
      );
    }
    if (Date.now() > unwindDeadline) {
      fail("backup-halt check: worker stayed 503 Busy for >200ms after backup halt");
    }
    await sleep(5);
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

function assertValidFormattedPlacement(placement, label) {
  if (placement === "none") {
    return;
  }
  if (typeof placement !== "string") {
    fail(`${label}: expected placement string, got ${typeof placement}`);
  }
  const parts = placement.split("|");
  if (parts.length !== 6) {
    fail(`${label}: expected 6 '|'-delimited placement fields, got ${parts.length} in ${JSON.stringify(placement)}`);
  }
  if (!/^-?\d+,-?\d+,-?\d+$/.test(parts[0])) {
    fail(`${label}: malformed placement coordinates ${JSON.stringify(parts[0])}`);
  }
  if (parts[1].length === 0) {
    fail(`${label}: empty input sequence in ${JSON.stringify(placement)}`);
  }
  if (!/^[01]{200}$/.test(parts[2])) {
    fail(`${label}: expected 200-char 0/1 board, got length ${parts[2].length}`);
  }
  for (const [idx, name] of [[3, "level"], [4, "lines"], [5, "dasCharge"]]) {
    if (!/^-?\d+$/.test(parts[idx])) {
      fail(`${label}: malformed ${name} field ${JSON.stringify(parts[idx])}`);
    }
  }
}

function assertValidAbTestJson(body, label) {
  let parsed;
  try {
    parsed = JSON.parse(body);
  } catch (err) {
    fail(`${label}: response is not valid JSON (${err.message}): ${JSON.stringify(body)}`);
  }
  if (!Array.isArray(parsed) || (parsed[0] !== "agree" && parsed[0] !== "disagree")) {
    fail(`${label}: expected ["agree"|"disagree", ...], got ${JSON.stringify(body)}`);
  }
  if (parsed[0] === "agree") {
    if (parsed.length !== 3) {
      fail(`${label}: agree response must have 3 elements, got ${parsed.length}`);
    }
    assertValidFormattedPlacement(parsed[1], `${label} main placement`);
    if (typeof parsed[2] !== "object" || parsed[2] === null || typeof parsed[2].explanationMain !== "string") {
      fail(`${label}: agree response missing explanationMain string in ${JSON.stringify(parsed[2])}`);
    }
  } else {
    if (parsed.length !== 4) {
      fail(`${label}: disagree response must have 4 elements, got ${parsed.length}`);
    }
    assertValidFormattedPlacement(parsed[1], `${label} main placement`);
    assertValidFormattedPlacement(parsed[2], `${label} test placement`);
    if (
      typeof parsed[3] !== "object" ||
      parsed[3] === null ||
      typeof parsed[3].explanationMain !== "string" ||
      typeof parsed[3].explanationTest !== "string"
    ) {
      fail(`${label}: disagree response missing explanation strings in ${JSON.stringify(parsed[3])}`);
    }
  }
  return parsed;
}

async function verifyPrecomputeSyncRoute(host, port, fixture) {
  const validPipe = buildPipeString(fixture);
  const ok = await get(host, port, "/precompute-sync?p=" + validPipe);
  if (ok.status !== 200) {
    fail(`/precompute-sync valid request returned ${ok.status}: ${JSON.stringify(ok.body)}`);
  }
  if (ok.body !== fixture.output) {
    fail(`/precompute-sync valid request mismatched golden on fixture 0`);
  }

  const missingP = await get(host, port, "/precompute-sync");
  if (missingP.status !== 400) {
    fail(`/precompute-sync missing p: expected 400, got ${missingP.status} ${JSON.stringify(missingP.body)}`);
  }

  const junk = await get(host, port, "/precompute-sync?p=" + encodeURIComponent(JUNK_PIPE));
  if (junk.status !== 500 || junk.body.length === 0) {
    fail(`/precompute-sync junk pipe: expected 500 + message, got ${junk.status} ${JSON.stringify(junk.body)}`);
  }

  const pipeDelim = await get(
    host,
    port,
    "/precompute-sync?p=" + encodeURIComponent(buildPipeString(fixture, "|"))
  );
  if (pipeDelim.status !== 500 || pipeDelim.body.length === 0) {
    fail(`/precompute-sync '|' delimiter: expected 500 + message, got ${pipeDelim.status} ${JSON.stringify(pipeDelim.body)}`);
  }
}

async function verifyGetMoveAbTestRoute(host, port, fixture) {
  const abRecord = {
    ...fixture,
    playoutCount: 4,
    playoutLength: 2,
    pruningBreadth: 4,
    dasCharge: -1,
  };
  const pipe11 = buildPipeString(abRecord, ",", { includeReactionTime: false });
  const pipe12 = buildPipeString(abRecord, ",", { includeReactionTime: true });

  const res11 = await get(host, port, "/get-move-ab-test?p=" + pipe11);
  if (res11.status !== 200) {
    fail(`/get-move-ab-test (11 fields) returned ${res11.status}: ${JSON.stringify(res11.body)}`);
  }
  assertValidAbTestJson(res11.body, "/get-move-ab-test (11 fields)");

  const res12 = await get(host, port, "/get-move-ab-test?p=" + pipe12);
  if (res12.status !== 200) {
    fail(`/get-move-ab-test (12 fields) returned ${res12.status}: ${JSON.stringify(res12.body)}`);
  }
  assertValidAbTestJson(res12.body, "/get-move-ab-test (12 fields)");
  if (res11.body !== res12.body) {
    fail(`/get-move-ab-test: 11-field and 12-field responses diverged`);
  }

  const missingP = await get(host, port, "/get-move-ab-test");
  if (missingP.status !== 400) {
    fail(`/get-move-ab-test missing p: expected 400, got ${missingP.status} ${JSON.stringify(missingP.body)}`);
  }

  const junk = await get(host, port, "/get-move-ab-test?p=" + encodeURIComponent(JUNK_PIPE));
  if (junk.status !== 500 || junk.body.length === 0) {
    fail(`/get-move-ab-test junk pipe: expected 500 + message, got ${junk.status} ${JSON.stringify(junk.body)}`);
  }

  const pipeDelim = await get(
    host,
    port,
    "/get-move-ab-test?p=" + encodeURIComponent(buildPipeString(abRecord, "|", { includeReactionTime: false }))
  );
  if (pipeDelim.status !== 500 || pipeDelim.body.length === 0) {
    fail(`/get-move-ab-test '|' delimiter: expected 500 + message, got ${pipeDelim.status} ${JSON.stringify(pipeDelim.body)}`);
  }
}

function loadRepresentativeFixtures() {
  const raw = fs.readFileSync(FIXTURES_PATH, "utf8").trim().split("\n");
  const all = raw.slice(1).map((line) => JSON.parse(line));

  const tapFixture = all.find(
    (f) => f.tier === "production" && f.dasCharge === -1 && f.output !== "none"
  );
  if (!tapFixture) {
    fail("no production tap-mode fixture found in " + FIXTURES_PATH);
  }

  const dasFixture = all.find(
    (f) =>
      f.tier === "production" &&
      f.dasCharge >= 0 &&
      f.reactionTime > 0 &&
      f.output !== "none"
  );
  if (!dasFixture) {
    fail("no production DAS-mode fixture with reactionTime > 0 found in " + FIXTURES_PATH);
  }

  const noneFixture =
    all.find((f) => f.tier === "production" && f.output === "none") ||
    all.find((f) => f.output === "none");
  if (!noneFixture) {
    fail("no whole-response 'none' fixture found in " + FIXTURES_PATH);
  }

  // run() relies on fixtures[0] being a non-"none" production fixture for the
  // backup-halt, /precompute-sync, and /get-move-ab-test checks.
  return [tapFixture, dasFixture, noneFixture];
}

function main() {
  const argv = process.argv.slice(2);
  let host = process.env.LIVE_SERVER_HOST || "127.0.0.1";
  let port = process.env.LIVE_SERVER_PORT || "3000";
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

  const fixtures = loadRepresentativeFixtures();
  console.log(
    `Live server differential verify: ${fixtures.length} representative fixtures against ${host}:${port}\n`
  );

  run(host, port, fixtures).catch((err) => fail(err && err.stack ? err.stack : String(err)));
}

async function verifyDebugPrecomputeRoute(host, port, fixture) {
  const pipe = buildPipeString(fixture);
  const ok = await get(host, port, `/debug-precompute?p=${pipe}`);
  if (ok.status !== 200) {
    fail(`/debug-precompute expected 200, got ${ok.status}: ${ok.body}`);
  }
  let parsed;
  try {
    parsed = JSON.parse(ok.body);
  } catch (err) {
    fail(`/debug-precompute returned invalid JSON: ${err.message}`);
  }
  if (
    !Array.isArray(parsed.phantoms) ||
    parsed.phantoms.length === 0 ||
    parsed.response !== fixture.output
  ) {
    fail(`/debug-precompute response mismatched golden or missing phantoms`);
  }

  const missing = await get(host, port, "/debug-precompute");
  if (missing.status !== 400) {
    fail(`/debug-precompute without p expected 400, got ${missing.status}`);
  }

  const junk = await get(host, port, `/debug-precompute?p=${JUNK_PIPE}`);
  if (junk.status !== 500) {
    fail(`/debug-precompute with junk p expected 500, got ${junk.status}`);
  }

  const legacy = await get(
    host,
    port,
    `/debug-precompute?p=${buildPipeString(fixture, "|")}`
  );
  if (legacy.status !== 500) {
    fail(`/debug-precompute with '|' delimiter expected 500, got ${legacy.status}`);
  }
}

async function verifyDebugGroundTruthRoute(host, port, fixture) {
  const pipe = buildPipeString(fixture);
  const ok = await get(host, port, `/debug-ground-truth?p=${pipe}&n=10&conts=2`);
  if (ok.status !== 200) {
    fail(`/debug-ground-truth expected 200, got ${ok.status}: ${ok.body}`);
  }
  let parsed;
  try {
    parsed = JSON.parse(ok.body);
  } catch (err) {
    fail(`/debug-ground-truth returned invalid JSON: ${err.message}`);
  }
  if (
    !Array.isArray(parsed.reachablePlacements) ||
    parsed.reachablePlacements.length === 0 ||
    !Array.isArray(parsed.candidates) ||
    parsed.candidates.length === 0
  ) {
    fail(`/debug-ground-truth JSON missing reachablePlacements or candidates`);
  }

  const missing = await get(host, port, "/debug-ground-truth");
  if (missing.status !== 400) {
    fail(`/debug-ground-truth without p expected 400, got ${missing.status}`);
  }

  const badN = await get(host, port, `/debug-ground-truth?p=${pipe}&n=abc`);
  if (badN.status !== 400) {
    fail(`/debug-ground-truth with bad n expected 400, got ${badN.status}`);
  }

  const okNnb = await get(host, port, `/debug-ground-truth?p=${pipe}&n=10&nnb=1`);
  if (okNnb.status !== 200) {
    fail(`/debug-ground-truth NNB expected 200, got ${okNnb.status}: ${okNnb.body}`);
  }
  let parsedNnb;
  try {
    parsedNnb = JSON.parse(okNnb.body);
  } catch (err) {
    fail(`/debug-ground-truth NNB returned invalid JSON: ${err.message}`);
  }
  if (parsedNnb.isNoNextBox !== true || !Array.isArray(parsedNnb.candidates) || parsedNnb.candidates.length === 0) {
    fail(`/debug-ground-truth NNB JSON missing isNoNextBox or candidates`);
  }

  const badNnb = await get(host, port, `/debug-ground-truth?p=${pipe}&nnb=maybe`);
  if (badNnb.status !== 400) {
    fail(`/debug-ground-truth with bad nnb expected 400, got ${badNnb.status}`);
  }

  const junk = await get(host, port, `/debug-ground-truth?p=${JUNK_PIPE}`);
  if (junk.status !== 500) {
    fail(`/debug-ground-truth with junk p expected 500, got ${junk.status}`);
  }

  const legacy = await get(
    host,
    port,
    `/debug-ground-truth?p=${buildPipeString(fixture, "|")}`
  );
  if (legacy.status !== 500) {
    fail(`/debug-ground-truth with '|' delimiter expected 500, got ${legacy.status}`);
  }
}

async function verifyDebugTournamentRoute(host, port, fixture) {
  const tmtFixture = { ...fixture, pruningBreadth: -1, playoutCount: 1500, playoutLength: 2 };
  const pipe = buildPipeString(tmtFixture);
  const ok = await get(host, port, `/debug-tournament?p=${pipe}&next=J`);
  if (ok.status !== 200) {
    fail(`/debug-tournament expected 200, got ${ok.status}: ${ok.body}`);
  }
  let parsed;
  try {
    parsed = JSON.parse(ok.body);
  } catch (err) {
    fail(`/debug-tournament returned invalid JSON: ${err.message}`);
  }
  if (
    parsed.nextPiece !== "J" ||
    parsed.isNoNextBox !== false ||
    !Array.isArray(parsed.candidates) ||
    parsed.candidates.length === 0
  ) {
    fail(`/debug-tournament JSON missing nextPiece or candidates`);
  }

  const okNnb = await get(host, port, `/debug-tournament?p=${pipe}&next=NNB`);
  if (okNnb.status !== 200) {
    fail(`/debug-tournament NNB expected 200, got ${okNnb.status}: ${okNnb.body}`);
  }
  let parsedNnb;
  try {
    parsedNnb = JSON.parse(okNnb.body);
  } catch (err) {
    fail(`/debug-tournament NNB returned invalid JSON: ${err.message}`);
  }
  if (
    parsedNnb.nextPiece !== "NNB" ||
    parsedNnb.isNoNextBox !== true ||
    parsedNnb.useTieredR1 !== false ||
    !Array.isArray(parsedNnb.candidates) ||
    parsedNnb.candidates.length === 0 ||
    parsedNnb.candidates.some((c) => !Array.isArray(c.conts) || c.conts.length !== 1 || c.conts[0].contKey !== "none")
  ) {
    fail(`/debug-tournament NNB JSON failed 1-ply invariants`);
  }

  const missingP = await get(host, port, "/debug-tournament?next=J");
  if (missingP.status !== 400) {
    fail(`/debug-tournament without p expected 400, got ${missingP.status}`);
  }

  const missingNext = await get(host, port, `/debug-tournament?p=${pipe}`);
  if (missingNext.status !== 400) {
    fail(`/debug-tournament without next expected 400, got ${missingNext.status}`);
  }

  const badNext = await get(host, port, `/debug-tournament?p=${pipe}&next=X`);
  if (badNext.status !== 400) {
    fail(`/debug-tournament with invalid next expected 400, got ${badNext.status}`);
  }

  const legacyPipe = buildPipeString({ ...fixture, pruningBreadth: 20 });
  const legacyRes = await get(host, port, `/debug-tournament?p=${legacyPipe}&next=J`);
  if (legacyRes.status !== 500) {
    fail(`/debug-tournament with non-tournament pruningBreadth expected 500, got ${legacyRes.status}`);
  }
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
  // check and the representative raw-comma fixtures below) must clear the error state.
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

  console.log("Backup-and-halt check (500ms timed request halted on backup poll; /precompute-sync 503 Busy) ...");
  await verifyBackupHaltsComputation(host, port, fixtures[0]);
  console.log("  200 backup served, /precompute-sync 503 Busy verified, and in-flight search halted.");

  console.log("Synchronous route check: /precompute-sync ...");
  await verifyPrecomputeSyncRoute(host, port, fixtures[0]);
  console.log("  200 byte-identical, 400 missing p, 500 junk & '|' delimiter.");

  console.log("Synchronous route check: /get-move-ab-test ...");
  await verifyGetMoveAbTestRoute(host, port, fixtures[0]);
  console.log("  200 valid JSON (11 & 12 fields), 400 missing p, 500 junk & '|' delimiter.");

  console.log("Synchronous route check: /debug-precompute ...");
  await verifyDebugPrecomputeRoute(host, port, fixtures[0]);
  console.log("  200 valid JSON (response byte-identical), 400 missing p, 500 junk & '|' delimiter.");

  console.log("Synchronous route check: /debug-ground-truth ...");
  await verifyDebugGroundTruthRoute(host, port, fixtures[0]);
  console.log("  200 valid JSON, 400 missing p / bad n, 500 junk & '|' delimiter.");

  console.log("Synchronous route check: /debug-tournament ...");
  await verifyDebugTournamentRoute(host, port, fixtures[0]);
  console.log("  200 valid JSON, 400 missing p / bad next, 500 legacy pruningBreadth.");

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
