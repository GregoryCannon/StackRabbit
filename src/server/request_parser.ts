import { PIECE_LOOKUP } from "../../docs/tetrominoes";
import { getBoardAndLinesClearedAfterPlacement } from "./board_helper";
import { DISABLE_LOGGING, LINE_CAP, MAX_CPP_PLAYOUT_MOVES } from "./params";
import { logBoard, parseBoard } from "./utils";

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
    arrWasReset: false,
    lookaheadDepth: 0,
    playoutCount: 49,
    playoutLength: 2,
    existingXOffset: 0,
    existingYOffset: 0,
    existingRotation: 0,
    existingFramesElapsed: 0,
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

      case "lookaheadDepth":
        if (requestType.includes("cpp")) {
          throw new Error(
            "Parameter 'lookaheadDepth' does not apply to C++ queries, please provide values for playoutCount and playoutLength instead."
          );
        }
        const depth = parseInt(value);
        if (depth < 0) {
          throw new Error("Invalid lookahead depth: " + depth);
        }
        if (depth > 1) {
          throw new Error("Maximum supported lookahead depth is currently 1.");
        }
        result.lookaheadDepth = depth;
        break;

      case "playoutCount":
        if (!requestType.includes("cpp")) {
          throw new Error(
            "Parameter 'playoutCount' does not apply to JS queries. Please use lookeaheadDepth instead."
          );
        }
        const count = parseInt(value);
        if (count < 0) {
          throw new Error("Invalid playout count: " + count);
        }
        if (count * result.playoutLength > MAX_CPP_PLAYOUT_MOVES) {
          throw new Error(
            `Playout volume exceeds the current limit of ${MAX_CPP_PLAYOUT_MOVES} moves. Current volume (count * length): ${
              count * result.playoutLength
            }"`
          );
        }
        result.playoutCount = count;
        break;

      case "playoutLength":
        if (!requestType.includes("cpp")) {
          throw new Error(
            "Parameter 'playoutLength' does not apply to JS queries. Please use lookeaheadDepth instead."
          );
        }
        const length = parseInt(value);
        if (length < 0) {
          throw new Error("Invalid playout length: " + length);
        }
        if (length > 20) {
          throw new Error("Invalid playout length (max is 20): " + length);
        }
        if (result.playoutCount * length > MAX_CPP_PLAYOUT_MOVES) {
          throw new Error(
            `Playout volume exceeds the current limit of ${MAX_CPP_PLAYOUT_MOVES} moves. Current volume (count * length): ${
              length * result.playoutCount
            }"`
          );
        }
        result.playoutLength = length;
        break;

      // These properties are pretty advanced, if you're using them you should know what you're doing
      case "existingXOffset":
        result.existingXOffset = parseInt(value);
        break;
      case "existingYOffset":
        result.existingYOffset = parseInt(value);
        break;
      case "existingRotation":
        result.existingRotation = parseInt(value);
        break;
      case "existingFramesElapsed":
        result.existingFramesElapsed = parseInt(value);
        break;
      case "arrWasReset":
        result.arrWasReset =
          value === "true" || value === "TRUE" || value === "1";
        break;
    }
  }

  if (!DISABLE_LOGGING) {
    logBoard(
      getBoardAndLinesClearedAfterPlacement(
        result.board,
        PIECE_LOOKUP[result.currentPiece][0][result.existingRotation],
        result.existingXOffset + 3,
        result.existingYOffset + (result.currentPiece === "I" ? -2 : -1)
      )[0]
    );
    if (result.secondBoard) {
      logBoard(result.secondBoard);
    }
  }

  // Manually top out if past line cap
  if (result.lines >= LINE_CAP) {
    result.inputFrameTimeline = "."; // Manually top out
  }

  return result;
}

export function getSearchStateFromUrlArguments(urlArgs): SearchState {
  return {
    board: urlArgs.board,
    currentPieceId: urlArgs.currentPiece,
    nextPieceId: urlArgs.nextPiece,
    level: urlArgs.level,
    lines: urlArgs.lines,
    existingXOffset: urlArgs.existingXOffset,
    existingYOffset: urlArgs.existingYOffset,
    existingRotation: urlArgs.existingRotation,
    reactionTime: urlArgs.reactionTime,
    framesAlreadyElapsed: urlArgs.existingFramesElapsed,
    canFirstFrameShift: urlArgs.arrWasReset,
  };
}

export function getCppEncodedInputString(
  searchState: SearchState,
  urlArgs: UrlArguments
) {
  const boardStr = searchState.board.map((x) => x.join("")).join("");
  const pieceLookup = ["I", "O", "L", "J", "T", "S", "Z"];
  const curPieceIndex = pieceLookup.indexOf(searchState.currentPieceId);
  const nextPieceIndex = pieceLookup.indexOf(searchState.nextPieceId);
  // Includes the final | character at the end due to how the string is parsed (cpp doesn't have an easy split method rip)
  return `${boardStr}|${searchState.level}|${searchState.lines}|${curPieceIndex}|${nextPieceIndex}|${urlArgs.inputFrameTimeline}|${urlArgs.playoutCount}|${urlArgs.playoutLength}|`;
}
