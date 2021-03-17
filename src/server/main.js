const evaluator = require("./evaluator");
const BoardHelper = require("./board_helper");
const { AI_MODE } = require("./params");

function getMove(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  shouldLog,
  aiParams
) {
  const possibilityList = BoardHelper.getPossibleMoves(
    startingBoard,
    currentPieceId,
    level,
    /* shouldLog= */ false && shouldLog
  );
  // const aiMode = evaluator.getAiMode(startingBoard, lines);
  const aiMode = AI_MODE.STANDARD;

  // Get the top contenders, sorted best -> worst
  const NUM_TO_CONSIDER = 20;
  const topN = evaluator.pickBestNMoves(
    possibilityList,
    nextPieceId,
    level,
    lines,
    aiMode,
    NUM_TO_CONSIDER,
    aiParams
  );

  if (shouldLog) {
    console.log("\n\n---------");
  }

  // For each contender, place the next piece and maximize the resulting value
  let bestPossibilityAfterNextPiece = null;
  let bestValueAfterNextPiece = -999;
  for (const possibility of topN) {
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
    if (innerBestMove == null) {
      continue;
    }
    const originalMovePartialValue = evaluator.getLineClearValue(
      possibility[4],
      aiParams
    );
    const totalValue = innerBestMove[6] + originalMovePartialValue;

    if (shouldLog) {
      console.log(
        `\nCurrent move: ${possibility[0]}, ${possibility[1]}. Next move: ${innerBestMove[0]}, ${innerBestMove[1]}.`
      );
      console.log(
        `Surface: ${innerBestMove[2]}, inner value: ${innerBestMove[6]}, original partial value: ${originalMovePartialValue}, total value: ${totalValue}`
      );
      console.log(innerBestMove[7]); // Log explanation
      console.log("---------------------------------------------");
    }

    if (totalValue > bestValueAfterNextPiece) {
      bestValueAfterNextPiece = totalValue;
      bestPossibilityAfterNextPiece = possibility;
    }
  }

  if (shouldLog) {
    console.log(
      `\nSelected: ${bestPossibilityAfterNextPiece[0]}, ${bestPossibilityAfterNextPiece[1]}`
    );
  }

  // Send back the highest value move after the next piece is placed
  return bestPossibilityAfterNextPiece;
}

module.exports = { getMove };
