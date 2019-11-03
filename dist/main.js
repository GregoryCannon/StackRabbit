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
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"Canvas\", function() { return Canvas; });\n/* harmony import */ var _constants_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./constants.js */ \"./src/constants.js\");\nconst cvs = document.getElementById(\"main-canvas\");\nconst ctx = cvs.getContext(\"2d\");\n\n\n\nfunction Canvas(board) {\n  this.board = board;\n}\n\n// draw a square\nCanvas.prototype.drawSquare = function(x, y, color) {\n  ctx.fillStyle = color;\n  ctx.fillRect(x * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], y * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"]);\n\n  ctx.strokeStyle = \"BLACK\";\n  ctx.strokeRect(x * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], y * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"], _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"]);\n};\n\n// draw the next box\nCanvas.prototype.drawNextBox = function(nextPiece) {\n  // All in units of SQUARE_SIZE\n  const startX = _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"COLUMN\"] + 1;\n  const startY = 2;\n  const width = 5;\n  const height = 4.5;\n  const pieceStartX = startX + 0.5;\n  const pieceStartY = startY + 0.5;\n\n  // background\n  ctx.fillStyle = \"BLACK\";\n  ctx.fillRect(\n    startX * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"],\n    startY * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"],\n    width * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"],\n    height * _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"SQUARE_SIZE\"]\n  );\n\n  // draw the piece\n  for (let r = 0; r < nextPiece.activeTetromino.length; r++) {\n    for (let c = 0; c < nextPiece.activeTetromino.length; c++) {\n      // Draw only occupied squares\n      if (nextPiece.activeTetromino[r][c]) {\n        this.drawSquare(pieceStartX + c, pieceStartY + r, nextPiece.color);\n      }\n    }\n  }\n};\n\n// draw the board\nCanvas.prototype.drawBoard = function() {\n  for (let r = 0; r < _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"ROW\"]; r++) {\n    for (let c = 0; c < _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"COLUMN\"]; c++) {\n      this.drawSquare(c, r, this.board[r][c]);\n    }\n  }\n};\n\n\n//# sourceURL=webpack:///./src/canvas.js?");

/***/ }),

/***/ "./src/constants.js":
/*!**************************!*\
  !*** ./src/constants.js ***!
  \**************************/
/*! exports provided: ROW, COLUMN, SQUARE_SIZE, VACANT, REWARDS, GRAVITY, GameState, Direction, DAS_TRIGGER, DAS_CHARGED, DAS_DOWN_CHARGED */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"ROW\", function() { return ROW; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"COLUMN\", function() { return COLUMN; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"SQUARE_SIZE\", function() { return SQUARE_SIZE; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"VACANT\", function() { return VACANT; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"REWARDS\", function() { return REWARDS; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"GRAVITY\", function() { return GRAVITY; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"GameState\", function() { return GameState; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"Direction\", function() { return Direction; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"DAS_TRIGGER\", function() { return DAS_TRIGGER; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"DAS_CHARGED\", function() { return DAS_CHARGED; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"DAS_DOWN_CHARGED\", function() { return DAS_DOWN_CHARGED; });\nconst ROW = 20;\nconst COLUMN = 10;\nconst SQUARE_SIZE = 20;\nconst VACANT = \"BLACK\"; // color of an empty square\n\n// How many points for X lines at a time (before scaling by level)\nconst REWARDS = {\n  1: 40,\n  2: 100,\n  3: 300,\n  4: 1200\n};\n// How many frames it takes to drop one square\nconst GRAVITY = {\n  0: 48,\n  1: 43,\n  2: 38,\n  3: 33,\n  4: 28,\n  5: 23,\n  6: 18,\n  7: 13,\n  8: 8,\n  9: 6,\n  10: 5,\n  11: 5,\n  12: 5,\n  13: 4,\n  14: 4,\n  15: 4,\n  16: 3,\n  17: 3,\n  18: 3,\n  19: 2\n};\n\nconst GameState = {\n  RUNNING: \"running\",\n  PAUSED: \"paused\",\n  GAME_OVER: \"game over\",\n  START_SCREEN: \"start screen\"\n};\n\nconst Direction = {\n  LEFT: \"left\",\n  RIGHT: \"right\",\n  DOWN: \"down\",\n  NONE: \"none\"\n};\nconst DAS_TRIGGER = 16;\nconst DAS_CHARGED = 10;\nconst DAS_DOWN_CHARGED = 14;\n\n\n//# sourceURL=webpack:///./src/constants.js?");

