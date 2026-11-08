#include "full_move_search.hpp"
#include "high_level_search.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>

using namespace std;

/**
 * Synchronous precompute pipeline. Chooses the current piece's placement by how
 * well each reachable mid-air adjustment leads into every possible next piece:
 *
 *   1. One full move search for the current piece with the player's real
 *      timeline, DAS charge and reaction time. No legal move ends the request.
 *   2. One lock value lookup per next piece, run in-process on the same game
 *      state. Each map value is rounded to two decimals first, because the
 *      collapse sums the values the way they arrive over the wire.
 *   3. Phantom placements: the placements whose input prefix still fits inside
 *      the reaction time, each carrying the mid-air state captured at the
 *      reaction-time frame. A reaction time of zero means the piece has not
 *      been placed yet, so the single phantom starts from the spawn state.
 *   4. One adjustment move search per phantom from its mid-air state.
 *   5. The collapse: per phantom, the best value per next piece weighted by
 *      the NES piece-transition probabilities and averaged over the seven
 *      pieces; the phantom with the highest average wins.
 *   6. The response string: a default line plus one line per next piece.
 *
 * The lock lookup and the adjustment search are two separate reachable sets by
 * design: the lookup values each lock cell once from a canonical spawn
 * context, while the adjustment search runs from per-phantom midair states.
 * Their intersection is what the collapse may consider; a cell outside the
 * lookup is skipped, and on a near-topped board an empty lookup makes the
 * per-piece fallback the intended response.
 */

namespace {

/** Number.MIN_SAFE_INTEGER, the starting value of the maximisation loops. */
const double PRECOMPUTE_MIN_VALUE = -9007199254740991.0;

/** The next pieces in response order; PIECE_CHAR_LIST shares this order. */
const char *const RESPONSE_PIECE_ORDER = "IOLJTSZ";

/** One placement outcome shaped for the collapse and the formatters. */
struct PrecomputePossibility {
  int relRot; // placement relative to the search's entry state
  int relX;
  int relY;
  int rot; // absolute lock coordinates: the lock value map keys
  int x;
  int y;
  std::string inputSequence;
  int inputCostX10; // 0 basic; -1 tuck; -2 spin; -3 spin-tuck
  int dasChargeAfter;
  int numLinesCleared;
};

/** One candidate initial placement and its adjustment search. */
struct PrecomputePhantom {
  std::string inputSequence; // the dedupe prefix (first reactionTime chars)
  bool hasInitialPlacement = false;
  PrecomputePossibility initial{};
  bool hasAdjustmentState = false;
  FullMoveSearchContext adjustmentCtx;
  int adjustmentDasCharge = 0;
  std::vector<PrecomputePossibility> possibleAdjustments;
};

int numOrientationsOf(const Piece *piece) {
  for (int r = 0; r < 4; r++) {
    if (piece->rowsByRotation[r][0] == NONE) {
      return r;
    }
  }
  return 4;
}

/** countInputs over the relative placement: a pure rotation counts as one. */
int countInputsOf(int relRot, int relX) {
  if (relRot == 3) {
    return 1 + abs(relX);
  }
  return relRot + abs(relX);
}

std::string encodeLockCell(int rot, int x, int y) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%d|%d|%d", rot, x, y);
  return std::string(buffer);
}

/** The 200-character 0/1 rendering of a board (bit 9 is the leftmost column). */
std::string renderBoard(unsigned int board[20]) {
  std::string out;
  out.reserve(200);
  for (int r = 0; r < 20; r++) {
    for (int c = 0; c < 10; c++) {
      out += ((board[r] >> (9 - c)) & 1U) ? '1' : '0';
    }
  }
  return out;
}

/** Places the piece and collapses full rows, mirroring the move search's own
 * board-after computation. Returns the number of cleared lines. */
