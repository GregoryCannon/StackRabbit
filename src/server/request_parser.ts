import { LINE_CAP, MAX_CPP_PLAYOUT_MOVES } from "./params";
import { INITIAL_PLACEMENT, PieceId, SearchState, UrlArguments } from "./types";
import { getDasEquivalentInputTimeline, parseBoard } from "./utils";

/**
 * Parses and validates the inputs
 * @returns {Object} an object with all the parsed arguments
 */
export function parseUrlArguments(
  reqString: string,
  requestType: string
): UrlArguments {
  // Get default values in the SearchState
  // Anything set to 'undefined' is required to be provided in the URL args
  const result: UrlArguments = {
    board: undefined,
    secondBoard: null,
    currentPiece: undefined,
    nextPiece: null,
    level: undefined,
    lines: undefined,
    reactionTime: 0,
    inputFrameTimeline: undefined,
    playoutCount: 49,
    playoutLength: 2,
    pruningBreadth: 20,
    dasCharge: -1,
  };

  // Query for non-default values
  const pairs = reqString.split("&").map((x) => x.split("="));

  for (const [argName, value] of pairs) {
    switch (argName) {
      case "board":
        result.board = parseBoard(value);
        break;

      case "secondBoard":
        if (requestType.includes("rate-move")) {
          result.secondBoard = parseBoard(value);
        } else {
          console.log(
            "Ignoring 'secondBoard' parameter, as it's not relevant to this request"
          );
        }
        break;

      case "currentPiece":
        const curPieceId = value.toUpperCase();
        if (!["I", "O", "L", "J", "T", "S", "Z"].includes(curPieceId)) {
          throw new Error("Unknown current piece:" + curPieceId);
        }
        result.currentPiece = curPieceId as PieceId;
        break;

      case "nextPiece":
        const nextPieceId = value.toUpperCase();
        if (!["I", "O", "L", "J", "T", "S", "Z"].includes(nextPieceId)) {
          throw new Error("Unknown next piece:" + nextPieceId);
        }
        result.nextPiece = nextPieceId as PieceId;
        break;

      case "level":
        const level = parseInt(value);
        if (isNaN(level) || level < 0) {
          throw new Error("Illegal level: " + value);
        }
        if (level < 18) {
          throw new Error(
            "Currently only 18, 19, and 29 starts are supported by StackRabbit. Requested: " +
            value
          );
        }
        result.level = level;
        break;

      case "lines":
        const lines = parseInt(value);
        if (isNaN(lines) || lines < 0) {
          throw new Error("Illegal line count: " + value);
        }
        result.lines = lines;
        break;

      case "reactionTime":
        const reactionTime = parseInt(value);
        if (isNaN(reactionTime) || reactionTime < 0) {
          throw new Error("Illegal reaction time: " + value);
        }
        if (reactionTime > 60) {
          throw new Error(
            "Reaction time exceeds the maximum possible of 60 frames. Did you accidentally send it in millseconds?"
          );
        }
        result.reactionTime = reactionTime;
        break;

      case "inputFrameTimeline":
        for (const char of value) {
          if (char !== "X" && char !== "." && char !== "-") {
            throw new Error("Invalid input frame timeline: " + value);
          }
        }
        // Replace hyphens with dots so that timelines can optionally be represented as X--- instead of X...
        result.inputFrameTimeline = value.replace(/-/g, ".");
        break;

      case "playoutCount":
        const count = parseInt(value);
        if (count < 0) {
          throw new Error("Invalid playout count: " + count);
        }
        if (count * result.playoutLength > MAX_CPP_PLAYOUT_MOVES) {
          throw new Error(
            `Playout volume exceeds the current limit of ${MAX_CPP_PLAYOUT_MOVES} moves. Current volume (count * length): ${count * result.playoutLength
            }"`
          );
        }
        result.playoutCount = count;
        break;

      case "playoutLength":
        const length = parseInt(value);
        if (length < 0) {
          throw new Error("Invalid playout length: " + length);
        }
        if (length > 20) {
          throw new Error("Invalid playout length (max is 20): " + length);
        }
        if (result.playoutCount * length > MAX_CPP_PLAYOUT_MOVES) {
          throw new Error(
            `Playout volume exceeds the current limit of ${MAX_CPP_PLAYOUT_MOVES} moves. Current volume (count * length): ${length * result.playoutCount
            }"`
          );
        }
        result.playoutLength = length;
        break;

      case "pruningBreadth":
        const breadth = parseInt(value);
        if (breadth < 0) {
          throw new Error("Invalid pruning breadth: " + breadth);
        }
        if (breadth > 1156) {
          throw new Error("Invalid pruning breadth (max is 1156): " + breadth); // 34 placements x 34 placements
        }
        result.pruningBreadth = breadth;
        break;

      case "dasCharge":
        result.dasCharge = parseInt(value);
        break;
    }
  }

  // Manually top out if past line cap
  if (result.lines >= LINE_CAP) {
    result.inputFrameTimeline = "."; // Manually top out
  }

  return result;
}

export function getSearchStateFromUrlArguments(urlArgs: UrlArguments): SearchState {
  return {
    board: urlArgs.board,
    currentPieceId: urlArgs.currentPiece,
    nextPieceId: urlArgs.nextPiece,
    level: urlArgs.level,
    lines: urlArgs.lines,
    existingXOffset: 0,
    existingYOffset: 0,
    existingRotation: 0,
    reactionTime: urlArgs.reactionTime,
    framesAlreadyElapsed: 0,
    dasCharge: urlArgs.dasCharge,
    adjustmentState: INITIAL_PLACEMENT
  };
}

export function getCppEncodedInputString(
  searchState: SearchState,
  urlArgs: UrlArguments
) {
  let boardStr = searchState.board.map((x) => x.join("")).join("");
  if (urlArgs.secondBoard) {
    boardStr += "|" + urlArgs.secondBoard.map((x) => x.join("")).join("");
  }
  const pieceLookup = ["I", "O", "L", "J", "T", "S", "Z"];
  const curPieceIndex = pieceLookup.indexOf(searchState.currentPieceId);
  const nextPieceIndex = pieceLookup.indexOf(searchState.nextPieceId);
  const useDAS = urlArgs.dasCharge !== -1
  // If DAS, tell the CPP backend we're a 10-12 Hz tapper 
  const effInputFrameTimeline = useDAS ? getDasEquivalentInputTimeline(urlArgs.inputFrameTimeline) : urlArgs.inputFrameTimeline;
  // Includes the final | character at the end due to how the string is parsed (cpp doesn't have an easy split method rip)
  return `${boardStr}|${searchState.level}|${searchState.lines}|${curPieceIndex}|${nextPieceIndex}|${effInputFrameTimeline}|${urlArgs.playoutCount}|${urlArgs.playoutLength}|${urlArgs.pruningBreadth}|`;
}
