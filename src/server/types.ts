type Board = Array<Array<number>>;

type PieceArray = Array<Array<number>>;

interface SimParams {
  board: Board;
  initialX: number;
  initialY: number;
  firstShiftDelay: number;
  maxGravity: number;
  maxArr: number;
  rotationsList: Array<Array<Array<number>>>;
  existingRotation: number;
}
