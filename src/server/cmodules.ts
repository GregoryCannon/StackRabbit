const cModule = require("../../../build/Release/cRabbit");

export function cTest(){
  console.time("C++");
  const result = cModule.calc();
  console.timeEnd("C++");
  console.log(result);
}
cTest();