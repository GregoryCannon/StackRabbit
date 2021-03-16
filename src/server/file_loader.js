const fs = require("fs");
const readline = require("readline");

// function loadFileToArray(fileAddress) {
//   console.log("reading file:", fileAddress);
//   const array = [];
//   const file = readline.createInterface({
//     input: fs.createReadStream(fileAddress),
//   });
//   file.on("line", (line) => {
//     console.log("line received");
//     array.push(line);
//   });
//   return array;
// }

async function loadFileToArray(fileName) {
  return new Promise(function (resolve, reject) {
    var array = [];
    console.log("Loading file:", fileName);
    fs.createReadStream(fileName)
      .on("line", (line) => {
        console.log("line received");
        array.push(line);
      })
      .on("end", () => {
        console.log("CSV file successfully processed");
        console.log(array);
        resolve(array);
      })
      .on("error", reject);
  });
}

module.exports = {
  loadFileToArray,
};
