type Board = Array<Array<number>>;

type Placement = [number, number];

type PieceArray = Array<Array<number>>;

type PieceId = "I" | "O" | "L" | "J" | "T" | "S" | "Z" | null;

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

interface Possibility {
  placement: Placement;
  surfaceArray: Array<number>;
  numHoles: number;
  numLinesCleared: number;
  boardAfter: Board;
  evalScore?: number;
  evalExplanation?: string;
}

interface ChainPossibility extends Possibility {
  parentPlacement: Placement;
}
