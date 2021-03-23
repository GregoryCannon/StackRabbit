const evaluator = require("./evaluator");
const BoardHelper = require("./board_helper");
const {
  AI_MODE,
  NUM_TO_CONSIDER,
  DIG_MODIFICATIONS,
  NEAR_KILLSCREEN_MODIFICATIONS,
} = require("./params");

function modifyParamsForAiMode(aiParams, aiMode) {
  // Modify the AI params based on the AI mode
  if (aiMode === AI_MODE.DIG) {
    // Modify some of the evaluation weights based on the fact that we're digging
    aiParams = JSON.parse(JSON.stringify(aiParams));
    for (const key in DIG_MODIFICATIONS) {
      aiParams[key] = DIG_MODIFICATIONS[key];
    }
  } else if (aiMode === AI_MODE.NEAR_KILLSCREEN) {
    // Modify some of the evaluation weights based on the fact that we're near killscreen
    aiParams = JSON.parse(JSON.stringify(aiParams));
    for (const key in NEAR_KILLSCREEN_MODIFICATIONS) {
      aiParams[key] = NEAR_KILLSCREEN_MODIFICATIONS[key];
    }
  }
  return aiParams;
}

function getMove(
  startingBoard,
  currentPieceId,
  nextPieceId,
  level,
  lines,
  shouldLog,
  aiParams
) {
  const startTime = Date.now();
  const possibilityList = BoardHelper.getPossibleMoves(
    startingBoard,
    currentPieceId,
    level,
    /* shouldLog= */ false && shouldLog
  );
  const aiMode = evaluator.getAiMode(startingBoard, lines);
  aiParams = modifyParamsForAiMode(aiParams, aiMode);

  const time2 = Date.now()
  console.log("Elapsed to get possible moves:", time2 - startTime);

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

  const time3 = Date.now()
  console.log("Elapsed to get N most promising moves:", time3 - time2);

  if (shouldLog) {
    console.log("\n\n---------");
  }

  // For each contender, place the next piece and maximize the resulting value
  let bestPossibilityAfterNextPiece = null;
  let bestValueAfterNextPiece = -999;
  let bestIndex = 0;
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
      console.log("Final state eval:", innerBestMove[7]); // Log inner explanation
      console.log(
        `\nSurface: ${innerBestMove[2]}, inner value: ${innerBestMove[6]}, original partial value: ${originalMovePartialValue}, \nFINAL TOTAL: ${totalValue}`
      );
      console.log("---------------------------------------------");
    }

    if (totalValue > bestValueAfterNextPiece) {
      bestValueAfterNextPiece = totalValue;
      bestPossibilityAfterNextPiece = possibility;
      bestIndex = i;
    }

    console.log("Elapsed from start of move eval to last move:", Date.now() - time2);
  }

  if (shouldLog && bestPossibilityAfterNextPiece) {
    console.log(
      `\nSelected: ${bestPossibilityAfterNextPiece[0]}, ${bestPossibilityAfterNextPiece[1]}`
    );
  }

  console.log("# Candidates:", topN.length, "Selected:", bestIndex);

  // Send back the highest value move after the next piece is placed
  return bestPossibilityAfterNextPiece;
}

module.exports = { getMove };
