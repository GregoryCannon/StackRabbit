const cModule = require("../../../build/Release/cRabbit");

export function cTest(){
  console.time("C++");
  const result = cModule.length("Bananas");
  console.timeEnd("C++");
  console.log(result);
}
cTest();