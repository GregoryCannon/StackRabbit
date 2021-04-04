const PIECE_LOOKUP = require("../tetrominoes").PIECE_LOOKUP;
const utils = require("./utils");
const NUM_COLUMN = utils.NUM_COLUMN;
const NUM_ROW = utils.NUM_ROW;
const SquareState = utils.SquareState;

// Collision function
function pieceCollision(
  board: Board,
  x: number,
  y: number,
  piece: PieceArray
) {
  for (let r = 0; r < piece.length; r++) {
    for (let c = 0; c < piece[r].length; c++) {
      // If the square is empty, we skip it
      if (!piece[r][c]) {
        continue;
      }
      // Coordinates of the piece after movement
      let newX = c + x;
      let newY = r + y;

      // If out of bounds on left, right or bottom, say it does collide
      if (newX < 0 || newX >= NUM_COLUMN || newY >= NUM_ROW) {
        return true;
      }
      // If over the top of the board, ignore
      if (newY < 0) {
        continue;
      }
      // Check if it overlaps the board
      if (board[newY][newX] != 0) {
        return true;
      }
    }
  }
  return false;
}

/** Clear all filled lines on a board
 * @returns the number of lines cleared
 */
function clearLines(board: any[][]) {
  let fullLines = [];
  for (let r = 0; r < NUM_ROW; r++) {
    let isRowFull = true;
    for (let c = 0; c < NUM_COLUMN; c++) {
      if (board[r][c] == SquareState.EMPTY) {
        isRowFull = false;
        break;
      }
    }
    if (isRowFull) {
      fullLines.push(r);
    }
  }
  for (const r of fullLines) {
    // Move down all the rows above it
    for (let y = r; y > 1; y--) {
      for (let c = 0; c < NUM_COLUMN; c++) {
        board[y][c] = board[y - 1][c];
      }
    }
    // Clear out the very top row (newly shifted into the screen)
    for (let c = 0; c < NUM_COLUMN; c++) {
      board[0][c] = SquareState.EMPTY;
    }
  }
  return fullLines.length;
}

function getBoardAndLinesClearedAfterPlacement(
  board: any,
  currentRotationPiece: string | any[],
  x: number,
  y: number
) {
  let tempBoard = JSON.parse(JSON.stringify(board));
  for (let r = 0; r < currentRotationPiece.length; r++) {
    for (let c = 0; c < currentRotationPiece[r].length; c++) {
      // If the square is empty, we skip it
      if (!currentRotationPiece[r][c]) {
        continue;
      }
      // Coordinates of the piece after movement
      let newX = c + x;
      let newY = r + y;

      // If out of bounds, ignore
      if (newX < 0 || newY < 0 || newX >= NUM_COLUMN || newY >= NUM_ROW) {
        continue;
      }
      // Add to board
      tempBoard[newY][newX] = SquareState.FULL;
    }
  }
  const numLinesCleared = clearLines(tempBoard);

  // utils.logBoard(tempBoard);
  return [tempBoard, numLinesCleared];
}

function _validateIntParam(value: number, min: number, max: number) {
  if (isNaN(value)) {
    throw new Error("Expected a number, but got NaN:" + value);
  }
  if (!(value >= min && value <= max)) {
    throw new Error(
      `Number parameter ${value} was out of range: ${min} to ${max}`
    );
  }
}

/**
 * Generates a list of possible moves, given a board and a piece. It achieves this by
 * placing it in each possible rotation and each possible starting column, and then
 * dropping it into the stack and letting the result play out.
 */
