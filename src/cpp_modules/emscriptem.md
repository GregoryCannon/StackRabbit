## Intro
StackRabbit can be compiled with [emscriptem](https://emscripten.org/) into a wasm module to do in browser placement recommendations in real time.

The emscriptembindings are in the file src/wasm.cpp

## Compile

Use this command

```bash 
emcc -O3 src/wasm.cpp --bind -lembind -g0 -o stackrabbit.html
```

## Use in JS

While the module is fast, is blocks for too long:

This simple comamnd:
```javascript
const move = Module.getMove("00000000000000000000000000000000000000000000000000000000000000000011100000001110000000111100000111110000011110000011111100011101110011101110001111111000111111100111111110011111111001111111101111111110|18|2|5|0|X...|");
```

takes 14ms to process, so it should not be set in the main thread.

Web workers should be used to make use of the module.



