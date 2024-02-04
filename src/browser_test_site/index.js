const canvasContainer = document.getElementById("canvas-container");
const canvas = document.getElementById("canvas");
const resultContainer = document.getElementById("results-container");
const lineDisplay = document.getElementById("line-display");

async function getRenderedMiniBoard(boardStr) {
    const squareSize = 10;
    // Create drawing canvas
    canvas.width = 10 * squareSize;
    canvas.height = 20 * squareSize;

    const drawContext = canvas.getContext("2d");
    drawContext.fillStyle = "#eee";
    drawContext.fillRect(0, 0, 10 * squareSize, 20 * squareSize);
    drawContext.fillStyle = "#444";
    for (let r = 0; r < 20; r++) {
        for (let c = 0; c < 10; c++) {
        if (boardStr[r*10+c] === "1") {
            drawContext.fillRect(
            c * squareSize + 0.5,
            r * squareSize + 0.5,
            squareSize - 0.5,
            squareSize - 0.5,
            );
        }
      }
    }

    return canvas;
}

const TRANSITIONS = [
    [2, 10, 12, 10, 10, 10, 10],
    [12, 2, 10, 10, 10, 10, 10],
    [10, 12, 2, 10, 10, 10, 10],
    [10, 10, 10, 4, 10, 10, 10],
    [10, 10, 10, 10, 4, 10, 10],
    [12, 10, 10, 10, 10, 2, 10],
    [10, 10, 10, 10, 12, 10, 2],
  ];

function getRandomPiece(prevPieceIndex) {
  let rand = Math.floor(Math.random() * 64);
  for (let i = 0; i < 7; i++) {
    const chance = TRANSITIONS[prevPieceIndex][i];
    if (rand < chance) {
      return i;
    }
    rand -= chance;
  }
}

function sleep(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

async function simulateGame(showBoardUi){
  // Initial Setup
  let boardStr = "";
  for (let i = 0; i < 200; i++) {
    boardStr += "0";
  }
  let curPieceIndex = Math.floor(Math.random() * 7);
  let nextPieceIndex = getRandomPiece(curPieceIndex);
  let level = 18;
  let lines = 0;

  while (true){
    const PIECES = ['I', 'O', 'L', 'J', 'T', 'S', 'Z'];
    const reqStr = `http://localhost:3000/get-move-cpp?board=${boardStr}&currentPiece=${PIECES[curPieceIndex]}&nextPiece=${PIECES[nextPieceIndex]}&level=${level}&lines=${lines}&inputFrameTimeline=X.&playoutCount=49&playoutLength=2`;

    // console.log(reqStr);
    const result = await fetch(reqStr).then(x => x.text());

    // Check for death
    if (result === "No legal moves"){
      await sleep(300);
      return lines;
    }
    // console.log(result);
    const [_, __, boardAfter, levelAfter, linesAfter] = result.split("|");
    if (showBoardUi){
      await getRenderedMiniBoard(boardAfter);
    }
    // await sleep(200);
    lineDisplay.innerHTML = linesAfter;

    // Prepare for next placement
    boardStr = boardAfter;
    level = levelAfter;
    lines = linesAfter;
    curPieceIndex = nextPieceIndex;
    nextPieceIndex = getRandomPiece(curPieceIndex);
  }
}

async function simulateGames(){
  let lineCounts = [];
  for (let i = 0; i < 100; i++){
    const result = await simulateGame(/* showBoardUi= */ false);
    lineCounts.push(result);
    resultContainer.innerHTML += "<br/>" + result;
  }

  // Print results
  console.log("RESULTS:");
  for (const count of lineCounts){
    console.log(count);
  }
}

document.addEventListener('keydown', function(event){
  if (event.key == "Enter"){
    alert("Paused");
  }
} );

// Entry point
simulateGames();