function getPossibleMoves(
  startingBoard: any,
  currentPieceId: string,
  level: any,
  existingXOffset: number,
  existingYOffset: number,
  tapArr: number,
  firstShiftDelay: any,
  existingRotation: number,
  shouldLog: any
) {
  _validateIntParam(level, 0, 999);
  _validateIntParam(existingXOffset, -5, 4);
  _validateIntParam(existingYOffset, 0, 20);
  _validateIntParam(firstShiftDelay, 0, 999);
  _validateIntParam(existingRotation, 0, 4);

  const initialX = 3 + existingXOffset;
  const initialY = (currentPieceId == "I" ? -2 : -1) + existingYOffset;
  const maxGravity = utils.GetGravity(level) - 1; // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const maxArr = tapArr - 1; // Similarly 0-indexed
  const rotationsList = PIECE_LOOKUP[currentPieceId][0];

  const simParams = {
    board: startingBoard,
    initialX,
    initialY,
    firstShiftDelay,
    maxGravity,
    maxArr,
    rotationsList,
    existingRotation,
  };

  const { rangesLeft, rangesRight } = getPieceRanges(currentPieceId, simParams);
  const NUM_ROTATIONS_FOR_PIECE = rangesLeft.length;

  const legalPlacements = [];

  // Loop over the range and validate the moves with more rotations than shifts
  for (
    let rotationIndex = 0;
    rotationIndex < NUM_ROTATIONS_FOR_PIECE;
    rotationIndex++
  ) {
    const rangeLeft = rangesLeft[rotationIndex];
    const rangeRight = rangesRight[rotationIndex];

    const rotationDifference = _modulus(
      rotationIndex - existingRotation,
      NUM_ROTATIONS_FOR_PIECE
    );
    const numRotationInputs = rotationDifference === 3 ? 1 : rotationDifference;

    for (let xOffset = rangeLeft; xOffset <= rangeRight; xOffset++) {
      // The range calculations are always accurate when there are at least as many shifts as rotations,
      // So we know any of those automatically get included.
      if (Math.abs(xOffset) >= numRotationInputs) {
        legalPlacements.push([rotationIndex, xOffset]);
      }
      // Otherwise we have to manually check that specific placement
      else if (placementIsLegal(rotationIndex, xOffset, simParams)) {
        legalPlacements.push([rotationIndex, xOffset]);
      }
    }
  }

  return _generatePossibilityList(
    legalPlacements,
    startingBoard,
    currentPieceId,
    initialX,
    initialY,
    shouldLog
  );
}

function _generatePossibilityList(
  legalPlacements: any[],
  startingBoard: any,
  currentPieceId: string | number,
  startingX: any,
  startingY: any,
  shouldLog: any
) {
  const possibilityList = []; // list of [rotationId, xOffset, columnHeightsStr]

  for (const [rotationIndex, xOffset] of legalPlacements) {
    const currentRotationPiece = PIECE_LOOKUP[currentPieceId][0][rotationIndex];
    const x = startingX + xOffset;
    let y = startingY;

    // Move the piece down until it hits the stack
    while (!pieceCollision(startingBoard, x, y + 1, currentRotationPiece)) {
      y++;
    }

    // Make a new board with that piece locked in
    const [boardAfter, numLinesCleared] = getBoardAndLinesClearedAfterPlacement(
      startingBoard,
      currentRotationPiece,
      x,
      y
    );
    const newSurfaceArray = utils.getSurfaceArray(boardAfter);
    const numHoles = utils.getHoleCount(boardAfter);

    // Add the possibility to the list
    if (shouldLog) {
      console.log(
        `Adding possibility [Index ${rotationIndex}, xOffset ${xOffset}], would make surface ${newSurfaceArray}`
      );
    }
    possibilityList.push([
      rotationIndex,
      xOffset,
      newSurfaceArray,
      numHoles,
      numLinesCleared,
      boardAfter,
    ]);
  }

  if (shouldLog) {
    console.log(
      `Result: ${possibilityList.length} possibilities for ${currentPieceId}`
    );
  }
  return possibilityList;
}

/**
 * Calculates how far in each direction a hypothetical piece can be tapped (for each rotation), given the AI's tap speed and
 * the piece's current position.
 *
 * (!!) NB: It performs one rotation for each shift. So if the placement requires 2 rotations, this result will only be valid for
 * placements with abs(xOffset) >= 2.
 */
