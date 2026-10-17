#ifndef CANONICAL_SEQUENCES_H
#define CANONICAL_SEQUENCES_H

#include "../src/config.hpp"

/*
 * Piece-sequence data used by getPlayoutScore (src/cpp_modules/src/playout.cpp) to play out
 * future piece sequences under NES Tetris randomizer semantics.
 *
 * Each array holds CANONICAL_SEQUENCE_BATCH_SIZE (config.hpp) rows of SEQUENCE_LENGTH (=20)
 * piece indexes. Per row, element e is the piece played as the e-th piece after the batch's
 * "last seen piece" (element 0 = the successor of the last seen piece itself). Integer labels
 * index PIECE_LIST: I=0, O=1, L=2, J=3, T=4, S=5, Z=6.
 *
 *   canonicalSequencesNoLastPieceKnown: no knowledge of the last piece (uniform first piece).
 *   canonicalSequencesAfterI / AfterO / AfterL / AfterJ / AfterT / AfterS / AfterZ:
 *       sequences that follow last seen piece I / O / L / J / T / S / Z.
 *
 * Lookup used by the engine: canonicalPieceSequencesForLastPiece[PIECE_LIST index].
 *
 * Batches reproduce the piece transition table of "NES Tetris RNG" by fractal161
 * (https://tetrissuomi.wordpress.com/wp-content/uploads/2020/04/nes_tetris_rng.pdf, Table 3
 * "Averages"): repeats 2/64 (4/64 for O and S), burst cells I->S, T->Z, J->T, Z->J and L->T
 * at 12/64, all others 10/64.
 */

extern const int canonicalSequencesNoLastPieceKnown[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];
extern const int canonicalSequencesAfterI[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];
extern const int canonicalSequencesAfterO[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];
extern const int canonicalSequencesAfterL[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];
extern const int canonicalSequencesAfterJ[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];
extern const int canonicalSequencesAfterT[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];
extern const int canonicalSequencesAfterS[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];
extern const int canonicalSequencesAfterZ[CANONICAL_SEQUENCE_BATCH_SIZE * SEQUENCE_LENGTH];

/*
 * Batch lookup by lastSeenPiece index in PIECE_LIST order (I,O,L,J,T,S,Z).
 */
extern const int *canonicalPieceSequencesForLastPiece[7];

/**
 * A list of all possible piece sequences of length 1 - 4.
 * Ordered such that the first 7 entries contains all sequences of length 1, the first 7^2 entries contain all of length 2, etc.
 */
extern const int exhaustivePieceSequences[EXHAUSTIVE_SEQUENCE_COUNT * EXHAUSTIVE_SEQUENCE_LENGTH];

#endif
