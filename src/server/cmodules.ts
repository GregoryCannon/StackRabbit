const cModule = require("../../../build/Release/cRabbit");

export function cTest() {
  console.time("C++");
  const result = cModule.getLockValueLookup(
    "00000000000000000000000000000000000000000000000000100000000011000000001100000000110010000011111100001111111000111111100011111110001111111100111111110011111111101111111110111111111011111111101111111110|18|12|2|0|X.|0|1"
  );
  console.timeEnd("C++");
  console.log("----Result----");
  console.log(result);
}
console.log("Making C++ module call");
cTest();
console.log("Done C++ module call");
