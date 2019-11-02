/******/ (function(modules) { // webpackBootstrap
/******/ 	// The module cache
/******/ 	var installedModules = {};
/******/
/******/ 	// The require function
/******/ 	function __webpack_require__(moduleId) {
/******/
/******/ 		// Check if module is in cache
/******/ 		if(installedModules[moduleId]) {
/******/ 			return installedModules[moduleId].exports;
/******/ 		}
/******/ 		// Create a new module (and put it into the cache)
/******/ 		var module = installedModules[moduleId] = {
/******/ 			i: moduleId,
/******/ 			l: false,
/******/ 			exports: {}
/******/ 		};
/******/
/******/ 		// Execute the module function
/******/ 		modules[moduleId].call(module.exports, module, module.exports, __webpack_require__);
/******/
/******/ 		// Flag the module as loaded
/******/ 		module.l = true;
/******/
/******/ 		// Return the exports of the module
/******/ 		return module.exports;
/******/ 	}
/******/
/******/
/******/ 	// expose the modules object (__webpack_modules__)
/******/ 	__webpack_require__.m = modules;
/******/
/******/ 	// expose the module cache
/******/ 	__webpack_require__.c = installedModules;
/******/
/******/ 	// define getter function for harmony exports
/******/ 	__webpack_require__.d = function(exports, name, getter) {
/******/ 		if(!__webpack_require__.o(exports, name)) {
/******/ 			Object.defineProperty(exports, name, { enumerable: true, get: getter });
/******/ 		}
/******/ 	};
/******/
/******/ 	// define __esModule on exports
/******/ 	__webpack_require__.r = function(exports) {
/******/ 		if(typeof Symbol !== 'undefined' && Symbol.toStringTag) {
/******/ 			Object.defineProperty(exports, Symbol.toStringTag, { value: 'Module' });
/******/ 		}
/******/ 		Object.defineProperty(exports, '__esModule', { value: true });
/******/ 	};
/******/
/******/ 	// create a fake namespace object
/******/ 	// mode & 1: value is a module id, require it
/******/ 	// mode & 2: merge all properties of value into the ns
/******/ 	// mode & 4: return value when already ns object
/******/ 	// mode & 8|1: behave like require
/******/ 	__webpack_require__.t = function(value, mode) {
/******/ 		if(mode & 1) value = __webpack_require__(value);
/******/ 		if(mode & 8) return value;
/******/ 		if((mode & 4) && typeof value === 'object' && value && value.__esModule) return value;
/******/ 		var ns = Object.create(null);
/******/ 		__webpack_require__.r(ns);
/******/ 		Object.defineProperty(ns, 'default', { enumerable: true, value: value });
/******/ 		if(mode & 2 && typeof value != 'string') for(var key in value) __webpack_require__.d(ns, key, function(key) { return value[key]; }.bind(null, key));
/******/ 		return ns;
/******/ 	};
/******/
/******/ 	// getDefaultExport function for compatibility with non-harmony modules
/******/ 	__webpack_require__.n = function(module) {
/******/ 		var getter = module && module.__esModule ?
/******/ 			function getDefault() { return module['default']; } :
/******/ 			function getModuleExports() { return module; };
/******/ 		__webpack_require__.d(getter, 'a', getter);
/******/ 		return getter;
/******/ 	};
/******/
/******/ 	// Object.prototype.hasOwnProperty.call
/******/ 	__webpack_require__.o = function(object, property) { return Object.prototype.hasOwnProperty.call(object, property); };
/******/
/******/ 	// __webpack_public_path__
/******/ 	__webpack_require__.p = "dist";
/******/
/******/
/******/ 	// Load entry module and return exports
/******/ 	return __webpack_require__(__webpack_require__.s = "./tetris.js");
/******/ })
/************************************************************************/
/******/ ({

/***/ "./tetris.js":
/*!*******************!*\
  !*** ./tetris.js ***!
  \*******************/
/*! no exports provided */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony import */ var _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./tetrominoes.js */ \"./tetrominoes.js\");\nconst cvs = document.getElementById(\"tetris\");\r\nconst ctx = cvs.getContext(\"2d\");\r\nconst scoreElement = document.getElementById(\"score\");\r\n\r\nconst ROW = 20;\r\nconst COLUMN = 10;\r\nconst SQ_SIZE = 20;\r\nconst VACANT = \"BLACK\"; // color of an empty square\r\n\r\nconst DROP_TIME_MS = 200;\r\n\r\n\r\n\r\n// draw a square\r\nfunction drawSquare(x,y,color){\r\n    ctx.fillStyle = color;\r\n    ctx.fillRect(x*SQ_SIZE,y*SQ_SIZE,SQ_SIZE,SQ_SIZE);\r\n\r\n    ctx.strokeStyle = \"BLACK\";\r\n    ctx.strokeRect(x*SQ_SIZE,y*SQ_SIZE,SQ_SIZE,SQ_SIZE);\r\n}\r\n\r\n// create the board\r\nlet board = [];\r\nfor(let r = 0; r <ROW; r++){\r\n    board[r] = [];\r\n    for(let c = 0; c < COLUMN; c++){\r\n        board[r][c] = VACANT;\r\n    }\r\n}\r\n\r\n// draw the board\r\nfunction drawBoard(){\r\n    for(let r = 0; r <ROW; r++){\r\n        for(let c = 0; c < COLUMN; c++){\r\n            drawSquare(c,r,board[r][c]);\r\n        }\r\n    }\r\n}\r\n\r\nlet gameOver = false;\r\n\r\ndrawBoard();\r\n\r\n// Generate random pieces\r\nfunction randomPiece(){\r\n    let r = Math.floor(Math.random() * _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECES\"].length) // 0 -> 6\r\n    return new Piece( _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECES\"][r][0],_tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECES\"][r][1]);\r\n}\r\n\r\nlet p = randomPiece();\r\n\r\n// The Object Piece\r\n\r\nfunction Piece(tetromino,color){\r\n    this.tetromino = tetromino;\r\n    this.color = color;\r\n    \r\n    this.tetrominoN = 0; // we start from the first pattern\r\n    this.activeTetromino = this.tetromino[this.tetrominoN];\r\n    \r\n    // we need to control the pieces\r\n    this.x = 3;\r\n    this.y = -2;\r\n}\r\n\r\n// fill function\r\nPiece.prototype.fill = function(color){\r\n    for(let r = 0; r < this.activeTetromino.length; r++){\r\n        for(let c = 0; c < this.activeTetromino.length; c++){\r\n            // we draw only occupied squares\r\n            if( this.activeTetromino[r][c]){\r\n                drawSquare(this.x + c,this.y + r, color);\r\n            }\r\n        }\r\n    }\r\n}\r\n\r\n// draw a piece to the board\r\nPiece.prototype.draw = function(){\r\n    this.fill(this.color);\r\n}\r\n\r\n// undraw a piece\r\nPiece.prototype.unDraw = function(){\r\n    this.fill(VACANT);\r\n}\r\n\r\n// move Down the piece\r\nPiece.prototype.moveDown = function(){\r\n    if(!this.collision(0,1,this.activeTetromino)){\r\n        this.unDraw();\r\n        this.y++;\r\n        this.draw();\r\n    }else{\r\n        // we lock the piece and generate a new one\r\n        this.lock();\r\n        p = randomPiece();\r\n    }   \r\n}\r\n\r\n// move Right the piece\r\nPiece.prototype.moveRight = function(){\r\n    if(!this.collision(1,0,this.activeTetromino)){\r\n        this.unDraw();\r\n        this.x++;\r\n        this.draw();\r\n    }\r\n}\r\n\r\n// move Left the piece\r\nPiece.prototype.moveLeft = function(){\r\n    if(!this.collision(-1,0,this.activeTetromino)){\r\n        this.unDraw();\r\n        this.x--;\r\n        this.draw();\r\n    }\r\n}\r\n\r\n// rotate the piece\r\nPiece.prototype.rotate = function(){\r\n    let nextPattern = this.tetromino[(this.tetrominoN + 1)%this.tetromino.length];\r\n    let kick = 0;\r\n    \r\n    if(this.collision(0,0,nextPattern)){\r\n        if(this.x > COLUMN/2){\r\n            // it's the right wall\r\n            kick = -1; // we need to move the piece to the left\r\n        }else{\r\n            // it's the left wall\r\n            kick = 1; // we need to move the piece to the right\r\n        }\r\n    }\r\n    \r\n    if(!this.collision(kick,0,nextPattern)){\r\n        this.unDraw();\r\n        this.x += kick;\r\n        this.tetrominoN = (this.tetrominoN + 1)%this.tetromino.length; // (0+1)%4 => 1\r\n        this.activeTetromino = this.tetromino[this.tetrominoN];\r\n        this.draw();\r\n    }\r\n}\r\n\r\nlet score = 0;\r\n\r\nPiece.prototype.lock = function(){\r\n    for(let r = 0; r < this.activeTetromino.length; r++){\r\n        for(let c = 0; c < this.activeTetromino.length; c++){\r\n            // we skip the vacant squares\r\n            if( !this.activeTetromino[r][c]){\r\n                continue;\r\n            }\r\n            // pieces to lock on top = game over\r\n            if(this.y + r < 0){\r\n                alert(\"Game Over\");\r\n                // stop request animation frame\r\n                gameOver = true;\r\n                break;\r\n            }\r\n            // we lock the piece\r\n            board[this.y+r][this.x+c] = this.color;\r\n        }\r\n    }\r\n    // remove full rows\r\n    for(let r = 0; r < ROW; r++){\r\n        let isRowFull = true;\r\n        for(let c = 0; c < COLUMN; c++){\r\n            isRowFull = isRowFull && (board[r][c] != VACANT);\r\n        }\r\n        if(isRowFull){\r\n            // if the row is full\r\n            // we move down all the rows above it\r\n            for(let y = r; y > 1; y--){\r\n                for(let c = 0; c < COLUMN; c++){\r\n                    board[y][c] = board[y-1][c];\r\n                }\r\n            }\r\n            // the top row board[0][..] has no row above it\r\n            for(let c = 0; c < COLUMN; c++){\r\n                board[0][c] = VACANT;\r\n            }\r\n            // increment the score\r\n            score += 10;\r\n        }\r\n    }\r\n    // update the board\r\n    drawBoard();\r\n    \r\n    // update the score\r\n    scoreElement.innerHTML = score;\r\n}\r\n\r\n// collision fucntion\r\n\r\nPiece.prototype.collision = function(x,y,piece){\r\n    for(let r = 0; r < piece.length; r++){\r\n        for(let c = 0; c < piece.length; c++){\r\n            // if the square is empty, we skip it\r\n            if(!piece[r][c]){\r\n                continue;\r\n            }\r\n            // coordinates of the piece after movement\r\n            let newX = this.x + c + x;\r\n            let newY = this.y + r + y;\r\n            \r\n            // conditions\r\n            if(newX < 0 || newX >= COLUMN || newY >= ROW){\r\n                return true;\r\n            }\r\n            // skip newY < 0; board[-1] will crush our game\r\n            if(newY < 0){\r\n                continue;\r\n            }\r\n            // check if there is a locked piece alrady in place\r\n            if( board[newY][newX] != VACANT){\r\n                return true;\r\n            }\r\n        }\r\n    }\r\n    return false;\r\n}\r\n\r\n// CONTROL the piece\r\n\r\ndocument.addEventListener(\"keydown\", keyDownListener);\r\ndocument.addEventListener(\"keyup\", keyUpListener)\r\nfunction keyDownListener(event){\r\n    if(event.keyCode == 37){\r\n        p.moveLeft();\r\n        dropStart = Date.now();\r\n    }else if(event.keyCode == 38){\r\n        p.rotate();\r\n        dropStart = Date.now();\r\n    }else if(event.keyCode == 39){\r\n        p.moveRight();\r\n        dropStart = Date.now();\r\n    }else if(event.keyCode == 40){\r\n        p.moveDown();\r\n    }\r\n}\r\n\r\nfunction keyUpListener(event){\r\n\r\n}\r\n\r\n// drop the piece every 1sec\r\n\r\nlet dropStart = Date.now();\r\nfunction drop(){\r\n    let now = Date.now();\r\n    let delta = now - dropStart;\r\n    if(delta > 1000){\r\n        p.moveDown();\r\n        dropStart = Date.now();\r\n    }\r\n    if(!gameOver){\r\n        requestAnimationFrame(drop);\r\n    }\r\n}\r\n\r\n//drop();\r\n\r\nlet framecount = 0;\r\nfunction gameLoop(){\r\n    framecount += 1;\r\n    if (framecount >= 10){\r\n        p.moveDown();\r\n        framecount = 0;\r\n    }\r\n    if(!gameOver){\r\n        requestAnimationFrame(gameLoop);\r\n    }\r\n}\r\ngameLoop();\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\n\n//# sourceURL=webpack:///./tetris.js?");

/***/ }),

