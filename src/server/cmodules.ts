const cModule = require("../../../build/Release/cRabbit");

export function cTest() {
  console.time("C++");
  const result = cModule.length(
    "00000000000000000000000000000000000000000000000000000000000000000011100000001110000000111100000111110000011110000011111100011101110011101110001111111000111111100111111110011111111001111111101111111110|2|5|0|"
  );
  console.timeEnd("C++");
  console.log("----Result----");
  console.log(result);
}
cTest();
