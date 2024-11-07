var fs = require("fs");

/** Compile each group of 8 results into a long long */
function generateCppBase7Ranks() {
  let ranks_NoNextBox_NoBars = fs.readFileSync(
    "docs/condensed_NoNextBox_NoBars.txt",
    "utf8"
  );

  const FILENAME = "docs/ranks_base_7.cpp";

  fs.writeFileSync(
    FILENAME,
    "const unsigned long long surfaceRanksChunked[] = {\n  "
  );
  let countRead = 0;
  let accum = BigInt(0);
  let lastIndex = Math.pow(7, 8);
  // let lastIndex = 100;
  for (let i = 0; i < lastIndex; i++) {
    if (countRead % 100000 == 0) {
      console.log(countRead / 1000000);
    }

    // Convert base 7 index to base 9
    const p1 = "00000000".split("").concat(i.toString(7).split("")).slice(-8);
    const convertedIndex = parseInt(p1.map((x) => parseInt(x) + 1).join(""), 9);

    if (i == 2882344) {
      console.log("i", i, "conv", convertedIndex);
    }

    // chunk is two bytes
    const chunk = ranks_NoNextBox_NoBars.substr(convertedIndex * 2, 2);
    const curNum = parseInt(chunk, 36) / 10 - 1;
    const scaledTo255 = Math.round((curNum / 33.8) * 255); // Originally ranges from 1 - 34.8, now ranges 0 to 255

    accum = accum * BigInt(256);
    accum += BigInt(scaledTo255);
    if (countRead % 8 === 7) {
      fs.appendFileSync(FILENAME, "0x" + accum.toString(16) + ",");
      accum = BigInt(0);
    }
    if (countRead % 800 === 799) {
      fs.appendFileSync(FILENAME, "\n  ");
    }
    countRead++;
  }

  console.log("finishing");
  fs.appendFileSync(FILENAME, "\n};");
}

// function convertToCFormat() {
//   const ranks_NoNextBox_NoBars = fs.readFileSync(
//     "docs/condensed_NoNextBox_NoBars.txt",
//     "utf8"
//   );
//   var readable = fs.createReadStream("docs/condensed_NoNextBox_NoBars.txt", {
//     encoding: "utf8",
//     fd: null,
//   });

//   fs.appendFileSync("docs/ranksOutput.cc", "short surfaceRanksRaw[] = {\n");
//   let countRead = 0;
//   let line = "";
//   for (let i = 0; i * 2 < ranks_NoNextBox_NoBars.length; i++) {
//     if (countRead % 100000 == 0) {
//       console.log(countRead / 1000000); // chunk is two bytes
//     }
//     let chunk = ranks_NoNextBox_NoBars.substr(i * 2, 2);
//     line += parseInt(chunk, 36) + ",";
//     if (countRead % 20 === 19) {
//       fs.appendFileSync("docs/ranksOutput.cc", line + "\n");
//       line = "";
//     }
//     countRead++;
//   }

//   console.log("finishing");
//   fs.appendFileSync("docs/ranksOutput.cc", "};");
// }

// function splitNextBoxData() {
//   var readable = fs.createReadStream("docs/condensed_NextBox_NoBars.txt", {
//     encoding: "utf8",
//     fd: null,
//   });

//   let countRead = 0;
//   let newFileStr = "";
//   let newFileStr2 = "";
//   readable.on("readable", function () {
//     var chunk;
//     while (
//       countRead < 200000000 &&
//       null !== (chunk = readable.read(2)) /* here */
//     ) {
//       if (countRead % 1000000 == 0) {
//         console.log(countRead / 1000000, chunk); // chunk is two bytes
//       }
//       fs.appendFileSync("_2byte_NextBox_1.txt", chunk);
//       countRead++;
//     }

//     while (
//       countRead < 40000000000 &&
//       null !== (chunk = readable.read(2)) /* here */
//     ) {
//       if (countRead % 1000000 == 0) {
//         console.log(countRead / 1000000, chunk); // chunk is two bytes
//       }
//       fs.appendFileSync("_2byte_NextBox_2.txt", chunk);
//       countRead++;
//     }
//   });
// }

// function convertToBase36() {
//   var readline = require("readline");

//   var inputfile = "docs/condensed_NextBox_NoBars.txt";
//   var outputfile = "docs/condensed_NoNextBox_NoBars_1.txt";
//   var instream = fs.createReadStream(inputfile);
//   var outstream = fs.createWriteStream(outputfile, { flags: "a" });
//   outstream.writable = true;

//   var rl = readline.createInterface(instream);

//   let i = 0;
//   rl.on("line", function (lineStr) {
//     const integerForm = parseInt((parseFloat(lineStr) * 10).toFixed(0));
//     let base36Str = integerForm.toString(36);
//     while (base36Str.length < 2) {
//       base36Str = "0" + base36Str;
//     }

//     if (i % 1000000 == 0) {
//       console.log(Number(i).toLocaleString());
//       console.log(`${integerForm} -> ${base36Str}`);
//     }

//     outstream.write(base36Str);
//     i++;
//   });

//   rl.on("close", function () {
//     console.log("close");
//   });

//   rl.on("error", function (e) {
//     console.error(e);
//   });
// }

generateCppBase7Ranks();
