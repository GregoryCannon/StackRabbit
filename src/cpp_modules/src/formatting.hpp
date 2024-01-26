#ifndef FORMATTING
#define FORMATTING

#include <string>
#include "types.hpp"
using namespace std;

const std::string BOARD_ENCODING = "abcdefghijklmnopqrstuvwxyzABCDEF"; // Encodes 5 bits of information (32 chars)

/** Concatenates the position of a piece into a single string. 
 * 
 * NB: differs from formatLockPosition by the order of the values and the X coordinate system.
 * There's no real need to have both beyond legacy reasons.
*/
std::string encodeLockPosition(LockLocation lockLocation){
  if (VARIABLE_RANGE_CHECKS_ENABLED || true) {
    // Check variable ranges to avoid buffer overflows
    if (lockLocation.rotationIndex > 4 || lockLocation.rotationIndex < 0) {
      printf("rotation index out of range %d\n", lockLocation.rotationIndex);
    }
    if (lockLocation.x > 7 || lockLocation.x < -2) {
      printf("x index out of range %d\n", lockLocation.x);
    }
    if (lockLocation.y < -2 || lockLocation.y > 19) {
      printf("y index out of range %d\n", lockLocation.y);
    }
  }
  char buffer[10]; // 2 for separators, 1 for rotIndex, 2 for x, 2 for y, 3 for expect the unexpected
  sprintf(buffer, "%d|%d|%d", lockLocation.rotationIndex, lockLocation.x, lockLocation.y);
  return string(buffer);
}

/** Formats a human-readable lock location */
std::string formatLockPosition(LockLocation lockLocation) {
  if (lockLocation.x == NULL_LOCK_LOCATION.x){
    return "null";
  }
  char buffer[12]; // 4 for separators, 1 for rotIndex, 2 for x, 2 for y, 3 for expect the unexpected
  sprintf(buffer, "[%d,%d,%d]", lockLocation.rotationIndex, lockLocation.x - INITIAL_X, lockLocation.y);
  return string(buffer);
}

/** Formats a board using a simple proprietary compression/encoding system.
 * 
 * Example compressed board:
 * 15,CaEa,3
 * 
 * - The first number is the # of empty rows at the top of the board
 * - The number at the end is the # of full-right-well rows at the bottom of the board
 * - Each letter in the middle section corresponds to half a row, based on the following encoding:
 * a = 00000
 * b = 00001
 * ...
 * z = 11001
 * A = 11010
 * ...
 * F = 11111
 */
std::string formatBoard(const unsigned int board[20]){
  std::string output;

  // Skip empty rows at the top
  int i = 0;
  while (i < 20 && ((board[i] & FULL_ROW) == 0)){
    i++;
  }

  // Skip full right well rows at the bottom
  int j = 20;
  while ((board[j-1] & FULL_ROW) == (FULL_ROW - 1)){
    j--;
  }

  output += std::to_string(i);
  output += ',';
  for (; i < j; i++){
    unsigned int rightHalfMask = 31U;
    unsigned int indexRight = board[i] & rightHalfMask;
    unsigned int indexLeft = (board[i] >> 5) & rightHalfMask;

    output += BOARD_ENCODING.at(indexLeft);
    output += BOARD_ENCODING.at(indexRight);
  }
  output += ',';
  output += std::to_string(20 - j);
  return output;
}

std::string formatPlayout(PlayoutData playoutData){
  std::string output = std::string("{ ");

  // Add the piece sequence
  output += "\"pieceSequence\":\"" + playoutData.pieceSequence;

  // Add the placements list
  output += "\", \"placements\": [";
  for (auto placement : playoutData.placements){
    output += formatLockPosition(placement) + ",";
  }
  if (playoutData.placements.size() > 0) {
    output.pop_back(); // Remove the last comma
  }
  output += " ]";

  // Add the resulting board
  output += ", \"resultingBoard\":\"";
  output += formatBoard(playoutData.resultingBoard);
  output += "\", ";

  // Add the score
  output += " \"score\":";
  char scoreBuffer[10]; // 4 digits for LHS + 1 for decimal + 2 for RHS + 1 for sign + 2 for expect the unexpected
  sprintf(scoreBuffer, "%.2f", playoutData.totalScore);
  output += scoreBuffer;
  output += " }";
  return output;
}

/** 
 * Formats a list of engine moves, for use in the "engine-movelist-cpp" API request.
 * The format is JSON-like, as follows:
 * "[
 *   {
 *     firstPlacement: [0, -4, 18],
 *     secondPlacement: [1, 4, 20],
 *     playoutScore: 31.587,
 *     shallowEvalScore: 28.44,
 *   },
 *   { ... },
 *   { ... },
 * ]"
*/
std::string formatEngineMoveList(list<EngineMoveData> moveList){
  std::string output = std::string("[");
  for( const auto& move : moveList ) {
    output += "{\"firstPlacement\":";
    output += formatLockPosition(move.firstPlacement);
    if (move.secondPlacement.x != NULL_LOCK_LOCATION.x){
      output += ", \"secondPlacement\":";
      output += formatLockPosition(move.secondPlacement);
    }
    char formattedMoveBuffer[70]; // Max length I got in my testing was 108 with for just the scores
    sprintf(formattedMoveBuffer, 
        ", \"playoutScore\":%.2f, \"shallowEvalScore\":%.2f",
        move.playoutScore,
        move.evalScore
        );
    output.append(formattedMoveBuffer);

    output += ", \"resultingBoard\":\"";
    output += move.resultingBoard;
    output += "\"";

    output += ", \"playout1\":";
    output += formatPlayout(move.playout1);
    output += ", \"playout2\":";
    output += formatPlayout(move.playout2);
    output += ", \"playout3\":";
    output += formatPlayout(move.playout3);
    output += ", \"playout4\":";
    output += formatPlayout(move.playout4);
    output += ", \"playout5\":";
    output += formatPlayout(move.playout5);
    output += ", \"playout6\":";
    output += formatPlayout(move.playout6);
    output += ", \"playout7\":";
    output += formatPlayout(move.playout7);

    output.append(" },");
  }
  if (moveList.size() > 0) {
    output.pop_back(); // Remove the last comma
  }
  output.append("]");
  return output;
}

std::string formatRateMove(float playerNoAdj, float bestNoAdj, float playerWithAdj, float bestWithAdj){
  std::string output = "{\"playerMoveNoAdjustment:\"";
  output += std::to_string(playerNoAdj);
  output += ", \"bestMoveNoAdjustment:\"";
  output += std::to_string(bestNoAdj);
  output += ", \"playerMoveAfterAdjustment:\"";
  output += std::to_string(playerWithAdj);
  output += ", \"bestMoveAfterAdjustment\":";
  output += std::to_string(bestWithAdj);
  output += "}";
  return output;
}


#endif