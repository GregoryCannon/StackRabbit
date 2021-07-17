#ifndef UTILS
#define UTILS

// No-op used to mark output parameters
#define OUT 

// Shifts a variable X by Y places, either left or right depending on the sign
#define SHIFTBY(x, y) ((y) > 0 ? x >> (y) : (x) << (-1 * (y)))

#endif