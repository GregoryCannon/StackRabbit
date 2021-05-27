import { DASSpeed, DASBehavior, StartingBoardType } from "./constants";
import { SettingType, SITE_DEFAULTS } from "./game_settings_presets";

const aiPlayerCheckbox = document.getElementById("ai-player-checkbox");
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

const playerSettings = getUserPreferencesFromCookie();

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

function saveUserPreferencesToCookie() {
  document.cookie =
    escape(JSON.stringify(playerSettings)) +
    "; expires=Thu, 18 Dec 2030 12:00:00 UTC";
  console.log("cookie:", document.cookie);
}

function getUserPreferencesFromCookie() {
  if (document.cookie) {
    console.log("cookie:", document.cookie);
    const firstCookie = document.cookie.split(";")[0];
    return JSON.parse(unescape(firstCookie));
  }
  return JSON.parse(JSON.stringify(SITE_DEFAULTS));
}

function onLevelChanged() {
  // Select the button corresponding to the current level, if there is one
  for (const button of document.getElementById("level-choice-buttons")
    .children) {
    if (button.id.split("-")[1] === levelSelectElement.value) {
      button.classList.add("selected");
    } else {
      button.classList.remove("selected");
    }
  }
}

function addOnChangeListeners() {
  // When user changes a preference manually, associate it with their user cookie so the preference is remembered
  dasSpeedDropdown.addEventListener("change", (e) => {
    playerSettings["DASSpeed"] = getDASSpeed();
    saveUserPreferencesToCookie();
  });
  dasBehaviorDropdown.addEventListener("change", (e) => {
    playerSettings["DASBehavior"] = getDASBehavior();
    saveUserPreferencesToCookie();
  });
  diggingHintsCheckbox.addEventListener("change", (e) => {
    playerSettings["DiggingHintsEnabled"] = getDiggingHintsEnabled();
    saveUserPreferencesToCookie();
  });
  parityHintsCheckbox.addEventListener("change", (e) => {
    playerSettings["ParityHintsEnabled"] = getParityHintsEnabled();
    saveUserPreferencesToCookie();
  });
  // AI player checkbox does not save to cookie for obvious reasons

  // Also update the level select buttons
  levelSelectElement.addEventListener("change", (e) => {
    onLevelChanged();
  });
}
addOnChangeListeners();

// Assign on click listeners, e.g. #level-0 sets it to 0.
[0, 5, 8, 9, 15, 18, 19, 29].forEach((num) => {
  document.getElementById("level-" + num).addEventListener("click", (e) => {
    levelSelectElement.value = num;
    onLevelChanged();

    // Update the starting level in cookies
    playerSettings["StartingLevel"] = getStartingLevel();
    saveUserPreferencesToCookie();
  });
});

function setSetting(settingName, value) {
  switch (settingName) {
    case "AIPlayerEnabled":
      aiPlayerCheckbox.checked = value;
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
      onLevelChanged();
      break;
  }
}

export function getStartingLevel() {
  const tempLevel = parseInt(levelSelectElement.value);
  return Math.max(tempLevel, 0);
}

export function getTransition10Lines() {
  return transition10Checkbox.checked;
}

export function getAIPlayerEnabled() {
  return aiPlayerCheckbox.checked;
}

export function getDroughtModeEnabled() {
  return droughtCheckbox.checked;
}

export function getDiggingHintsEnabled() {
  return diggingHintsCheckbox.checked;
}

export function getParityHintsEnabled() {
  return parityHintsCheckbox.checked;
}

export function getGameSpeedMultiplier() {
  return gameSpeedDropdown.value;
}

export function getDASSpeed() {
  const speedIndex = parseInt(dasSpeedDropdown.value);
  return DAS_SPEED_LIST[speedIndex];
}

export function getDASBehavior() {
  const behaviorIndex = parseInt(dasBehaviorDropdown.value);
  return DAS_BEHAVIOR_LIST[behaviorIndex];
}

export function getPieceSequence() {
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

export function getStartingBoardType() {
  return STARTING_BOARD_LIST[startingBoardDropdown.value];
}

/* ---------- PRESETS ----------- */

export function loadPreset(presetObj) {
  const settingsList = [
    ["AIPlayerEnabled", aiPlayerCheckbox],
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
