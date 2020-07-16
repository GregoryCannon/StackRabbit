import {
  Direction,
  DAS_TRIGGER,
  DAS_CHARGED_FLOOR,
  GameState,
  GameSubState,
} from "./constants.js";

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
  getGameSubStateFunc,
  getAREFunc
) {
  this.leftHeld = false;
  this.rightHeld = false;
  this.downHeld = false;
  this.dasCount = 0;
  this.softDroppedLastFrame = false;

  this.togglePauseFunc = togglePauseFunc;
  this.moveDownFunc = moveDownFunc;
  this.moveLeftFunc = moveLeftFunc;
  this.moveRightFunc = moveRightFunc;
  this.rotateLeftFunc = rotateLeftFunc;
  this.rotateRightFunc = rotateRightFunc;
  this.getGameStateFunc = getGameStateFunc;
  this.getGameSubStateFunc = getGameSubStateFunc;
  this.getAREFunc = getAREFunc;
}

InputManager.prototype.handleInputsThisFrame = function () {
  const numKeysHeld = this.downHeld + this.leftHeld + this.rightHeld;
  // If holding multiple keys, do nothing
  if (numKeysHeld > 1) {
    return;
  }

  // Move piece down
  if (this.downHeld && !this.softDroppedLastFrame) {
    console.log("Soft dropping");
    const didMove = this.moveDownFunc();
    if (!didMove) {
      // If it didn't move, then it locked in. Reset pushdown between pieces.
      this.downHeld = false;
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

InputManager.prototype.isSoftDropping = function () {
  return this.downHeld;
};

InputManager.prototype.handleHeldDirection = function (direction) {
  // Increment DAS
  this.dasCount = Math.min(DAS_TRIGGER, this.dasCount + 1);

  if (this.dasCount == 1 || this.dasCount == DAS_TRIGGER) {
    // Attempt to shift the piece
    const didMove =
      direction == Direction.LEFT ? this.moveLeftFunc() : this.moveRightFunc();
    if (didMove) {
      // DAS is still "charged" so we reset it to the charged floor instead of 0
      if (this.dasCount == DAS_TRIGGER) {
        this.dasCount = DAS_CHARGED_FLOOR;
      }
    } else {
      // Wall charge
      this.dasCount = 16;
    }
  }
};

InputManager.prototype.keyDownListener = function (event) {
  // Override the browser's built-in key repeating
  if (event.repeat) {
    return;
  }
  // Piece movement - on key down
  // Move the piece once, and if appropriate, save that the key is held (for DAS)
  if (this.getGameStateFunc() == GameState.RUNNING) {
    switch (event.keyCode) {
      case LEFT_KEYCODE:
        // Reset DAS, unless in ARE state
        if (this.getAREFunc() == 0) {
          this.dasCount = 0;
        }
        this.leftHeld = true;
        break;
      case RIGHT_KEYCODE:
        // Reset DAS, unless in ARE state
        if (this.getAREFunc() == 0) {
          this.dasCount = 0;
        }
        this.rightHeld = true;
        break;
      case ROTATE_LEFT_KEYCODE:
        this.rotateLeftFunc();
        break;
      case ROTATE_RIGHT_KEYCODE:
        this.rotateRightFunc();
        break;
      case DOWN_KEYCODE:
        this.downHeld = true;
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
  // Piece movement - on key up
  const mainGameState = this.getGameStateFunc();
  const gameSubState = this.getGameSubStateFunc();
  if (mainGameState == GameState.RUNNING) {
    if (event.keyCode == LEFT_KEYCODE) {
      this.leftHeld = false;
    } else if (event.keyCode == RIGHT_KEYCODE) {
      this.rightHeld = false;
    } else if (
      event.keyCode == DOWN_KEYCODE &&
      gameSubState == GameSubState.PIECE_ACTIVE
    ) {
      this.downHeld = false;
    }
  }
};

InputManager.prototype.getDebugText = function () {
  let debugStr = "";
  let dasVisualized = "";
  for (let i = 0; i < this.dasCount; i++) {
    dasVisualized += "x";
  }
  debugStr += "DAS: " + this.dasCount + "\n" + dasVisualized;
  return debugStr;
};

InputManager.prototype.resetLocalVariables = function () {
  this.leftHeld = false;
  this.rightHeld = false;
  this.downHeld = false;
  this.dasCount = 0;
  this.softDroppedLastFrame = false;
};
