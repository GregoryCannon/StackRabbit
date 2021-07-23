#ifndef BOARD_METHODS
#define BOARD_METHODS

void encodeBoard(char *boardStr, int outBoard[20]);

void getSurfaceArray(int board[20], int outSurface[10]);

void duplicateBoard(int inBoard[20], int outBoard[20]);

void printBoard(int board[20]);

void printSurface(int surfaceArray[10]);

#endif