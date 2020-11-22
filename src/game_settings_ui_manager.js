import { DASSpeed, DASBehavior } from "./constants";

const dasSpeedDropdown = document.getElementById("das-speed-dropdown");
const dasBehaviorDropdown = document.getElementById("das-behavior-dropdown");
const gameSpeedDropdown = document.getElementById("game-speed-dropdown");
const droughtCheckbox = document.getElementById("drought-checkbox");
const diggingHintsCheckbox = document.getElementById("digging-hints-checkbox");
const parityHintsCheckbox = document.getElementById("parity-hints-checkbox");
const transition10Checkbox = document.getElementById("transition-10-checkbox");
const pieceSequenceText = document.getElementById("piece-sequence");
const levelSelectElement = document.getElementById("level-select");

/* List of DAS speeds in order that they're listed in the UI dropdown.
   This essentially acts as a lookup table because the <option>s in the HTML have 
   value according to the index here. 
   e.g. value="2" maps to index 2, etc. */
const DAS_SPEED_LIST = [
  DASSpeed.STANDARD,
  DASSpeed.SLOW_MEDIUM,
  DASSpeed.MEDIUM,
  DASSpeed.FAST,
  DASSpeed.FASTDAS,
];

/* List of DAS charging behaviors in order that they're listed in the UI dropdown.
     This essentially acts as a lookup table because the <option>s in the HTML have 
     value according to the index here. 
     e.g. value="2" maps to index 2, etc. */
const DAS_BEHAVIOR_LIST = [
  DASBehavior.STANDARD,
  DASBehavior.ALWAYS_CHARGED,
  DASBehavior.CHARGE_ON_PIECE_SPAWN,
];

export function GetStartingLevel() {
  return parseInt(levelSelectElement.value);
}

function SetStartingLevel(value) {
  levelSelectElement.value = value;
}

export function GetTransition10Lines() {
  return transition10Checkbox.checked;
}

function SetTransition10Lines(enabled) {
  transition10Checkbox.checked = enabled;
}

export function GetDroughtModeEnabled() {
  return droughtCheckbox.checked;
}

function SetDroughtModeEnabled(value) {
  droughtCheckbox.checked = value;
}

export function GetDiggingHintsEnabled() {
  return diggingHintsCheckbox.checked;
}

function SetDiggingHintsEnabled(enabled) {
  diggingHintsCheckbox.checked = enabled;
}

export function GetParityHintsEnabled() {
  return parityHintsCheckbox.checked;
}

function SetParityHintsEnabled(enabled) {
  parityHintsCheckbox.checked = enabled;
}

export function GetGameSpeedMultiplier() {
  return gameSpeedDropdown.value;
}

function SetGameSpeedMultiplier(value) {
  gameSpeedDropdown.value = value;
}

export function GetDASSpeed() {
  const speedIndex = parseInt(dasSpeedDropdown.value);
  return DAS_SPEED_LIST[speedIndex];
}

function SetDASSpeed(value) {
  dasSpeedDropdown.value = DAS_SPEED_LIST.findIndex((x) => x == value);
}

export function GetDASBehavior() {
  const behaviorIndex = parseInt(dasBehaviorDropdown.value);
  return DAS_BEHAVIOR_LIST[behaviorIndex];
}

function SetDASBehavior(value) {
  dasBehaviorDropdown.value = DAS_BEHAVIOR_LIST.findIndex((x) => x == value);
}

export function GetPieceSequence() {
  const sequenceRaw = pieceSequenceText.value;
  const allCaps = sequenceRaw.toUpper();
  let cleansedStr = "";
  for (const char of allCaps) {
    if (["I", "O", "L", "J", "T", "S", "Z"].includes(char)) {
      cleansedStr += char;
    }
  }
  return cleansedStr;
}

function SetPieceSequence(value) {
  pieceSequenceText.value = value;
}

/* ---------- PRESETS ----------- */

function disableOptionalMods() {}

export function loadStandardPreset() {
  SetDASSpeed(DASSpeed.STANDARD);
  SetDASBehavior(DASBehavior.STANDARD);
  SetGameSpeedMultiplier(1);
  SetDroughtModeEnabled(false);
  SetPieceSequence("");
  SetTransition10Lines(false);
  if (GetStartingLevel() > 18) {
    SetStartingLevel(18);
  }
}

export function loadStandardTapPreset() {
  SetDASSpeed(DASSpeed.MEDIUM);
  SetDASBehavior(DASBehavior.CHARGE_ON_PIECE_SPAWN);
  SetGameSpeedMultiplier(1);
  SetDroughtModeEnabled(false);
  SetPieceSequence("");
  SetTransition10Lines(false);
  if (GetStartingLevel() > 18) {
    SetStartingLevel(18);
  }
}

export function loadDigPracticePreset() {
  SetDiggingHintsEnabled(true);
  SetParityHintsEnabled(false);
  SetGameSpeedMultiplier(1);
  SetDroughtModeEnabled(false);
  SetPieceSequence("");
  SetTransition10Lines(false);
  if (GetStartingLevel() > 18) {
    SetStartingLevel(18);
  }
}

export function loadKillscreenPreset() {
  SetDASSpeed(DASSpeed.MEDIUM);
  SetDASBehavior(DASBehavior.CHARGE_ON_PIECE_SPAWN);
  SetStartingLevel(29);
  SetGameSpeedMultiplier(1);
  SetDiggingHintsEnabled(false);
  SetDroughtModeEnabled(false);
  SetPieceSequence("");
  SetTransition10Lines(false);
}

export function loadSlowKillscreenPreset() {
  SetDASSpeed(DASSpeed.MEDIUM);
  SetDASBehavior(DASBehavior.CHARGE_ON_PIECE_SPAWN);
  SetStartingLevel(29);
  SetGameSpeedMultiplier(0.5);
  SetDiggingHintsEnabled(false);
  SetDroughtModeEnabled(false);
  SetPieceSequence("");
  SetTransition10Lines(false);
}

export function loadSlow19Preset() {
  SetStartingLevel(19);
  SetDASBehavior(DASBehavior.CHARGE_ON_PIECE_SPAWN);
  SetGameSpeedMultiplier(0.5);
  SetDiggingHintsEnabled(false);
  SetDroughtModeEnabled(false);
  SetPieceSequence("");
  SetTransition10Lines(true);
}

export function loadDroughtPreset() {
  SetGameSpeedMultiplier(1);
  SetDiggingHintsEnabled(false);
  SetDroughtModeEnabled(true);
  SetPieceSequence("");
  SetTransition10Lines(false);
  if (GetStartingLevel() > 18) {
    SetStartingLevel(18);
  }
}
