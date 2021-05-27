import { Direction, GameState } from "./constants.js";
import { GetIsPaused } from "./index.js";
const GameSettings = require("./game_settings_manager");

const dasStatsDiv = document.getElementById("das-stats");

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

InputManager.prototype.getCellsSoftDropped = function () {
  return this.cellSoftDropped;
};

InputManager.prototype.onPieceLock = function () {
  if (GameSettings.shouldSetDASChargeOnPieceStart()) {
    this.setDASCharge(GameSettings.getDASWallChargeAmount());
  } else {
    // Don't allow DAS charges higher than the wall charge amount.
    // This is used on DAS speeds with higher ARR but intentionally handicapped starting charges
    this.setDASCharge(
      Math.min(GameSettings.getDASWallChargeAmount(), this.dasCharge)
    );
  }
};

InputManager.prototype.resetLocalVariables = function () {
  this.leftHeld = false;
  this.rightHeld = false;
  this.downHeld = false;
  this.isSoftDropping = false;
  this.cellSoftDropped = 0;
  this.dasCharge = GameSettings.getDASTriggerThreshold(); // Starts charged on the first piece
  this.softDroppedLastFrame = false;
};

InputManager.prototype.handleInputsThisFrame = function () {
  // If AI is playing, do nothing
  if (GameSettings.isAIPlaying()) {
    return;
  }

  // If holding multiple keys, do nothing
  const dpadDirectionsHeld = this.downHeld + this.leftHeld + this.rightHeld;
  if (dpadDirectionsHeld > 1) {
    this.isSoftDropping = false;
    this.cellSoftDropped = 0;
    return;
  }

  // Move piece down
  if (this.isSoftDropping && !this.softDroppedLastFrame) {
    const didMove = this.moveDownFunc();
    if (didMove) {
      this.cellSoftDropped += 1;
    } else {
      // If it didn't move, then it locked in. Reset soft drop between pieces.
      this.isSoftDropping = false;
      this.cellSoftDropped = 0;
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

  // If AI is playing, do nothing
  if (GameSettings.isAIPlaying()) {
    return;
  }

  // Track whether keys are held regardless of state
  switch (event.keyCode) {
    case LEFT_KEYCODE:
      this.leftHeld = true;
      event.preventDefault();
      break;
    case RIGHT_KEYCODE:
      this.rightHeld = true;
      event.preventDefault();
      break;
    case DOWN_KEYCODE:
      this.downHeld = true;
      break;
  }

  // Only actually move the pieces if in the proper game state
  const gameState = this.getGameStateFunc();
  if (canMovePiecesSidewaysOrRotate(gameState)) {
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
    }
  } else {
    switch (event.keyCode) {
      case ROTATE_LEFT_KEYCODE:
        console.log("rotate rejected, state: ", this.getGameStateFunc());
        break;
      case ROTATE_RIGHT_KEYCODE:
        console.log("rotate rejected, state: ", this.getGameStateFunc());
        break;
    }
  }

  if (canDoAllPieceMovements(gameState)) {
    switch (event.keyCode) {
      case DOWN_KEYCODE:
        this.isSoftDropping = true;
        break;
    }
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
    this.cellSoftDropped = 0;
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
  if (!didMove) {
    this.setDASCharge(GameSettings.getDASTriggerThreshold());
  }
  return didMove;
};

InputManager.prototype.handleHeldDirection = function (direction) {
  const DASTriggerThreshold = GameSettings.getDASTriggerThreshold();
  // Increment DAS
  this.setDASCharge(Math.min(DASTriggerThreshold, this.dasCharge + 1));

  // Attempt to shift the piece once it hits the trigger
  if (this.dasCharge == DASTriggerThreshold) {
    const didMove = this.tryShiftPiece(direction);
    if (didMove) {
      // Move DAS to charged floor for another cycle of ARR
      this.setDASCharge(GameSettings.getDASChargedFloor());
    }
  }
};

// Handle single taps of the dpad, if in the proper state
InputManager.prototype.handleTappedDirection = function (direction) {
  if (canMovePiecesSidewaysOrRotate(this.getGameStateFunc())) {
    // Update the DAS charge
    this.setDASCharge(GameSettings.getDASChargeAfterTap());

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
    dasVisualized += "|";
  }
  // Have something on the second line so it's always the same height
  if (this.dasCharge == 0) {
    dasVisualized = ".";
  }
  debugStr +=
    this.dasCharge +
    "/" +
    GameSettings.getDASTriggerThreshold() +
    "\n" +
    dasVisualized;
  dasStatsDiv.innerText = debugStr;
};

// Checks if the game state allows for piece movements horizontally
function canMovePiecesSidewaysOrRotate(gameState) {
  return (
    !GetIsPaused() &&
    (gameState == GameState.RUNNING || gameState == GameState.FIRST_PIECE)
  );
}

// Checks if the game state allows for downward piece movement
function canDoAllPieceMovements(gameState) {
  return !GetIsPaused() && gameState == GameState.RUNNING;
}
