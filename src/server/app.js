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

  const startTime = Date.now();

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
  console.log(req.url);

  // Decode the board from the URL
  boardSerialized = boardSerialized.replace(/2|3/g, "1"); // Cleanse color data (1/2/3) to just 1s
  // console.log(boardSerialized.match(/.{1,10}/g).join("\n"));
  const startingBoard = boardSerialized
    .match(/.{1,10}/g)
    .map((rowSerialized) => rowSerialized.split(""));

  // Get the best move
  const bestMove = mainApp.getMove(
    startingBoard,
    currentPieceStr,
    nextPieceStr,
    level,
    lines,
    /* shouldLog= */ false,
    params.getParams()
  );

  const msElapsed = Date.now() - startTime;
  console.log("Best move:", bestMove.slice(0,2))
  console.log("Calculation time (ms):", msElapsed)
  if (!bestMove) {
    return res.end("No legal moves");
  }

  res.end(bestMove[0] + "," + bestMove[1]);
});

server.listen(port, hostname, () => {
  console.log(`Server running at http://${hostname}:${port}/`);
});
