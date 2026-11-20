const { execFile, spawnSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const isWin = process.platform === "win32";
const buildDir = path.join(root, "build");
const src = path.join(root, "src", "backend", "live_game_server.cpp");
const bin = path.join(buildDir, isWin ? "live_game_server.exe" : "live_game_server");

function exitOnFailure(res) {
  if (res.error) { console.error(res.error.message); process.exit(1); }
  if (res.signal) process.kill(process.pid, res.signal);
  if (res.status !== 0) process.exit(res.status ?? 1);
}

function buildLiveServer() {
  fs.mkdirSync(buildDir, { recursive: true });
  const gppArgs = ["-O2", "-std=c++17", "-pthread", "-o", bin, src, ...(isWin ? ["-lws2_32"] : [])];
  let res = spawnSync("g++", gppArgs, { stdio: "inherit" });
  if (isWin && res.error && res.error.code === "ENOENT") {
    res = spawnSync("cl", ["/O2", "/std:c++17", "/EHsc", `/Fe:${bin}`, `/Fo:${buildDir}${path.sep}`, src, "ws2_32.lib"], { stdio: "inherit" });
  }
  exitOnFailure(res);
}

const [, , cmd, ...args] = process.argv;
if (cmd === "build-live-server") {
  buildLiveServer();
} else if (cmd === "live-server") {
  buildLiveServer();
  exitOnFailure(spawnSync(bin, args, { stdio: "inherit" }));
} else if (cmd === "open" && args[0]) {
  const file = path.resolve(root, args[0]);
  const [opener, openerArgs] =
    process.platform === "darwin" ? ["open", [file]] :
    isWin ? ["cmd", ["/c", "start", "", file]] : ["xdg-open", [file]];
  execFile(opener, openerArgs, (e) => { if (e) { console.error(e.message); process.exit(1); } });
} else {
  console.error("Usage: node scripts/run.js <build-live-server|live-server|open <path>>");
  process.exit(1);
}