function getPieceRanges(
  pieceId: string | number,
  simParams: {
    board: any;
    initialX: any;
    initialY: any;
    firstShiftDelay: any;
    maxGravity: number;
    maxArr: number;
    rotationsList: any;
    existingRotation: any;
  }
) {
  const rotationsList = PIECE_LOOKUP[pieceId][0];

  // Piece ranges, indexed by rotation index
  const rangesLeft = [];
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    rangesLeft.push(repeatedlyShiftPiece(-1, rotationIndex, simParams));
  }
  const rangesRight = [];
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    rangesRight.push(repeatedlyShiftPiece(1, rotationIndex, simParams));
  }
  return { rangesLeft, rangesRight };
}

function getBoardHeightAtColumn(board: Board, col: number) {
  let row = 0;
  while (row < NUM_ROW && board[row][col] == 0) {
    row++;
  }
  return 20 - row;
}

function hasHolesNearTopOfColumn(board: { [x: string]: any }[], col: number) {
  let row = 0;
  while (row < NUM_ROW && board[row][col] === SquareState.EMPTY) {
    row++;
  }
  const HEIGHT_NEAR_TOP = 3;
  for (let i = 0; i < HEIGHT_NEAR_TOP && row + i < NUM_ROW - 1; i++) {
    if (board[row + i][col] === SquareState.EMPTY) {
      return true;
    }
  }
  return false;
}

function canDoPlacement(
  board: any,
  level: any,
  pieceId: string,
  rotationIndex: number,
  xOffset: number,
  aiArr: number,
  aiTapDelay: any
) {
  if (!aiArr) {
    throw new Error("Unknown ARR when checking placement");
  }
  const maxGravity = utils.GetGravity(level) - 1; // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const maxArr = aiArr - 1;
  const rotationsList = PIECE_LOOKUP[pieceId][0];
  const simParams: SimParams = {
    board,
    initialX: 3,
    initialY: -2,
    firstShiftDelay: aiTapDelay,
    maxGravity,
    maxArr,
    rotationsList,
    existingRotation: 0,
  };
  return placementIsLegal(rotationIndex, xOffset, simParams);
}

/** Returns true if the board needs a 5 tap to resolve, and the tap speed is not sufficient to get a piece there. */
function boardHasInaccessibileLeft(
  board: Board,
  level: number,
  averageHeight: number,
  aiArr: number,
  aiTapDelay: number
) {
  // If has holes near the top, it has a bad left
  if (hasHolesNearTopOfColumn(board, 0) || hasHolesNearTopOfColumn(board, 1)) {
    return true;
  }

  const col1Height = getBoardHeightAtColumn(board, 0);
  const col2Height = getBoardHeightAtColumn(board, 1);
  const col3Height = getBoardHeightAtColumn(board, 2);

  // If the left is built out, we good
  if (col1Height >= col2Height && col1Height > averageHeight) {
    return false;
  }

  // If left is accessible by square, the left is good
  if (
    col1Height == col2Height &&
    canDoPlacement(board, level, "O", 0, -4, aiArr, aiTapDelay)
  ) {
    return false;
  }

  // If the left is accessible by L, the left is good
  if (
    col1Height === col2Height - 1 &&
    col2Height === col3Height &&
    canDoPlacement(board, level, "L", 0, -4, aiArr, aiTapDelay)
  ) {
    return false;
  }

  // If the left is superflat, the left is good
  if (
    col1Height === col2Height &&
    col2Height === col3Height &&
    canDoPlacement(board, level, "L", 2, -4, aiArr, aiTapDelay)
  ) {
    return false;
  }

  // If the left is accessible by T, the left is good
  if (
    col1Height === col2Height + 1 &&
    col1Height === col3Height &&
    canDoPlacement(board, level, "T", 0, -4, aiArr, aiTapDelay)
  ) {
    return false;
  }

  // Otherwise we need 5 tap
  return !canDoPlacement(board, level, "I", 1, -5, aiArr, aiTapDelay);
}

