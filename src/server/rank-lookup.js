const fs = require("fs");
const ranks_NoNextBox_NoBars = fs.readFileSync(
  "docs/condensed_NoNextBox_NoBars.txt",
  "utf8"
);

/**
 * Converts a list of board heights into the index into the ranks array.
 * Uses fractal's proprietary hashing method which involves base 9 and other shenanigans.
 * e.g
 *     3  2  2  2  1  1  2  1  0
 *  ->   -1  0  0 -1  0  1 -1 -1    make diff array
 *  ->    3  4  4  3  4  5  3  3    add 4
 *  ->    parse that number as base 10
 * @param {Array<number>} surfaceHeightsArray - DOES NOT INCLUDE COL 10
 */
function surfaceHeightsToNoNBIndex(surfaceHeightsArray) {
  let diffs = [];
  for (let i = 0; i < surfaceHeightsArray.length - 1; i++) {
    diffs.push(
      parseInt(surfaceHeightsArray[i + 1]) -
        parseInt(surfaceHeightsArray[i]) +
        4
    );
  }
  const diffString = diffs.join("");
  return parseInt(diffString, 9);
}

/**
 * Converts a list of board heights, along with the next piece ID into the index into the ranks array.
 * Is encoded such that the index / 7 = the standard NNB index, and index % 7 = the next piece index,
 * in the array [TJZOSLI].
 * @param {Array<number>} surfaceArray - DOES NOT INCLUDE COL 10
 */
function surfaceHeightsToNBIndex(surfaceArray, nextPieceId) {
  const noNBIndex = surfaceHeightsToNoNBIndex(surfaceArray);
  const pieceIndex = ["T", "J", "Z", "O", "S", "L", "I"].findIndex(
    (x) => x == nextPieceId
  );
  return 7 * noNBIndex + pieceIndex;
}
/**
 * Looks up the rank at a given index in a rank string, and decodes it from the
 * custom space-saving string encoding.
 * @param {string} rankStr
 * @param {number} index
 */
function lookUpRankInString(lookupStr, index) {
  const rankStr = lookupStr.substr(index * 2, 2);
  return parseInt(rankStr, 36) / 10;
}

function getValueOfBoardSurfaceNoNextBox(surfaceArray) {
  const index = surfaceHeightsToNoNBIndex(surfaceArray);
  return lookUpRankInString(ranks_NoNextBox_NoBars, index) - 1; // All ranks get 1 by default, so cancel that out
}

function getValueOfBoardSurface(surfaceArray) {
  if (surfaceArray.length == 10) {
    throw new Error("Expected surface of length 9");
  }
  return getValueOfBoardSurfaceNoNextBox(surfaceArray);
}

module.exports = {
  getValueOfBoardSurface,
};
