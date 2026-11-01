# StackRabbit

An AI that plays NES Tetris at a super-human level. Can play live games in an emulator or on console, and can also be used for analyzing human gameplay or individual board positions.

### Theory

At it's core, StackRabbit isk based on traditional search & heuristic methods, but it achieves high-quality board evaluation through high-depth playouts. (For example, 3000 playouts at depth 10, for each of 8 promising moves.)

Unlike other games like chess, the "opponent" (the game's piece selector) doesn't make its selections adversarially (much as players may feel otherwise). That means that many optimizations like alpha-beta pruning don't apply here. Since any possible upcoming sequence is equally likely (aside from quirks of the NES RNG), we instead take a large sample of possible sequences, and test how well each position turns out when playing out those sequences.

The static eval function was created and tuned with a combination of intentional design (from my own high-level gameplay experience), statistical tuning, A/B testing, and years of experimentation. It has limitations, as any static eval does, but its main purpose is to guide the agent during playouts, not so much to give definitive ratings to positions by themselves.

# Recommended Configurations

The quality of evaluation you get from StackRabbit does scale up with more computation time. 

### Live Games
In live gameplay, the agent is only guaranteed to get 160ms (10 frames) of thinking time between when the previous piece locks into the stack and the next piece spawns. My recommendation for working under this limitation:

`playoutCount = 400`, `playoutLength = 6`

These numbers work on my 2026 Macbook Air M5. You can scale it up or down depending on how strong your processor is. Just keep in mind that the higher the depth (playoutLength), more playouts are needed to reduce the variance.

### In-depth Analysis

If your client can afford the wait time, doing a deeper search with much more playouts provides even better analysis. My recommendation:

`playoutCount = 3000`, `playoutLength = 10`

### Fast, Zero-variance Analysis

At lower depth (depth 3 or less), Stackrabbit can do a fully exhaustive search over all possible upcoming sequences. This has more precision (zero variance from sampling), but due to the lower depth, it has less "wisdom" when evaluating long-term macro decisions.

# Input Methods

StackRabbit supports all major input methods (DAS, hypertap, rolling) and can be configured between them, even at runtime. The input speed (often measured in Hz) of tapping and rolling can be configured as well. Note that rolling and tapping are indistinguishable from an agent's perspective, differing only in what Hz values would commonly be associated with each (<15 Hz for tapping, and >15Hz for rolling).

### Tap/Roll Speed

The code generally uses a string `inputFrameTimeline` to control the input speed. Each character in the string represents one frame, and a "X" means the agent can perform inputs on that frame, and "." means it's on cooldown. For example, "X....." means one input and then 5 frames in between, which comes out to 10 Hz of effective speed.

### DAS 
The Lua script (which handles live gameplay in FCEUX) has a parameter `USE_DAS` to toggle between DAS and Tap/Rolling. 

When `USE_DAS` is `true`, the `inputFrameTimeline` variable controls how fast the agent can "quicktap":
- A value of "X.." or faster allows for 8-high-left quicktaps on 19 speed
- A value of "X...." or faster allows for 7-high-left quicktaps on 19 speed
- A value of "X....." or slower doesn't allow for any quicktaps.

# Overview of Code Segments

## Backend

The core computation code lives in `src/cpp_modules` and is shared by all entrypoints.

There are three main ways to run the StackRabbit backend:

1) `src/wasm` contains instructions for compiling this repo into a WASM binary, which then can be consumed by any Javascript client.
2) `src/server` contains a Node.js server which supports many API endpoints, but is primarily only used for live gameplay (via "precompute" requests).
3) `entrypoint.cpp` is a local C++ file that can be run from the command line, for local testing. The test inputs can be configured at the top of the file.

## Possible Frontends

There are a number of different clients for interacting with the main StackRabbit backend:

- [TetrisTrainer](https://github.com/GregoryCannon/TetrisTrainer) is a public web client ([try it live!](https://gregorycannon.github.io/TetrisTrainer)) that lets users draw a board and ask AI about the best placements.
- `fceux` contains a Lua client for playing in the FCEUX emulator (primary client for running live games with StackRabbit)
-  `console_client` contains a python client that runs on Raspberry Pi to play on a real console.
- Any other Javascript client can consume the WASM binary (details in `src/wasm`) and call all the major endpoints.

# How to Set Up

## Requirements

- **Node.js**: [Download here](https://nodejs.org)
- **FCEUX**: [Download here](https://fceux.com)
- **Python3**: [Download here](https://python.org)
- (Windows-only) **Visual Studio Build Tools**

## Steps

1. **Clone or Extract**:
   - Clone this repository or extract the zip file from the releases page.

2. **Obtain NES Tetris ROM**:
   - **Legally** acquire the ROM for NES Tetris. You can use TetrisGYM.

3. **Open Command Line**:
   - Press `Win + R`, type `cmd`, and press Enter.
   - Navigate to the folder for Stackrabbit using `cd path/to/stackrabbit`.

4. **Install Dependencies**:
   - Run `node install.js` in the command line.

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


# If Anyone Found An error, please tell me in the issues tab
  - We may have made some errors, so if you tell us those errors, We will make corrections as soon as possible, Thank You!

# Credits (In contributor join sequence):
  - Making the AI: [@GregoryCannon](https://github.com/GregoryCannon)
  - Making the Tutorial on setting up: [@BenP1236691](https://github.com/thehindsight)
  - Bug fixing the TypeScript: [@NguyenQuangMinh0504](https://github.com/NguyenQuangMinh0504)
  - Compiling StackRabbit into web assembly: [@timotheeg](https://github.com/timotheeg)
  - Adding column 9 vits: [@fractal161](https://github.com/fractal161)