/** Returns true if the tap speed is not sufficient to get a long bar to the right. */
function boardHasInaccessibileRight(
  board: any,
  level: any,
  averageHeight: number,
  aiArr: any,
  aiTapDelay: any
) {
  // If has holes near the top, it has a bad right
  if (
    hasHolesNearTopOfColumn(board, NUM_COLUMN - 1) ||
    hasHolesNearTopOfColumn(board, NUM_COLUMN - 2)
  ) {
    return true;
  }

  const col9Height = getBoardHeightAtColumn(board, NUM_COLUMN - 2);
  const col10Height = getBoardHeightAtColumn(board, NUM_COLUMN - 1);

  // If right is built out, we're good
  if (col10Height >= col9Height && col10Height > averageHeight) {
    return false;
  }

  // Otherwise we need a 4 tap
  return !canDoPlacement(board, level, "I", 1, 4, aiArr, aiTapDelay);
}

/** A modulus function that correctly handles negatives. */
function _modulus(n: number, m: number) {
  return (n + m) % m;
}

function placementIsLegal(
  goalRotationIndex: number,
  goalOffsetX: number,
  simulationParams: SimParams
) {
  const {
    board,
    initialX,
    initialY,
    firstShiftDelay,
    maxGravity,
    maxArr,
    rotationsList,
    existingRotation,
  } = simulationParams;

  // Get initial sim state
  const shiftIncrement = goalOffsetX < 0 ? -1 : 1;
  const simState = {
    x: initialX,
    y: initialY,
    gravityCounter: maxGravity,
    arrCounter: firstShiftDelay,
    rotationIndex: existingRotation,
  };

  // Check for immediate collisions
  if (
    pieceCollision(
      board,
      simState.x,
      simState.y,
      rotationsList[simState.rotationIndex]
    )
  ) {
    return false;
  }

  while (
    simState.x !== initialX + goalOffsetX ||
    simState.rotationIndex !== goalRotationIndex
  ) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does

    if (simState.arrCounter == 0) {
      const shiftThisStep =
        simState.x !== initialX + goalOffsetX ? shiftIncrement : 0;
      const inputsSucceeded = performSimulationInputs(
        goalRotationIndex,
        shiftThisStep,
        simState,
        board,
        rotationsList
      );
      if (!inputsSucceeded) {
        return false;
      }
      simState.arrCounter = maxArr;
    } else {
      simState.arrCounter--;
    }

    if (simState.gravityCounter == 0) {
      if (
        pieceCollision(
          board,
          simState.x,
          simState.y + 1,
          rotationsList[simState.rotationIndex]
        )
      ) {
        // console.log("DEBUG: GRAVITY");
        // utils.logBoard(
        //   getBoardAndLinesClearedAfterPlacement(
        //     board,
        //     rotationsList[simState.rotationIndex],
        //     simState.x,
        //     simState.y
        //   )[0]
        // );
        return false; // Piece would lock in
      }
      simState.y++;
      simState.gravityCounter = maxGravity;
    } else {
      simState.gravityCounter--;
    }
  }
  return true;
}

/** Carry out one "frame" worth of inputs, modifying the simulation state in-place. */
function performSimulationInputs(
  goalRotationIndex: number,
  xIncrement: number,
  simState: {
    x: any;
    y: any;
    gravityCounter?: any;
    arrCounter?: any;
    rotationIndex: any;
  },
  board: any,
  rotationsList: string | any[]
) {
  // Plan for a rotation if needed
  const prevRotationIndex = simState.rotationIndex;
  if (simState.rotationIndex !== goalRotationIndex) {
    if (_modulus(simState.rotationIndex - 1, 4) === goalRotationIndex) {
      // Left rotation
      simState.rotationIndex--;
    } else {
      simState.rotationIndex++;
    }
    simState.rotationIndex = _modulus(simState.rotationIndex, 4);
  }

  if (
    simState.rotationIndex >= rotationsList.length ||
    simState.rotationIndex < 0
  ) {
    throw new Error(`Invalid rotation index ${simState.rotationIndex}`);
  }

  // For the input sequence to go through, both of 1) the shift, and 2) the rotation + shift, must be valid.
  if (
    pieceCollision(
      board,
      simState.x + xIncrement,
      simState.y,
      rotationsList[prevRotationIndex]
    ) ||
    pieceCollision(
      board,
      simState.x + xIncrement,
      simState.y,
      rotationsList[simState.rotationIndex]
    )
  ) {
    // console.log("DEBUG: COLLISION");
    // utils.logBoard(
    //   getBoardAndLinesClearedAfterPlacement(
    //     board,
    //     rotationsList[prevRotationIndex],
    //     simState.x,
    //     simState.y
    //   )[0]
    // );
    return false; // We're done, can't go any further
  }
  simState.x += xIncrement;
  return true;
}

