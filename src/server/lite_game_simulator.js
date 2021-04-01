const mainApp = require("./main");
const utils = require("./utils");
const NUM_ROW = utils.NUM_ROW;
const NUM_COLUMN = utils.NUM_COLUMN;
const SquareState = utils.SquareState;
const REWARDS = {
  1: 40,
  2: 100,
  3: 300,
  4: 1200,
};
const {
  getParams,
  getParamMods,
} = require("./params");
const DIG_LINE_CAP = 25;

function simulateManyGames(numIterations, startingLevel, aiParams, paramMods) {
  const results = [];
  for (let i = 0; i < numIterations; i++) {
    // Progress indicator
    if (((i / numIterations) * 100) % 5 == 0) {
      console.log(`${(i / numIterations) * 100}% complete`);
    }

    // Simulate one game
    results.push(
      simulateGame(
        startingLevel,
        getEmptyBoard(),
        aiParams,
        paramMods,
        /* predefinedPieceSequence= */ null,
        /* isDig= */ false,
        /* shouldLog= */ false
      )
    );
  }
  return results;
}

function simulateDigPractice(
  numIterations,
  startingLevel,
  aiParams,
  paramMods
) {
  const results = [];
  for (let i = 0; i < numIterations; i++) {
    // Progress indicator
    if (((i / numIterations) * 100) % 5 == 0) {
      console.log(`${(i / numIterations) * 100}% complete`);
    }

    // Simulate a game with a dirty starting board and capped lines
    results.push(
      simulateGame(
        startingLevel,
        utils.generateDigPracticeBoard(5, 6),
        aiParams,
        paramMods,
        /* predefinedPieceSequence= */ null,
        /* isDig= */ true,
        /* shouldLog= */ i == 0
      )
    );
  }
  // console.log(results);
  return results;
}

/**
 * Run one simulation of a game.
 * @param {function(lines, numHoles)} gameOverCondition - function to check custom game over conditions
 * @returns [score, lines, level]
 */
function simulateGame(
  startingLevel,
  startingBoard,
  aiParams,
  paramMods,
  presetPieceSequence,
  isDig,
  shouldLog
) {
  let board = startingBoard;
  let score = 0;
  let lines = 0;
  let level = startingLevel;
  let numHoles = utils.getHoleCount(startingBoard);
  let nextTransitionLineCount =
    startingLevel == 19 ? 140 : startingLevel == 18 ? 130 : 200;
  let pieceIndex = 0;
  const pieceSequence = presetPieceSequence
    ? presetPieceSequence
    : getPieceSequence();
  let gameOver = false;
  let i = 0;

  while (!gameOver && i < 10000) {
    const currentPieceId = pieceSequence[pieceIndex];
    const nextPieceId = pieceSequence[pieceIndex + 1];

    // Place one piece
    const bestMove = mainApp.getBestMoveWithSearch(
      board,
      currentPieceId,
      nextPieceId,
      level,
      lines,
      /* existingXOffset= */ 0,
      /* existingYOffset= */ 0,
      /* firstShiftDelay= */ 0,
      /* existingRotation= */ 0,
      /* shouldLog= */ false,
      aiParams,
      paramMods
    );

    // Set the board to the resulting board after making that move
    if (bestMove == null) {
      gameOver = true;
      continue;
    }
    board = bestMove[5];
    numHoles = bestMove[3];

    const numLinesCleared = bestMove[4];
    if (numLinesCleared > 0) {
      // Update lines, level, then score (needs to be that order)
      lines += numLinesCleared;
      if (lines >= nextTransitionLineCount) {
        level++;
        nextTransitionLineCount += 10;
        if (shouldLog) {
          console.log(`TRANSITIONING TO LEVEL ${level}`);
        }
      }
      score += REWARDS[numLinesCleared] * (level + 1);
    }

    if (false) {
      console.log(`Score: ${score}, Lines: ${lines}, Level: ${level}`);
      utils.logBoard(board);
    }

    // Check for game over, or increment and loop
    gameOver =
      hasToppedOut(board) ||
      (isDig && (numHoles === 0 || lines >= DIG_LINE_CAP));
    i++;
    pieceIndex++;
  }
  if (shouldLog) {
    console.log(
      `GAME OVER - Score: ${score}, Lines: ${lines}, Level: ${level}`
    );
    utils.logBoard(board);
  }
  return [score, lines, level, numHoles];
}

/**
 * Checks if the player has topped out by scanning the piece spawn
 * area for filled blocks.
 * @param {Array<Array<number>>} board
 */
function hasToppedOut(board) {
  for (let row = 0; row < 1; row++) {
    for (let col = 3; col < 7; col++) {
      if (board[row][col]) {
        return true;
      }
    }
  }
  return false;
}

function getEmptyBoard() {
  const board = []; // All board changes are in-place, so it is a const
  for (let r = 0; r < NUM_ROW; r++) {
    board[r] = [];
    for (let c = 0; c < NUM_COLUMN; c++) {
      board[r][c] = SquareState.EMPTY;
    }
  }
  return board;
}

function getPieceSequence() {
  let sequence = [];
  for (let writeIndex = 0; writeIndex < 2000; writeIndex++) {
    const prevPieceId = writeIndex == 0 ? null : sequence[writeIndex - 1];
    sequence[writeIndex] = getRandomPiece(prevPieceId);
  }
  return sequence;
}

// Get the ID of a random piece, following the original RNG of NES tetris
function getRandomPiece(previousPieceId) {
  const PIECE_LIST = ["I", "O", "L", "J", "T", "S", "Z"];
  // Roll once 0-7, where 7 is a dummy value
  let r = Math.floor(Math.random() * (PIECE_LIST.length + 1));
  const tempPieceId = r !== PIECE_LIST.length ? PIECE_LIST[r] : "";
  if (r == PIECE_LIST.length || tempPieceId === previousPieceId) {
    // Reroll once for repeats (or dummy) to reduce repeated pieces
    r = Math.floor(Math.random() * PIECE_LIST.length);
  }
  return PIECE_LIST[r];
}

function runScoreExperiment(numTrials) {
  const resultList = simulateManyGames(numTrials, 18, getParams());
  const only1_3s = resultList.filter((x) => x[0] > 1300000);
  console.log(resultList);
  console.log("1.3 count:", only1_3s.length);
  console.log("\n1.3s:", only1_3s);
  console.log("\nScores:\n" + resultList.map((x) => x[0]).join("\n"));
}

function regressionTest() {
  console.log("Running regression test...");

  const regressionTestPieceSequence =
    "STZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJO";
  const result = simulateGame(
    18,
    getEmptyBoard(),
    getParams(),
    getParamMods(),
    regressionTestPieceSequence,
    /* isDig= */ false,
    /* shouldLog= */ false
  )

  console.log(result);
  // // Validate result
  // const [score, lines, level] = result;
  // if (score === 1245300 && lines === 266 && level === 32) {
  //   console.log("Test passed!", result);
  // } else {
  //   console.log("Test FAILED!", result);
  // }
}

// regressionTest();
// runScoreExperiment(100);

module.exports = {
  simulateManyGames,
  simulateDigPractice,
  DIG_LINE_CAP,
};