int boardAfterLock(unsigned int board[20], const Piece *piece, int rotationIndex, int x, int y,
                   OUT unsigned int newBoard[20]) {
  unsigned int temp[20];
  memcpy(temp, board, sizeof(temp));
  for (int r = 0; r < 4; r++) {
    const int pieceRow = piece->rowsByRotation[rotationIndex][r];
    if (pieceRow == NONE) {
      break;
    }
    if (pieceRow == 0) {
      continue;
    }
    if (y + r < 0 || y + r >= 20) {
      continue;
    }
    temp[y + r] |= SHIFTBY(pieceRow, x) & 0x3FF;
  }

  int cleared = 0;
  int writeRow = 19;
  for (int r = 19; r >= 0; r--) {
    if ((temp[r] & 0x3FF) == 0x3FF) {
      cleared++;
      continue;
    }
    temp[writeRow--] = temp[r];
  }
  for (; writeRow >= 0; writeRow--) {
    temp[writeRow] = 0;
  }
  memcpy(newBoard, temp, sizeof(temp));
  return cleared;
}

/** The two-decimal rounding the JSON encoding applies to every map value. */
double roundForConsumer(float value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
  return strtod(buffer, NULL);
}

typedef std::unordered_map<std::string, double> PrecomputeValueMap;

void roundCopyMap(const std::unordered_map<std::string, float> &source, OUT PrecomputeValueMap &out) {
  out.reserve(source.size());
  for (const auto &entry : source) {
    out[entry.first] = roundForConsumer(entry.second);
  }
}

/** A missing key reads as NaN, which poisons the phantom's total and drops it. */
double lockValueOrNan(const PrecomputeValueMap &map, const std::string &key) {
  const auto it = map.find(key);
  return it == map.end() ? std::numeric_limits<double>::quiet_NaN() : it->second;
}

double dasPenalty(bool useDas, int dasChargeAfter, const std::string &key,
                  const PrecomputeValueMap &partialPenalties, const PrecomputeValueMap &minPenalties) {
  if (!useDas) {
    return 0;
  }
  if (partialPenalties.find(key) == partialPenalties.end()
      || minPenalties.find(key) == minPenalties.end()) {
    return 0;
  }
  if (dasChargeAfter >= 15) {
    return 0;
  }
  if (dasChargeAfter >= 10) {
    return partialPenalties.at(key);
  }
  return minPenalties.at(key);
}

/**
 * Per-character input cost over the adjustment's full sequence, plus the
 * possibility's own cost. The tuck character's cost is therefore counted
 * twice: once in the per-character sum and once via inputCostX10. This is
 * legacy TS behaviour, replicated deliberately.
 */
double adjustmentInputCost(const PrecomputePossibility &possibility) {
  double cost = 0;
  for (const char c : possibility.inputSequence) {
    switch (c) {
    case 'E':
    case 'F':
    case 'I':
    case 'G':
      cost += -0.3;
      break;
    case 'L':
    case 'R':
      cost += -0.1;
      break;
    case 'A':
    case 'B':
      cost += -0.2;
      break;
    case 'e':
    case 'f':
    case 'i':
    case 'g':
      cost += -0.2;
      break;
    default:
      break;
    }
  }
  return cost + possibility.inputCostX10 / 10.0;
}

/** NES PRNG transition odds, indexed T J Z O S L I. */
double pieceProbability(const Piece *current, const Piece *next) {
  static const int TRANSITIONS[7][7] = {
      {2, 10, 12, 10, 10, 10, 10}, {12, 2, 10, 10, 10, 10, 10}, {10, 12, 2, 10, 10, 10, 10},
      {10, 10, 10, 4, 10, 10, 10}, {10, 10, 10, 10, 4, 10, 10}, {12, 10, 10, 10, 10, 2, 10},
      {10, 10, 10, 10, 12, 10, 2}};
  auto tableIndex = [](char id) {
    switch (id) {
    case 'T':
      return 0;
    case 'J':
      return 1;
    case 'Z':
      return 2;
    case 'O':
      return 3;
    case 'S':
      return 4;
    case 'L':
      return 5;
    case 'I':
      return 6;
    default:
      return 0;
    }
  };
  return TRANSITIONS[tableIndex(current->id)][tableIndex(next->id)] / 64.0;
}

PrecomputePossibility toPrecomputePossibility(const FullMovePossibility &possibility,
                                              int existingRotation, int initialX, int initialY,
                                              int numOrientations) {
  PrecomputePossibility out;
  out.rot = possibility.rotationIndex;
  out.x = possibility.x;
  out.y = possibility.y;
  out.relRot = (possibility.rotationIndex - existingRotation + numOrientations) % numOrientations;
  out.relX = possibility.x - initialX;
  out.relY = possibility.y - initialY;
  out.inputSequence = possibility.inputSequence;
  out.inputCostX10 = possibility.inputCostX10;
  out.dasChargeAfter = possibility.dasChargeAfter;
  out.numLinesCleared = possibility.numLinesCleared;
  return out;
}

