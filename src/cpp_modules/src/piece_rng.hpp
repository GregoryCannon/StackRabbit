#ifndef PIECE_RNG
#define PIECE_RNG

#include "types.hpp"

Piece getRandomPiece(Piece previousPiece);

/**
 * Probability of a piece sequence under the NES randomizer, given the piece that came before it.
 * @param lastPieceIndex - PIECE_LIST index of the last known piece, or -1 if unknown (uniform first piece).
 */
float getSequenceProbability(const int *pieceSequence, int length, int lastPieceIndex);

#endif
