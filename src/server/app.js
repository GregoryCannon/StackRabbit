const http = require("http");
const hostname = "127.0.0.1";
const port = 3000;

const mainApp = require("./main");
const params = require("./params");
const evolution = require("./evolution");

/**
 * Create HTTP server for the frontend to request
 */
const server = http.createServer((req, res) => {
  res.statusCode = 200;
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Content-Type", "text/plain");

  let [
    _,
    boardSerialized,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
  ] = req.url.split("/");
  // Check for bad inputs
  if (!["I", "O", "L", "J", "T", "S", "Z"].includes(currentPieceStr)) {
    return res.end("bad next piece:" + currentPieceStr);
  }

  // Get the list of possible moves
  const startingBoard = JSON.parse(boardSerialized.replace(/2|3/g, "1")); // Cleanse color data (1/2/3) to just 1s

  const bestMove = mainApp.getMove(
    startingBoard,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
    /* shouldLog= */ true,
    params.getParams()
  );

  res.end("" + bestMove);
});

server.listen(port, hostname, () => {
  console.log(`Server running at http://${hostname}:${port}/`);
});
