var Module = {
  initialized: false,
  onRuntimeInitialized: () => {
    console.log("onRuntimeInitialized");
    Module.initialized = true;
    workerInit();
  },
};

const SR_PIECES_INDEXES = {
  I: 0,
  O: 1,
  L: 2,
  J: 3,
  T: 4,
  S: 5,
  Z: 6,
};

const DELIM = "|";

function getStackRabbitArgString(args) {
  // Make playouts exhaustive for small lengths
  const playoutCount =
    args.playoutLength <= 3
      ? Math.pow(7, args.playoutLength)
      : args.playoutCount;
  const currentPieceIndex = SR_PIECES_INDEXES[args.currentPiece];
  const nextPieceIndex = SR_PIECES_INDEXES.hasOwnProperty(args.nextPiece)
    ? SR_PIECES_INDEXES[args.nextPiece]
    : -1;

  // Optional second board (player's resulting board), e.g. for getTopMoves to
  // guarantee the player's move shows up even if it's outside the top N.
  // Formats as follows: board1|board2|rest.
  const boardPrefix = args.secondBoard
    ? args.board + DELIM + args.secondBoard
    : args.board;

  const fieldsToJoin = [
    boardPrefix,
    args.level,
    args.lines,
    currentPieceIndex,
    nextPieceIndex,
    args.inputFrameTimeline,
    playoutCount,
    args.playoutLength,
    args.pruningBreadth || 12, // 12 = the C++ DEFAULT_PRUNING_BREADTH this client never sent before
    args.aggressionLevel || "safe",
  ];

  return fieldsToJoin.join(DELIM) + DELIM; // The C++ parsing requires an extra delimiter at the end.
}

// supported methods
const API = {
  getLockValueLookup: (args) => {
    const rawRes = Module.getLockValueLookup(getStackRabbitArgString(args));
    return JSON.parse(rawRes);
  },
  getMove: (args) => {
    const rawRes = Module.getMove(getStackRabbitArgString(args));
    return JSON.parse(rawRes);
  },
  getTopMoves: (args) => {
    const rawRes = Module.getTopMoves(getStackRabbitArgString(args));
    return JSON.parse(rawRes);
  },
  getTopMovesHybrid: (args) => {
    const rawRes = Module.getTopMovesHybrid(getStackRabbitArgString(args));
    return JSON.parse(rawRes);
  },
  rateMove: (args) => {
    const rawRes = Module.rateMove(getStackRabbitArgString(args));
    return JSON.parse(rawRes);
  },
};

function handle_message(e) {
  const reply_channel = e.ports[0];

  try {
    const [method, ...args] = e.data;
    const result = API[method].apply(API, args);
    reply_channel.postMessage({ result });
  } catch (err) {
    console.error(err);
    reply_channel.postMessage({ error: err.message });
  }
}

self.onmessage = () => {
  console.log("Worker not initialized");
};

function workerInit() {
  console.log("workerInit");
  postMessage({ type: "init" });
  self.onmessage = handle_message;
}

importScripts("./compiledWasmRabbit.js");
