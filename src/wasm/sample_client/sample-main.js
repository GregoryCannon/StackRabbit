const output = document.getElementById('output');

const stackRabbitWorker = new Worker('./wasmRabbit-worker.js');

stackRabbitWorker.rpc = (...command) => {
    return new Promise((resolve, reject) => {
        const channel = new MessageChannel();
        channel.port1.onmessage = ({ data }) => {
            if (data.error) {
                reject(data.error);
            } else {
                resolve(data.result);
            }
        };
        stackRabbitWorker.postMessage(command, [channel.port2]);
    });
};

stackRabbitWorker.onmessage = async function (event) {
    if (event.data.type == 'init') init();
};


const params = {
    level: 18,
    lines: 50,
    inputFrameTimeline: 'X....',
    currentPiece: 'I',
    nextPiece: 'I',
    board: "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000011000000001100000000110000001011110111101111111110111111111011111111101111111110",
    playoutLength: 2,
};

const extraParams = {
    rateMove: {
        secondBoard: "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000011001000001100100000110010001011111111101111111110111111111011111111101111111110",
    }
}

async function runFunction(funcName) {
    const args = {
        ...params,
        ...extraParams[funcName]
    };

    start = Date.now();
    res = await stackRabbitWorker.rpc(funcName, args);
    elapsed = Date.now() - start;

    output.textContent = `${funcName} - ${elapsed} milliseconds\n\n`;
    output.textContent += JSON.stringify(res, null, 2);
}

function init() {
    [
        'getLockValueLookup',
        'getMove',
        'getTopMoves',
        'getTopMovesHybrid',
        'rateMove'
    ]
    .forEach(funcName => {
        const button = document.createElement('button');
        button.textContent = funcName;
        button.addEventListener('click', () => runFunction(funcName));
        document.getElementById('controls').append(button);
    });
}