/** The push-down rewrite: hold the piece on the stack with D inputs. */
std::string pushDownSequence(const std::string &inputString) {
  const std::string::size_type lockStart = inputString.find('*');
  std::string preLock = inputString.substr(0, lockStart);
  int fallenFrames = 0;
  for (int i = static_cast<int>(preLock.length()) - 1; i > 0; i--) {
    if (preLock[i] != '.') {
      break;
    }
    fallenFrames++;
  }
  preLock = preLock.substr(0, preLock.length() - fallenFrames);
  const int holdFrames = static_cast<int>(ceil(0.666 * fallenFrames)) + 2;
  for (int i = 0; i < holdFrames; i++) {
    preLock += "D";
  }
  return preLock;
}

/** rot,x,y | sequence split at the reaction time | board | level | lines | das. */
std::string formatDefaultLine(const PrecomputePossibility &placement, int reactionTime,
                              unsigned int board[20], const Piece *piece, int level, int lines) {
  char head[32];
  snprintf(head, sizeof(head), "%d,%d,%d|", placement.relRot, placement.relX, placement.relY);

  const std::string sequence = placement.inputSequence.empty() ? "none" : placement.inputSequence;
  const bool holdsThroughAre = placement.inputSequence.find('_') != std::string::npos;
  const int sliceIndex = holdsThroughAre ? reactionTime + 1 : reactionTime;
  const int clamped = min(sliceIndex, static_cast<int>(sequence.size()));

  std::string out = head;
  out += sequence.substr(0, clamped);
  out += "~~";
  out += sequence.substr(clamped);
  out += "|";

  unsigned int afterBoard[20];
  boardAfterLock(board, piece, placement.rot, placement.x, placement.y, afterBoard);
  out += renderBoard(afterBoard);

  char tail[48];
  snprintf(tail, sizeof(tail), "|%d|%d|%d",
           getLevelAfterLineClears(level, lines, placement.numLinesCleared),
           lines + placement.numLinesCleared, placement.dasChargeAfter);
  out += tail;
  return out;
}

/** rot,x,y | sequence | board | level | lines | das. */
std::string formatPieceLine(const PrecomputePossibility &placement, unsigned int board[20],
                            const Piece *piece, int level, int lines) {
  const int levelAfter = getLevelAfterLineClears(level, lines, placement.numLinesCleared);
  const std::string sequence =
      (SHOULD_PUSHDOWN && levelAfter == 18) ? pushDownSequence(placement.inputSequence)
                                            : (placement.inputSequence.empty() ? "none"
                                                                               : placement.inputSequence);

  char head[32];
  snprintf(head, sizeof(head), "%d,%d,%d|", placement.relRot, placement.relX, placement.relY);
  std::string out = head;
  out += sequence;

  unsigned int afterBoard[20];
  boardAfterLock(board, piece, placement.rot, placement.x, placement.y, afterBoard);
  out += "|";
  out += renderBoard(afterBoard);

  char tail[48];
  snprintf(tail, sizeof(tail), "|%d|%d|%d", levelAfter, lines + placement.numLinesCleared,
           placement.dasChargeAfter);
  out += tail;
  return out;
}

/** Default line plus one line per next piece, with the fallback markers. */
std::string formatPrecomputeResult(const PrecomputePhantom &phantom,
                                   const PrecomputePossibility *const bestPerPiece[7], int reactionTime,
                                   unsigned int board[20], const Piece *piece, int level, int lines) {
  std::string out = "Default:";
  if (phantom.hasInitialPlacement) {
    out += formatDefaultLine(phantom.initial, reactionTime, board, piece, level, lines);
  } else {
    out += "N/A (0 reaction time)";
  }

  const bool canContinue = phantom.hasInitialPlacement
                           && phantom.initial.inputSequence.length() > static_cast<size_t>(reactionTime);
  for (int i = 0; i < 7; i++) {
    out += "\n";
    out += RESPONSE_PIECE_ORDER[i];
    out += ":";
    if (bestPerPiece[i] != NULL) {
      out += formatPieceLine(*bestPerPiece[i], board, piece, level, lines);
    } else {
      out += canContinue ? "continue" : "none";
    }
  }
  return out;
}

} // namespace

