const fs = require("fs");

const testData = Uint8Array.from([255, 40, 50, 60, 70, 80, 90, 100]);
const FILE_NAME = "test_encoded_bytes";

function writeRawBytesToFile(arrayBuffer) {
  const writeBuffer = Buffer.from(arrayBuffer);
  console.log("WriteBuffer:", writeBuffer);
  fs.writeFileSync(FILE_NAME, writeBuffer, function (err) {
    if (err) return console.log(err);
  });
}

function readRawBytesFromFile() {
  const fileBuffer = fs.readFileSync(FILE_NAME);
  const length = fileBuffer.byteLength;

  const intArray = new Uint8Array(length);
  for (var i = 0; i < length; i++) {
    intArray[i] = fileBuffer[i];
  }

  console.log("Int array read from file:", intArray);
}

writeRawBytesToFile(testData);
readRawBytesFromFile();
