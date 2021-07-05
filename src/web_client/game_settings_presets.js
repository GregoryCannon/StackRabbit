const { DASSpeed, StartingBoardType, DASBehavior } = require("./constants");

export const SettingType = Object.freeze({
  REQUIRED: "required",
  DEFAULT: "default",
  NONE: "none",
});

export const SITE_DEFAULTS = Object.freeze({
  AIPlayerEnabled: false,
  DASSpeed: DASSpeed.STANDARD,
  DASBehavior: DASBehavior.STANDARD,
  DroughtModeEnabled: false,
  DiggingHintsEnabled: false,
  GameSpeedMultiplier: 1,
  ParityHintsEnabled: false,
  PieceSequence: "",
  Transition10Lines: false,
  StartingBoardType: StartingBoardType.EMPTY,
  StartingLevel: 18,
});

// No overrides
export const STANDARD_PRESET = {};

export const STANDARD_TAPPER_PRESET = {
  DASSpeed: {
    type: SettingType.DEFAULT,
    value: DASSpeed.MEDIUM,
  },
  DASBehavior: {
    type: SettingType.DEFAULT,
    value: DASBehavior.CHARGE_ON_PIECE_SPAWN,
  },
};

export const DIG_PRACTICE_PRESET = {
  DiggingHintsEnabled: {
    type: SettingType.DEFAULT,
    value: true,
  },
  StartingBoardType: {
    type: SettingType.REQUIRED,
    value: StartingBoardType.DIG_PRACTICE,
  },
};

export const KILLSCREEN_PRESET = {
  StartingLevel: {
    type: SettingType.REQUIRED,
    value: 29,
  },
  DASSpeed: {
    type: SettingType.DEFAULT,
    value: DASSpeed.MEDIUM,
  },
  DASBehavior: {
    type: SettingType.DEFAULT,
    value: DASBehavior.CHARGE_ON_PIECE_SPAWN,
  },
};

export const SLOW_KILLSCREEN_PRESET = {
  GameSpeedMultiplier: {
    type: SettingType.DEFAULT,
    value: 0.5,
  },
  StartingLevel: {
    type: SettingType.REQUIRED,
    value: 29,
  },
  DASSpeed: {
    type: SettingType.DEFAULT,
    value: DASSpeed.MEDIUM,
  },
  DASBehavior: {
    type: SettingType.DEFAULT,
    value: DASBehavior.CHARGE_ON_PIECE_SPAWN,
  },
};

export const SLOW_19_PRESET = {
  GameSpeedMultiplier: {
    type: SettingType.DEFAULT,
    value: 0.5,
  },
  StartingLevel: {
    type: SettingType.REQUIRED,
    value: 19,
  },
  Transition10Lines: {
    type: SettingType.REQUIRED,
    value: true,
  },
  DASBehavior: {
    type: SettingType.DEFAULT,
    value: DASBehavior.CHARGE_ON_PIECE_SPAWN,
  },
};

export const DROUGHT_PRESET = {
  DroughtModeEnabled: {
    type: SettingType.REQUIRED,
    value: true,
  },
};

export const EDIT_BOARD_PRESET = {
  StartingBoardType: {
    type: SettingType.REQUIRED,
    value: StartingBoardType.CUSTOM,
  },
};

export const CUSTOM_SEQUENCE_PRESET = {
  PieceSequence: {
    type: SettingType.DEFAULT,
    value: "",
  },
};

export const AI_PLAYER_PRESET = {
  AIPlayerEnabled: {
    type: SettingType.REQUIRED,
    value: true,
  },
};
