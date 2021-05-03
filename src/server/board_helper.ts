const PIECE_LOOKUP = require("../tetrominoes").PIECE_LOOKUP;
const utils = require("./utils");
const NUM_COLUMN = utils.NUM_COLUMN;
const NUM_ROW = utils.NUM_ROW;
const SquareState = utils.SquareState;

// Collision function
export function pieceCollision(
  board: Board,
  x: number,
  y: number,
  piece: PieceArray
) {
  if (!piece || piece.length < 1) {
    throw new Error("Unknown piece passed into collision function");
  }
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
function clearLines(board: Board) {
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

export function getBoardAndLinesClearedAfterPlacement(
  board: Board,
  currentRotationPiece: PieceArray,
  x: number,
  y: number
) {
  let tempBoard = utils.cloneBoard(board);
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

export function _validateIntParam(value: number, min: number, max: number) {
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
export function getPossibleMoves(
  startingBoard: Board,
  currentPieceId: string,
  level: number,
  existingXOffset: number,
  existingYOffset: number,
  framesAlreadyElapsed: number,
  inputFrameTimeline: string,
  existingRotation: number,
  canFirstFrameShift: boolean,
  shouldLog: boolean
) {
  _validateIntParam(level, 0, 999);
  _validateIntParam(existingXOffset, -5, 4);
  _validateIntParam(existingYOffset, 0, 20);
  _validateIntParam(framesAlreadyElapsed, 0, 999);
  _validateIntParam(existingRotation, 0, 4);

  const initialX = 3 + existingXOffset;
  const initialY = (currentPieceId == "I" ? -2 : -1) + existingYOffset;
  const gravity = utils.GetGravity(level);
  const rotationsList = PIECE_LOOKUP[currentPieceId][0];

  const simParams = {
    board: startingBoard,
    initialX,
    initialY,
    framesAlreadyElapsed,
    gravity,
    inputFrameTimeline,
    rotationsList,
    existingRotation,
    canFirstFrameShift,
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
    inputFrameTimeline,
    framesAlreadyElapsed,
    existingRotation,
    shouldLog
  );
}

function _generatePossibilityList(
  legalPlacements: Array<Placement>,
  startingBoard: Board,
  currentPieceId: string,
  startingX: number,
  startingY: number,
  inputFrameTimeline: string,
  framesAlreadyElapsed: number,
  existingRotation: number,
  shouldLog: boolean
): Array<Possibility> {
  const possibilityList = [];

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
    let [surfaceArray, numHoles, holeCells] = utils.getSurfaceArrayAndHoles(
      boardAfter
    );
    surfaceArray = surfaceArray.slice(0, 9);

    // Add the possibility to the list
    if (shouldLog) {
      console.log(
        `Adding possibility [Index ${rotationIndex}, xOffset ${xOffset}], would make surface ${surfaceArray}`
      );
    }
    possibilityList.push({
      placement: [rotationIndex, xOffset],
      inputSequence: generateInputSequence(
        _modulus(rotationIndex - existingRotation, 4),
        xOffset,
        inputFrameTimeline,
        framesAlreadyElapsed
      ),
      surfaceArray,
      numHoles,
      holeCells,
      numLinesCleared,
      boardAfter,
    });
  }

  if (shouldLog) {
    console.log(
      `Result: ${possibilityList.length} possibilities for ${currentPieceId}`
    );
  }
  return possibilityList;
}

/** Generates a string representing the inputs to perform, frame-by-frame. It uses the following encoding:
 * . = do nothing
 * A = press A
 * B = press B
 * L = press L
 * R = press R
 * e = press L + B (2nd letter of 'left')
 * f = press L + A (3rd letter of 'left')
 * i = press R + B (2nd letter of 'right')
 * g = press R + A (3rd letter of 'right')
 *
 * e.g. L piece 5-tap left, 12Hz tapping: f....L....L....L....L
 */
function generateInputSequence(
  rotationIndex,
  xOffset,
  inputFrameTimeline,
  framesAlreadyElapsed = 0
) {
  let inputsLeft = xOffset < 0 && Math.abs(xOffset);
  let inputsRight = xOffset > 0 && xOffset;
  let rotationsLeft = rotationIndex === 3 && 1;
  let rotationsRight = rotationIndex < 3 && rotationIndex;

  if (inputsLeft > 0 && inputsRight > 0) {
    throw new Error("Invalid shift parsing");
  }
  if (rotationsLeft > 0 && rotationsRight > 0) {
    throw new Error("Invalid rotation parsing");
  }

  let inputSequence = "";
  for (
    let i = framesAlreadyElapsed;
    inputsLeft + inputsRight + rotationsLeft + rotationsRight > 0;
    i++
  ) {
    if (utils.shouldPerformInputsThisFrame(inputFrameTimeline, i)) {
      if (inputsLeft > 0) {
        // Do a left shift, possibly with a rotation
        if (rotationsRight > 0) {
          inputSequence += "e";
          rotationsRight--;
        } else if (rotationsLeft > 0) {
          inputSequence += "f";
          rotationsLeft--;
        } else {
          inputSequence += "L";
        }
        inputsLeft--;
      } else if (inputsRight > 0) {
        // Do a right shift, possibly with a rotation
        if (rotationsRight > 0) {
          inputSequence += "i";
          rotationsRight--;
        } else if (rotationsLeft > 0) {
          inputSequence += "g";
          rotationsLeft--;
        } else {
          inputSequence += "R";
        }
        inputsRight--;
      } else {
        // Do a rotation
        if (rotationsLeft > 0) {
          inputSequence += "B";
          rotationsLeft--;
        } else {
          inputSequence += "A";
          rotationsRight--;
        }
      }
    } else {
      inputSequence += ".";
    }
  }
  return inputSequence;
}

/**
 * Calculates how far in each direction a hypothetical piece can be tapped (for each rotation), given the AI's tap speed and
 * the piece's current position.
 *
 * (!!) NB: It performs one rotation for each shift. So if the placement requires 2 rotations, this result will only be valid for
 * placements with abs(xOffset) >= 2.
 */
function getPieceRanges(pieceId: string, simParams: SimParams) {
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

/** Gets the surface of just the left-most 3 columns, if they don't have any holes.
 * (This is used for killscreen play)
 */
export function getLeftSurface(board: Board, maxHeight) {
  if (
    hasHoleInColumn(board, 0) ||
    hasHoleInColumn(board, 1) ||
    hasHoleInColumn(board, 2)
  ) {
    return null;
  }

  const getColHeight = (col) =>
    Math.min(Math.min(9, maxHeight), getBoardHeightAtColumn(board, col));

  return "" + getColHeight(0) + getColHeight(1) + getColHeight(2);
}

/** Gets the height in a particular column. */
export function getBoardHeightAtColumn(board: Board, col: number) {
  let row = 0;
  while (row < NUM_ROW && board[row][col] == 0) {
    row++;
  }
  return 20 - row;
}

/** Searches a column for holes.
 * @param heightFromTop (Optional) limits the search to N rows below the top of the stack in that column
 */
export function hasHoleInColumn(board: Board, col: number, heightFromTop = 20) {
  let row = 0;
  while (row < NUM_ROW && board[row][col] === SquareState.EMPTY) {
    row++;
  }
  for (let i = 0; i < heightFromTop && row + i < NUM_ROW; i++) {
    if (board[row + i][col] === SquareState.EMPTY) {
      return true;
    }
  }
  return false;
}

export function canDoPlacement(
  board: Board,
  level: number,
  pieceId: string,
  rotationIndex: number,
  xOffset: number,
  inputFrameTimeline: string
) {
  if (!inputFrameTimeline) {
    throw new Error("Unknown input timeline when checking placement");
  }
  const gravity = utils.GetGravity(level); // 0-indexed, executes on the 0 frame. e.g. 2... 1... 0(shift).. 2... 1... 0(shift)
  const rotationsList = PIECE_LOOKUP[pieceId][0];
  const simParams: SimParams = {
    board,
    initialX: 3,
    initialY: pieceId === "I" ? -2 : -1,
    framesAlreadyElapsed: 0,
    gravity,
    rotationsList,
    existingRotation: 0,
    inputFrameTimeline,
    canFirstFrameShift: false, // This function refers to doing a placement from the start, not starting from an adjustment or anything
  };
  return placementIsLegal(rotationIndex, xOffset, simParams);
}

/** Returns true if the board needs a 5 tap to resolve, and the tap speed is not sufficient to get a piece there. */
export function boardHasInaccessibileLeft(
  board: Board,
  level: number,
  aiParams: AiParams,
  aiMode: AiMode
) {
  const col1Height = getBoardHeightAtColumn(board, 0);
  const col2Height = getBoardHeightAtColumn(board, 1);

  if (aiMode === AiMode.KILLSCREEN) {
    // On killscreen, we mainly access the left with 4-taps. So we need either
    // 1) a left built as high as the 4 tap height
    // 2) access to the left with a 4 tap
    if (
      col1Height >= col2Height &&
      col1Height > aiParams.MAX_4_TAP_LOOKUP[level]
    ) {
      return false;
    }
    const canDo4TapLeft = canDoPlacement(
      board,
      level,
      "L",
      0,
      -4,
      aiParams.INPUT_FRAME_TIMELINE
    );
    return !canDo4TapLeft;
  }

  //In normal stacking, we mainly access the left with 5-taps. So we need either
  // 1) a left built as high as the 5 tap height
  // 2) access to the left with a 5 tap
  if (
    col1Height >= col2Height &&
    col1Height > aiParams.MAX_5_TAP_LOOKUP[level]
  ) {
    return false;
  }
  return !canDoPlacement(
    board,
    level,
    "I",
    1,
    -5,
    aiParams.INPUT_FRAME_TIMELINE
  );
}

/** Returns true if the tap speed is not sufficient to get a long bar to the right. */
export function boardHasInaccessibileRight(
  board: Board,
  level: number,
  aiParams: AiParams
) {
  const col9Height = getBoardHeightAtColumn(board, NUM_COLUMN - 2);
  const col10Height = getBoardHeightAtColumn(board, NUM_COLUMN - 1);
  // If right is built out, we're good
  if (
    col10Height >= col9Height &&
    col10Height > aiParams.MAX_4_TAP_LOOKUP[level]
  ) {
    return false;
  }

  // Otherwise we need a 4 tap
  return !canDoPlacement(
    board,
    level,
    "I",
    1,
    4,
    aiParams.INPUT_FRAME_TIMELINE
  );
}

/** A modulus function that correctly handles negatives. */
export function _modulus(n: number, m: number) {
  return (n + m) % m;
}

function placementIsLegal(
  goalRotationIndex: number,
  goalOffsetX: number,
  simulationParams: SimParams,
  shouldLog: boolean = false
) {
  const {
    board,
    initialX,
    initialY,
    framesAlreadyElapsed,
    gravity,
    rotationsList,
    existingRotation,
    inputFrameTimeline,
    canFirstFrameShift,
  } = simulationParams;

  // Get initial sim state
  const shiftIncrement = goalOffsetX < 0 ? -1 : 1;
  const simState: SimState = {
    x: initialX,
    y: initialY,
    frameIndex: framesAlreadyElapsed,
    arrFrameIndex: canFirstFrameShift ? 0 : framesAlreadyElapsed,
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
    const isInputFrame = utils.shouldPerformInputsThisFrame(
      inputFrameTimeline,
      simState.arrFrameIndex
    );
    const isGravityFrame = simState.frameIndex % gravity === gravity - 1; // Returns true every Nth frame, where N = gravity

    if (isInputFrame) {
      const xIncrement =
        simState.x !== initialX + goalOffsetX ? shiftIncrement : 0;
      const inputSucceeded = performSimulationShift(
        xIncrement,
        simState,
        board,
        rotationsList[simState.rotationIndex]
      );
      if (!inputSucceeded) {
        return false;
      }
    }

    if (isInputFrame) {
      const inputSucceeded = performSimulationRotation(
        goalRotationIndex,
        simState,
        board,
        rotationsList
      );
      if (!inputSucceeded) {
        return false;
      }
    }

    if (isGravityFrame) {
      if (
        pieceCollision(
          board,
          simState.x,
          simState.y + 1,
          rotationsList[simState.rotationIndex]
        )
      ) {
        if (shouldLog) {
          console.log("DEBUG: GRAVITY");
          utils.logBoard(
            getBoardAndLinesClearedAfterPlacement(
              board,
              rotationsList[simState.rotationIndex],
              simState.x,
              simState.y
            )[0]
          );
        }
        return (
          simState.x == initialX + goalOffsetX &&
          simState.rotationIndex === goalRotationIndex
        ); // Piece would lock in
      }
      simState.y++;
    }

    if (shouldLog) {
      utils.logBoard(
        getBoardAndLinesClearedAfterPlacement(
          board,
          rotationsList[simState.rotationIndex],
          simState.x,
          simState.y
        )[0]
      );
    }
    simState.frameIndex += 1;
    simState.arrFrameIndex += 1;
  }
  return true;
}

export function performSimulationShift(
  xIncrement: number,
  simState: SimState,
  board: Board,
  currentRotationPiece: PieceArray
): boolean {
  if (
    pieceCollision(
      board,
      simState.x + xIncrement,
      simState.y,
      currentRotationPiece
    )
  ) {
    // console.log("DEBUG: COLLISION");
    // utils.logBoard(
    //   getBoardAndLinesClearedAfterPlacement(
    //     board,
    //     currentRotationPiece,
    //     simState.x,
    //     simState.y
    //   )[0]
    // );
    return false; // We're done, can't go any further
  }
  simState.x += xIncrement;
  return true;
}

function performSimulationRotation(
  goalRotationIndex: number,
  simState: SimState,
  board: Board,
  rotationsList: Array<PieceArray>
): boolean {
  // Plan for a rotation if needed
  if (simState.rotationIndex === goalRotationIndex) {
    return true;
  }
  const prevRotationIndex = simState.rotationIndex;
  if (_modulus(simState.rotationIndex - 1, 4) === goalRotationIndex) {
    // Left rotation
    simState.rotationIndex--;
  } else {
    simState.rotationIndex++;
  }
  simState.rotationIndex = _modulus(simState.rotationIndex, 4);

  if (
    simState.rotationIndex >= rotationsList.length ||
    simState.rotationIndex < 0
  ) {
    throw new Error(`Invalid rotation index ${simState.rotationIndex}`);
  }

  if (
    pieceCollision(
      board,
      simState.x,
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
  return true;
}

/**
 * Helper function for getPieceRanges that shifts a hypothetical piece as many times as it can in
 * each direction, before it hits the stack or the edge of the screen.
 */
function repeatedlyShiftPiece(
  shiftIncrement: number,
  goalRotationIndex: number,
  simulationParams: SimParams
) {
  const {
    board,
    initialX,
    initialY,
    framesAlreadyElapsed,
    inputFrameTimeline,
    gravity,
    rotationsList,
    existingRotation,
    canFirstFrameShift,
  } = simulationParams;

  // Get initial sim state
  const simState = {
    x: initialX,
    y: initialY,
    frameIndex: framesAlreadyElapsed,
    arrFrameIndex: canFirstFrameShift ? 0 : framesAlreadyElapsed,
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
    const isInputFrame = utils.shouldPerformInputsThisFrame(
      inputFrameTimeline,
      simState.arrFrameIndex
    );
    const isGravityFrame = simState.frameIndex % gravity === gravity - 1; // Returns true every Nth frame, where N = gravity

    if (isInputFrame) {
      const inputsSucceeded = performSimulationShift(
        shiftIncrement,
        simState,
        board,
        rotationsList[simState.rotationIndex]
      );
      if (!inputsSucceeded) {
        return rangeCurrent;
      }
      rangeCurrent += shiftIncrement;
    }

    if (isInputFrame) {
      const inputSucceeded = performSimulationRotation(
        goalRotationIndex,
        simState,
        board,
        rotationsList
      );
      if (!inputSucceeded) {
        return rangeCurrent;
      }
    }

    if (isGravityFrame) {
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
    }

    // console.log(simState.frameIndex, "wasinput?", isInputFrame, "isGrav?", isGravityFrame);
    // utils.logBoard(
    //   getBoardAndLinesClearedAfterPlacement(
    //     board,
    //     rotationsList[simState.rotationIndex],
    //     simState.x,
    //     simState.y
    //   )[0]
    // );

    simState.frameIndex += 1;
    simState.arrFrameIndex += 1;
  }
}

/** Helper method for testing. */
export function getTestBoardWithHeight(height: number) {
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

/** Gets the max height that can be cleared, given a level, ARR, delay, and number of taps desired. */
export function calculateTapHeight(level, inputFrameTimeline, numTaps) {
  let height = 0;
  while (
    canDoPlacement(
      getTestBoardWithHeight(height),
      level,
      "I",
      1,
      -1 * numTaps,
      inputFrameTimeline
    )
  ) {
    height++;
  }
  height--;
  return height;
}

function tapRangeTest() {
  const timeline10Hz = utils.generateInputFrameTimeline([5]);
  const timeline12Hz = utils.generateInputFrameTimeline([4]);
  const timeline13Hz = utils.generateInputFrameTimeline([4, 3]);
  const timeline14Hz = utils.generateInputFrameTimeline([4, 3, 3, 3]);

  let expected1 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(0), 29, "I", 1, -5, timeline13Hz) !==
    expected1
  ) {
    console.log(`Failed: 0 left 29 2 delay. Expected: ${expected1}`);
  }

  const expected2 = false;
  if (
    canDoPlacement(getTestBoardWithHeight(1), 29, "I", 1, -5, timeline13Hz) !==
    expected2
  ) {
    console.log(`Failed: 1 left 29 2 delay. Expected: ${expected2}`);
  }

  const expected3 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(1), 29, "I", 1, -5, timeline14Hz) !==
    expected3
  ) {
    console.log(`Failed: 1 left 29 1 delay. Expected: ${expected3}`);
  }

  const expected4 = false;
  if (
    canDoPlacement(getTestBoardWithHeight(2), 29, "I", 1, -5, timeline14Hz) !==
    expected4
  ) {
    console.log(`Failed: 2 left 29 1 delay. Expected: ${expected4}`);
  }

  const expected5 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(8), 19, "I", 1, -5, timeline12Hz) !==
    expected5
  ) {
    console.log(`Failed: 8 left 19 12 Hz. Expected: ${expected5}`);
  }

  const expected6 = false;
  if (
    canDoPlacement(getTestBoardWithHeight(9), 19, "I", 1, -5, timeline12Hz) !==
    expected6
  ) {
    console.log(`Failed: 9 left 19 12 Hz. Expected: ${expected6}`);
  }

  const expected7 = false;
  if (
    canDoPlacement(getTestBoardWithHeight(5), 29, "I", 1, 4, timeline13Hz) !==
    expected7
  ) {
    console.log(`Failed: 5 right 29 0 delay. Expected: ${expected7}`);
  }

  const expected8 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(4), 29, "I", 1, 4, timeline13Hz) !==
    expected8
  ) {
    console.log(`Failed: 4 right 29 0 delay. Expected: ${expected8}`);
  }

  const expected9 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(11), 19, "I", 1, 4, timeline13Hz) !==
    expected9
  ) {
    console.log(`Failed: 11 left 19 2 delay. Expected: ${expected9}`);
  }

  const expected10 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(11), 19, "I", 1, 4, timeline13Hz) !==
    expected10
  ) {
    console.log(`Failed: 12 right 19 2 delay. Expected: ${expected10}`);
  }

  const expected11 = false;
  if (
    canDoPlacement(getTestBoardWithHeight(7), 19, "I", 1, -5, timeline10Hz) !==
    expected11
  ) {
    console.log(`Failed: 7 left 19 10 Hz. Expected: ${expected11}`);
  }

  const expected12 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(3), 29, "O", 0, -4, timeline12Hz) !==
    expected12
  ) {
    console.log(`Failed: 3 right 29 12Hz. Expected: ${expected12}`);
  }

  const expected13 = false;
  if (
    canDoPlacement(getTestBoardWithHeight(4), 29, "O", 0, -4, timeline12Hz) !==
    expected13
  ) {
    console.log(`Failed: 4 right 29 12Hz. Expected: ${expected13}`);
  }

  const expected14 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(11), 19, "I", 1, 4, timeline12Hz) !==
    expected14
  ) {
    console.log(`Failed: 11 right 19 12Hz. Expected: ${expected14}`);
  }

  const expected15 = false;
  if (
    canDoPlacement(getTestBoardWithHeight(12), 19, "I", 1, 4, timeline12Hz) !==
    expected15
  ) {
    console.log(`Failed: 12 right 19 12Hz. Expected: ${expected15}`);
  }
}

