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
/******/ 	return __webpack_require__(__webpack_require__.s = "./src/tetris.js");
/******/ })
/************************************************************************/
/******/ ({

/***/ "./src/canvas.js":
/*!***********************!*\
  !*** ./src/canvas.js ***!
  \***********************/
/*! exports provided: Canvas */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"Canvas\", function() { return Canvas; });\n/* harmony import */ var _tetris_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./tetris.js */ \"./src/tetris.js\");\nconst cvs = document.getElementById(\"tetris\");\nconst ctx = cvs.getContext(\"2d\");\n\n\n\nfunction Canvas(board) {\n  this.board = board;\n}\n\n// draw a square\nCanvas.prototype.drawSquare = function(x, y, color) {\n  ctx.fillStyle = color;\n  ctx.fillRect(x * _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], y * _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"]);\n\n  ctx.strokeStyle = \"BLACK\";\n  ctx.strokeRect(x * _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], y * _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"]);\n};\n\n// draw the board\nCanvas.prototype.drawBoard = function() {\n  for (let r = 0; r < _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"ROW\"]; r++) {\n    for (let c = 0; c < _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"COLUMN\"]; c++) {\n      this.drawSquare(c, r, this.board[r][c]);\n    }\n  }\n};\n\n\n//# sourceURL=webpack:///./src/canvas.js?");

/***/ }),

/***/ "./src/piece.js":
/*!**********************!*\
  !*** ./src/piece.js ***!
  \**********************/
/*! exports provided: Piece */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"Piece\", function() { return Piece; });\n/* harmony import */ var _tetris_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./tetris.js */ \"./src/tetris.js\");\n\n\n// The Object Piece\nfunction Piece(tetromino, color, board, canvas, onGameOver) {\n  this.tetromino = tetromino;\n  this.color = color;\n  this.board = board;\n  this.canvas = canvas;\n  this.onGameOver = onGameOver;\n\n  this.tetrominoN = 0; // we start from the first pattern\n  this.activeTetromino = this.tetromino[this.tetrominoN];\n\n  // we need to control the pieces\n  this.x = 3;\n  this.y = -2;\n}\n\n// fill function\nPiece.prototype.fill = function(color) {\n  for (let r = 0; r < this.activeTetromino.length; r++) {\n    for (let c = 0; c < this.activeTetromino.length; c++) {\n      // we draw only occupied squares\n      if (this.activeTetromino[r][c]) {\n        this.canvas.drawSquare(this.x + c, this.y + r, color);\n      }\n    }\n  }\n};\n\n// draw a piece to the board\nPiece.prototype.draw = function() {\n  this.fill(this.color);\n};\n\n// undraw a piece\nPiece.prototype.unDraw = function() {\n  this.fill(_tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"VACANT\"]);\n};\n\nPiece.prototype.shouldLock = function() {\n  return this.collision(0, 1, this.activeTetromino);\n};\n\n// move Down the piece\nPiece.prototype.moveDown = function() {\n  this.unDraw();\n  this.y++;\n  this.draw();\n};\n\n// move Right the piece\nPiece.prototype.moveRight = function() {\n  if (!this.collision(1, 0, this.activeTetromino)) {\n    this.unDraw();\n    this.x++;\n    this.draw();\n  }\n};\n\n// move Left the piece\nPiece.prototype.moveLeft = function() {\n  if (!this.collision(-1, 0, this.activeTetromino)) {\n    this.unDraw();\n    this.x--;\n    this.draw();\n  }\n};\n\n// rotate the piece\nPiece.prototype.rotate = function() {\n  let nextPattern = this.tetromino[\n    (this.tetrominoN + 1) % this.tetromino.length\n  ];\n  let kick = 0;\n\n  if (this.collision(0, 0, nextPattern)) {\n    if (this.x > _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"COLUMN\"] / 2) {\n      // it's the right wall\n      kick = -1; // we need to move the piece to the left\n    } else {\n      // it's the left wall\n      kick = 1; // we need to move the piece to the right\n    }\n  }\n\n  if (!this.collision(kick, 0, nextPattern)) {\n    this.unDraw();\n    this.x += kick;\n    this.tetrominoN = (this.tetrominoN + 1) % this.tetromino.length; // (0+1)%4 => 1\n    this.activeTetromino = this.tetromino[this.tetrominoN];\n    this.draw();\n  }\n};\n\nPiece.prototype.lock = function() {\n  for (let r = 0; r < this.activeTetromino.length; r++) {\n    for (let c = 0; c < this.activeTetromino.length; c++) {\n      // we skip the vacant squares\n      if (!this.activeTetromino[r][c]) {\n        continue;\n      }\n      // pieces to lock on top = game over\n      if (this.y + r < 0) {\n        this.onGameOver();\n        break;\n      }\n      // we lock the piece\n      this.board[this.y + r][this.x + c] = this.color;\n    }\n  }\n\n  // update the board\n  this.canvas.drawBoard();\n};\n\n// Collision fucntion\nPiece.prototype.collision = function(x, y, piece) {\n  for (let r = 0; r < piece.length; r++) {\n    for (let c = 0; c < piece.length; c++) {\n      // if the square is empty, we skip it\n      if (!piece[r][c]) {\n        continue;\n      }\n      // coordinates of the piece after movement\n      let newX = this.x + c + x;\n      let newY = this.y + r + y;\n\n      // conditions\n      if (newX < 0 || newX >= _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"COLUMN\"] || newY >= _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"ROW\"]) {\n        return true;\n      }\n      // skip newY < 0; board[-1] will crush our game\n      if (newY < 0) {\n        continue;\n      }\n      // check if there is a locked piece alrady in place\n      if (this.board[newY][newX] != _tetris_js__WEBPACK_IMPORTED_MODULE_0__[\"VACANT\"]) {\n        return true;\n      }\n    }\n  }\n  return false;\n};\n\n\n//# sourceURL=webpack:///./src/piece.js?");

