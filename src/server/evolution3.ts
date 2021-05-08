/**
 * A grid search / gradient descent algorithm for choosing the best parameter set.
 */

import {
  getEmptyBoard,
  getPieceSequence,
  hasToppedOut,
  REWARDS,
} from "./lite_game_simulator";
import { getBestMove } from "./main";
import { cloneBoard, logBoard } from "./utils";

const { getParams, getParamMods } = require("./params");

// Hyperparameters
const NUM_DIFFERING_STATES = 300;
const TEST_MAX_LINES = 10; // How many lines to play forward from differing states

function getMove(state: LiteGameState, aiParams: InitialAiParams) {
  const currentPieceId = state.pieceSequence[state.pieceIndex];
  const nextPieceId = state.pieceSequence[state.pieceIndex + 1];

  return getBestMove(
    {
      board: state.board,
      currentPieceId,
      nextPieceId: nextPieceId,
      level: state.level,
      lines: state.lines,
      framesAlreadyElapsed: 0,
      canFirstFrameShift: false,
      existingXOffset: 0,
      existingYOffset: 0,
      existingRotation: 0,
    },
    /* shouldLog= */ false,
    aiParams,
    getParamMods(),
    "X....",
    /* searchDepth= */ 2,
    /* hypotheticalSearchDepth= */ 0
  );
}

/** Updates the game state in-place for the result of one move. */
function advanceState(state: LiteGameState, move: Possibility) {
  // Set the board to the resulting board after making that move
  if (move == null) {
    state.gameOver = true;
    return;
  }
  state.board = move.boardAfter;
  state.numHoles = move.numHoles;

  const numLinesCleared = move.numLinesCleared;
  // console.log(`Cleared ${numLinesCleared} lines`);
  if (numLinesCleared > 0) {
    // Update lines, level, then score (needs to be that order)
    state.lines += numLinesCleared;
    if (state.lines >= state.nextTransitionLineCount) {
      state.level++;
      state.nextTransitionLineCount += 10;
    }
    state.score += REWARDS[numLinesCleared] * (state.level + 1);
  }

  // Check for game over, or increment and loop
  state.gameOver = hasToppedOut(state.board);
  state.pieceIndex++;
}

/** Function to clone a state instead of using the slow JSON methods. */
function cloneState(state: LiteGameState): LiteGameState {
  return {
    board: cloneBoard(state.board),
    score: state.score,
    lines: state.lines,
    level: state.level,
    numHoles: state.numHoles,
    nextTransitionLineCount: state.nextTransitionLineCount,
    pieceSequence: state.pieceSequence,
    pieceIndex: state.pieceIndex,
    gameOver: state.gameOver,
  };
}

function calculateReward(
  numTetrises: number,
  numBurns: number,
  numHoles: number,
  isGameOver: boolean
) {
  if (isGameOver) {
    return -300000 + numBurns * 5000 + numTetrises * 20000;
  }
  return numHoles * -10000 + numBurns * -5000 + numTetrises * 20000;
}

function continueGame(startState: LiteGameState, aiParams: InitialAiParams) {
  const state = cloneState(startState);
  const initialNumHoles = state.numHoles;
  const initialLines = state.lines;
  let numTetrises = 0;
  let numBurns = 0;
  for (let i = 0; i < 10000; i++) {
    // Place one piece
    const baseMove: Possibility = getMove(state, aiParams);

    advanceState(state, baseMove);

    // Track the Tetrises and burns for the reward
    if (baseMove.numLinesCleared === 4) {
      numTetrises += 1;
    } else if (baseMove.numLinesCleared > 0) {
      numBurns += baseMove.numLinesCleared;
    }

    if (state.lines - initialLines >= TEST_MAX_LINES || state.gameOver) {
      break;
    }
  }
  const reward = calculateReward(
    numTetrises,
    numBurns,
    state.numHoles - initialNumHoles,
    state.gameOver
  );
  return reward;
}

