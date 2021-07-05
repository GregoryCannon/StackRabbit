var fs = require("fs");

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