/***/ }),

/***/ "./src/tetris.js":
/*!***********************!*\
  !*** ./src/tetris.js ***!
  \***********************/
/*! exports provided: ROW, COLUMN, SQUARE_SIZE, VACANT */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"ROW\", function() { return ROW; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"COLUMN\", function() { return COLUMN; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"SQUARE_SIZE\", function() { return SQUARE_SIZE; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"VACANT\", function() { return VACANT; });\n/* harmony import */ var _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./tetrominoes.js */ \"./src/tetrominoes.js\");\n/* harmony import */ var _piece_js__WEBPACK_IMPORTED_MODULE_1__ = __webpack_require__(/*! ./piece.js */ \"./src/piece.js\");\n/* harmony import */ var _canvas_js__WEBPACK_IMPORTED_MODULE_2__ = __webpack_require__(/*! ./canvas.js */ \"./src/canvas.js\");\nconst scoreElement = document.getElementById(\"score\");\r\n\r\n\r\n\r\n\r\n\r\nconst ROW = 20;\r\nconst COLUMN = 10;\r\nconst SQUARE_SIZE = 20;\r\nconst VACANT = \"BLACK\"; // color of an empty square\r\n\r\nconst DROP_TIME_MS = 200;\r\nconst rewards = {\r\n  1: 40,\r\n  2: 100,\r\n  3: 300,\r\n  4: 1200\r\n};\r\n\r\nlet m_board = [];\r\nfor (let r = 0; r < ROW; r++) {\r\n  m_board[r] = [];\r\n  for (let c = 0; c < COLUMN; c++) {\r\n    m_board[r][c] = VACANT;\r\n  }\r\n}\r\n\r\nlet m_canvas = new _canvas_js__WEBPACK_IMPORTED_MODULE_2__[\"Canvas\"](m_board);\r\nm_canvas.drawBoard();\r\n\r\nlet m_level = 0;\r\nlet m_gameOver = false;\r\nlet m_currentPiece = randomPiece();\r\nlet m_score = 0;\r\n\r\nfunction onGameOver(argument) {\r\n  m_gameOver = true;\r\n  alert(\"Game over!\");\r\n}\r\n\r\nfunction randomPiece() {\r\n  let r = Math.floor(Math.random() * _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECES\"].length); // 0 -> 6\r\n  return new _piece_js__WEBPACK_IMPORTED_MODULE_1__[\"Piece\"](_tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECES\"][r][0], _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECES\"][r][1], m_board, m_canvas, onGameOver);\r\n}\r\n\r\nfunction incrementScore(numRowsCleared) {\r\n  return rewards[numRowsCleared];\r\n}\r\n\r\nfunction removeFullRows() {\r\n  let numRowsCleared = 0;\r\n  for (let r = 0; r < ROW; r++) {\r\n    let isRowFull = true;\r\n    for (let c = 0; c < COLUMN; c++) {\r\n      if (m_board[r][c] == VACANT) {\r\n        isRowFull = false;\r\n        break;\r\n      }\r\n    }\r\n    if (isRowFull) {\r\n      numRowsCleared += 1;\r\n      // Move down all the rows above it\r\n      for (let y = r; y > 1; y--) {\r\n        for (let c = 0; c < COLUMN; c++) {\r\n          m_board[y][c] = m_board[y - 1][c];\r\n        }\r\n      }\r\n      // Clear out the very top row\r\n      for (let c = 0; c < COLUMN; c++) {\r\n        m_board[0][c] = VACANT;\r\n      }\r\n    }\r\n  }\r\n  if (numRowsCleared > 0) {\r\n    // Update the board\r\n    m_canvas.drawBoard();\r\n\r\n    // Update the score\r\n    incrementScore(numRowsCleared);\r\n    scoreElement.innerHTML = m_score;\r\n  }\r\n}\r\n\r\nfunction moveCurrentPieceDown() {\r\n  if (m_currentPiece.shouldLock()) {\r\n    m_currentPiece.lock();\r\n    removeFullRows();\r\n    m_currentPiece = randomPiece();\r\n  } else {\r\n    m_currentPiece.moveDown();\r\n  }\r\n}\r\n\r\n// Control the piece\r\ndocument.addEventListener(\"keydown\", keyDownListener);\r\ndocument.addEventListener(\"keyup\", keyUpListener);\r\nfunction keyDownListener(event) {\r\n  if (event.keyCode == 37) {\r\n    m_currentPiece.moveLeft();\r\n  } else if (event.keyCode == 38) {\r\n    m_currentPiece.rotate();\r\n  } else if (event.keyCode == 39) {\r\n    m_currentPiece.moveRight();\r\n  } else if (event.keyCode == 40) {\r\n    moveCurrentPieceDown();\r\n  }\r\n}\r\n\r\nfunction keyUpListener(event) {}\r\n\r\nlet framecount = 0;\r\nfunction gameLoop() {\r\n  framecount += 1;\r\n  if (framecount >= 10) {\r\n    moveCurrentPieceDown();\r\n    framecount = 0;\r\n  }\r\n  if (!m_gameOver) {\r\n    requestAnimationFrame(gameLoop);\r\n  }\r\n}\r\ngameLoop();\r\n\n\n//# sourceURL=webpack:///./src/tetris.js?");

