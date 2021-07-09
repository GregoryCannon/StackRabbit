var fs = require("fs");


function convertToCFormat() {
  const ranks_NoNextBox_NoBars = fs.readFileSync(
    "docs/condensed_NoNextBox_NoBars.txt",
    "utf8"
  );
  var readable = fs.createReadStream("docs/condensed_NoNextBox_NoBars.txt", {
    encoding: "utf8",
    fd: null,
  });

  fs.appendFileSync("docs/ranksOutput.cc", "short surfaceRanksRaw[] = {\n");
  let countRead = 0;
  let line = "";
  for (let i = 0; i * 2 < ranks_NoNextBox_NoBars.length; i++) {
    if (countRead % 100000 == 0) {
      console.log(countRead / 1000000); // chunk is two bytes
    }
    chunk = ranks_NoNextBox_NoBars.substr(i * 2, 2);
    line += parseInt(chunk, 36) + ","
    if (countRead % 20 === 19) {
      fs.appendFileSync("docs/ranksOutput.cc", line + "\n");
      line = "";
    }
    countRead++;
  }

  console.log("finishing");
  fs.appendFileSync("docs/ranksOutput.cc", "};");
}


function splitNextBoxData() {
  var readable = fs.createReadStream("docs/condensed_NextBox_NoBars.txt", {
    encoding: "utf8",
    fd: null,
  });

  let countRead = 0;
  let newFileStr = "";
  let newFileStr2 = "";
  readable.on("readable", function () {
    var chunk;
    while (
      countRead < 200000000 &&
      null !== (chunk = readable.read(2)) /* here */
    ) {
      if (countRead % 1000000 == 0) {
        console.log(countRead / 1000000, chunk); // chunk is two bytes
      }
      fs.appendFileSync("_2byte_NextBox_1.txt", chunk);
      countRead++;
    }

    while (
      countRead < 40000000000 &&
      null !== (chunk = readable.read(2)) /* here */
    ) {
      if (countRead % 1000000 == 0) {
        console.log(countRead / 1000000, chunk); // chunk is two bytes
      }
      fs.appendFileSync("_2byte_NextBox_2.txt", chunk);
      countRead++;
    }
  });
}

function convertToBase36() {
  var readline = require("readline");

  var inputfile = "docs/condensed_NextBox_NoBars.txt";
  var outputfile = "docs/condensed_NoNextBox_NoBars_1.txt";
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
}