function abTest(baseParams: InitialAiParams, expParams: InitialAiParams) {
  const differingStates: Set<LiteGameState> = new Set();

  while (differingStates.size < NUM_DIFFERING_STATES) {
    const state: LiteGameState = {
      board: getEmptyBoard(),
      score: 0,
      lines: 0,
      level: 18,
      numHoles: 0,
      nextTransitionLineCount: 130,
      pieceSequence: getPieceSequence(),
      pieceIndex: 0,
      gameOver: false,
    };

    for (let i = 0; !state.gameOver && i < 10000; i++) {
      // Place one piece
      const baseMove: PossibilityChain = getMove(state, baseParams);

      if (expParams !== null) {
        const expMove: PossibilityChain = expParams
          ? getMove(state, expParams)
          : null;
        if (expMove.placement.toString() !== baseMove.placement.toString()) {
          // Add to the set of differing states
          differingStates.add(cloneState(state));
          console.log(`Found ${differingStates.size} differing states!`);
          // logBoard(state.board);
          // console.log(`Placing: ${state.pieceSequence[state.pieceIndex]}, ${state.pieceSequence[state.pieceIndex+1]}`);
          // logBoard(baseMove.boardAfter);
          // console.log(baseMove.evalExplanation);
          // logBoard(expMove.boardAfter);
          // console.log(expMove.evalExplanation);
        }

        if (differingStates.size >= NUM_DIFFERING_STATES) {
          break;
        }
      }

      advanceState(state, baseMove);
    }
    console.log(
      `Game over: score ${state.score}, lines ${state.lines}, level ${state.level}`
    );
  }

  // Now play out the differing states
  let baseTotal = 0;
  let expTotal = 0;
  for (const differingState of differingStates) {
    const rewardBase = continueGame(differingState, baseParams);
    const rewardExp = continueGame(differingState, expParams);
    baseTotal += rewardBase;
    expTotal += rewardExp;
    console.log("EXP - BASE:", (rewardExp - rewardBase) / 1000);
  }
  console.log(`Base average: ${baseTotal / NUM_DIFFERING_STATES}`);
  console.log(`Exp average: ${expTotal / NUM_DIFFERING_STATES}`);
  console.log(`Diff: ${(expTotal - baseTotal) / NUM_DIFFERING_STATES}`);
}

const EXP_PARAMS = {
  BURN_COEF: -5,
  SURFACE_COEF: 1,
  AVG_HEIGHT_EXPONENT: 1.36000000000004,
  AVG_HEIGHT_COEF: -4.50624,
  SCARE_HEIGHT_OFFSET: -2,
  HOLE_COEF: -30,
  COL_10_COEF: -2, // changed due to feature changing
  COL_10_HEIGHT_MULTIPLIER_EXP: 3,
  TETRIS_COEF: 28.248,
  TETRIS_READY_COEF: 5.909760000000001,
  MAX_DIRTY_TETRIS_HEIGHT: 0.15, // (As a multiple of the scare height) Added manually since didn't exist at time of training
  EXTREME_GAP_COEF: -3,
  BUILT_OUT_LEFT_COEF: 1.5, // changed due to feature changing
  BUILT_OUT_RIGHT_COEF: 0, // Added manually since didn't exist at time of training
  HOLE_WEIGHT_COEF: 0,
  SPIRE_HEIGHT_EXPONENT: 1.215999999999999, // changed due to feature changing
  SPIRE_HEIGHT_COEF: -1.1556000000000002, // changed due to feature changing
  UNABLE_TO_BURN_COEF: -1, // changed due to feature changing
  UNABLE_TO_BURN_DIFF_EXP: 1.5,
  HIGH_COL_9_COEF: -1.5,
  HIGH_COL_9_EXP: 2,
  LEFT_SURFACE_COEF: 0,
  INACCESSIBLE_LEFT_COEF: -30, // Added manually since didn't exist at time of training
  INACCESSIBLE_RIGHT_COEF: -100, // Added manually since didn't exist at time of training
};

abTest(getParams(), EXP_PARAMS);