function lastMinuteRotationsTest() {
  let expected1 = true;
  if (
    placementIsLegal(2, 0, {
      board: getTestBoardWithHeight(14),
      initialX: 3,
      initialY: -1,
      gravity: 0,
      framesAlreadyElapsed: 0,
      inputFrameTimeline: "X...",
      rotationsList: PIECE_LOOKUP["J"][0],
      existingRotation: 0,
      canFirstFrameShift: false,
    }) !== expected1
  ) {
    console.log(`Failed: double rotate J 14 high 29. Expected ${expected1}`);
  }

  let expected2 = false;
  if (
    placementIsLegal(2, 0, {
      board: getTestBoardWithHeight(15),
      initialX: 3,
      initialY: -1,
      gravity: 0,
      framesAlreadyElapsed: 0,
      inputFrameTimeline: "X...",
      rotationsList: PIECE_LOOKUP["J"][0],
      existingRotation: 0,
      canFirstFrameShift: false,
    }) !== expected2
  ) {
    console.log(`Failed: double rotate J 15 high 29. Expected ${expected2}`);
  }
}

tapRangeTest();
// lastMinuteRotationsTest();

// const simParams = {
//   board: getTestBoardWithHeight(11),
//   initialX: 3,
//   initialY: -2,
//   firstShiftDelay: 0,
//   maxGravity: 1,
//   maxArr: 4,
//   rotationsList: PIECE_LOOKUP["O"][0],
//   existingRotation: 0,
// }
// console.log("11 right 19 = ", repeatedlyShiftPiece(1, 0, simParams));
// console.log(canDoPlacement(getTestBoardWithHeight(11), 19, "I", 1, 4, 5, 0));

// console.log(placementIsLegal(2, 0, {
//   board: getTestBoardWithHeight(14),
//   initialX: 3,
//   initialY: -1,
//   firstShiftDelay: 0,
//   maxGravity: 0,
//   maxArr: 3,
//   rotationsList: PIECE_LOOKUP["J"][0],
//   existingRotation: 0,
// }));
// console.log(canDoPlacement(getTestBoardWithHeight(8), 19, "I", 1, -5, 4, 2));
// console.log(getPieceRanges("I", {
//   board: getTestBoardWithHeight(9),
//   initialX: 3,
//   initialY: -2,
//   firstShiftDelay: 0,
//   maxGravity: 1,
//   maxArr: 4,
//   rotationsList: PIECE_LOOKUP["I"][0],
//   existingRotation: 0,
// }));