std::string runPrecompute(GameState gameState, const Piece *curPiece,
                          char const *inputFrameTimeline, int dasCharge, int reactionTime,
                          int pruningBreadth, int playoutCount, int playoutLength,
                          const EvalContext *evalContext,
                          const PieceRangeContext pieceRangeContextLookup[3]) {
  const int numOrientations = numOrientationsOf(curPiece);

  // Step 1: the current piece's placements, with the mid-air snapshots.
  std::vector<FullMovePossibility> initialResults;
  std::vector<AdjTimeSimStateSlot> captures;
  fullMoveSearch(gameState, curPiece, inputFrameTimeline, dasCharge, /* trackInputSequences= */ true,
                 initialResults, FullMoveSearchContext(), reactionTime, &captures);
  if (initialResults.empty()) {
    return "none";
  }

  // Step 2: one lock value lookup per next piece, values rounded for the collapse.
  const bool useDAS = dasCharge != -1;
  PrecomputeValueMap lockValues[7];
  PrecomputeValueMap partialPenalties[7];
  PrecomputeValueMap minPenalties[7];
  for (int i = 0; i < 7; i++) {
    const Piece *nextPiece = &PIECE_LIST[i];
    if (useDAS) {
      const DasLockValueMaps maps = getLockValueLookupDasMaps(
          gameState, curPiece, nextPiece, pruningBreadth, playoutCount, playoutLength,
          inputFrameTimeline, evalContext, pieceRangeContextLookup);
      roundCopyMap(maps.partialPenalty, partialPenalties[i]);
      roundCopyMap(maps.minPenalty, minPenalties[i]);
      roundCopyMap(maps.lockValue, lockValues[i]);
    } else {
      roundCopyMap(getLockValueLookupMapped(gameState, curPiece, nextPiece, pruningBreadth,
                                            playoutCount, playoutLength, evalContext,
                                            pieceRangeContextLookup),
                   lockValues[i]);
    }
  }

  // Step 3: the phantom placements.
  std::vector<PrecomputePhantom> phantoms;
  if (reactionTime == 0) {
    // The piece is still unplaced at the reaction time, so the single phantom
    // starts from the spawn state and has no initial placement to show.
    PrecomputePhantom phantom;
    phantom.hasAdjustmentState = true;
    phantom.adjustmentCtx = FullMoveSearchContext();
    phantom.adjustmentDasCharge = dasCharge;
    phantoms.push_back(phantom);
  } else {
    std::vector<PrecomputePossibility> initialPlacements;
    initialPlacements.reserve(initialResults.size());
    for (const FullMovePossibility &result : initialResults) {
      initialPlacements.push_back(
          toPrecomputePossibility(result, /* existingRotation= */ 0, INITIAL_X, curPiece->initialY,
                                  numOrientations));
    }

    // Fewest inputs first; ties keep the search order.
    std::vector<size_t> order(initialPlacements.size());
    for (size_t i = 0; i < order.size(); i++) {
      order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&initialPlacements](size_t a, size_t b) {
      return countInputsOf(initialPlacements[a].relRot, initialPlacements[a].relX)
             < countInputsOf(initialPlacements[b].relRot, initialPlacements[b].relX);
    });

    std::unordered_set<std::string> seenPrefixes;
    for (const size_t index : order) {
      const std::string prefix = initialPlacements[index].inputSequence.substr(0, reactionTime);
      if (!seenPrefixes.insert(prefix).second) {
        continue;
      }

      PrecomputePhantom phantom;
      phantom.inputSequence = prefix;
      phantom.hasInitialPlacement = true;
      phantom.initial = initialPlacements[index];
      const AdjTimeSimStateSlot &slot = captures[index];
      if (slot.hasCapture) {
        const AdjTimeSimState &capture = slot.capture;
        phantom.hasAdjustmentState = true;
        phantom.adjustmentCtx.existingXOffset = capture.x - INITIAL_X;
        phantom.adjustmentCtx.existingYOffset = capture.y - curPiece->initialY;
        phantom.adjustmentCtx.framesAlreadyElapsed = capture.frameIndex;
        phantom.adjustmentCtx.existingRotation = capture.rotationIndex;
        phantom.adjustmentCtx.isAdjustment = true;
        phantom.adjustmentCtx.isArrContinued = capture.isArrContinued;
        phantom.adjustmentCtx.dasButtonHeld = capture.dasButtonHeld;
        phantom.adjustmentCtx.preAdjInputSequence = capture.preAdjInputSequence;
        phantom.adjustmentDasCharge = capture.dasCharge;
      }
      phantoms.push_back(phantom);
    }
  }

  // Step 4: one adjustment search per phantom that captured a mid-air state.
  for (PrecomputePhantom &phantom : phantoms) {
    if (!phantom.hasAdjustmentState) {
      continue;
    }
    std::vector<FullMovePossibility> adjustmentResults;
    fullMoveSearch(gameState, curPiece, inputFrameTimeline, phantom.adjustmentDasCharge,
                   /* trackInputSequences= */ true, adjustmentResults, phantom.adjustmentCtx,
                   /* reactionTime= */ -1, /* adjTimeCaptures= */ NULL);
    phantom.possibleAdjustments.reserve(adjustmentResults.size());
    for (const FullMovePossibility &result : adjustmentResults) {
      phantom.possibleAdjustments.push_back(toPrecomputePossibility(
          result, phantom.adjustmentCtx.existingRotation, INITIAL_X + phantom.adjustmentCtx.existingXOffset,
          curPiece->initialY + phantom.adjustmentCtx.existingYOffset, numOrientations));
    }
  }

  // Step 5: the collapse.
  double bestPhantomValue = PRECOMPUTE_MIN_VALUE;
  std::string overallResponse = "none";
  for (const PrecomputePhantom &phantom : phantoms) {
    double totalValue = 0;
    const PrecomputePossibility *bestPerPiece[7] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};

    if (phantom.possibleAdjustments.empty()) {
      if (!phantom.hasInitialPlacement) {
        // The spawn-state search already proved non-empty (step 1 returned
        // early otherwise), so the reaction-time-zero phantom always has
        // adjustments. Reaching this point would mean the invariant broke.
        throw std::runtime_error("Precompute phantom has no placement and no adjustments.");
      }
      for (int i = 0; i < 7; i++) {
        const std::string key =
            encodeLockCell(phantom.initial.rot, phantom.initial.x, phantom.initial.y);
        const double value = lockValueOrNan(lockValues[i], key)
                             + phantom.initial.inputCostX10 / 10.0
                             + dasPenalty(useDAS, phantom.initial.dasChargeAfter, key,
                                          partialPenalties[i], minPenalties[i]);
        // Keep the multiply out of the addition: a fused multiply-add rounds
        // once per term instead of twice, and the single-rounding result can
        // flip one of the exact phantom ties the collapse must resolve
        // first-wins. The two-rounding accumulation is the contract here.
        const double term = value * pieceProbability(curPiece, &PIECE_LIST[i]);
        totalValue += term;
      }
    } else {
      for (int i = 0; i < 7; i++) {
        double maxValue = PRECOMPUTE_MIN_VALUE;
        const PrecomputePossibility *bestAdjustment = NULL;
        for (const PrecomputePossibility &adjustment : phantom.possibleAdjustments) {
          const std::string key = encodeLockCell(adjustment.rot, adjustment.x, adjustment.y);
          if (lockValues[i].find(key) == lockValues[i].end()) {
            continue;
          }
          const double value = lockValues[i].at(key) + adjustmentInputCost(adjustment)
                               + dasPenalty(useDAS, adjustment.dasChargeAfter, key,
                                            partialPenalties[i], minPenalties[i]);
          // A tie keeps the later adjustment.
          if (value >= maxValue) {
            maxValue = value;
            bestAdjustment = &adjustment;
          }
        }
        bestPerPiece[i] = bestAdjustment;
        // Two roundings per term, never a fused multiply-add (see the
        // default-placement branch above for why the fused form is wrong).
        const double term = maxValue * pieceProbability(curPiece, &PIECE_LIST[i]);
        totalValue += term;
      }
    }

    const double phantomValue = totalValue / 7;
    // A tie keeps the earlier phantom.
    if (phantomValue > bestPhantomValue) {
      overallResponse = formatPrecomputeResult(phantom, bestPerPiece, reactionTime, gameState.board,
                                               curPiece, gameState.level, gameState.lines);
      bestPhantomValue = phantomValue;
    }
  }

  return overallResponse;
}
