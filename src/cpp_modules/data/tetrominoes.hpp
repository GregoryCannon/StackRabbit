#ifndef TETROMINOES
#define TETROMINOES

#include "../src/types.hpp"
#include <list>

extern const Piece PIECE_I;
extern const Piece PIECE_O;
extern const Piece PIECE_L;
extern const Piece PIECE_J;
extern const Piece PIECE_T;
extern const Piece PIECE_S;
extern const Piece PIECE_Z;

extern const Piece *PIECE_LIST;

/* A set of very particular data about each piece, used for finding tucks/spins in move search.

   The idea is that for any legal tuck/spin, either the top rightmost mino or the top leftmost mino must be placed on an overhang cell (except for absurdly rare contrived cases).
   Therefore, for each piece, we store a list of where these cells are within the 4x4 grid occupied by each cell.
   Then, during move search, we can line up these key cells with the overhang cells on the board to find tuck/spins. */

struct TuckOriginSpot {
  int orientation;
  int x;
  int y;
};

extern std::list<TuckOriginSpot> TUCK_SPOTS_I;
extern std::list<TuckOriginSpot> TUCK_SPOTS_O;
extern std::list<TuckOriginSpot> TUCK_SPOTS_L;
extern std::list<TuckOriginSpot> TUCK_SPOTS_J;
extern std::list<TuckOriginSpot> TUCK_SPOTS_T;
extern std::list<TuckOriginSpot> TUCK_SPOTS_S;
extern std::list<TuckOriginSpot> TUCK_SPOTS_Z;

extern std::list<TuckOriginSpot> TUCK_SPOTS_LIST[7];

struct TuckInput {
  char notation;
  int xChange;
  int rotationChange;
};

std::list<TuckInput> TUCK_INPUTS;

#endif
