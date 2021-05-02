import { getBestMove } from "./main";

const main = require("./main");
const utils = require("./utils");
const boardHelper = require("./board_helper");
const NUM_ROW = utils.NUM_ROW;
const NUM_COLUMN = utils.NUM_COLUMN;
const SquareState = utils.SquareState;
export const REWARDS = {
  1: 40,
  2: 100,
  3: 300,
  4: 1200,
};
const INPUT_SEQUENCE_12_HZ = "X....";
const paramsManager = require("./params");
export const DIG_LINE_CAP = 25;

export function simulateManyGames(
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

    // Simulate one game
    results.push(
      simulateGame(
        startingLevel,
        getEmptyBoard(),
        aiParams,
        paramMods,
        INPUT_SEQUENCE_12_HZ,
        /* predefinedPieceSequence= */ null,
        /* isDig= */ false,
        /* onPlacementCallback= */ null,
        /* shouldLog= */ false
      )
    );
  }
  return results;
}

export function simulateDigPractice(
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
        INPUT_SEQUENCE_12_HZ,
        /* predefinedPieceSequence= */ null,
        /* isDig= */ true,
        /* onPlacementCallback= */ null,
        /* shouldLog= */ i == 0
      )
    );
  }
  // console.log(results);
  return results;
}

/** Runs trials on the killscreen and logs down how good various surfaces are for the left-most 3 columns. */
function simulateKillscreenTraining(numIterations) {
  const ranks: Map<string, Array<number>> = new Map();

  const aiParams = paramsManager.getParams();
  const MAX_4_TAP_HEIGHT = boardHelper.calculateTapHeight(
    29,
    aiParams.TAP_ARR,
    aiParams.FIRST_TAP_DELAY,
    4
  );
  console.log("height: ", MAX_4_TAP_HEIGHT);

  for (let i = 0; i < numIterations; i++) {
    console.log(`Iteration ${i + 1} of ${numIterations}`);
    const history = [];
    const afterPlacementCallback = (board) => {
      const leftSurface = boardHelper.getLeftSurface(
        board,
        MAX_4_TAP_HEIGHT + 3
      );
      if (leftSurface !== null) {
        history.push(leftSurface);
      }
    };

    // Play out one game
    simulateGame(
      29,
      getEmptyBoard(),
      paramsManager.getParams(),
      paramsManager.getParamMods(),
      INPUT_SEQUENCE_12_HZ,
      /* presetSequence= */ null,
      /* isDig= */ false,
      afterPlacementCallback,
      /* shouldLog= */ true
    );

    // Process the history
    console.log(history);
    if (history.length === 0) {
      continue;
    }
    history.reverse(); // We'll work back to front
    let lastAdded = null;
    // Score each surface based on how many clean surfaces succeeded it
    for (let t = 0; t < history.length; t++) {
      const surface = history[t];
      // Don't repeatedly add for long sequences of the same surface
      if (surface === lastAdded) {
        continue;
      }
      if (!ranks.has(surface)) {
        ranks.set(surface, []);
      }
      // Add the score to the list of scores for that surface
      ranks.get(surface).push(t);
      lastAdded = surface;
    }
  }
  console.log(ranks);
  const avgRanks = {};
  ranks.forEach((scoreList, surface) => {
    if (scoreList.length == 0) {
      return;
    }
    let total = scoreList.reduce((a, b) => a + b);
    const avgScore = total / scoreList.length;
    avgRanks[surface] = avgScore;
  });
  console.log(avgRanks);
}

function sleep(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
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
  inputFrameTimeline: string,
  presetPieceSequence,
  isDig,
  afterPlacementCallback,
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

    // await sleep(1000);

    // Place one piece
    const bestMove: Possibility = getBestMove(
      {
        board: board,
        currentPieceId,
        nextPieceId,
        level,
        lines,
        framesAlreadyElapsed: 0,
        canFirstFrameShift: false,
        existingXOffset: 0,
        existingYOffset: 0,
        existingRotation: 0,
      },
      /* shouldLog= */ false,
      aiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 0
    );

    // Set the board to the resulting board after making that move
    if (bestMove == null) {
      gameOver = true;
      continue;
    }
    board = bestMove.boardAfter;
    numHoles = bestMove.numHoles;

    const numLinesCleared = bestMove.numLinesCleared;
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

    // Call the post-placement callback if needed
    if (afterPlacementCallback !== null) {
      afterPlacementCallback(board);
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
export function hasToppedOut(board) {
  for (let row = 0; row < 1; row++) {
    for (let col = 3; col < 7; col++) {
      if (board[row][col]) {
        return true;
      }
    }
  }
  return false;
}

export function getEmptyBoard() {
  const board = []; // All board changes are in-place, so it is a const
  for (let r = 0; r < NUM_ROW; r++) {
    board[r] = [];
    for (let c = 0; c < NUM_COLUMN; c++) {
      board[r][c] = SquareState.EMPTY;
    }
  }
  return board;
}

export function getPieceSequence() {
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
  const resultList = simulateManyGames(
    numTrials,
    18,
    paramsManager.getParams(),
    paramsManager.getParamMods()
  );
  const scores = resultList.map((x) => x[0]);
  const only1_3s = scores.filter((x) => x > 1300000);
  console.log(resultList);
  // console.log("1.3 count:", only1_3s.length);
  // console.log("\n1.3s:", only1_3s);
  console.log("Average: ", scores.reduce((a, b) => a + b) / scores.length);
  // console.log("\nScores:\n" + resultList.map((x) => x[0]).join("\n"));
}

function regressionTest() {
  console.log("Running regression test...");

  const regressionTestPieceSequence =
    "STZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJOSTZILJO";
  const result = simulateGame(
    18,
    getEmptyBoard(),
    paramsManager.getParams(),
    paramsManager.getParamMods(),
    INPUT_SEQUENCE_12_HZ,
    regressionTestPieceSequence,
    /* isDig= */ false,
    null,
    /* shouldLog= */ true
  );

  console.log(result);
}

if (typeof require !== "undefined" && require.main === module) {
  // regressionTest();
  // runScoreExperiment(100);
  // simulateKillscreenTraining(500);
  runScoreExperiment(1000);
}
