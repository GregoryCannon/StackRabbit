#include "piece_rng.hpp"

int transitionProbability[7][7] = {
  {2, 10, 12, 10, 10, 10, 10},
  {12, 2, 10, 10, 10, 10, 10},
  {10, 12, 2, 10, 10, 10, 10},
  {10, 10, 10, 4, 10, 10, 10},
  {10, 10, 10, 10, 4, 10, 10},
  {12, 10, 10, 10, 10, 2, 10},
  {10, 10, 10, 10, 12, 10, 2},
};

Piece getRandomPiece(Piece previousPiece) {
  int rand = qualityRandom(0, 64);
  for (int i = 0; i < 7; i++) {
    int chance = transitionProbability[previousPiece.index][i];
    if (rand < chance) {
      return PIECE_LIST[i];
    }
    rand -= chance;
  }
  // Never reaches here since cumulative probability is always == 64;
  return {};
}
