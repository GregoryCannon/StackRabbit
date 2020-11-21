import { DASSpeed, DASBehavior } from "./constants";

const dasSpeedDropdown = document.getElementById("das-speed-dropdown");
const dasBehaviorDropdown = document.getElementById("das-behavior-dropdown");
const gameSpeedDropdown = document.getElementById("game-speed-dropdown");
const diggingHintsCheckbox = document.getElementById("digging-hints-checkbox");
const transition10Checkbox = document.getElementById("transition-10-checkbox");

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

export function GetTransition10Lines() {
  return transition10Checkbox.checked;
}

export function SetTransition10Lines(enabled) {
  transition10Checkbox.checked = enabled;
}

export function GetDiggingHintsEnabled() {
  return diggingHintsCheckbox.checked;
}

export function SetDiggingHintsEnabled(enabled) {
  diggingHintsCheckbox.checked = enabled;
}

export function GetGameSpeedMultiplier() {
  return gameSpeedDropdown.value;
}

export function SetGameSpeedMultiplier(value) {
  gameSpeedDropdown.value = value;
}

export function GetDASSpeed() {
  const speedIndex = parseInt(dasSpeedDropdown.value);
  return DAS_SPEED_LIST[speedIndex];
}

export function SetDASSpeed(value) {
  dasSpeedDropdown.value = DAS_SPEED_LIST.findIndex((x) => x == value);
}

export function GetDASBehavior() {
  const behaviorIndex = parseInt(dasBehaviorDropdown.value);
  return DAS_BEHAVIOR_LIST[behaviorIndex];
}

/* ---------- PRESETS ----------- */

function loadStandardPreset() {}
