#ifndef UTILS
#define UTILS

#include <stdarg.h>
#include <stdio.h>

#define LOGGING_ENABLED 0

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

#endif