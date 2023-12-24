#include "../src/types.hpp"
#include <list>

/* A set of very particular data about each piece, used for finding tucks/spins in move search.

   The idea is that for any legal tuck/spin, either the top rightmost mino or the top leftmost mino must be placed on an overhang cell (except for absurdly rare contrived cases).
   Therefore, for each piece, we store a list of where these cells are within the 4x4 grid occupied by each cell.
   Then, during move search, we can line up these key cells with the overhang cells on the board to find tuck/spins. */

struct TuckOriginSpot {
  int orientation;
  int x;
  int y;
};

struct TuckInput {
  char notation;
  int xChange;
  int rotationChange;
};

const Piece PIECE_I = { 'I', 0, {
                 {0, 0, 960, 0}, // e.g. 960 = 1111000000
                 {128, 128, 128, 128},
                 {NONE, NONE, NONE, NONE}, // NONEs indicate that there are no more rotations of this piece
                 {NONE, NONE, NONE, NONE}
               },{
                 {2, 2, 2, 2},
                 {NONE, NONE, 0, NONE}, // NONEs indicate the piece is not present in that column
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{
                 {3, 3, 3, 3},
                 {NONE, NONE, 4, NONE},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{ 17, 16, NONE, NONE},
               -2 };

const Piece PIECE_O = { 'O', 1, {
                 {0, 384, 384, 0},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE}
               },{
                 {NONE, 1, 1, NONE},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{
                 {NONE, 3, 3, NONE},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{17, NONE, NONE, NONE},
               -1 };

const Piece PIECE_L = { 'L', 2, {
                 {0, 448, 256, 0},
                 {384, 128, 128, 0},
                 {64, 448, 0, 0},
                 {128, 128, 192, 0}
               },{
                 {NONE, 1, 1, 1},
                 {NONE, 0, 0, NONE},
                 {NONE, 1, 1, 0},
                 {NONE, NONE, 0, 2}
               },{
                 {NONE, 3, 2, 2},
                 {NONE, 1, 3, NONE},
                 {NONE, 2, 2, 2},
                 {NONE, NONE, 3, 3},
               },{17, 17, 18, 17},
               -1 };

const Piece PIECE_J = { 'J', 3, {
                 {0, 448, 64, 0},
                 {128, 128, 384, 0},
                 {256, 448, 0, 0},
                 {192, 128, 128, 0}
               },{
                 {NONE, 1, 1, 1},
                 {NONE, 2, 0, NONE},
                 {NONE, 0, 1, 1},
                 {NONE, NONE, 0, 0}
               },{
                 {NONE, 2, 2, 3},
                 {NONE, 3, 3, NONE},
                 {NONE, 2, 2, 2},
                 {NONE, NONE, 3, 1},
               },{17, 17, 18, 17},
               -1 };

const Piece PIECE_T = { 'T', 4, {
                 {0, 448, 128, 0},
                 {128, 384, 128, 0},
                 {128, 448, 0, 0},
                 {128, 192, 128, 0}
               },{
                 {NONE, 1, 1, 1},
                 {NONE, 1, 0, NONE},
                 {NONE, 1, 0, 1},
                 {NONE, NONE, 0, 1}
               },{
                 {NONE, 2, 3, 2},
                 {NONE, 2, 3, NONE},
                 {NONE, 2, 2, 2},
                 {NONE, NONE, 3, 2},
               },{ 17, 17, 18, 17},
               -1 };

const Piece PIECE_S = { 'S', 5, {
                 {0, 192, 384, 0},
                 {128, 192, 64, 0},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE}
               },{
                 {NONE, 2, 1, 1},
                 {NONE, NONE, 0, 1},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{
                 {NONE, 3, 3, 2},
                 {NONE, NONE, 2, 3},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{ 17, 17, NONE, NONE},
               -1 };

const Piece PIECE_Z = { 'Z', 6, {
                 {0, 384, 192, 0},
                 {64, 192, 128, 0},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{
                 {NONE, 1, 1, 2},
                 {NONE, NONE, 1, 0},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{
                 {NONE, 2, 3, 3},
                 {NONE, NONE, 3, 2},
                 {NONE, NONE, NONE, NONE},
                 {NONE, NONE, NONE, NONE},
               },{ 17, 17, NONE, NONE},
               -1 };

static std::list<TuckOriginSpot> TUCK_SPOTS_I = {
  {0, 0, 2},
  {0, 3, 2},
  {1, 2, 0}
};

static std::list<TuckOriginSpot> TUCK_SPOTS_O = {
  {0, 1, 1},
  {0, 2, 1}
};

static std::list<TuckOriginSpot> TUCK_SPOTS_L = {
  {0, 1, 1},
  {0, 3, 1},
  {1, 1, 0},
  {1, 2, 0},
  {2, 1, 1},
  {2, 3, 0},
  {3, 2, 0},
  {3, 3, 2}
};

static std::list<TuckOriginSpot> TUCK_SPOTS_J = {
  {0, 1, 1},
  {0, 3, 1},
  {1, 1, 2},
  {1, 2, 0},
  {2, 1, 0},
  {2, 3, 1},
  {3, 2, 0},
  {3, 3, 0}
};

static std::list<TuckOriginSpot> TUCK_SPOTS_T = {
  {0, 1, 1},
  {0, 3, 1},
  {1, 1, 1},
  {1, 2, 0},
  {2, 1, 1},
  {2, 3, 1},
  {3, 2, 0},
  {3, 3, 1},
};

static std::list<TuckOriginSpot> TUCK_SPOTS_S = {
  {0, 1, 2},
  {0, 3, 1},
  {1, 2, 0},
  {1, 3, 1}
};

static std::list<TuckOriginSpot> TUCK_SPOTS_Z = {
  {0, 1, 1},
  {0, 3, 2},
  {1, 3, 0},
  {1, 2, 1}
};

const Piece PIECE_LIST[7] = {PIECE_I, PIECE_O, PIECE_L, PIECE_J, PIECE_T, PIECE_S, PIECE_Z};

static std::list<TuckOriginSpot> TUCK_SPOTS_LIST[7] = {TUCK_SPOTS_I, TUCK_SPOTS_O, TUCK_SPOTS_L, TUCK_SPOTS_J, TUCK_SPOTS_T, TUCK_SPOTS_S, TUCK_SPOTS_Z};

std::list<TuckInput> TUCK_INPUTS {
  {'L', -1, 0},
  {'R', 1, 0},
  {'A', 0, 1},
  {'B', 0, -1},
  {'E', -1, 1},
  {'I', 1, 1},
  {'F', -1, -1},
  {'G', 1, -1}
};