/***/ }),

/***/ "./src/piece.js":
/*!**********************!*\
  !*** ./src/piece.js ***!
  \**********************/
/*! exports provided: Piece */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"Piece\", function() { return Piece; });\n/* harmony import */ var _constants_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./constants.js */ \"./src/constants.js\");\n\n\n// The Object Piece\nfunction Piece(tetromino, color, id, board, canvas, onGameOver) {\n  this.tetromino = tetromino;\n  this.color = color;\n  this.board = board;\n  this.canvas = canvas;\n  this.onGameOver = onGameOver;\n  this.id = id;\n\n  this.tetrominoN = 0; // Start from the first rotation\n  this.activeTetromino = this.tetromino[this.tetrominoN];\n\n  this.x = 3;\n  this.y = -2;\n}\n\nPiece.prototype.equals = function(otherPiece) {\n  return this.id === otherPiece.id;\n};\n\n// fill function\nPiece.prototype.fill = function(color) {\n  for (let r = 0; r < this.activeTetromino.length; r++) {\n    for (let c = 0; c < this.activeTetromino.length; c++) {\n      // Draw only occupied squares\n      if (this.activeTetromino[r][c]) {\n        this.canvas.drawSquare(this.x + c, this.y + r, color);\n      }\n    }\n  }\n};\n\n// draw a piece to the board\nPiece.prototype.draw = function() {\n  this.fill(this.color);\n};\n\n// undraw a piece\nPiece.prototype.unDraw = function() {\n  this.fill(_constants_js__WEBPACK_IMPORTED_MODULE_0__[\"VACANT\"]);\n};\n\nPiece.prototype.shouldLock = function() {\n  return this.collision(0, 1, this.activeTetromino);\n};\n\n// move Down the piece\nPiece.prototype.moveDown = function() {\n  this.unDraw();\n  this.y++;\n  this.draw();\n};\n\n// move Right the piece\nPiece.prototype.moveRight = function() {\n  if (!this.collision(1, 0, this.activeTetromino)) {\n    this.unDraw();\n    this.x++;\n    this.draw();\n  }\n};\n\n// move Left the piece\nPiece.prototype.moveLeft = function() {\n  if (!this.collision(-1, 0, this.activeTetromino)) {\n    this.unDraw();\n    this.x--;\n    this.draw();\n  }\n};\n\n// rotate the piece\nPiece.prototype.rotate = function(directionInversed) {\n  const offset = directionInversed ? -1 : 1;\n  const nextIndex =\n    (this.tetrominoN + offset + this.tetromino.length) % this.tetromino.length;\n  const nextPattern = this.tetromino[nextIndex];\n\n  let kick = 0;\n\n  // if (this.collision(0, 0, nextPattern)) {\n  //   if (this.x > COLUMN / 2) {\n  //     // it's the right wall\n  //     kick = -1; // we need to move the piece to the left\n  //   } else {\n  //     // it's the left wall\n  //     kick = 1; // we need to move the piece to the right\n  //   }\n  // }\n\n  if (!this.collision(kick, 0, nextPattern)) {\n    this.unDraw();\n    this.x += kick;\n    this.tetrominoN = nextIndex;\n    this.activeTetromino = this.tetromino[this.tetrominoN];\n    this.draw();\n  }\n};\n\n// Lock the piece in place\nPiece.prototype.lock = function() {\n  for (let r = 0; r < this.activeTetromino.length; r++) {\n    for (let c = 0; c < this.activeTetromino.length; c++) {\n      // we skip the vacant squares\n      if (!this.activeTetromino[r][c]) {\n        continue;\n      }\n      // pieces to lock on top = game over\n      if (this.y + r < 0) {\n        this.onGameOver();\n        break;\n      }\n      // we lock the piece\n      this.board[this.y + r][this.x + c] = this.color;\n    }\n  }\n\n  // update the board\n  this.canvas.drawBoard();\n};\n\n// Collision fucntion\nPiece.prototype.collision = function(x, y, piece) {\n  for (let r = 0; r < piece.length; r++) {\n    for (let c = 0; c < piece.length; c++) {\n      // if the square is empty, we skip it\n      if (!piece[r][c]) {\n        continue;\n      }\n      // coordinates of the piece after movement\n      let newX = this.x + c + x;\n      let newY = this.y + r + y;\n\n      // conditions\n      if (newX < 0 || newX >= _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"COLUMN\"] || newY >= _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"ROW\"]) {\n        return true;\n      }\n      // skip newY < 0; board[-1] will crush our game\n      if (newY < 0) {\n        continue;\n      }\n      // check if there is a locked piece alrady in place\n      if (this.board[newY][newX] != _constants_js__WEBPACK_IMPORTED_MODULE_0__[\"VACANT\"]) {\n        return true;\n      }\n    }\n  }\n  return false;\n};\n\n\n//# sourceURL=webpack:///./src/piece.js?");

