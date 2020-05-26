import { ROW, COLUMN, VACANT } from "./constants.js";

export function debugPrintBoard(board) {
  let boardStr = "";
  for (let r = 0; r < ROW; r++) {
    for (let c = 0; c < COLUMN; c++) {
      boardStr += board[r][c] == VACANT ? "_" : "0";
    }
    boardStr += "\n";
  }
  console.log(boardStr);
}
