type Board = Array<Array<number>>;

type Placement = [number, number]; // [numRightRotations, numShifts]

type PieceArray = Array<Array<number>>;

type PieceId = "I" | "O" | "L" | "J" | "T" | "S" | "Z" | null;

type SimulatedGameResult = [number, number, number, number]; // score, lines, level, numHoles

type ReachableCols = [number, number, number, number]; // 4 tap left, 4 tap right, 5 tap left, 5 tap right

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

const testResponseObj = [
  {
    piece: "T",
    inputSequence: "I...R...R.......................********",
    isSpecialMove: false,
    totalValue: 31.424,
    adjustments: [
      {
        piece: "T",
        inputSequence: "..B..........***********",
        totalValue: 33.71,
        isSpecialMove: true,
        followUp: {
          piece: "L",
          inputSequence: "A...A...........**********",
          totalValue: 29.411,
        },
      },
      {
        piece: "T",
        inputSequence: ".............***********",
        totalValue: 32.589,
        isSpecialMove: false,
        followUp: {
          piece: "L",
          inputSequence: "A...A...........**********",
          totalValue: 28.395,
        },
      },
    ],
  },
  {
    piece: "T",
    inputSequence: "E...E...L...L......................********",
    isSpecialMove: false,
    totalValue: 26.335,
    adjustments: [
      {
        piece: "T",
        inputSequence: ".............***********",
        isSpecialMove: false,
        totalValue: 30.689,
        followUp: {
          piece: "L",
          inputSequence: "A...A...........**********",
          totalValue: 28.395,
        },
      },
    ],
  },
];

/* ----------- Engine Lookup Data Structures ----------- */

type EngineResult = Array<
  [
    /* defaultPlacement: */ PossibilityChain,
    /* adjustments: */ Array<PossibilityChain>
  ]
>;

interface FormattedMove {
  piece: PieceId;
  inputSequence: string;
  isSpecialMove: boolean;
  totalValue: number;
}

interface FormattedAdjustment extends FormattedMove {
  followUp: FormattedMove;
}

interface FormattedInitialMove extends FormattedMove {
  adjustments: Array<FormattedAdjustment>;
}

type EngineResponseJson = Array<FormattedInitialMove>;

/* ----------- Move Search-Related Types ------------ */

type MoveSearchResult = [Array<PossibilityChain>, Array<PossibilityChain>]; // [bestMoves, prunedMoves]

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
  evExplanation?: string;
}

interface HypotheticalResult {
  expectedValue: number;
  bestMoves: Array<HypotheticalBestMove>;
  possibilityChain: PossibilityChain;
}

interface HypotheticalBestMove {
  pieceSequence: string;
  moveSequence: Array<Placement>;
  moveSequenceAsInputs: Array<string>;
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
  initialPlacement: PossibilityChain;
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
