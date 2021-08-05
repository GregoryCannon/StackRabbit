#ifndef UTILS
#define UTILS

#include <stdarg.h>
#include <stdio.h>

#define LOGGING_ENABLED 0
#define PLAYOUT_LOGGING_ENABLED 0

// No-op used to mark output parameters
#define OUT

// Shifts a variable X by Y places, either left or right depending on the sign
#define SHIFTBY(x, y) ((y) > 0 ? x >> (y) : (x) << (-1 * (y)))

// Piece spawn locations
#define SPAWN_X 3
#define SPAWN_Y(pieceIndex) ((pieceIndex) == 0 ? -2 : -1)

// Useful bit-rows
#define FULL_ROW 1023 // = 1111111111

void maybePrint(const char *format, ...) {
  if (!LOGGING_ENABLED){
    return;
  }
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

const FastEvalWeights DEBUG_WEIGHTS = {
    /* avgHeightCoef= */ -5,
    /* avgHeightExponent= */ 2,
    /* burnCoef= */ -1,
    /* coveredWellCoef= */ -5,
    /* col9Coef= */ -3,
    /* deathCoef= */ -10000,
    0,
    /* holeCoef= */ -40,
    /* tetrisCoef= */ 40,
    0,
    /* surfaceCoef= */ 1};
const EvalContext DEBUG_CONTEXT = {
    /* inputFrameTimeline= */ 1 << 4,
    /* maxSafeCol9Height= */ 7,
    /* scareHeight= */ 5,
    /* wellColumn= */ 9,
    /* countWellHoles= */ false};

#endif
