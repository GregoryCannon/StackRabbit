import { PIECE_LIST } from "../../../docs/tetrominoes";
import { getRandomPiece } from "../piece_rng";

const fs = require("fs");
const lookupStr = fs.readFileSync(
  "docs/condensed_NoNextBox_NoBars.txt",
  "utf8"
);

/**
 * Generates a set of piece sequences that follow the NES RNG patterns.
 *
 * The sequences are grouped into batches of 1000 depending on the last known piece.
 * The first 1000 assume no knowledge of the previous piece, then after that each batch
 * of 1000 assumes a different last-known-piece (in the order IOLJTSZ).
 *
 * e.g. sequences 3000 - 3999 all assume the last known piece was an L
 */
function generateCanonicalPieceSequences() {
  const filename = "docs/canonical_sequences_output.txt";
  // fs.open(filename, "w");
  fs.truncateSync(filename, 0);

  for (let i = 0; i < 8000; i++) {
    let sequence = [];
    let lastKnownPiece =
      i < 1000
        ? (PIECE_LIST[i % 7][2] as PieceId)
        : (PIECE_LIST[Math.floor(i / 1000) - 1][2] as PieceId);
    // sequence.push(i % 7); // First piece loops around the possible 7 pieces
    for (let j = 0; j < 20; j++) {
      if (j == 0 && i < 1000){
        sequence.push(PIECE_LIST.findIndex((x) => x[2] == lastKnownPiece));
        continue;
      }
      lastKnownPiece = getRandomPiece(lastKnownPiece, /* isDrought= */ false);
      sequence.push(PIECE_LIST.findIndex((x) => x[2] == lastKnownPiece));
    }
    fs.appendFileSync(filename, "\t" + sequence.join(", ") + ",\n");
  }
}

function generateExhaustivePieceSequences() {
  const filename = "docs/exhaustive_sequences_output.txt";
  // fs.open(filename, "w");
  fs.truncateSync(filename, 0);

  console.log("writing to file");

  for (let i = 0; i < 7; i++){
    for (let j = 0; j < 7; j++){
      for (let k = 0; k < 7; k++){
        const sequenceStr = `${k}, ${j}, ${i}`; // In reverse order to ensure that the first 7 contains all 7 pieces, first 49 contains first 49, etc.
        fs.appendFileSync(filename, "\t" + sequenceStr + ",\n")
      }
    }
  }
}

function generateRanksDataset() {
  fs.appendFileSync(
    "docs/ranks_dataset.csv",
    "d1,d2,d3,d4,d5,d6,d7,d8,score\n"
  );
  for (let i = 0; i < 50000; i++) {
    const diffs = [];
    let index = 0;
    for (let j = 0; j < 8; j++) {
      const rand = Math.floor(Math.random() * 9);
      diffs.push(rand - 4);
      index *= 9;
      index += rand;
    }

    const rankStr = lookupStr.substr(index * 2, 2);
    const yVal = parseInt(rankStr, 36);

    // Reroll low-value entries with 80% chance, because they're far overrepresented
    if (yVal < 100) {
      if (Math.random() < 0.87) {
        i--;
        continue;
      }
    } else if (yVal < 200) {
      if (Math.random() < 0.5) {
        i--;
        continue;
      }
    }
    fs.appendFileSync(
      "docs/ranks_dataset.csv",
      diffs.join(",") + "," + yVal + "\n"
    );
  }
}

// generateCanonicalPieceSequences();
generateExhaustivePieceSequences();
