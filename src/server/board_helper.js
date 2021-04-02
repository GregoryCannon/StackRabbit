const PIECE_LOOKUP = require("../tetrominoes").PIECE_LOOKUP;
const utils = require("./utils");
const NUM_COLUMN = utils.NUM_COLUMN;
const NUM_ROW = utils.NUM_ROW;
const SquareState = utils.SquareState;
const AI_TAP_ARR = utils.AI_TAP_ARR;

// Collision function
function pieceCollision(board, x, y, piece) {
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
        console.log(`Out of bounds at at ${newX}, ${newY}`);
        return true;
      }
      // If over the top of the board, ignore
      if (newY < 0) {
        continue;
      }
      // Check if it overlaps the board
      if (board[newY][newX] != 0) {
        console.log(`Collision at ${newX}, ${newY}`);
        return true;
      }
    }
  }
  return false;
}

/** Clear all filled lines on a board
 * @returns the number of lines cleared
 */
function clearLines(board) {
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
  board,
  currentRotationPiece,
  x,
  y
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

/**
 * Generates a list of possible moves, given a board and a piece. It achieves this by
 * placing it in each possible rotation and each possible starting column, and then
 * dropping it into the stack and letting the result play out.
 */
function getPossibleMoves(
  startingBoard,
  currentPieceId,
  level,
  existingXOffset,
  existingYOffset,
  firstShiftDelay,
  existingRotation,
  shouldLog
) {
  const startingX = 3 + existingXOffset;
  const startingY = (currentPieceId == "I" ? -2 : -1) + existingYOffset;
  
  const { rangesLeft, rangesRight } = getPieceRanges(
    currentPieceId,
    startingBoard,
    level,
    startingX,
    startingY,
    firstShiftDelay,
    existingRotation
  );
  const NUM_ROTATIONS_FOR_PIECE = rangesLeft.length;

  const legalPlacements = [];

  // Loop over the range and validate the moves with more rotations than shifts
  for (let rotationIndex = 0; rotationIndex < NUM_ROTATIONS_FOR_PIECE; rotationIndex++){
    const rangeLeftForRotation = rangesLeft[rotationIndex];
    const rangeRightForRotation = rangesLeft[rotationIndex];
    
    const rotationDifference = _correctModulus(rotationIndex - existingRotation, NUM_ROTATIONS_FOR_PIECE);
    const numRotationInputs = rotationDifference === 3 ? 1 : rotationDifference;

    // Automatically add the placements where the range calculations are always accurate
    for (let xOffset = numRotationInputs; xOffset <= rangeRightForRotation; xOffset++){
      legalPlacements.push([rotationIndex, xOffset]);
    }
    for (let xOffset = -1 * numRotationInputs; xOffset <= rangeLeftForRotation; xOffset++){
      legalPlacements.push([rotationIndex, xOffset]);
    }

  }

  return _generatePossibilityList(legalPlacements, startingBoard, currentPieceId);
}

function _generatePossibilityList(legalPlacements, startingBoard, startingX, startingY){
  const possibilityList = []; // list of [rotationId, xOffset, columnHeightsStr]


  

  for (const [rotationIndex, xOffset] of legalPlacements){
    const x = startingX + xOffset;
      let y = startingY;

      // Skip configurations that are out of bounds
      if (pieceCollision(startingBoard, x, -2, currentRotationPiece)) {
        if (shouldLog) {
          console.log(
            `Out of bounds: index ${rotationIndex}, xOffset ${xOffset}`
          );
        }
        continue;
      }

      // Move the piece down until it hits the stack
      while (!pieceCollision(startingBoard, x, y + 1, currentRotationPiece)) {
        y++;
      }

      // Make a new board with that piece locked in
      const [
        trialBoard,
        numLinesCleared,
      ] = getBoardAndLinesClearedAfterPlacement(
        startingBoard,
        currentRotationPiece,
        x,
        y
      );
      const newSurfaceArray = utils.getSurfaceArray(trialBoard);
      const numHoles = utils.getHoleCount(trialBoard);

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
        trialBoard,
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
  pieceId,
  board,
  level,
  initialX,
  initialY,
  firstShiftDelay,
  existingRotation
) {
  const maxGravity = utils.GetGravity(level) - 1; // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const maxArr = AI_TAP_ARR - 1;
  const rotationsList = PIECE_LOOKUP[pieceId][0];

  // Piece ranges, indexed by rotation index
  const rangesLeft = [];
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    rangesLeft.push(
      repeatedlyShiftPiece(
        -1,
        board,
        initialX,
        initialY,
        firstShiftDelay,
        maxGravity,
        maxArr,
        rotationsList,
        rotationIndex,
        existingRotation
      )
    );
  }
  const rangesRight = [];
  for (
    let rotationIndex = 0;
    rotationIndex < rotationsList.length;
    rotationIndex++
  ) {
    rangesRight.push(
      repeatedlyShiftPiece(
        1,
        board,
        initialX,
        initialY,
        firstShiftDelay,
        maxGravity,
        maxArr,
        rotationsList,
        rotationIndex,
        existingRotation
      )
    );
  }
  return { rangesLeft, rangesRight };
}

function getBoardHeightAtColumn(board, col) {
  let row = 0;
  while (row < NUM_ROW && board[row][col] == 0) {
    row++;
  }
  return 20 - row;
}


/** Returns true if the board needs a 5 tap to resolve, and the tap speed is not sufficient to get a piece there. */
function boardHasInaccessibileLeft(board, level) {
  // If left is built out, we're fine
  if (getBoardHeightAtColumn(board, 0) > getBoardHeightAtColumn(board, 1)) {
    return false;
  }

  const maxGravity = utils.GetGravity(level) - 1; // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const maxArr = AI_TAP_ARR - 1;
  const iPieceRotationsList = PIECE_LOOKUP["I"][0];
  const vertIPieceRangeLeft = repeatedlyShiftPiece(
    /* offsetX= */ -1,
    board,
    /* initialX= */ 3,
    /* initialY= */ -2,
    /* firstShiftDelay= */ 0,
    maxGravity,
    maxArr,
    iPieceRotationsList,
    /* goalRotationIndex= */ 1,
    /* existingRotation= */ 0
  );

  return vertIPieceRangeLeft !== -5;
}


/** Returns true if the tap speed is not sufficient to get a long bar to the right. */
function boardHasInaccessibileRight(board, level) {
  // If right is built out, we're fine
  if (
    getBoardHeightAtColumn(board, NUM_COLUMN - 1) >
    getBoardHeightAtColumn(board, NUM_COLUMN - 2)
  ) {
    return false;
  }

  const maxGravity = utils.GetGravity(level) - 1; // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const maxArr = AI_TAP_ARR - 1;
  const iPieceRotationsList = PIECE_LOOKUP["I"][0];
  const vertIPieceRangeRight = repeatedlyShiftPiece(
    /* offsetX= */ 1,
    board,
    /* initialX= */ 3,
    /* initialY= */ -2,
    /* firstShiftDelay= */ 0,
    maxGravity,
    maxArr,
    iPieceRotationsList,
    /* goalRotationIndex= */ 1,
    /* existingRotation= */ 0
  );

  return vertIPieceRangeRight !== 4;
}



/** A modulus function that correctly handles negatives. */
function _correctModulus(n, m) {
  return (n + m) % m;
}

function trySpecificPlacement(goalOffsetX,
board,
initialX,
initialY,
firstShiftDelay,
maxGravity,
maxArr,
rotationsList,
goalRotationIndex,
existingRotation){
  console.log(`\n\n TRYING: ${goalRotationIndex}, ${goalOffsetX}`);

  // Search left/right based on offset X
  const shiftIncrement = goalOffsetX < 0 ? -1 : 1;
  let rangeCurrent = 0;
  let x = initialX;
  let y = initialY;
  let gravityCounter = maxGravity;
  let arrCounter = firstShiftDelay;
  let rotationIndex = existingRotation;

  // If a bar can't spawn or is almost immediately crashed into the stack, it's definitely out of reach
  if (pieceCollision(board, x, y, rotationsList[rotationIndex])) {
    console.log("COLLISION AT SPAWN");
    return false;
  }

  while (x !== initialX + goalOffsetX || rotationIndex !== goalRotationIndex) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does
    
    if (arrCounter == 0) {
      console.log(`performing inputs, x ${x}`);

      // Plan for a rotation if needed
      const prevRotationIndex = rotationIndex;
      console.log(`previousRotation ${prevRotationIndex}`);
      if (rotationIndex !== goalRotationIndex) {
        if (_correctModulus(rotationIndex - 1, 4) === goalRotationIndex) {
          // Left rotation
          rotationIndex--;
        } else {
          rotationIndex++;
        }
        rotationIndex = _correctModulus(rotationIndex, 4);
      }

      if (rotationIndex >= rotationsList.length || rotationIndex < 0) {
        throw new Error(`Invalid rotation index ${rotationIndex}`);
      }

      // For the input sequence to go through, both of 1) the shift, and 2) the rotation + shift, must be valid.
      if (
        pieceCollision(board, x + shiftIncrement, y, rotationsList[prevRotationIndex]) ||
        pieceCollision(board, x + shiftIncrement, y, rotationsList[rotationIndex])
      ) {
        console.log("DEBUG: COLLISION");
        utils.logBoard(getBoardAndLinesClearedAfterPlacement(board, rotationsList[prevRotationIndex], x, y)[0])
        return false; // We're done, can't go any further
      }
      x += shiftIncrement;
      rangeCurrent += shiftIncrement;
      arrCounter = maxArr;
    } else {
      arrCounter--;
    }

    if (gravityCounter == 0) {
      if (pieceCollision(board, x, y + 1, rotationsList[rotationIndex])) {
        // Piece would lock in
        console.log("DEBUG: GRAVITY");
        utils.logBoard(getBoardAndLinesClearedAfterPlacement(board, rotationsList[rotationIndex], x, y)[0])
        return false;
      }
      y++;
      gravityCounter = maxGravity;
    } else {
      gravityCounter--;
    }
  }

  return true;
}

