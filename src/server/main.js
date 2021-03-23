const evaluator = require("./evaluator");
const BoardHelper = require("./board_helper");
const { NUM_TO_CONSIDER, modifyParamsForAiMode } = require("./params");

/**
 * Finds the N highest valued moves, without doing any search into placements of the next piece.
 * Can be called with or without a next piece, and will function accordingly.
 */
function getMostPromisingMoves(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  shouldLog,
  aiParams
) {
  // Get the possible moves
  const possibilityList = BoardHelper.getPossibleMoves(
    startingBoard,
    currentPieceId,
    level,
    /* shouldLog= */ false && shouldLog
  );

  // Get the AI mode (e.g. digging, scoring)
  const aiMode = evaluator.getAiMode(startingBoard, lines);
  aiParams = modifyParamsForAiMode(aiParams, aiMode);

  // Get the top contenders, sorted best -> worst
  const topN = evaluator.pickBestNMoves(
    possibilityList,
    nextPieceId,
    level,
    lines,
    aiMode,
    NUM_TO_CONSIDER,
    aiParams
  );
  return { topN, aiMode, aiParams };
}

function getBestMoveNoSearch(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  shouldLog,
  initialAiParams
) {
  let { topN } = getMostPromisingMoves(
    startingBoard,
    currentPieceId,
    nextPieceId,
    level,
    lines,
    shouldLog,
    initialAiParams
  );
  return topN ? topN[0] : null;
}

function getBestMoveWithSearch(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  shouldLog,
  initialAiParams
) {
  const startTime = Date.now();

  const { topN, aiMode, aiParams } = getMostPromisingMoves(
    startingBoard,
    currentPieceId,
    nextPieceId,
    level,
    lines,
    shouldLog,
    initialAiParams
  );

  const time2 = Date.now();
  console.log("\tElapsed to get N most promising moves:", time2 - startTime);
  if (shouldLog) {
    console.log("\n\n---------");
  }

  // For each contender, place the next piece and maximize the resulting value
  let bestPossibilityAfterNextPiece = null;
  let bestValueAfterNextPiece = -999;
  let bestIndex = 0; // The rank of the best placement (in terms of the original 'promising-ness' sort)
  let i = 0;
  for (const possibility of topN) {
    i++;
    // Place the next piece in each possibility
    const trialBoard = possibility[5];
    const innerPossibilityList = BoardHelper.getPossibleMoves(
      trialBoard,
      nextPieceId,
      level,
      /* shouldLog= */ false && shouldLog
    );
    const innerBestMove = evaluator.pickBestMoveNoNextBox(
      innerPossibilityList,
      level,
      lines,
      aiMode,
      /* shouldLog= */ false && shouldLog,
      aiParams
    );

    // Get a total score for this possibility (including line clears from the outer placement)
    if (innerBestMove == null) {
      continue;
    }
    const originalMovePartialValue = evaluator.getLineClearValue(
      possibility[4],
      aiParams
    );
    const totalValue = innerBestMove[6] + originalMovePartialValue;

    // If new best, update local vars
    if (totalValue > bestValueAfterNextPiece) {
      bestValueAfterNextPiece = totalValue;
      bestPossibilityAfterNextPiece = possibility;
      bestIndex = i;
    }

    // Log details about the top-level possibility
    if (shouldLog) {
      console.log(
        `\nCurrent move: ${possibility[0]}, ${possibility[1]}. Next move: ${innerBestMove[0]}, ${innerBestMove[1]}.`
      );
      console.log("Final state eval:", innerBestMove[7], "mode:", aiMode); // Log inner explanation
      console.log(
        `\nSurface: ${innerBestMove[2]}, inner value: ${innerBestMove[6]}, original partial value: ${originalMovePartialValue}, \nFINAL TOTAL: ${totalValue}`
      );
      console.log("---------------------------------------------");
    }
  }

  // Log performance info
  const msElapsedMoves = Date.now() - time2;
  console.log("\tElapsed per possibility:", msElapsedMoves / topN.length);
  console.log("\tElapsed on all moves:", msElapsedMoves);

  if (shouldLog && bestPossibilityAfterNextPiece) {
    console.log(
      `\nSelected: ${bestPossibilityAfterNextPiece[0]}, ${bestPossibilityAfterNextPiece[1]}`
    );
  }

  console.log("# Candidates:", topN.length, "Selected:", bestIndex);

  // Send back the highest value move after the next piece is placed
  return bestPossibilityAfterNextPiece;
}

module.exports = { getBestMoveWithSearch, getBestMoveNoSearch };
