export const NUM_ROW = 20;
export const NUM_COLUMN = 10;
export const SQUARE_SIZE = 28;

export const VACANT = "black"; // color of an empty square
export const RED_COLOR = "red";
export const BLUE_COLOR = "#2105f2";
export const WHITE_COLOR = "white";

// color 1 is COLOR_3 with white in the center, which is used for I, T, and O
export const COLOR_1 = {
  0: "rgb(0,88,248)",
  1: "rgb(0,168,0)",
  2: "rgb(216,0,204)",
  3: "rgb(0,88,248)",
  4: "rgb(228,0,88",
  5: "rgb(88,248,152)",
  6: "rgb(248,56,0)",
  7: "rgb(104,68,252)",
  8: "rgb(0,88,248)",
  9: "rgb(248,56,0)",
};
// color 2 is the main color of L and Z
export const COLOR_2 = {
  0: "rgb(60,188,252)",
  1: "rgb(148,248,24)",
  2: "rgb(248,120,248)",
  3: "rgb(88,216,84)",
  4: "rgb(88,248,152)",
  5: "rgb(104,136,252)",
  6: "rgb(124,124,124)",
  7: "rgb(168,0,32)",
  8: "rgb(248,56,0)",
  9: "rgb(252,160,68)",
};
// COLOR_3 is the main color of J and S
// It is the same as COLOR_1, but fills the whole square
export const COLOR_3 = Object.assign(COLOR_1);

export const COLOR_PALETTE = {
  1: COLOR_1,
  2: COLOR_2,
  3: COLOR_3,
};

export const Direction = Object.freeze({
  LEFT: 1,
  RIGHT: 2,
  DOWN: 3,
  UP: 4,
});

// How many points for X lines at a time (before scaling by level)
export const REWARDS = {
  1: 40,
  2: 100,
  3: 300,
  4: 1200,
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
  19: 2,
  29: 1,
};

export const GameState = {
  RUNNING: "running",
  PAUSED: "paused",
  GAME_OVER: "game over",
  START_SCREEN: "start screen",
};

export const DAS_TRIGGER = 16;
export const DAS_CHARGED_FLOOR = 10;