/***/ }),

/***/ "./src/piece_selector.js":
/*!*******************************!*\
  !*** ./src/piece_selector.js ***!
  \*******************************/
/*! exports provided: PieceSelector */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"PieceSelector\", function() { return PieceSelector; });\n/* harmony import */ var _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./tetrominoes.js */ \"./src/tetrominoes.js\");\n/* harmony import */ var _piece_js__WEBPACK_IMPORTED_MODULE_1__ = __webpack_require__(/*! ./piece.js */ \"./src/piece.js\");\nconst pieceListElement = document.getElementById(\"piece-sequence\");\nconst pasteAreaElement = document.getElementById(\"paste-area\");\n\n\n\n\n// Read piece string from input box\nlet m_pieceString = \"\";\nlet m_readIndex = -1;\npieceListElement.addEventListener(\"input\", function(event) {\n  m_pieceString = this.value;\n  m_readIndex = 0;\n});\n\npasteAreaElement.onpaste = function(event) {\n  // use event.originalEvent.clipboard for newer chrome versions\n  var items = (event.clipboardData || event.originalEvent.clipboardData).items;\n  console.log(JSON.stringify(items)); // will give you the mime types\n  // find pasted image among pasted items\n  var blob = null;\n  for (var i = 0; i < items.length; i++) {\n    if (items[i].type.indexOf(\"image\") === 0) {\n      blob = items[i].getAsFile();\n    }\n  }\n  // load image if there is a pasted image\n  if (blob !== null) {\n    var reader = new FileReader();\n    reader.onload = function(event) {\n      console.log(event.target.result); // data url!\n      document.getElementById(\"pastedImage\").src = event.target.result;\n    };\n    reader.readAsDataURL(blob);\n  }\n};\n\nfunction PieceSelector(board, canvas, onGameOver) {\n  this.board = board;\n  this.canvas = canvas;\n  this.onGameOver = onGameOver;\n}\n\nPieceSelector.prototype.presetPiece = function() {\n  const nextPieceId = m_pieceString[m_readIndex];\n  const nextPieceData = _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LOOKUP\"][nextPieceId];\n  m_readIndex += 1;\n  return new _piece_js__WEBPACK_IMPORTED_MODULE_1__[\"Piece\"](\n    nextPieceData[0],\n    nextPieceData[1],\n    nextPieceData[2],\n    this.board,\n    this.canvas,\n    this.onGameOver\n  );\n};\n\n// Get a random piece, following the original RNG of NES tetris\nPieceSelector.prototype.randomPiece = function(previousPieceId) {\n  // Roll once 0-7, where 7 is a dummy value\n  let r = Math.floor(Math.random() * (_tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LIST\"].length + 1));\n  if (r == _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LIST\"].length || previousPieceId === _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LIST\"][r][2]) {\n    // Reroll once for repeats (or dummy) to reduce repeated pieces\n    r = Math.floor(Math.random() * _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LIST\"].length);\n  }\n  return new _piece_js__WEBPACK_IMPORTED_MODULE_1__[\"Piece\"](\n    _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LIST\"][r][0], // tetromino\n    _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LIST\"][r][1], // color\n    _tetrominoes_js__WEBPACK_IMPORTED_MODULE_0__[\"PIECE_LIST\"][r][2], // string ID\n    this.board, // reference to the board\n    this.canvas, // reference to the canvas\n    this.onGameOver // callback function for game over\n  );\n};\n\n// Get the next piece, whether that be specified or random\nPieceSelector.prototype.chooseNextPiece = function(currentPieceId) {\n  // If there is a next specified piece, select that\n  if (m_readIndex != -1 && m_readIndex < m_pieceString.length) {\n    return this.presetPiece();\n  }\n  // Otherwise pick one randomly\n  return this.randomPiece(currentPieceId);\n};\n\n\n//# sourceURL=webpack:///./src/piece_selector.js?");

