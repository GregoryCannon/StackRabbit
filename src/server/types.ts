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

interface PossibilityDepth2 extends Possibility {
  innerPlacement: Placement;
  outerPlacementPartialValue: number;
  totalValue: number;
}

interface SearchState {
  board: Board;
  currentPieceId: PieceId;
  nextPieceId: PieceId;
  level: number;
  lines: number;
  existingXOffset: number;
  existingYOffset: number;
  firstShiftDelay: number;
  existingRotation: number;
}

const enum AiMode {
  STANDARD,
  DIG,
  NEAR_KILLSCREEN,
  KILLSCREEN,
}

interface AiParams {
  AVG_HEIGHT_EXPONENT: number;
  AVG_HEIGHT_COEF: number;
  BURN_COEF: number;
  COL_10_COEF: number;
  MAX_DIRTY_TETRIS_HEIGHT: number;
  EXTREME_GAP_COEF: number;
  BUILT_OUT_LEFT_COEF: number;
  BUILT_OUT_RIGHT_COEF: number;
  HOLE_COEF: number;
  HOLE_WEIGHT_COEF: number;
  SPIRE_HEIGHT_EXPONENT: number;
  SPIRE_HEIGHT_COEF: number;
  SCARE_HEIGHT_18: number;
  SCARE_HEIGHT_19: number;
  SCARE_HEIGHT_29: number;
  HIGH_COL_9_COEF_COEF: number;
  SURFACE_COEF: number;
  TETRIS_BONUS: number;
  TETRIS_READY_BONUS: number;
  TETRIS_READY_BONUS_BAR_NEXT: number;
  INACCESSIBLE_LEFT_COEF: number;
  INACCESSIBLE_RIGHT_COEF: number;
  TAP_ARR: number;
  FIRST_TAP_DELAY: number;
}

interface ParamMods {
  DIG: any;
  NEAR_KILLSCREEN: any;
  KILLSCREEN: any;
}