/***/ "./tetrominoes.js":
/*!************************!*\
  !*** ./tetrominoes.js ***!
  \************************/
/*! exports provided: PIECES */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"PIECES\", function() { return PIECES; });\nconst PIECE_I = [\n  [\n    [0, 0, 0, 0],\n    [1, 1, 1, 1],\n    [0, 0, 0, 0],\n    [0, 0, 0, 0],\n  ],\n  [\n    [0, 0, 1, 0],\n    [0, 0, 1, 0],\n    [0, 0, 1, 0],\n    [0, 0, 1, 0],\n  ],\n  [\n    [0, 0, 0, 0],\n    [0, 0, 0, 0],\n    [1, 1, 1, 1],\n    [0, 0, 0, 0],\n  ],\n  [\n    [0, 1, 0, 0],\n    [0, 1, 0, 0],\n    [0, 1, 0, 0],\n    [0, 1, 0, 0],\n  ]\n];\n\nconst PIECE_J = [\n  [\n    [1, 0, 0],\n    [1, 1, 1],\n    [0, 0, 0]\n  ],\n  [\n    [0, 1, 1],\n    [0, 1, 0],\n    [0, 1, 0]\n  ],\n  [\n    [0, 0, 0],\n    [1, 1, 1],\n    [0, 0, 1]\n  ],\n  [\n    [0, 1, 0],\n    [0, 1, 0],\n    [1, 1, 0]\n  ]\n];\n\nconst PIECE_L = [\n  [\n    [0, 0, 1],\n    [1, 1, 1],\n    [0, 0, 0]\n  ],\n  [\n    [0, 1, 0],\n    [0, 1, 0],\n    [0, 1, 1]\n  ],\n  [\n    [0, 0, 0],\n    [1, 1, 1],\n    [1, 0, 0]\n  ],\n  [\n    [1, 1, 0],\n    [0, 1, 0],\n    [0, 1, 0]\n  ]\n];\n\nconst PIECE_O = [\n  [\n    [0, 0, 0, 0],\n    [0, 1, 1, 0],\n    [0, 1, 1, 0],\n    [0, 0, 0, 0],\n  ]\n];\n\nconst PIECE_S = [\n  [\n    [0, 1, 1],\n    [1, 1, 0],\n    [0, 0, 0]\n  ],\n  [\n    [0, 1, 0],\n    [0, 1, 1],\n    [0, 0, 1]\n  ],\n  [\n    [0, 0, 0],\n    [0, 1, 1],\n    [1, 1, 0]\n  ],\n  [\n    [1, 0, 0],\n    [1, 1, 0],\n    [0, 1, 0]\n  ]\n];\n\nconst PIECE_T = [\n  [\n    [0, 1, 0],\n    [1, 1, 1],\n    [0, 0, 0]\n  ],\n  [\n    [0, 1, 0],\n    [0, 1, 1],\n    [0, 1, 0]\n  ],\n  [\n    [0, 0, 0],\n    [1, 1, 1],\n    [0, 1, 0]\n  ],\n  [\n    [0, 1, 0],\n    [1, 1, 0],\n    [0, 1, 0]\n  ]\n];\n\nconst PIECE_Z = [\n  [\n    [1, 1, 0],\n    [0, 1, 1],\n    [0, 0, 0]\n  ],\n  [\n    [0, 0, 1],\n    [0, 1, 1],\n    [0, 1, 0]\n  ],\n  [\n    [0, 0, 0],\n    [1, 1, 0],\n    [0, 1, 1]\n  ],\n  [\n    [0, 1, 0],\n    [1, 1, 0],\n    [1, 0, 0]\n  ]\n];\n\n\n// The piece list, with colors\nconst PIECES = [\n    [PIECE_Z,\"red\"],\n    [PIECE_S,\"blue\"],\n    [PIECE_T,\"white\"],\n    [PIECE_O,\"white\"],\n    [PIECE_L,\"red\"],\n    [PIECE_I,\"white\"],\n    [PIECE_J,\"blue\"]\n];\n\n\n//# sourceURL=webpack:///./tetrominoes.js?");

/***/ })

/******/ });