/***/ }),

/***/ "./src/tetris.js":
/*!***********************!*\
  !*** ./src/tetris.js ***!
  \***********************/
/*! no exports provided */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony import */ var _piece_selector_js__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(/*! ./piece_selector.js */ \"./src/piece_selector.js\");\n/* harmony import */ var _canvas_js__WEBPACK_IMPORTED_MODULE_1__ = __webpack_require__(/*! ./canvas.js */ \"./src/canvas.js\");\n/* harmony import */ var _constants_js__WEBPACK_IMPORTED_MODULE_2__ = __webpack_require__(/*! ./constants.js */ \"./src/constants.js\");\n\r\n\r\n\r\n\r\nconst scoreTextElement = document.getElementById(\"score\");\r\nconst headerTextElement = document.getElementById(\"header-text\");\r\nconst debugTextElement = document.getElementById(\"debug\");\r\nconst startGameButton = document.getElementById(\"start-game\");\r\nconst levelSelectElement = document.getElementById(\"level-select\");\r\n\r\n// Initial empty board\r\nlet m_board = [];\r\nfor (let r = 0; r < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"ROW\"]; r++) {\r\n  m_board[r] = [];\r\n  for (let c = 0; c < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"COLUMN\"]; c++) {\r\n    m_board[r][c] = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"VACANT\"];\r\n  }\r\n}\r\nlet m_canvas = new _canvas_js__WEBPACK_IMPORTED_MODULE_1__[\"Canvas\"](m_board);\r\nlet m_pieceSelector = new _piece_selector_js__WEBPACK_IMPORTED_MODULE_0__[\"PieceSelector\"](m_board, m_canvas, onGameOver);\r\nlet m_currentPiece;\r\nlet m_nextPiece;\r\n\r\nlet m_level;\r\nlet m_gameState;\r\nlet m_score;\r\nlet m_framecount;\r\n\r\n// Controls\r\nlet m_left_held;\r\nlet m_right_held;\r\nlet m_down_held;\r\nlet m_DAS_count;\r\n\r\n// Default control setup\r\nlet LEFT_KEYCODE = 37;\r\nlet RIGHT_KEYCODE = 39;\r\nlet ROTATE_LEFT_KEYCODE = 90;\r\nlet ROTATE_RIGHT_KEYCODE = 88;\r\nlet DOWN_KEYCODE = 40;\r\n\r\n// My personal control setup\r\nif (true) {\r\n  LEFT_KEYCODE = 90;\r\n  RIGHT_KEYCODE = 88;\r\n  ROTATE_LEFT_KEYCODE = 86;\r\n  ROTATE_RIGHT_KEYCODE = 66;\r\n  DOWN_KEYCODE = 18;\r\n}\r\n\r\nfunction refreshHeaderText() {\r\n  let newText = \"\";\r\n  switch (m_gameState) {\r\n    case _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].START_SCREEN:\r\n      newText = \"Welcome to Tetris Trainer!\";\r\n      break;\r\n    case _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].RUNNING:\r\n      newText = \"\";\r\n      break;\r\n    case _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].GAME_OVER:\r\n      newText = \"Game over!\";\r\n      break;\r\n    case _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].PAUSED:\r\n      newText = \"Paused\";\r\n      break;\r\n  }\r\n  headerTextElement.innerText = newText;\r\n}\r\n\r\nfunction refreshDebugText() {\r\n  let debugStr = \"\";\r\n  debugStr += \"DAS: \" + m_DAS_count;\r\n  debugStr += \"\\nLeftKey: \" + m_left_held;\r\n  debugStr += \"\\nRightKey: \" + m_right_held;\r\n  debugStr += \"\\nDownKey: \" + m_down_held;\r\n  debugTextElement.innerText = debugStr;\r\n}\r\n\r\n// Starts the game (called from html button onClick)\r\nfunction startGame() {\r\n  reset();\r\n  const levelSelected = parseInt(levelSelectElement.value);\r\n  if (Number.isInteger(levelSelected)) {\r\n    m_level = levelSelected;\r\n  } else {\r\n    m_level = 0;\r\n  }\r\n  m_gameState = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].RUNNING;\r\n  m_nextPiece = m_pieceSelector.chooseNextPiece(\"\"); // Will become the first piece\r\n  getNewPiece();\r\n}\r\nstartGameButton.addEventListener(\"click\", startGame);\r\n\r\nfunction onGameOver(argument) {\r\n  m_gameState = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].GAME_OVER;\r\n  refreshHeaderText();\r\n}\r\n\r\nfunction removeFullRows() {\r\n  let numRowsCleared = 0;\r\n  for (let r = 0; r < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"ROW\"]; r++) {\r\n    let isRowFull = true;\r\n    for (let c = 0; c < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"COLUMN\"]; c++) {\r\n      if (m_board[r][c] == _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"VACANT\"]) {\r\n        isRowFull = false;\r\n        break;\r\n      }\r\n    }\r\n    if (isRowFull) {\r\n      numRowsCleared += 1;\r\n      // Move down all the rows above it\r\n      for (let y = r; y > 1; y--) {\r\n        for (let c = 0; c < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"COLUMN\"]; c++) {\r\n          m_board[y][c] = m_board[y - 1][c];\r\n        }\r\n      }\r\n      // Clear out the very top row\r\n      for (let c = 0; c < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"COLUMN\"]; c++) {\r\n        m_board[0][c] = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"VACANT\"];\r\n      }\r\n    }\r\n  }\r\n  if (numRowsCleared > 0) {\r\n    // Update the board\r\n    m_canvas.drawBoard();\r\n\r\n    // Update the score\r\n    m_score += _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"REWARDS\"][numRowsCleared] * (m_level + 1);\r\n    scoreTextElement.innerText = \"Score: \" + m_score;\r\n  }\r\n}\r\n\r\nfunction getNewPiece() {\r\n  m_currentPiece = m_nextPiece;\r\n  m_nextPiece = m_pieceSelector.chooseNextPiece(m_currentPiece.id);\r\n  m_canvas.drawNextBox(m_nextPiece);\r\n}\r\n\r\nfunction moveCurrentPieceDown() {\r\n  if (m_currentPiece.shouldLock()) {\r\n    // Lock in piece and get another piece\r\n    m_currentPiece.lock();\r\n    removeFullRows();\r\n    getNewPiece();\r\n    m_down_held = false; // make the player press down again if currently holding down\r\n  } else {\r\n    // Move down as usual\r\n    m_currentPiece.moveDown();\r\n  }\r\n}\r\n\r\nfunction resetDAS() {\r\n  m_DAS_count = 0;\r\n}\r\n\r\nfunction tickDAS(callback) {\r\n  m_DAS_count += 1;\r\n  if (m_DAS_count >= _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"DAS_TRIGGER\"]) {\r\n    m_DAS_count = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"DAS_CHARGED\"];\r\n    callback();\r\n  }\r\n}\r\n\r\nfunction updateDAS() {\r\n  const numKeysHeld = m_down_held + m_left_held + m_right_held;\r\n  // If holding none or multiple keys, do nothing\r\n  if (numKeysHeld != 1) {\r\n    m_DAS_count = 0;\r\n  }\r\n  // Move piece down\r\n  if (m_down_held) {\r\n    // No need to wait when pressing down\r\n    if (m_DAS_count == 0) {\r\n      m_DAS_count = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"DAS_DOWN_CHARGED\"];\r\n    }\r\n    m_DAS_count += 1;\r\n    if (m_DAS_count >= _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"DAS_TRIGGER\"]) {\r\n      m_DAS_count = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"DAS_DOWN_CHARGED\"];\r\n      moveCurrentPieceDown();\r\n    }\r\n  }\r\n  // DAS left\r\n  if (m_left_held) {\r\n    tickDAS(function() {\r\n      m_currentPiece.moveLeft();\r\n    });\r\n  }\r\n  // DAS right\r\n  else if (m_right_held) {\r\n    tickDAS(function() {\r\n      m_currentPiece.moveRight();\r\n    });\r\n  }\r\n}\r\n\r\nfunction keyDownListener(event) {\r\n  // Override the browser's built-in key repeating\r\n  if (event.repeat) {\r\n    return;\r\n  }\r\n  // Piece movement - on key down\r\n  // Move the piece once, and if appropriate, save that the key is held (for DAS)\r\n  if (m_gameState == _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].RUNNING) {\r\n    switch (event.keyCode) {\r\n      case LEFT_KEYCODE:\r\n        m_left_held = true;\r\n        m_currentPiece.moveLeft();\r\n        break;\r\n      case RIGHT_KEYCODE:\r\n        m_right_held = true;\r\n        m_currentPiece.moveRight();\r\n        break;\r\n      case ROTATE_LEFT_KEYCODE:\r\n        m_currentPiece.rotate(true);\r\n        break;\r\n      case ROTATE_RIGHT_KEYCODE:\r\n        m_currentPiece.rotate(false);\r\n        break;\r\n      case DOWN_KEYCODE:\r\n        m_down_held = true;\r\n        moveCurrentPieceDown();\r\n        break;\r\n    }\r\n  }\r\n\r\n  // Client controls\r\n  if (event.keyCode == 80) {\r\n    // Letter 'P' pauses and unpauses\r\n    if (m_gameState == _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].RUNNING) {\r\n      m_gameState = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].PAUSED;\r\n      refreshHeaderText();\r\n    } else if (m_gameState == _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].PAUSED) {\r\n      m_gameState = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].RUNNING;\r\n      refreshHeaderText();\r\n    }\r\n  }\r\n}\r\n\r\nfunction keyUpListener(event) {\r\n  // Piece movement - on key up\r\n  if (m_gameState == _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].RUNNING) {\r\n    if (event.keyCode == LEFT_KEYCODE) {\r\n      m_left_held = false;\r\n      m_DAS_count = 0;\r\n    } else if (event.keyCode == RIGHT_KEYCODE) {\r\n      m_right_held = false;\r\n      m_DAS_count = 0;\r\n    } else if (event.keyCode == DOWN_KEYCODE) {\r\n      m_down_held = false;\r\n      m_DAS_count = 0;\r\n    }\r\n  }\r\n}\r\ndocument.addEventListener(\"keydown\", keyDownListener);\r\ndocument.addEventListener(\"keyup\", keyUpListener);\r\n\r\nfunction reset() {\r\n  // Wipe the board\r\n  for (let r = 0; r < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"ROW\"]; r++) {\r\n    m_board[r] = [];\r\n    for (let c = 0; c < _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"COLUMN\"]; c++) {\r\n      m_board[r][c] = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"VACANT\"];\r\n    }\r\n  }\r\n  m_canvas.drawBoard();\r\n\r\n  m_score = 0;\r\n  m_framecount = 0;\r\n  m_level = 0;\r\n  m_gameState = _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].START_SCREEN;\r\n\r\n  m_left_held = false;\r\n  m_right_held = false;\r\n  m_down_held = false;\r\n  m_DAS_count = 0;\r\n\r\n  refreshHeaderText();\r\n}\r\n\r\n// 60 FPS game loop\r\nfunction gameLoop() {\r\n  updateDAS();\r\n\r\n  if (m_gameState == _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GameState\"].RUNNING) {\r\n    m_framecount += 1;\r\n    refreshDebugText();\r\n    if (m_framecount >= _constants_js__WEBPACK_IMPORTED_MODULE_2__[\"GRAVITY\"][m_level]) {\r\n      moveCurrentPieceDown();\r\n      m_framecount = 0;\r\n    }\r\n  }\r\n  requestAnimationFrame(gameLoop);\r\n}\r\n\r\nreset();\r\ngameLoop();\r\n\n\n//# sourceURL=webpack:///./src/tetris.js?");

