/** An insight into what the odds are of having frequent enough bars to have a perfect game */

import { getPieceSequence } from "../lite_game_simulator";

/** Runs a simulation and returns the number of lines it was capable of playing perfect. */
function runSimulation() {
  let lines = 0;
  let cellsInStack = 0;
  const MAX_CELLS_IN_STACK = 9 * 12 + 8 * 2 + 23; // A full stack
  const CELLS_FOR_TETRIS = 9 * 4;
  const pieceSequence = getPieceSequence();

  for (const pieceId of pieceSequence) {
    // Win and lose conditions
    if (cellsInStack >= MAX_CELLS_IN_STACK || lines >= 230) {
      return lines;
    }

    // Update state after each piece
    if (pieceId === "I" && cellsInStack > CELLS_FOR_TETRIS) {
      cellsInStack -= 9 * 4;
      lines += 4;
    } else {
      cellsInStack += 4;
    }
  }
}

function runTrials(numTrials) {
  let successes = 0;
  for (let i = 0; i < numTrials; i++) {
    const result = runSimulation();
    if (result >= 230) {
      successes += 1;
    }
    // console.log(i, result);
  }
  console.log(
    `After ${numTrials} simulations, found ${successes} games where it may be possible to play perfect.`
  );
}

runTrials(10000);
