import { getPieceSequence } from "../lite_game_simulator";
import * as fs from "fs";

function getSequences(numSequences) {
  let sequenceStr = "";
  for (let i = 0; i < numSequences; i++){
    console.log(i);
    sequenceStr += getPieceSequence().join("");
  }
  fs.writeFileSync("docs/sequence_pool.txt", sequenceStr)
}

getSequences(10000);