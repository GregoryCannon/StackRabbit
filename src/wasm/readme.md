## Intro

StackRabbit can be compiled with [emscriptem](https://emscripten.org/) into a wasm module to do in browser placement recommendations in real time.

The emscriptembindings are in the file src/wasm.cpp

## Compilation Instructions

1. Run the terminal command
    > npm run wasm_build 
2. That creates two output files:
    -  _compiledWasmRabbit.js
        This file serves as the "glue" between WASM and callers, giving method signatures and such
    -  _compiledWasmRabbit.wasm
        This is the actual binary compiled from the SR C++ code

3. Copy those two files into the desired frontend (for example, the test client in this folder)

## How to run the test client

1. > cd test_client
2. Launch a local server in this folder with 
    - `python3 -m http.server`
2. Open up [index.html](http://localhost:8000)