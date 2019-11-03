export const ROW = 20;
export const COLUMN = 10;
export const SQUARE_SIZE = 20;
export const VACANT = "BLACK"; // color of an empty square

// How many points for X lines at a time (before scaling by level)
export const REWARDS = {
  1: 40,
  2: 100,
  3: 300,
  4: 1200
};
// How many frames it takes to drop one square
export const GRAVITY = {
  0: 48,
  1: 43,
  2: 38,
  3: 33,
  4: 28,
  5: 23,
  6: 18,
  7: 13,
  8: 8,
  9: 6,
  10: 5,
  11: 5,
  12: 5,
  13: 4,
  14: 4,
  15: 4,
  16: 3,
  17: 3,
  18: 3,
  19: 2
};

export const GameState = {
  RUNNING: "running",
  PAUSED: "paused",
  GAME_OVER: "game over",
  START_SCREEN: "start screen"
};
