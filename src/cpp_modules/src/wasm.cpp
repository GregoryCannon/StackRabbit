#include "main.cpp"
#include "types.hpp"

#undef NONE
#include <emscripten/bind.h>

std::string jsGetLockValueLookup(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_LOCK_VALUE_LOOKUP);
}

std::string jsGetMove(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_MOVE);
}

std::string jsGetTopMoves(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_TOP_MOVES);
}

std::string jsGetTopMovesHybrid(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_TOP_MOVES_HYBRID);
}

std::string jsRateMove(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, RATE_MOVE);
}


EMSCRIPTEN_BINDINGS(my_module) {
    emscripten::function("getLockValueLookup", &jsGetLockValueLookup);
    emscripten::function("getMove", &jsGetMove);
    emscripten::function("getTopMoves", &jsGetTopMoves);
    emscripten::function("getTopMovesHybrid", &jsGetTopMovesHybrid);
    emscripten::function("rateMove", &jsRateMove);
}

