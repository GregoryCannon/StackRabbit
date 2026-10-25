#include "piece_rng.hpp"

// Rows/cols of the transition table are in T, J, Z, O, S, L, I order (the order the NES
// randomizer tables were documented in), while PIECE_LIST and piece indexes are I, O, L,
// J, T, S, Z. Map between the two rather than indexing the table with piece.index directly.
const int pieceIndexToTableIndex[7] = {6, 3, 5, 1, 0, 4, 2};
const int tableIndexToPieceIndex[7] = {4, 3, 6, 1, 5, 2, 0};

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
  int tableIndex = pieceIndexToTableIndex[previousPiece.index];
  int rand = qualityRandom(0, 64);
  for (int i = 0; i < 7; i++) {
    int chance = transitionProbability[tableIndex][i];
    if (rand < chance) {
      return PIECE_LIST[tableIndexToPieceIndex[i]];
    }
    rand -= chance;
  }
  // Never reaches here since cumulative probability is always == 64;
  return {};
}

float getSequenceProbability(const int *pieceSequence, int length, int lastPieceIndex) {
  float probability = 1.0f;
  int prevTableIndex = lastPieceIndex < 0 ? -1 : pieceIndexToTableIndex[lastPieceIndex];
  for (int i = 0; i < length; i++) {
    int tableIndex = pieceIndexToTableIndex[pieceSequence[i]];
    probability *= prevTableIndex < 0
      ? 1.0f / 7
      : transitionProbability[prevTableIndex][tableIndex] / 64.0f;
    prevTableIndex = tableIndex;
  }
  return probability;
}