/**
 * Helper function for getPieceRanges that shifts a hypothetical piece as many times as it can in
 * each direction, before it hits the stack or the edge of the screen.
 */
function repeatedlyShiftPiece(
  shiftIncrement: number,
  goalRotationIndex: number,
  simulationParams: {
    board: any;
    initialX: any;
    initialY: any;
    firstShiftDelay: any;
    maxGravity: any;
    maxArr: any;
    rotationsList: any;
    existingRotation: any;
  }
) {
  const {
    board,
    initialX,
    initialY,
    firstShiftDelay,
    maxGravity,
    maxArr,
    rotationsList,
    existingRotation,
  } = simulationParams;

  // Get initial sim state
  const simState = {
    x: initialX,
    y: initialY,
    gravityCounter: maxGravity,
    arrCounter: firstShiftDelay,
    rotationIndex: existingRotation,
  };
  let rangeCurrent = 0;

  // Check for immediate collisions
  if (
    pieceCollision(
      board,
      simState.x,
      simState.y,
      rotationsList[simState.rotationIndex]
    )
  ) {
    return rangeCurrent;
  }

  while (true) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does

    if (simState.arrCounter == 0) {
      const inputsSucceeded = performSimulationInputs(
        goalRotationIndex,
        shiftIncrement,
        simState,
        board,
        rotationsList
      );
      if (!inputsSucceeded) {
        return rangeCurrent;
      }
      rangeCurrent += shiftIncrement;
      simState.arrCounter = maxArr;
    } else {
      simState.arrCounter--;
    }

    if (simState.gravityCounter == 0) {
      if (
        pieceCollision(
          board,
          simState.x,
          simState.y + 1,
          rotationsList[simState.rotationIndex]
        )
      ) {
        // console.log("DEBUG: GRAVITY");
        // utils.logBoard(
        //   getBoardAndLinesClearedAfterPlacement(
        //     board,
        //     rotationsList[simState.rotationIndex],
        //     simState.x,
        //     simState.y
        //   )[0]
        // );
        return rangeCurrent; // Piece would lock in
      }
      simState.y++;
      simState.gravityCounter = maxGravity;
    } else {
      simState.gravityCounter--;
    }
  }
}

/** Helper method for testing. */
function getTestBoardWithHeight(height: number) {
  const board = [];
  for (let i = 0; i < NUM_ROW; i++) {
    board.push(
      i < NUM_ROW - height
        ? [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
        : [1, 1, 1, 1, 1, 1, 1, 1, 1, 0]
    );
  }
  return board;
}

// console.log(
//   trySpecificPlacement(2, 0, {
//     board: getTestBoardWithHeight(18),
//     initialX: 3,
//     initialY: -1,
//     firstShiftDelay: 0,
//     maxGravity: 1,
//     maxArr: 4,
//     rotationsList: PIECE_LOOKUP["L"][0],
//     existingRotation: 0,
//   })
// );
// console.log(
//   getPossibleMoves(
//     getTestBoardWithHeight(4),
//     "O",
//     29,
//     0,
//     0,
//     0,
//     0,
//     true
//   ).map((p) => p.slice(0, 2))
// );
// console.log(boardHasInaccessibileLeft(getTestBoardWithHeight(9), 19));
// console.log(boardHasInaccessibileRight(getTestBoardWithHeight(10), 19));

module.exports = {
  getPossibleMoves,
  getBoardAndLinesClearedAfterPlacement,
  getBoardHeightAtColumn,
  pieceCollision,
  boardHasInaccessibileLeft,
  boardHasInaccessibileRight,
};
