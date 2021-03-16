var fs = require("fs");
var readline = require("readline");

var inputfile = "docs/ranks_NoNextBox_WithoutIPieces.txt";
var outputfile = "docs/condensed_NoNextBox_NoBars.txt";

var instream = fs.createReadStream(inputfile);
var outstream = fs.createWriteStream(outputfile, { flags: "a" });
outstream.writable = true;

var rl = readline.createInterface(instream);

let i = 0;
rl.on("line", function (lineStr) {
  const integerForm = parseInt((parseFloat(lineStr) * 10).toFixed(0));
  let base36Str = integerForm.toString(36);
  while (base36Str.length < 2) {
    base36Str = "0" + base36Str;
  }

  if (i % 1000000 == 0) {
    console.log(Number(i).toLocaleString());
    console.log(`${integerForm} -> ${base36Str}`);
  }

  outstream.write(base36Str);
  i++;
});

rl.on("close", function () {
  console.log("close");
});

rl.on("error", function (e) {
  console.error(e);
});
