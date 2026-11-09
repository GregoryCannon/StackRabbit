const { spawn } = require("child_process");
const net = require("net");

const PORT = 3000;
const LISTEN_PATTERN = "Listening on port " + PORT;
const STARTUP_TIMEOUT_MS = 15000;
const KILL_GRACE_MS = 3000;
const PORT_FREE_TIMEOUT_MS = 10000;
const PORT_FREE_POLL_MS = 300;

function fail(message) {
  console.error("\n[start_check] FAIL: " + message);
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

async function main() {
  if (!(await isPortFree(PORT))) {
    fail(
      "port " + PORT + " is ALREADY OCCUPIED before start_check spawned anything. " +
        "A stale server (likely a long-running dev server holding the OLD node-gyp addon in memory) " +
        "is squatting on the port. Kill it yourself with: lsof -ti :" + PORT + " | xargs kill -- " +
        "then re-run. start_check deliberately does not auto-kill: the loud failure is the point."
    );
  }

  const child = spawn(process.execPath, ["built/app.js"], {
    detached: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  childPid = child.pid;
  if (!child.pid) {
    fail("failed to spawn `node built/app.js`: " + (child.error ? child.error.message : "unknown error"));
  }

  let output = "";
  let sawListening = false;
  let sawEaddrinuse = false;
  let exited = null;

  const onStreamData = (chunk) => {
    output += chunk;
    if (output.length > 200000) output = output.slice(-100000);
    process.stdout.write(chunk);
    if (output.includes(LISTEN_PATTERN)) sawListening = true;
    if (/EADDRINUSE/.test(output)) sawEaddrinuse = true;
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
    if (sawEaddrinuse) {
      await killProcessGroupAndWait(child);
      fail(
        "the server hit EADDRINUSE binding port " + PORT + " even though the port looked free " +
          "just before spawn — something grabbed it in between (or a stale server raced us). " +
          "Check with: lsof -i :" + PORT
      );
    }
    if (exited !== null) {
      fail(
        "the server exited before printing \"" + LISTEN_PATTERN + "\" " +
          "(code=" + exited.code + ", signal=" + exited.signal + "). Output above; the most recent " +
          "lines usually name the cause. If the addon built but the process still died, run " +
          "`npm start` manually to see the full error."
      );
    }
    await sleep(100);
  }

  if (!sawListening) {
    await killProcessGroupAndWait(child);
    fail(
      "the server never printed \"" + LISTEN_PATTERN + "\" within " + (STARTUP_TIMEOUT_MS / 1000) +
        "s. Output above. Either startup is hung/slow or the message moved — check src/server/app.ts."
    );
  }

  const graceless = await killProcessGroupAndWait(child);
  if (!graceless) {
    console.log("\n[start_check] SIGTERM insufficient; SIGKILLed the server process group");
  }
  await verifyPortFreed();

  console.log("\n[start_check] OK: server reached \"" + LISTEN_PATTERN + "\", was killed, " +
    "and port " + PORT + " is free.");
}

main().catch((err) => fail(err && err.stack ? err.stack : String(err)));
