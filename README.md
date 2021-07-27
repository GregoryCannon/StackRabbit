# StackRabbit

An AI that plays NES Tetris at a high level. Primarily based on search & heuristic, with high-quality board eval through value iteration.

Due to the logistics of playing NES Tetris, there are two different clients for interacting with the main AI backend:

- `fceux` contains a Lua client for playing in the FCEUX emulator (primary client)
-  `console_client` contains a python client that runs on Raspberry Pi to play on a real console.
- [TetrisTrainer](https://github.com/GregoryCannon/TetrisTrainer) is a public web client ([try it live!](https://gregorycannon.github.io/TetrisTrainer)) that lets users draw a board and ask AI about the best placements.


Then there are two components of the backend:

- `server` contains the primary server, written in Node.js. It handles the request parsing, and the delegation to worker threads. It also contains lots of AI code (that's slowly becoming deprecated), since the initial implmentation was entirely in JS (oops).
- `cpp_modules` contains modules that perform the core AI computation at literally 100x the speed of the original JS implementation. I'm still working on expanding the modules' functionality and integrating them into main AI flow.
