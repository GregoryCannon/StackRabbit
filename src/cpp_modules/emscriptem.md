## Intro
StackRabbit can be compiled with [emscriptem](https://emscripten.org/) into a wasm module to do in browser placement recommendations in real time.

The emscriptembindings are in the file src/wasm.cpp

## Compile

Use this command

```bash 
emcc -O3 src/wasm.cpp --bind -lembind -g0 -o wasmRabbit.js
```

This will produce 2 files 
* `wasmRabbit.js`
* `wasmRabbit.wasm`


## Use in JS

To test the worker, look at the sample app [here](../wasm/)



