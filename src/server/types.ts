export type Board = Array<Array<number>>;

export type Placement = [number, number, number]; // [numRightRotations, numShifts]

export type PieceArray = Array<Array<number>>;

export type PieceId = "I" | "O" | "L" | "J" | "T" | "S" | "Z" | null;

export type NonNullPieceId = "I" | "O" | "L" | "J" | "T" | "S" | "Z";

export interface SimParams {
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
  adjustmentState: AdjustmentState;
  dasCharge: number;
}

export interface UrlArguments {
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
  dasCharge?: number;
}

/* ----------- Move Search-Related Types ------------ */

export type MoveSearchResult = [Array<PossibilityChain>, Array<PossibilityChain>]; // [bestMoves, prunedMoves]

export interface SimState {
  x: number;
  y: number;
  frameIndex: number;
  arrFrameIndex: number; // Sometimes differs from overall frame index (during adjustments)
  rotationIndex: number;
  dasCharge: number;
  inputSequence: string;
}

export interface AdjustmentSimState extends SimState {
  adjustmentState: AdjustmentState
}

export interface LegalPlacementSimState extends SimState {
  hasAlreadyLocked: boolean;
  adjTimeSimState?: AdjustmentSimState;
}

export interface DFSState extends SimState {
  inputSequence: string;
}

export interface Possibility {
  placement: Placement;
  inputSequence: string;
  numLinesCleared: number;
  boardAfter: Board;
  inputCost: number;
  lockPositionEncoded: string;
  dasChargeAfter?: number;
  adjTimeSimState?: AdjustmentSimState;
}

export interface PossibilityChain extends Possibility {
  totalValue: number;
  searchStateAfterMove: SearchState; // The search state after the current move
}

export interface SearchState {
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
  adjustmentState: AdjustmentState;
  dasCharge: number; // 0 to 16
}

export interface PhantomPlacement {
  inputSequence: string;
  initialPlacement: PossibilityChain;
  adjustmentSearchState?: SearchState | null; // Would be null if the default placement has the piece lock before reaction time
  possibleAdjustmentsLookup?: Array<Possibility>;
}

export const enum AiMode {
  STANDARD,
  DIG,
  DIG_INTO_KILLSCREEN,
  NEAR_KILLSCREEN,
  KILLSCREEN,
  KILLSCREEN_FOR_TETRISES,
  IMMINENT_DEATH,
}

export const enum DasButtonHeld {
  LEFT,
  RIGHT,
  NONE,
}

export interface AdjustmentState {
  readonly isAdjustment: boolean;
  readonly isArrContinued: boolean;
  readonly dasButtonHeld: DasButtonHeld;
}

export const INITIAL_PLACEMENT = {
  isAdjustment: false,
  isArrContinued: false,
  dasButtonHeld: DasButtonHeld.NONE
} as const satisfies AdjustmentState;

export function getAdjustmentState(isArrContinued: boolean, dasButtonHeld: DasButtonHeld) {
  return {
    isAdjustment: true,
    isArrContinued,
    dasButtonHeld,
  } as const satisfies AdjustmentState;
}

/* ------------ Messages for Worker Threads ------------ */

export interface WorkerDataArgs {
  piece: PieceId;
  newSearchState: SearchState;
  inputFrameTimeline: string;
}

export interface WorkerResponse {
  type: string;
  piece?: PieceId;
  result?: PossibilityChain;
}
