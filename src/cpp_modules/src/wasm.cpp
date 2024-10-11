#include "main.cpp"
#include "types.hpp"

#undef NONE
#include <emscripten/bind.h>

std::string wasmGetLockValueLookup(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_LOCK_VALUE_LOOKUP);
}

std::string wasmGetMove(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_MOVE);
}

std::string wasmGetTopMoves(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_TOP_MOVES);
}

std::string wasmGetTopMovesHybrid(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, GET_TOP_MOVES_HYBRID);
}

std::string wasmRateMove(std::string inputStr) {
    const char* cInputStr = inputStr.c_str();
    return mainProcess(cInputStr, RATE_MOVE);
}


EMSCRIPTEN_BINDINGS(my_module) {
    emscripten::function("getLockValueLookup", &wasmGetLockValueLookup);
    emscripten::function("getMove", &wasmGetMove);
    emscripten::function("getTopMoves", &wasmGetTopMoves);
    emscripten::function("getTopMovesHybrid", &wasmGetTopMovesHybrid);
    emscripten::function("rateMove", &wasmRateMove);
}

