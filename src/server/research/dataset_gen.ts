import { PIECE_LIST } from "../../../docs/tetrominoes";
import { getRandomPiece } from "../piece_rng";

const fs = require("fs");
const lookupStr = fs.readFileSync(
  "docs/condensed_NoNextBox_NoBars.txt",
  "utf8"
);

function generateCanonicalPieceSequences() {
  const filename = "docs/canonical_sequences.cc";
  fs.truncateSync(filename, 0);

  // This is outputted as a C++ file, so do some setup before starting the data
  fs.appendFileSync(filename, "const int canonicalPieceSequences[] = {\n");

  for (let i = 0; i < 1000; i++) {
    let sequence = [];
    let lastKnownPiece = PIECE_LIST[i % 7][2] as PieceId;
    sequence.push(i % 7); // First piece loops around the possible 7 pieces
    for (let j = 0; j < 9; j++) {
      lastKnownPiece = getRandomPiece(lastKnownPiece, /* isDrought= */ false);
      sequence.push(PIECE_LIST.findIndex((x) => x[2] == lastKnownPiece));
    }
    fs.appendFileSync(filename, "\t" + sequence.join(", ") + ",\n");
  }
  fs.appendFileSync(filename, "};\n");
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

generateCanonicalPieceSequences();
