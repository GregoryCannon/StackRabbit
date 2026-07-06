type Board = Array<Array<number>>;

type Placement = [number, number, number]; // [numRightRotations, numShifts]

type PieceArray = Array<Array<number>>;

type PieceId = "I" | "O" | "L" | "J" | "T" | "S" | "Z" | null;

interface SimParams {
  board: Board;
  initialX: number;
  initialY: number;
  framesAlreadyElapsed: number;
  gravity: number;
  doubleGravity: boolean;
  rotationsList: Array<PieceArray>;
  pieceId: PieceId;
  existingRotation: number;
  inputFrameTimeline: string;
  canFirstFrameShift: boolean;
  dasCharge: number;
}

interface UrlArguments {
  board: Board;
  secondBoard?: Board;
  currentPiece?: PieceId;
  nextPiece?: PieceId;
  level?: number;
  lines?: number;
  reactionTime?: number;
  inputFrameTimeline?: string;
  playoutCount: number; // Only used in C++ queries
  playoutLength: number; // Only used in C++ queries
  pruningBreadth: number; // Only used in C++ queries
  arrWasReset?: boolean;
  existingXOffset?: number;
  existingYOffset?: number;
  existingRotation?: number;
  existingFramesElapsed?: number;
  dasCharge?: number;
}

/* ----------- Move Search-Related Types ------------ */

type MoveSearchResult = [Array<PossibilityChain>, Array<PossibilityChain>]; // [bestMoves, prunedMoves]

interface SimState {
  x: number;
  y: number;
  frameIndex: number;
  arrFrameIndex: number; // Sometimes differs from overall frame index (during adjustments)
  rotationIndex: number;
  dasCharge: number;
  inputSequence: string;
}

interface LegalPlacementSimState extends SimState {
  hasAlreadyLocked: boolean;
}

interface DFSState extends SimState {
  inputSequence: string;
}

interface Possibility {
  placement: Placement;
  inputSequence: string;
  numLinesCleared: number;
  boardAfter: Board;
  inputCost: number;
  lockPositionEncoded: string;
  dasChargeAfter?: number;
}

interface PossibilityChain extends Possibility {
  totalValue: number;
  searchStateAfterMove: SearchState; // The search state after the current move
  partialValue?: number; // If it has subsequent moves, the value of just the line clears involved in this move
  innerPossibility?: PossibilityChain; // The subsequent move in the chain, or null if this is the end of the chain
  expectedValue?: number; // If hypothetical analysis has been done, the EV of this possibility chain.
}

interface SearchState {
  board: Board;
  currentPieceId: PieceId;
  nextPieceId: PieceId;
  level: number;
  lines: number;
  framesAlreadyElapsed: number;
  existingXOffset: number;
  existingYOffset: number;
  existingRotation: number;
  reactionTime: number;
  canFirstFrameShift: boolean;
  dasCharge: number; // 0 to 16
  dasButtonHeld: DasButtonHeld;
}

interface PhantomPlacement {
  inputSequence: string;
  initialPlacement: Possibility;
  adjustmentSearchState: SearchState;
  possibleAdjustmentsLookup?: Array<Possibility>;
}

const enum AiMode {
  STANDARD,
  DIG,
  DIG_INTO_KILLSCREEN,
  NEAR_KILLSCREEN,
  KILLSCREEN,
  KILLSCREEN_FOR_TETRISES,
  IMMINENT_DEATH,
}

const enum DasButtonHeld {
  LEFT,
  RIGHT,
  NONE,
}

/* ----------- Evaluation Parameters ------------- */

interface InitialAiParams {
  AVG_HEIGHT_EXPONENT: number;
  AVG_HEIGHT_COEF: number;
  BURN_COEF: number;
  BURN_COEF_POST: number;
  COL_10_COEF: number;
  COL_10_HEIGHT_MULTIPLIER_EXP: number;
  DEAD_COEF: number;
  MAX_DIRTY_TETRIS_HEIGHT: number;
  EXTREME_GAP_COEF: number;
  BUILT_OUT_LEFT_COEF: number;
  BUILT_OUT_RIGHT_COEF: number;
  LOW_LEFT_EXP: number;
  HOLE_COEF: number;
  HOLE_WEIGHT_COEF: number;
  SPIRE_HEIGHT_EXPONENT: number;
  SPIRE_HEIGHT_COEF: number;
  UNABLE_TO_BURN_COEF: number;
  UNABLE_TO_BURN_HEIGHT_EXP: number;
  HIGH_COL_9_COEF: number;
  HIGH_COL_9_EXP: number;
  SURFACE_COEF: number;
  LEFT_SURFACE_COEF: number;
  TETRIS_COEF: number;
  TETRIS_READY_COEF: number;
  INACCESSIBLE_LEFT_COEF: number;
  INACCESSIBLE_RIGHT_COEF: number;
}

interface AiParams extends InitialAiParams {
  INPUT_FRAME_TIMELINE: string;
  MAX_5_TAP_LOOKUP: Object;
  MAX_4_TAP_LOOKUP: Object;
  BURN_QUOTA?: number; // Can optionally have a limit to the number of burns
}

interface ParamMods {
  DIG: any;
  NEAR_KILLSCREEN: any;
  KILLSCREEN: any;
}

/* ------------ Messages for Worker Threads ------------ */

interface WorkerDataArgs {
  piece: PieceId;
  newSearchState: SearchState;
  inputFrameTimeline: string;
}

interface WorkerResponse {
  type: string;
  piece?: PieceId;
  result?: PossibilityChain;
}