/**
 * Helper function for getPieceRanges that shifts a hypothetical piece as many times as it can in
 * each direction, before it hits the stack or the edge of the screen.
 */
function repeatedlyShiftPiece(
  shiftIncrement,
  board,
  initialX,
  initialY,
  firstShiftDelay,
  maxGravity,
  maxArr,
  rotationsList,
  goalRotationIndex,
  existingRotation
) {
  // Search left/right based on offset X
  let rangeCurrent = 0;
  let x = initialX;
  let y = initialY;
  let gravityCounter = maxGravity;
  let arrCounter = firstShiftDelay;
  let rotationIndex = existingRotation;

  // If a bar can't spawn or is almost immediately crashed into the stack, it's definitely out of reach
  if (pieceCollision(board, x, y, rotationsList[rotationIndex])) {
    console.log("COLLISION AT SPAWN");
    return rangeCurrent;
  }

  while (true) {
    // Run a simulated 'frame' of gravity, shifting, and collision checking
    // We simulate shifts and rotations on the ARR triggers, just like the Lua script does
    
    if (arrCounter == 0) {
      // Plan for a rotation if needed
      const prevRotationIndex = rotationIndex;
      if (rotationIndex !== goalRotationIndex) {
        if (_correctModulus(rotationIndex - 1, 4) === goalRotationIndex) {
          // Left rotation
          rotationIndex--;
        } else {
          rotationIndex++;
        }
        rotationIndex = _correctModulus(rotationIndex, 4);
      }

      if (rotationIndex >= rotationsList.length || rotationIndex < 0) {
        throw new Error(`Invalid rotation index ${rotationIndex}`);
      }

      // For the input sequence to go through, both of 1) the shift, and 2) the rotation + shift, must be valid.
      if (
        pieceCollision(board, x + shiftIncrement, y, rotationsList[prevRotationIndex]) ||
        pieceCollision(board, x + shiftIncrement, y, rotationsList[rotationIndex])
      ) {
        console.log("DEBUG: COLLISION");
        utils.logBoard(getBoardAndLinesClearedAfterPlacement(board, rotationsList[prevRotationIndex], x, y)[0])
        break; // We're done, can't go any further
      }
      x += shiftIncrement;
      rangeCurrent += shiftIncrement;
      console.log("Updated rangeCurrent:", rangeCurrent);
      arrCounter = maxArr;
    } else {
      arrCounter--;
    }

    if (gravityCounter == 0) {
      if (pieceCollision(board, x, y + 1, rotationsList[rotationIndex])) {
        // Piece would lock in
        console.log("DEBUG: GRAVITY");
        utils.logBoard(getBoardAndLinesClearedAfterPlacement(board, rotationsList[prevRotationIndex], x, y)[0])
        break;
      }
      y++;
      gravityCounter = maxGravity;
    } else {
      gravityCounter--;
    }
  }
  return rangeCurrent;
}



/** Helper method for testing. */
function getTestBoardWithHeight(height) {
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

// console.log(trySpecificPlacement(-5, getTestBoardWithHeight(9), 3, -2, 0, 1, 4, PIECE_LOOKUP['I'][0], 1, 0));
// console.log(getPieceRanges("I", getTestBoardWithHeight(0), 19, 3, 6, 0, 0));
// getPossibleMoves(getTestBoardWithHeight(8), "I", 19, 0, 6, 0, true);
// console.log(boardHasInaccessibileLeft(getTestBoardWithHeight(9), 19));
// console.log(boardHasInaccessibileRight(getTestBoardWithHeight(10), 19));

module.exports = {
  getPossibleMoves,
  getBoardAndLinesClearedAfterPlacement,
  pieceCollision,
  boardHasInaccessibileLeft,
  boardHasInaccessibileRight,
};