/***/ }),

/***/ "./src/tetrominoes.js":
/*!****************************!*\
  !*** ./src/tetrominoes.js ***!
  \****************************/
/*! exports provided: PIECES */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"PIECES\", function() { return PIECES; });\nconst PIECE_I = [\n  [[0, 0, 0, 0], [1, 1, 1, 1], [0, 0, 0, 0], [0, 0, 0, 0]],\n  [[0, 0, 1, 0], [0, 0, 1, 0], [0, 0, 1, 0], [0, 0, 1, 0]],\n  [[0, 0, 0, 0], [0, 0, 0, 0], [1, 1, 1, 1], [0, 0, 0, 0]],\n  [[0, 1, 0, 0], [0, 1, 0, 0], [0, 1, 0, 0], [0, 1, 0, 0]]\n];\n\nconst PIECE_J = [\n  [[1, 0, 0], [1, 1, 1], [0, 0, 0]],\n  [[0, 1, 1], [0, 1, 0], [0, 1, 0]],\n  [[0, 0, 0], [1, 1, 1], [0, 0, 1]],\n  [[0, 1, 0], [0, 1, 0], [1, 1, 0]]\n];\n\nconst PIECE_L = [\n  [[0, 0, 1], [1, 1, 1], [0, 0, 0]],\n  [[0, 1, 0], [0, 1, 0], [0, 1, 1]],\n  [[0, 0, 0], [1, 1, 1], [1, 0, 0]],\n  [[1, 1, 0], [0, 1, 0], [0, 1, 0]]\n];\n\nconst PIECE_O = [[[0, 0, 0, 0], [0, 1, 1, 0], [0, 1, 1, 0], [0, 0, 0, 0]]];\n\nconst PIECE_S = [\n  [[0, 1, 1], [1, 1, 0], [0, 0, 0]],\n  [[0, 1, 0], [0, 1, 1], [0, 0, 1]],\n  [[0, 0, 0], [0, 1, 1], [1, 1, 0]],\n  [[1, 0, 0], [1, 1, 0], [0, 1, 0]]\n];\n\nconst PIECE_T = [\n  [[0, 1, 0], [1, 1, 1], [0, 0, 0]],\n  [[0, 1, 0], [0, 1, 1], [0, 1, 0]],\n  [[0, 0, 0], [1, 1, 1], [0, 1, 0]],\n  [[0, 1, 0], [1, 1, 0], [0, 1, 0]]\n];\n\nconst PIECE_Z = [\n  [[1, 1, 0], [0, 1, 1], [0, 0, 0]],\n  [[0, 0, 1], [0, 1, 1], [0, 1, 0]],\n  [[0, 0, 0], [1, 1, 0], [0, 1, 1]],\n  [[0, 1, 0], [1, 1, 0], [1, 0, 0]]\n];\n\n// The piece list, with colors\nconst PIECES = [\n  [PIECE_Z, \"red\"],\n  [PIECE_S, \"blue\"],\n  [PIECE_T, \"white\"],\n  [PIECE_O, \"white\"],\n  [PIECE_L, \"red\"],\n  [PIECE_I, \"white\"],\n  [PIECE_J, \"blue\"]\n];\n\n\n//# sourceURL=webpack:///./src/tetrominoes.js?");

/***/ })

/******/ });