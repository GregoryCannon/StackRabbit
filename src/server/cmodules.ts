const cModule = require("../../../build/Release/cRabbit");

export function cTest() {
  console.time("C++");
  const result = cModule.length(
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011111000001111111110|45|2|5|"
  );
  console.timeEnd("C++");
  console.log("----Result----");
  console.log(result);
}
cTest();
