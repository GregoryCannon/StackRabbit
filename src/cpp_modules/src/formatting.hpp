#ifndef FORMATTING
#define FORMATTING

#include <string>
#include "types.hpp"
using namespace std;


/**
 * Processes a Possibility object and formats the result how the various agents would expect it.
 * @returns a string of resulting data, in the format:
 * {number of shifts | number of right rotations | string of inputs | resulting board | resulting level | resulting lines}
 */
std::string formatPossibility(Possibility possibility){
  // TODO: implement
  // char buffer[300];
  // sprintf(buffer, "%d")
  return "";
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
  for( const auto& n : moveList ) {
    char formattedMoveBuffer[150]; // Max length I got in my testing was 108 with for just the scores
    sprintf(formattedMoveBuffer, 
        "{ \"firstPlacement\":[%d,%d,%d], \"secondPlacement\":[%d,%d,%d], \"playoutScore\":%.2f, \"shallowEvalScore\":%.2f },", 
        n.firstPlacement.rotationIndex, 
        n.firstPlacement.x - INITIAL_X, 
        n.firstPlacement.y,
        n.secondPlacement.rotationIndex, 
        n.secondPlacement.x - INITIAL_X, 
        n.secondPlacement.y,
        n.playoutScore,
        n.evalScore
        );
    output.append(formattedMoveBuffer);
  }
  if (moveList.size() > 0) {
    output.pop_back(); // Remove the last comma
  }
  output.append("]");
  return output;
}

#endif