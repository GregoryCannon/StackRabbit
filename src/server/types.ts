type Board = Array<Array<number>>;

type Placement = [number, number]; // [numRightRotations, numShifts]

type LockPlacement = [number, number, number, string]; // [numRightRotations, x, y, inputSequence, arrFrameIndex]

type PieceArray = Array<Array<number>>;

type PieceId = "I" | "O" | "L" | "J" | "T" | "S" | "Z" | null;

type Direction = "L" | "R" | "";

type SimulatedGameResult = [number, number, number, number]; // score, lines, level, numHoles

interface SimParams {
  board: Board;
  initialX: number;
  initialY: number;
  framesAlreadyElapsed: number;
  gravity: number;
  rotationsList: Array<PieceArray>;
  pieceId: PieceId;
  existingRotation: number;
  inputFrameTimeline: string;
  canFirstFrameShift: boolean;
}

/* ----------- Move Search-Related Types ------------ */

type MoveSearchResult = [Array<PossibilityChain>, Array<PossibilityChain>];

interface SimState {
  x: number;
  y: number;
  frameIndex: number;
  arrFrameIndex: number; // Sometimes differs from overall frame index (during adjustments)
  rotationIndex: number;
}

interface BFSState extends SimState {
  hasPassedOnInput: boolean;
  grammarToken: string;
  inputSequence: string; // Tracks the sequence up to this state
}

interface DFSState extends SimState {
  inputSequence: string;
}

interface LiteGameState {
  board: Board;
  score: number;
  lines: number;
  level: number;
  numHoles: number;
  nextTransitionLineCount: number;
  pieceSequence: Array<PieceId>;
  pieceIndex: number;
  gameOver: boolean;
}

interface Possibility {
  placement: Placement;
  inputSequence: string;
  surfaceArray: Array<number>;
  numHoles: number;
  holeCells: Set<number>;
  numLinesCleared: number;
  boardAfter: Board;
  inputCost: number;
  lockPositionEncoded: string;
  fastEvalScore?: number;
  evalScore?: number;
  evalExplanation?: string;
}

interface PossibilityChain extends Possibility {
  totalValue: number;
  searchStateAfterMove: SearchState; // The search state after the current move
  partialValue?: number; // If it has subsequent moves, the value of just the line clears involved in this move
  innerPossibility?: PossibilityChain; // The subsequent move in the chain, or null if this is the end of the chain
  expectedValue?: number; // If hypothetical analysis has been done, the EV of this possibility chain.
}

interface HypotheticalResult {
  expectedValue: number;
  bestMoves: Array<HypotheticalBestMove>;
  possibilityChain: PossibilityChain;
}

interface HypotheticalBestMove {
  pieceSequence: string;
  moveSequence: Array<Placement>;
  resultingValue: number;
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
}

interface PhantomPlacement {
  inputSequence: string;
  defaultPlacement: PossibilityChain;
  adjustmentSearchState: SearchState;
  possibleAdjustmentsLookup?: Map<PieceId, Array<Possibility>>;
}

const enum AiMode {
  STANDARD,
  DIG,
  NEAR_KILLSCREEN,
  KILLSCREEN,
  KILLSCREEN_RIGHT_WELL,
}

/* ----------- Evaluation Parameters ------------- */

interface InitialAiParams {
  AVG_HEIGHT_EXPONENT: number;
  AVG_HEIGHT_COEF: number;
  BURN_COEF: number;
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
  SCARE_HEIGHT_OFFSET: number;
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
  computationType: string; // Either 'finesse' or 'standard'
  piece: PieceId;
  newSearchState: SearchState;
  initialAiParams: InitialAiParams;
  paramMods: ParamMods;
  inputFrameTimeline: string;
}

interface WorkerResponse {
  type: string;
  piece?: PieceId;
  result?: PossibilityChain;
}