/***/ }),

/***/ "./src/tetrominoes.js":
/*!****************************!*\
  !*** ./src/tetrominoes.js ***!
  \****************************/
/*! exports provided: PIECE_LIST, PIECE_LOOKUP */
/***/ (function(module, __webpack_exports__, __webpack_require__) {

"use strict";
eval("__webpack_require__.r(__webpack_exports__);\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"PIECE_LIST\", function() { return PIECE_LIST; });\n/* harmony export (binding) */ __webpack_require__.d(__webpack_exports__, \"PIECE_LOOKUP\", function() { return PIECE_LOOKUP; });\nconst PIECE_I = [\n  [[0, 0, 0, 0], [0, 0, 0, 0], [1, 1, 1, 1], [0, 0, 0, 0]],\n\n  [[0, 0, 1, 0], [0, 0, 1, 0], [0, 0, 1, 0], [0, 0, 1, 0]]\n];\n\nconst PIECE_J = [\n  [[0, 0, 0], [1, 1, 1], [0, 0, 1]],\n\n  [[0, 1, 0], [0, 1, 0], [1, 1, 0]],\n\n  [[1, 0, 0], [1, 1, 1], [0, 0, 0]],\n\n  [[0, 1, 1], [0, 1, 0], [0, 1, 0]]\n];\n\nconst PIECE_L = [\n  [[0, 0, 0], [1, 1, 1], [1, 0, 0]],\n\n  [[1, 1, 0], [0, 1, 0], [0, 1, 0]],\n\n  [[0, 0, 1], [1, 1, 1], [0, 0, 0]],\n\n  [[0, 1, 0], [0, 1, 0], [0, 1, 1]]\n];\n\nconst PIECE_O = [[[0, 0, 0, 0], [0, 1, 1, 0], [0, 1, 1, 0], [0, 0, 0, 0]]];\n\nconst PIECE_S = [\n  [[0, 0, 0], [0, 1, 1], [1, 1, 0]],\n\n  [[0, 1, 0], [0, 1, 1], [0, 0, 1]]\n];\n\nconst PIECE_T = [\n  [[0, 0, 0], [1, 1, 1], [0, 1, 0]],\n\n  [[0, 1, 0], [1, 1, 0], [0, 1, 0]],\n\n  [[0, 1, 0], [1, 1, 1], [0, 0, 0]],\n\n  [[0, 1, 0], [0, 1, 1], [0, 1, 0]]\n];\n\nconst PIECE_Z = [\n  [[0, 0, 0], [1, 1, 0], [0, 1, 1]],\n\n  [[0, 0, 1], [0, 1, 1], [0, 1, 0]]\n];\n\n// The piece list, with colors and letter identifiers\nconst PIECE_LIST = [\n  [PIECE_Z, \"red\", \"Z\"],\n  [PIECE_S, \"blue\", \"S\"],\n  [PIECE_T, \"white\", \"T\"],\n  [PIECE_O, \"white\", \"O\"],\n  [PIECE_L, \"red\", \"L\"],\n  [PIECE_I, \"white\", \"I\"],\n  [PIECE_J, \"blue\", \"J\"]\n];\n\nconst PIECE_LOOKUP = {\n  Z: [PIECE_Z, \"red\", \"Z\"],\n  S: [PIECE_S, \"blue\", \"S\"],\n  T: [PIECE_T, \"white\", \"T\"],\n  O: [PIECE_O, \"white\", \"O\"],\n  L: [PIECE_L, \"red\", \"L\"],\n  I: [PIECE_I, \"white\", \"I\"],\n  J: [PIECE_J, \"blue\", \"J\"]\n};\n\n\n//# sourceURL=webpack:///./src/tetrominoes.js?");

/***/ })

/******/ });