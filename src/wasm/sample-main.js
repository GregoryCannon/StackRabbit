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
    if (event.data.type == 'init') {
        const params = {
            level: 18,
            lines: 2,
            inputFrameTimeline: 'X....',
            currentPiece: 'J',
            nextPiece: 'L',
            board: "00000000000000000000000000000000000000000000000000000000000000000011100000001110000000111100000111110000011110000011111100011101110011101110001111111000111111100111111110011111111001111111101111111110",
        };
        
        const start = Date.now();
        const move = await stackRabbitWorker.rpc('getMove', params);
        const elapsed = Date.now() - start;
    
        output.textContent = `${JSON.stringify(move)}\nin ${elapsed} milliseconds`;
    }
};

