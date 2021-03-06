import { getBestMove } from "./main";
import { getParamMods, getParams, IS_DROUGHT_MODE } from "./params";
import { getPieceSequence, getRandomPiece } from "./piece_rng";
import { predictSearchStateAtAdjustmentTime } from "./precompute";
import { POSSIBLE_NEXT_PIECES } from "./utils";

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
const REACTION_TIME_FRAMES = 18;
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
        /* shouldAdjust= */ true,
        /* isDig= */ false,
        /* maxLines= */ 230,
        /* onPlacementCallback= */ null,
        /* shouldLog= */ false
      )
    );
  }
  return results;
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
export function simulateGame(
  startingLevel,
  startingBoard,
  aiParams,
  paramMods,
  inputFrameTimeline: string,
  presetPieceSequence,
  shouldAdjust,
  isDig,
  maxLines,
  afterPlacementCallback,
  shouldLog
) {
  let board = startingBoard;
  let score = 0;
  let lines = 0;
  let level = startingLevel;
  let numHoles = utils.getHoleCount(startingBoard);
  let pieceIndex = 0;
  const pieceSequence = presetPieceSequence
    ? presetPieceSequence
    : getPieceSequence();
  let gameOver = false;
  let i = 0;

  while (!gameOver && i < 2000) {
    const currentPieceId = pieceSequence[pieceIndex];
    const nextPieceId = pieceSequence[pieceIndex + 1];

    // await sleep(1000);

    // Place one piece
    const searchState = {
      board: board,
      currentPieceId,
      nextPieceId,
      level,
      lines,
      framesAlreadyElapsed: 0,
      reactionTime: REACTION_TIME_FRAMES,
      canFirstFrameShift: false,
      existingXOffset: 0,
      existingYOffset: 0,
      existingRotation: 0,
    };
    const bestMove = getMoveThisStep(
      searchState,
      aiParams,
      paramMods,
      inputFrameTimeline,
      shouldAdjust
    );

    // Set the board to the resulting board after making that move
    if (bestMove == null) {
      gameOver = true;
      if (afterPlacementCallback !== null) {
        afterPlacementCallback(board, true);
      }
      continue;
    }
    board = bestMove.boardAfter;
    numHoles = bestMove.numHoles;
    lines = bestMove.searchStateAfterMove.lines;
    level = bestMove.searchStateAfterMove.level;
    if (bestMove.numLinesCleared > 0) {
      score += REWARDS[bestMove.numLinesCleared] * (level + 1);
    }

    // Check for game over via various conditions
    gameOver =
      hasToppedOut(board) || (isDig && numHoles === 0) || lines >= maxLines;

    // Call the post-placement callback if needed
    if (afterPlacementCallback !== null) {
      afterPlacementCallback(board, gameOver);
    }

    // Maybe log per-piece stats
    if (false) {
      console.log(searchState.currentPieceId);
      utils.logBoard(board);
      console.log(`Score: ${score}, Lines: ${lines}, Level: ${level}`);
    }

    i++;
    pieceIndex++;
  }

  // Maybe log post-game stats
  if (shouldLog) {
    console.log(
      `GAME OVER - Score: ${score}, Lines: ${lines}, Level: ${level}`
    );
    utils.logBoard(board);
  }

  return [score, lines, level, numHoles];
}

function getMoveThisStep(
  searchState: SearchState,
  aiParams: AiParams,
  paramMods: ParamMods,
  inputFrameTimeline: string,
  shouldAdjust: boolean
) {
  if (shouldAdjust) {
    const initalMove = getBestMove(
      {
        ...searchState,
      },
      /* shouldLog= */ false,
      aiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 1
    );
    if (initalMove == null) {
      return null;
    }
    const newSearchState = predictSearchStateAtAdjustmentTime(
      searchState,
      initalMove.inputSequence,
      inputFrameTimeline
    );
    newSearchState.nextPieceId = searchState.nextPieceId;
    const adjustment = getBestMove(
      newSearchState,
      /* shouldLog= */ false,
      aiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 2,
      /* hypotheticalSearchDepth= */ 1
    );
    // console.log("INITIAL MOVE:", initalMove.placement);
    // console.log("ADJUSTMENT:", adjustment.placement);
    return adjustment || initalMove;
  } else {
    return getBestMove(
      searchState,
      /* shouldLog= */ false,
      aiParams,
      paramMods,
      inputFrameTimeline,
      /* searchDepth= */ 1,
      /* hypotheticalSearchDepth= */ 1
    );
  }
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
    /* shouldAdjust= */ false,
    /* isDig= */ false,
    /* maxLines= */ 230,
    null,
    /* shouldLog= */ true
  );

  console.log(result);
}

if (typeof require !== "undefined" && require.main === module) {
  // regressionTest();
  runScoreExperiment(5);
  // const results = simulateManyGames(100, 18, getParams(), getParamMods());
  // console.log(results);
  // console.log(results.map((x) => x[1]));
}
