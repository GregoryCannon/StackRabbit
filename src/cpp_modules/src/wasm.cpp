#include "main.cpp"
#include "types.hpp"

#undef NONE
#include <emscripten/bind.h>

std::string getLockValueLookup(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_LOCK_VALUE_LOOKUP);
}

std::string getMove(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_MOVE);
}

std::string getTopMoves(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_TOP_MOVES);
}

std::string getTopMovesHybrid(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_TOP_MOVES_HYBRID);
}

// std::string rateMove(std::string inputStr) {
//     const char* cInputStr = inputStr.c_str();
//     return mainProcess(cInputStr, RATE_MOVE);
// }


EMSCRIPTEN_BINDINGS(my_module) {
    emscripten::function("getLockValueLookup", &getLockValueLookup);
    emscripten::function("getMove", &getMove);
    emscripten::function("getTopMoves", &getTopMoves);
    emscripten::function("getTopMovesHybrid", &getTopMovesHybrid);
    // emscripten::function("rateMove", &rateMove);
}

