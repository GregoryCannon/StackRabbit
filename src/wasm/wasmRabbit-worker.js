var Module = {
	initialized: false,
	onRuntimeInitialized: () => {
		console.log('onRuntimeInitialized');
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

function getStackRabbitArgString(args) {
	const {
		level,
		lines,
		inputFrameTimeline,
		currentPiece,
		nextPiece,
		board,
	} = args;

	return [
		board,
		level,
		lines,
		SR_PIECES_INDEXES[currentPiece],
		SR_PIECES_INDEXES[nextPiece],
		inputFrameTimeline,
		'', // playoutCount?
		// '', // playoutLength?
	].join('|');
}

// supported methods
const API = {
	getMove: args => {
		const start = Date.now();
		const rawRes = Module.getMove(getStackRabbitArgString(args));
		const getMoveTime = Date.now() - start;
		console.log({ getMoveTime });
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
	console.log('Worker not initialized');
};

function workerInit() {
	console.log('workerInit');
	postMessage({ type: 'init' });
	self.onmessage = handle_message;
}

importScripts('./wasmRabbit.js');
