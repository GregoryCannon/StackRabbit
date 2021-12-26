const cModule = require("../../../build/Release/cRabbit");

export function cTest() {
  console.time("C++");
  const result = cModule.getLockValueLookup(
    "00000000000000000000000000000000000000000000000000000000000000011000000001100100000110110000011111000011111100011111110001111111001111111100111111111011111111101111111110111111111011111111101111111110|19|192|2|0|X.|"
  );
  console.timeEnd("C++");
  console.log("----Result----");
  console.log(result);
}
console.log("Making C++ module call");
cTest();
console.log("Done C++ module call");
