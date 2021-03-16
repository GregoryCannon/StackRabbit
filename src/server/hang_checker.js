const { NUM_ROW } = require("./utils");

const Direction = Object.freeze({
  LEFT: "left",
  RIGHT: "right",
});

/**
 * Checks whether our defined tap speed can make a given placement. All of the taps allowed by this function should be DAS-able with quicktaps.
 * @param {number} xOffset - number of taps left/right
 * @param {number} height - how high it needs to clear over
 * @param {number} level
 */
function tappingSpeedSufficient(xOffset, height, level) {
  if (level >= 29) {
    switch (Math.abs(xOffset)) {
      case 5:
        return height <= -1;
      case 4:
        return height <= 4;
      case 3:
        return height <= 9;
      case 2:
        return height <= 14;
      default:
        return true;
    }
  } else if (level >= 19) {
    switch (Math.abs(xOffset)) {
      case 5:
        return height <= 8;
      case 4:
        return height <= 11;
      case 3:
        return height <= 14;
      case 2:
        return height <= 16;
      default:
        return true;
    }
  } else {
    switch (Math.abs(xOffset)) {
      case 5:
        return height <= 12;
      case 4:
        return height <= 14;
      case 3:
        return height <= 15;
      case 2:
        return height <= 17;
      default:
        return true;
    }
  }
}

/**
 * Get the "height" of a piece at a given Y value. This used in determining how high of a stack it must have cleared to get to where it is.
 * @param {*} piece - the raw array of the piece in a given rotation
 * @param {*} y - the y value of the piece
 */
function getBoardHeight(board, column) {
  let row = 0;
  while (row < NUM_ROW && board[row][column] == 0) {
    row++;
  }
  return NUM_ROW - row;
}

/**
 * Gets the height off the bottom of placing a particular piece at a certain Y value
 * @param {*} piece
 * @param {*} y
 */
function getPiecePlacementHeight(piece, y) {
  let maxY = -999;
  for (let r = 0; r < piece.length; r++) {
    for (let c = 0; c < piece[r].length; c++) {
      // Skip empty cells
      if (!piece[r][c]) {
        continue;
      }

      maxY = Math.max(maxY, r + y);
    }
  }
  // Convert from y value (origin top left) to height it can be placed on top of
  // e.g. a 1 high left long bar has its lowest cell at y=18
  return NUM_ROW - 1 - maxY;
}

/**
 * Gets the effective width of the piece depending on which direction it's going
 * This property is the key factor in determining which row will be the limiting factor
 * in whether or not a tap sequence gets over or hangs on the stack.
 * @param {string} pieceId - one letter code
 * @param {number} rotationIndex - number of right rotations from NORO state
 * @param {Direction} direction - whether tapping the piece left or right
 */
function getEffectivePieceWidth(pieceId, rotationIndex, direction) {
  switch (pieceId) {
    case "I":
      switch (rotationIndex) {
        case 0:
          return 4;
        case 1:
          return 1;
      }
    case "O":
      return 2;
    case "L":
      switch (rotationIndex) {
        case 0:
          return direction == Direction.LEFT ? 1 : 3;
        case 1:
          return direction == Direction.LEFT ? 2 : 1;
        case 2:
          return 3;
        case 3:
          return 2;
      }
    case "J":
      switch (rotationIndex) {
        case 0:
          return direction == Direction.LEFT ? 3 : 1;
        case 1:
          return 2;
        case 2:
          return 3;
        case 3:
          return direction == Direction.LEFT ? 1 : 2;
      }
    case "T":
      switch (rotationIndex) {
        case 0:
          return 2;
        case 1:
          return direction == Direction.LEFT ? 2 : 1;
        case 2:
          return 3;
        case 3:
          return direction == Direction.LEFT ? 1 : 2;
      }
    case "S":
      switch (rotationIndex) {
        case 0:
          return direction == Direction.LEFT ? 2 : 3;
        case 1:
          return direction == Direction.LEFT ? 2 : 1;
      }
    case "Z":
      switch (rotationIndex) {
        case 0:
          return direction == Direction.LEFT ? 3 : 2;
        case 1:
          return direction == Direction.LEFT ? 1 : 2;
      }
  }
  throw new Error(
    `Invalid piece/rotation/direction combo:
    ${pieceId} 
    ${rotationIndex}
    ${direction}`
  );
}

/**
 * Determines whether a given placement can be made.
 * @param {Array<Array<number>>} currentRotationPiece
 * @param {number} xOffset - number of taps left/right
 * @param {number} y - y value of the top left corner of the piece
 * @param {*} level
 */
function canMakePlacement(
  pieceId,
  rotationIndex,
  currentRotationPiece,
  xOffset,
  y,
  startingBoard,
  level,
  shouldLog
) {
  const pieceWidth = getEffectivePieceWidth(
    pieceId,
    rotationIndex,
    xOffset > 0 ? Direction.RIGHT : Direction.LEFT
  );
  const columnNeededToClear =
    xOffset > 0 ? 5 + xOffset - pieceWidth : 5 + xOffset + pieceWidth;
  const boardHangHeight = getBoardHeight(startingBoard, columnNeededToClear);
  const placementHeight = getPiecePlacementHeight(currentRotationPiece, y);
  const canDo = tappingSpeedSufficient(
    xOffset,
    Math.max(boardHangHeight, placementHeight),
    level
  );

  if (!canDo && shouldLog) {
    console.log(
      `Out of piece movement range: [Index: ${rotationIndex}, Offset: ${xOffset}, Board Height: ${boardHangHeight}, Placement Height: ${placementHeight}, Level: ${level}]`
    );
  }

  return canDo;
}

module.exports = {
  canMakePlacement,
};
