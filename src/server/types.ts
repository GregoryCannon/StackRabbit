type Board = Array<Array<number>>;

type PieceArray = Array<Array<number>>;

interface SimParams {
  board: Board;
  initialX: number;
  initialY: number;
  firstShiftDelay: number;
  maxGravity: number;
  maxArr: number;
  rotationsList: Array<PieceArray>;
  existingRotation: number;
}

interface SimState {
    x: number;
    y: number;
    gravityCounter: number;
    arrCounter: number;
    rotationIndex: number;
}
