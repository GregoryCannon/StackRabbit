import { PIECE_LOOKUP } from "../../built/src/tetrominoes";
import { getTestBoardWithHeight } from "./board_helper";
import {
  canDoPlacement,
  getPossibleMoves,
  placementIsLegal,
} from "./move_search";
import { generateInputFrameTimeline, GetGravity } from "./utils";

function legalMovesTest() {
  const BOARD_3 = getTestBoardWithHeight(3);
  const verifyNumMoves = (pieceId, expectedLength) => {
    const possibilites = getPossibleMoves(
      BOARD_3,
      pieceId,
      18,
      0,
      0,
      0,
      "X...",
      0,
      false,
      false
    );
    const adjustmentPossibilites = getPossibleMoves(
      BOARD_3,
      pieceId,
      18,
      0,
      2,
      1,
      "X...",
      pieceId == "O" ? 0 : 1,
      false,
      false
    );
    if (new Set(possibilites).size !== expectedLength) {
      console.log(possibilites.map((x) => x.placement));
      throw new Error(`Found duplicate possibilities for ${pieceId} piece`);
    }
    if (possibilites.length !== expectedLength) {
      throw new Error(
        `Expected ${expectedLength} moves for ${pieceId} piece, instead got ${possibilites.length}`
      );
    }
    if (new Set(possibilites).size !== expectedLength) {
      console.log(possibilites.map((x) => x.placement));
      throw new Error(
        `Found duplicate adjustment possibilities for ${pieceId} piece`
      );
    }
    if (possibilites.length !== expectedLength) {
      throw new Error(
        `Expected ${expectedLength} adjustment moves for ${pieceId} piece, instead got ${possibilites.length}`
      );
    }
  };

  verifyNumMoves("O", 9);

  verifyNumMoves("I", 17);
  verifyNumMoves("S", 17);
  verifyNumMoves("Z", 17);

  verifyNumMoves("L", 34);
  verifyNumMoves("J", 34);
  verifyNumMoves("T", 34);
}

function tapRangeTest() {
  const timeline10Hz = generateInputFrameTimeline([5]);
  const timeline12Hz = generateInputFrameTimeline([4]);
  const timeline13Hz = generateInputFrameTimeline([4, 3]);
  const timeline13_5Hz = generateInputFrameTimeline([4, 3, 3]);
  const timeline14Hz = generateInputFrameTimeline([4, 3, 3, 3]);

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

  const expected16 = true;
  if (
    canDoPlacement(getTestBoardWithHeight(9), 29, "I", 1, 3, timeline13_5Hz) !==
    expected16
  ) {
    console.log(`Failed: 9 high 3 tap 29 13.5 Hz. Expected: ${expected16}`);
  }
}

function lastMinuteRotationsTest() {
  let expected1 = true;
  if (
    placementIsLegal(2, 0, {
      board: getTestBoardWithHeight(14),
      initialX: 3,
      initialY: -1,
      gravity: GetGravity(29),
      framesAlreadyElapsed: 0,
      inputFrameTimeline: "X...",
      rotationsList: PIECE_LOOKUP["J"][0] as Array<PieceArray>,
      pieceId: "J",
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
      gravity: GetGravity(29),
      framesAlreadyElapsed: 0,
      inputFrameTimeline: "X...",
      rotationsList: PIECE_LOOKUP["J"][0] as Array<PieceArray>,
      pieceId: "J",
      existingRotation: 0,
      canFirstFrameShift: false,
    }) !== expected2
  ) {
    console.log(`Failed: double rotate J 15 high 29. Expected ${expected2}`);
  }
}

function speedTest(x) {
  console.time("\nspeedtest");
  for (let i = 0; i < 10; i++) {
    getPossibleMoves(
      getTestBoardWithHeight(x),
      "L",
      18,
      0,
      0,
      0,
      "X...",
      0,
      false,
      false
    );
  }
  console.timeEnd("\nspeedtest");
}

// MAIN

for (let i = 0; i < 20; i++) {
  speedTest(4);
}
tapRangeTest();
lastMinuteRotationsTest();
legalMovesTest();
