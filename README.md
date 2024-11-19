# StackRabbit

An AI that plays NES Tetris at a high level. Primarily based on search & heuristic, with high-quality board eval through value iteration.

Due to the logistics of playing NES Tetris, there are two different clients for interacting with the main AI backend:

- `fceux` contains a Lua client for playing in the FCEUX emulator (primary client)
-  `console_client` contains a python client that runs on Raspberry Pi to play on a real console.
- [TetrisTrainer](https://github.com/GregoryCannon/TetrisTrainer) is a public web client ([try it live!](https://gregorycannon.github.io/TetrisTrainer)) that lets users draw a board and ask AI about the best placements.


Then there are two components of the backend:

- `server` contains the primary server, written in Node.js. It handles the request parsing, and the delegation to worker threads. It also contains lots of deprecated AI code, since the initial implmentation was entirely in JS (oops).
- `cpp_modules` contains modules that perform the core AI computation at literally 100x the speed of the original JS implementation. The main flow involves a Node server thread sending a game state to the C++ module, which returns the value of each possible move as an encoded JSON map.


# How to Set Up

## Requirements

- **Node.js**: [Download here](https://nodejs.org)
- **FCEUX**: [Download here](https://fceux.com)
- **Python3**: [Download here](https://python.org)
- **node-gyp & nan**: Install with `npm i node-gyp nan` (Run this AFTER installing node.js And reopening the terminal)
- **Visual Studio Build Tools**

## Steps

1. **Clone or Extract**:
   - Clone this repository or extract the zip file from the releases page.

2. **Obtain NES Tetris ROM**:
   - **Legally** acquire the ROM for NES Tetris. You can use TetrisGYM.

3. **Open Command Line**:
   - Press `Win + R`, type `cmd`, and press Enter.
   - Navigate to the folder for Stackrabbit using `cd path/to/stackrabbit`.

4. **Install Dependencies**:
   - Run `npm i` in the command line.

5. **Start the Application**:
   - If the previous command runs without errors, execute `npm start`.

6. **Setup FCEUX**:
   - Open the FCEUX folder.]
   - Add all `.lua` files (excluding `itn12.lua`, `mime.lua`, and `socket.lua`, put them in `C:/path/to/fceux/lua`) from the [Luasocket repository](https://github.com/lunarmodules/luasocket) to `C:/path/to/fceux/lua/socket/`. (if there is no `lua` folder, create it and the socket folder inside)

7. **Load Tetris ROM**:
   - In FCEUX, click `File > Open` and select the Tetris ROM.

8. **Run Stackrabbit Script**:
   - In the FCEUX window, go to `File > Lua > New Lua Script Window`.
   - In the new window, browse and run `path/to/stackrabbit/src/fceux/stackrabbit.lua`.
   - Open level 19 and let it start!


#If Anyone Found An error, please tell me in the issues tab
   - We may have made some errors, so if you tell us those errors, We will make corrections as soon as possible, Thank You!

##Credits (In contributor join sequence):
   - Making the AI: [@GregoryCannon](https://github.com/GregoryCannon)
   - Helping with the AI: [@wikedawsom](https://github.com/wikedawsom)
   - Making the Tutorial on setting up: [@BenP1236691](https://github.com/BenP1236691)
   - Bug fixing the TypeScript: [@NguyenQuangMinh0504](https://github.com/NguyenQuangMinh0504)
   - Compiling StackRabbit into web assembly: [@timotheeg](https://github.com/timotheeg)
   - Adding column 9 vits: [@fractal161](https://github.com/fractal161)
