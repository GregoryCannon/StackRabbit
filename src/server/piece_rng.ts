/*
The PRNG of NES Tetris is pretty weird. For example, S bursts are twice as likely as Z bursts, for no good reason.
Thanks to Adrien Wu and HydrantDude for providing the following lookup tables. They represent the odds of getting 
each piece, given a particular previous piece. The indexing goes Array[firstPiece][secondPiece]. 
*/

import { IS_DROUGHT_MODE } from "./params";
import { POSSIBLE_NEXT_PIECES } from "./utils";

const PIECES: Array<PieceId> = ["T", "J", "Z", "O", "S", "L", "I"];
const PIECE_INDICES = {
  T: 0,
  J: 1,
  Z: 2,
  O: 3,
  S: 4,
  L: 5,
  I: 6,
};

const TRANSITIONS = [
  [2, 10, 12, 10, 10, 10, 10],
  [12, 2, 10, 10, 10, 10, 10],
  [10, 12, 2, 10, 10, 10, 10],
  [10, 10, 10, 4, 10, 10, 10],
  [10, 10, 10, 10, 4, 10, 10],
  [12, 10, 10, 10, 10, 2, 10],
  [10, 10, 10, 10, 12, 10, 2],
];
const TRANSITIONS_DROUGHT_CODE = [
  [3, 11, 14, 11, 11, 11, 3],
  [14, 3, 11, 11, 11, 11, 3],
  [11, 14, 3, 11, 11, 11, 3],
  [11, 11, 11, 6, 11, 11, 3],
  [11, 11, 11, 11, 6, 11, 3],
  [14, 11, 11, 11, 11, 3, 3],
  [10, 10, 10, 10, 12, 10, 2],
];

export function getPieceProbability(
  current: PieceId,
  next: PieceId,
  isDroughtCode: boolean
) {
  const index1 = PIECE_INDICES[current];
  const index2 = PIECE_INDICES[next];
  return isDroughtCode
    ? TRANSITIONS_DROUGHT_CODE[index1][index2] / 64
    : TRANSITIONS[index1][index2] / 64;
}

export function getRandomPiece(prevPiece: PieceId, isDroughtCode: boolean) {
  const transitions = isDroughtCode ? TRANSITIONS_DROUGHT_CODE : TRANSITIONS;
  const index1 = PIECE_INDICES[prevPiece];
  let rand = Math.floor(Math.random() * 64);
  for (let i = 0; i < 7; i++) {
    const chance = transitions[index1][i];
    if (rand < chance) {
      return PIECES[i];
    }
    rand -= chance;
  }
}

export function getPieceSequence() {
  let sequence = [];
  for (let writeIndex = 0; writeIndex < 2000; writeIndex++) {
    const prevPieceId =
      writeIndex == 0
        ? POSSIBLE_NEXT_PIECES[Math.floor(Math.random() * 7)] // Random first piece
        : sequence[writeIndex - 1];
    sequence[writeIndex] = getRandomPiece(prevPieceId, IS_DROUGHT_MODE);
  }
  return sequence;
}
