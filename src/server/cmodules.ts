const cModule = require("../../../build/Release/cRabbit");

export function cTest() {
  console.time("C++");
  const testInput =
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000001000011100100011110011011111111111111110|19|0|5|4|X....|";
  const result = cModule.length(testInput);
  console.timeEnd("C++");
  console.log("----Result----");
  console.log(result);
}
cTest();
