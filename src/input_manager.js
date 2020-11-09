import { Direction, GameState } from "./constants.js";
const GameSettings = require("./game_settings_manager");

const debugTextElement = document.getElementById("debug");

// Default control setup
let LEFT_KEYCODE = 37;
let RIGHT_KEYCODE = 39;
let ROTATE_LEFT_KEYCODE = 90;
let ROTATE_RIGHT_KEYCODE = 88;
let DOWN_KEYCODE = 40;

export function InputManager(
  moveDownFunc,
  moveLeftFunc,
  moveRightFunc,
  rotateLeftFunc,
  rotateRightFunc,
  togglePauseFunc,
  getGameStateFunc,
  getAREFunc
) {
  this.resetLocalVariables();

  this.debugFrameCount = 0;
  this.togglePauseFunc = togglePauseFunc;
  this.moveDownFunc = moveDownFunc;
  this.moveLeftFunc = moveLeftFunc;
  this.moveRightFunc = moveRightFunc;
  this.rotateLeftFunc = rotateLeftFunc;
  this.rotateRightFunc = rotateRightFunc;
  this.getGameStateFunc = getGameStateFunc;
  this.getAREFunc = getAREFunc;
}

/* ---------------------
    Called by parent
---------------------- */

InputManager.prototype.getIsSoftDropping = function () {
  return this.isSoftDropping;
};

InputManager.prototype.onPieceLock = function () {
  if (GameSettings.IsDASAlwaysCharged()) {
    this.setDASCharge(GameSettings.GetDASTriggerThreshold());
  }
};

InputManager.prototype.resetLocalVariables = function () {
  this.leftHeld = false;
  this.rightHeld = false;
  this.downHeld = false;
  this.isSoftDropping = false;
  this.dasCharge = GameSettings.GetDASTriggerThreshold(); // Starts charged on the first piece
  this.softDroppedLastFrame = false;
};

InputManager.prototype.handleInputsThisFrame = function () {
  this.debugFrameCount += 1;
  console.log(this.debugFrameCount, this.dasCharge);

  // If holding multiple keys, do nothing
  const dpadDirectionsHeld = this.downHeld + this.leftHeld + this.rightHeld;
  if (dpadDirectionsHeld > 1) {
    this.isSoftDropping = false;
    return;
  }

  // Move piece down
  if (this.isSoftDropping && !this.softDroppedLastFrame) {
    console.log("Soft dropping");
    const didMove = this.moveDownFunc();
    if (!didMove) {
      // If it didn't move, then it locked in. Reset soft drop between pieces.
      this.isSoftDropping = false;
    }
    this.softDroppedLastFrame = true;
    return;
  } else {
    this.softDroppedLastFrame = false;
  }

  // DAS left
  if (this.leftHeld) {
    this.handleHeldDirection(Direction.LEFT);
    return;
  }

  // DAS right
  if (this.rightHeld) {
    this.handleHeldDirection(Direction.RIGHT);
  }
};

/* ---------------------
    Key listeners 
---------------------- */

InputManager.prototype.keyDownListener = function (event) {
  // Override the browser's built-in key repeating
  if (event.repeat) {
    return;
  }

  // Track whether keys are held regardless of state
  switch (event.keyCode) {
    case LEFT_KEYCODE:
      this.leftHeld = true;
      break;
    case RIGHT_KEYCODE:
      this.rightHeld = true;
      break;
    case DOWN_KEYCODE:
      this.downHeld = true;
      break;
  }

  // Only actually move the pieces if in the proper game state
  if (shouldPerformPieceMovements(this.getGameStateFunc())) {
    switch (event.keyCode) {
      case LEFT_KEYCODE:
        this.handleTappedDirection(Direction.LEFT);
        break;
      case RIGHT_KEYCODE:
        this.handleTappedDirection(Direction.RIGHT);
        break;
      case ROTATE_LEFT_KEYCODE:
        this.rotateLeftFunc();
        break;
      case ROTATE_RIGHT_KEYCODE:
        this.rotateRightFunc();
        break;
      case DOWN_KEYCODE:
        this.isSoftDropping = true;
        break;
    }
  }

  // Client controls
  if (event.keyCode == 80) {
    // Letter 'P' pauses and unpauses
    this.togglePauseFunc();
  }
};

InputManager.prototype.keyUpListener = function (event) {
  // Track whether keys are held regardless of state
  if (event.keyCode == LEFT_KEYCODE) {
    this.leftHeld = false;
  } else if (event.keyCode == RIGHT_KEYCODE) {
    this.rightHeld = false;
  } else if (event.keyCode == DOWN_KEYCODE) {
    this.downHeld = false;
    this.isSoftDropping = false; // Can stop soft dropping in any state
  }
};

/* ---------------------
    Private helpers
---------------------- */

InputManager.prototype.tryShiftPiece = function (direction) {
  // Try to move the piece and store whether it actually did or not
  const didMove =
    direction == Direction.LEFT ? this.moveLeftFunc() : this.moveRightFunc();
  // Wall charge if it didn't move
  if (didMove) {
    console.log(this.debugFrameCount, "Shift");
  }

  if (!didMove) {
    console.log("wallcharge");
    this.setDASCharge(GameSettings.GetDASTriggerThreshold());
  }
  return didMove;
};

InputManager.prototype.handleHeldDirection = function (direction) {
  const DASTriggerThreshold = GameSettings.GetDASTriggerThreshold();
  // Increment DAS
  this.setDASCharge(Math.min(DASTriggerThreshold, this.dasCharge + 1));

  // Attempt to shift the piece once it hits the trigger
  if (this.dasCharge == DASTriggerThreshold) {
    const didMove = this.tryShiftPiece(direction);
    if (didMove) {
      // Move DAS to charged floor for another cycle of ARR
      this.setDASCharge(GameSettings.GetDASChargedFloor());
    }
  }
};

// Handle single taps of the dpad, if in the proper state
InputManager.prototype.handleTappedDirection = function (direction) {
  if (shouldPerformPieceMovements(this.getGameStateFunc())) {
    // DAS loses charges on tap
    this.setDASCharge(GameSettings.GetDASUnchargedFloor());
    this.tryShiftPiece(direction);
  }
};

// Updates the DAS charge and refreshes the debug text
InputManager.prototype.setDASCharge = function (value) {
  this.dasCharge = value;
  this.refreshDebugText();
};

InputManager.prototype.refreshDebugText = function () {
  let debugStr = "";
  let dasVisualized = "";
  for (let i = 0; i < this.dasCharge; i++) {
    dasVisualized += "x";
  }
  // Have something on the second line so it's always the same height
  if (this.dasCharge == 0) {
    dasVisualized = ".";
  }
  debugStr += "DAS: " + this.dasCharge + "\n" + dasVisualized;
  debugTextElement.innerText = debugStr;
};

// Checks if the game state allows for piece movements
function shouldPerformPieceMovements(gameState) {
  return gameState == GameState.RUNNING || gameState == GameState.FIRST_PIECE;
}
