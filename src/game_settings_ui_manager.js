import { DASSpeed, DASBehavior, StartingBoardType } from "./constants";
import {
  SettingType,
  SITE_DEFAULTS,
  STANDARD_PRESET,
  STANDARD_TAPPER_PRESET,
} from "./game_settings_presets";

const dasSpeedDropdown = document.getElementById("das-speed-dropdown");
const dasBehaviorDropdown = document.getElementById("das-behavior-dropdown");
const gameSpeedDropdown = document.getElementById("game-speed-dropdown");
const startingBoardDropdown = document.getElementById("starting-board");
const droughtCheckbox = document.getElementById("drought-checkbox");
const diggingHintsCheckbox = document.getElementById("digging-hints-checkbox");
const parityHintsCheckbox = document.getElementById("parity-hints-checkbox");
const transition10Checkbox = document.getElementById("transition-10-checkbox");
const pieceSequenceText = document.getElementById("piece-sequence");
const levelSelectElement = document.getElementById("level-select");

const playerSettings = JSON.parse(JSON.stringify(SITE_DEFAULTS));

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

/* List of starting board types in order that they're listed in the UI dropdown. */
const STARTING_BOARD_LIST = [
  StartingBoardType.EMPTY,
  StartingBoardType.DIG_PRACTICE,
  StartingBoardType.CUSTOM,
];

function setSetting(settingName, value) {
  switch (settingName) {
    case "DASSpeed":
      dasSpeedDropdown.value = DAS_SPEED_LIST.findIndex((x) => x == value);
      break;
    case "DASBehavior":
      dasBehaviorDropdown.value = DAS_BEHAVIOR_LIST.findIndex(
        (x) => x == value
      );
      break;
    case "DroughtModeEnabled":
      droughtCheckbox.checked = value;
      break;
    case "DiggingHintsEnabled":
      diggingHintsCheckbox.checked = value;
      break;
    case "GameSpeedMultiplier":
      gameSpeedDropdown.value = value;
      break;
    case "ParityHintsEnabled":
      parityHintsCheckbox.checked = value;
      break;
    case "PieceSequence":
      pieceSequenceText.value = value;
      break;
    case "Transition10Lines":
      transition10Checkbox.checked = value;
      break;
    case "StartingBoardType":
      startingBoardDropdown.value = STARTING_BOARD_LIST.findIndex(
        (x) => x == value
      );
      break;
    case "StartingLevel":
      levelSelectElement.value = value;
  }
}

export function GetStartingLevel() {
  return parseInt(levelSelectElement.value);
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

export function GetDiggingHintsEnabled() {
  return diggingHintsCheckbox.checked;
}

export function GetParityHintsEnabled() {
  return parityHintsCheckbox.checked;
}

export function GetGameSpeedMultiplier() {
  return gameSpeedDropdown.value;
}

export function GetDASSpeed() {
  const speedIndex = parseInt(dasSpeedDropdown.value);
  return DAS_SPEED_LIST[speedIndex];
}

export function GetDASBehavior() {
  const behaviorIndex = parseInt(dasBehaviorDropdown.value);
  return DAS_BEHAVIOR_LIST[behaviorIndex];
}

export function GetPieceSequence() {
  const sequenceRaw = pieceSequenceText.value;
  const allCaps = sequenceRaw.toUpperCase();
  let cleansedStr = "";
  for (const char of allCaps) {
    if (["I", "O", "L", "J", "T", "S", "Z"].includes(char)) {
      cleansedStr += char;
    }
  }
  return cleansedStr;
}

export function GetStartingBoardType() {
  return STARTING_BOARD_LIST[startingBoardDropdown.value];
}

/* ---------- PRESETS ----------- */

/*
const dasSpeedDropdown = document.getElementById("das-speed-dropdown");
const dasBehaviorDropdown = document.getElementById("das-behavior-dropdown");
const gameSpeedDropdown = document.getElementById("game-speed-dropdown");
const startingBoardDropdown = document.getElementById("starting-board");
const droughtCheckbox = document.getElementById("drought-checkbox");
const diggingHintsCheckbox = document.getElementById("digging-hints-checkbox");
const parityHintsCheckbox = document.getElementById("parity-hints-checkbox");
const transition10Checkbox = document.getElementById("transition-10-checkbox");
const pieceSequenceText = document.getElementById("piece-sequence");
const levelSelectElement = document.getElementById("level-select");

*/

export function loadPreset(presetObj) {
  const settingsList = [
    ["DASSpeed", dasSpeedDropdown],
    ["DASBehavior", dasBehaviorDropdown],
    ["DroughtModeEnabled", droughtCheckbox],
    ["DiggingHintsEnabled", diggingHintsCheckbox],
    ["GameSpeedMultiplier", gameSpeedDropdown],
    ["ParityHintsEnabled", parityHintsCheckbox],
    ["PieceSequence", pieceSequenceText],
    ["Transition10Lines", transition10Checkbox],
    ["StartingBoardType", startingBoardDropdown],
    ["StartingLevel", levelSelectElement],
  ];

  for (const entry of settingsList) {
    const key = entry[0];
    const inputElement = entry[1];
    const containerToHighlight =
      key == "StartingLevel"
        ? inputElement.parentElement
        : inputElement.parentElement.parentElement;
    const containerToDisable = inputElement.parentElement.parentElement;
    if (key in presetObj) {
      // Specified in the preset
      setSetting(key, presetObj[key].value);

      // Highlight that section to indicate it was changed by the preset
      containerToHighlight.classList.add("selected-row");
      if (presetObj[key].type == SettingType.REQUIRED) {
        containerToDisable.classList.add("disabled-row");
      }
    } else {
      // Not specified in the preset
      setSetting(key, playerSettings[key]);

      // Remove any highlights on that section
      containerToHighlight.classList.remove("selected-row");
      containerToDisable.classList.remove("disabled-row");
    }
  }
}
