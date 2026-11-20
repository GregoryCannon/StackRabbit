/**
 * Battery item: boots the C++ live game server on the battery-dedicated port
 * 3101 (never 3000/3001, so this can never collide with a dev server or a
 * manually-run live server), runs the differential harness against it
 * (src/cpp_modules/tests/live_server_verify.js — representative precompute golden
 * responses plus protocol, error, backup-halt, and synchronous-route checks
 * over HTTP), kills the server process group, and verifies the port is freed.
 *
 * Fails loudly with actionable text if the port is already occupied, the
 * server fails to start, the harness finds a mismatch, or the port is not
 * freed after shutdown.
 */
const { spawn } = require("child_process");
const net = require("net");
const path = require("path");

const PORT = 3101;
const SERVER_BIN = path.join(__dirname, "../../../build/live_game_server");
const HARNESS_PATH = path.join(__dirname, "live_server_verify.js");
const LISTEN_PATTERN = "listening on 127.0.0.1:" + PORT;
const STARTUP_TIMEOUT_MS = 15000;
const KILL_GRACE_MS = 3000;
const PORT_FREE_TIMEOUT_MS = 10000;
const PORT_FREE_POLL_MS = 300;

function fail(message) {
  console.error("\n[live_server_check] FAIL: " + message);
  process.exit(1);
}

function isPortFree(port) {
  return new Promise((resolve) => {
    const server = net.createServer();
    server.once("error", () => resolve(false));
    server.once("listening", () => server.close(() => resolve(true)));
    server.listen(port);
  });
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function killProcessGroup(child) {
  try {
    process.kill(-child.pid, "SIGTERM");
  } catch (err) {
    if (err.code !== "ESRCH") throw err;
  }
}

async function killProcessGroupAndWait(child) {
  killProcessGroup(child);
  const deadline = Date.now() + KILL_GRACE_MS;
  while (Date.now() < deadline) {
    try {
      process.kill(-child.pid, 0);
    } catch (err) {
      if (err.code === "ESRCH") return true;
      throw err;
    }
    await sleep(100);
  }
  try {
    process.kill(-child.pid, "SIGKILL");
  } catch (err) {
    if (err.code !== "ESRCH") throw err;
  }
  return false;
}

async function verifyPortFreed() {
  const deadline = Date.now() + PORT_FREE_TIMEOUT_MS;
  while (Date.now() < deadline) {
    if (await isPortFree(PORT)) return;
    await sleep(PORT_FREE_POLL_MS);
  }
  fail(
    "port " + PORT + " is still occupied after killing the server process group " +
      "(pid " + childPid + "). Something survived the SIGTERM/SIGKILL. Find it with: " +
      "lsof -ti :" + PORT + " | xargs kill"
  );
}

let childPid = null;

async function runHarness() {
  return new Promise((resolve, reject) => {
    const harness = spawn(process.execPath, [HARNESS_PATH, "--port", String(PORT)], {
      stdio: "inherit",
    });
    harness.on("error", reject);
    harness.on("exit", (code, signal) => resolve({ code, signal }));
  });
}

async function main() {
  if (!(await isPortFree(PORT))) {
    fail(
      "port " + PORT + " is ALREADY OCCUPIED before live_server_check spawned anything. " +
        "The battery port must be free to boot the live game server. Kill the squatter with: " +
        "lsof -ti :" + PORT + " | xargs kill -- then re-run."
    );
  }

  const child = spawn(SERVER_BIN, [String(PORT)], {
    detached: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  childPid = child.pid;
  if (!child.pid) {
    fail("failed to spawn " + SERVER_BIN + " (did `npm run live_server_build` run?): " +
      (child.error ? child.error.message : "unknown error"));
  }

  let output = "";
  let sawListening = false;
  let sawBindFailure = false;
  let exited = null;

  const onStreamData = (chunk) => {
    output += chunk;
    if (output.length > 200000) output = output.slice(-100000);
    process.stdout.write(chunk);
    if (output.includes(LISTEN_PATTERN)) sawListening = true;
    if (/Address already in use/.test(output)) sawBindFailure = true;
  };
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", onStreamData);
  child.stderr.on("data", onStreamData);

  child.on("exit", (code, signal) => {
    exited = { code, signal };
  });

  process.on("SIGINT", () => {
    killProcessGroup(child);
    process.exit(130);
  });
  process.on("SIGTERM", () => {
    killProcessGroup(child);
    process.exit(143);
  });

  const deadline = Date.now() + STARTUP_TIMEOUT_MS;
  while (Date.now() < deadline) {
    if (sawListening) break;
    if (sawBindFailure) {
      await killProcessGroupAndWait(child);
      fail(
        "the server failed to bind port " + PORT + " even though the port looked free " +
          "just before spawn — something grabbed it in between (or a stale server raced us). " +
          "Check with: lsof -i :" + PORT
      );
    }
    if (exited !== null) {
      fail(
        "the server exited before printing \"" + LISTEN_PATTERN + "\" " +
          "(code=" + exited.code + ", signal=" + exited.signal + "). Output above; the most recent " +
          "lines usually name the cause. Try `npm run live_server_build && ./build/live_game_server " +
          PORT + "` manually to see the full error."
      );
    }
    await sleep(100);
  }

  if (!sawListening) {
    await killProcessGroupAndWait(child);
    fail(
      "the server never printed \"" + LISTEN_PATTERN + "\" within " + (STARTUP_TIMEOUT_MS / 1000) +
        "s. Output above. Either startup is hung/slow or the message moved — check " +
        "src/cpp_modules/src/live_game_server.cpp."
    );
  }

  let harnessResult;
  try {
    harnessResult = await runHarness();
  } finally {
    const graceless = await killProcessGroupAndWait(child);
    if (!graceless) {
      console.log("\n[live_server_check] SIGTERM insufficient; SIGKILLed the server process group");
    }
    await verifyPortFreed();
  }

  if (harnessResult.code !== 0) {
    fail(
      "the differential harness exited with code " + harnessResult.code + " (signal=" +
        harnessResult.signal + "). Its report is above: any non-byte-identical golden, hard " +
        "failure, or failed error-path assertion fails the battery. Re-run it standalone with: " +
        "node src/cpp_modules/tests/live_server_verify.js --port " + PORT + " (server must be running)."
    );
  }

  console.log(
    "\n[live_server_check] OK: server reached \"" + LISTEN_PATTERN + "\", the harness verified " +
      "representative precompute goldens and HTTP routes, the server was killed, " +
      "and port " + PORT + " is free."
  );
}

main().catch((err) => fail(err && err.stack ? err.stack : String(err)));
