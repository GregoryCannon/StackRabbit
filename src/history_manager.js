/**
 * A manager class that stores a snapshot of the game every time a piece locks.
 * This allows the player to rewind the game if they make a mistake.
 */

export function HistoryManager() {
  this.history = [];
  this.readIndex = -1;
}

HistoryManager.prototype.addSnapshotToHistory = function (snapshotObj) {
  this.readIndex += 1;
  this.history[this.readIndex] = snapshotObj;
};

HistoryManager.prototype.rewindOnePiece = function () {
  this.readIndex -= 1;
  this.readIndex = Math.max(0, this.readIndex);
};

HistoryManager.prototype.fastForwardOnePiece = function () {
  this.readIndex += 1;
  this.readIndex = Math.min(this.readIndex, this.history.length - 1);
};

HistoryManager.prototype.loadSnapshotFromHistory = function () {
  if (this.readIndex >= this.history.length) {
    return null;
  }
  return this.history[this.readIndex];
};

HistoryManager.prototype.resetHistory = function () {
  this.history = [];
  this.readIndex = -1;
};
