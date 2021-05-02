export function AIPlayer(moveLeftFunc, moveRightFunc, rotateRightFunc) {
  this.moveLeftFunc = moveLeftFunc;
  this.moveRightFunc = moveRightFunc;
  this.rotateRightFunc = rotateRightFunc;
  this.linesArray = null; // The array of raw input text in the ranks text file.
  this.totalApiTime = 0;
  this.totalApiCalls = 0;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

AIPlayer.prototype.placeCurrentPiece = async function (
  piece,
  nextPiece,
  board,
  level,
  lines
) {
  // Fix the board if it's in an invalid state (still not sure why this happens but this bandaid fixes it)
  for (let i = 0; i < board.length; i++) {
    const row = board[i];
    if (row.length > 10) {
      console.log(JSON.stringify(board));
      console.log(board);
      // throw new Error("Invalid board!");
      board[i] = row.slice(0, 10);
    }
  }

  const startTime = performance.now();
  // Convert to 1D string of 0s and 1s
  const encodedBoard = board
    .map((row) => row.slice(0, 10).join(""))
    .join("")
    .replace(/2|3/g, "1");
  console.log(board);
  const result = await fetch(
    `http://127.0.0.1:3000/sync-nb/${encodedBoard}/${piece.id}/${nextPiece.id}/${level}/${lines}/0/0/0/0/X..../false`
  );
  const endTime = performance.now();
  this.totalApiCalls++;
  if (this.totalApiCalls > 1) {
    this.totalApiTime += endTime - startTime;
    console.log(
      "Avg API time:",
      this.totalApiTime / (this.totalApiCalls - 1),
      endTime - startTime
    );
  }

  const resultArray = (await result.text()).split(/,|\|/g);
  const bestRotationIndex = parseInt(resultArray[0]);
  const bestXOffset = parseInt(resultArray[1]);

  console.log("best:", bestRotationIndex, bestXOffset);

  // Rotate the piece
  for (let i = 0; i < bestRotationIndex; i++) {
    this.rotateRightFunc();
  }

  // Shift the piece, with intermittent sleeps to slow it down.
  // Note that the piece range it's allowed to attempt is hardcoded in hang_checker.js, so its actual DAS speed is irrelevant
  const targetX = 3 + bestXOffset;
  const sleepDelay = level >= 29 ? 20 : 50;
  if (targetX > piece.getX()) {
    const numShifts = targetX - piece.getX();
    for (let i = 0; i < numShifts; i++) {
      this.moveRightFunc();
      await sleep(sleepDelay);
    }
  } else if (targetX < piece.getX()) {
    const numShifts = piece.getX() - targetX;
    for (let i = 0; i < numShifts; i++) {
      this.moveLeftFunc();
      await sleep(sleepDelay);
    }
  }
};
