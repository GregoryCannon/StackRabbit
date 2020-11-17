const dasSpeedDropdown = document.getElementById("das-speed-dropdown");
const dasBehaviorDropdown = document.getElementById("das-behavior-dropdown");
const gameSpeedDropdown = document.getElementById("game-speed-dropdown");
const diggingHintsCheckbox = document.getElementById("digging-hints-checkbox");
const transition10Checkbox = document.getElementById("transition-10-checkbox");

export const DASSpeed = Object.freeze({
  STANDARD: "standard",
  SLOW_MEDIUM: "slow_medium",
  MEDIUM: "medium",
  FAST: "fast",
  FASTDAS: "Fast DAS",
});

/* List of DAS speeds in order that they're listed in the UI dropdown.
   This essentially acts as a lookup table because the <option>s in the HTML have 
   value according to the index here. 
   e.g. value="2" maps to index 2, etc. */
const DAS_SPEED_LSIT = [
  DASSpeed.STANDARD,
  DASSpeed.SLOW_MEDIUM,
  DASSpeed.MEDIUM,
  DASSpeed.FAST,
  DASSpeed.FASTDAS,
];

export const DASBehavior = Object.freeze({
  STANDARD: "standard",
  ALWAYS_CHARGED: "always_charged",
  CHARGE_ON_PIECE_SPAWN: "charge_on_piece_spawn",
});

/* List of DAS charging behaviors in order that they're listed in the UI dropdown.
   This essentially acts as a lookup table because the <option>s in the HTML have 
   value according to the index here. 
   e.g. value="2" maps to index 2, etc. */
const DAS_BEHAVIOR_LIST = [
  DASBehavior.STANDARD,
  DASBehavior.ALWAYS_CHARGED,
  DASBehavior.CHARGE_ON_PIECE_SPAWN,
];

/* ------ LEVEL TRANSITIONS -------- */

export function ShouldTransitionEvery10Lines() {
  return transition10Checkbox.checked;
}

export function ShouldTransitionEveryLine() {
  return false;
}

/* -------- GAMEPLAY --------- */

export function ShouldShowDiggingHints() {
  return diggingHintsCheckbox.checked;
}

export function GetGameSpeedMultiplier() {
  return gameSpeedDropdown.value;
}

/* --------- DAS --------- */

function GetDASSpeed() {
  const speedIndex = parseInt(dasSpeedDropdown.value);
  return DAS_SPEED_LSIT[speedIndex];
}

function GetDASBehavior() {
  const behaviorIndex = parseInt(dasBehaviorDropdown.value);
  return DAS_BEHAVIOR_LIST[behaviorIndex];
}

export function ShouldSetDASChargeOnPieceStart() {
  const dasBehavior = GetDASBehavior();
  return (
    dasBehavior == DASBehavior.ALWAYS_CHARGED ||
    dasBehavior == DASBehavior.CHARGE_ON_PIECE_SPAWN
  );
}

export function IsDASAlwaysCharged() {
  return GetDASBehavior() == DASBehavior.ALWAYS_CHARGED;
}

export function GetDASChargeAfterTap() {
  // If DAS is set to 'always charged', set it to the charged floor (to avoid double shifts)
  if (GetDASBehavior() == DASBehavior.ALWAYS_CHARGED) {
    return GetDASChargedFloor();
  }
  // Otherwise, DAS loses its charge on tap
  else {
    return GetDASUnchargedFloor();
  }
}

/** Gets the DAS value given to all pieces on piece spawn, assuming IsDASAlwaysCharged is true. */
export function GetDASWallChargeAmount() {
  // This settings only applies when DAS is always charged
  if (!ShouldSetDASChargeOnPieceStart()) {
    throw new Error(
      "Requested DASChargeOnPieceStart when ShouldSetDASChargeOnPieceStart evaluated to 'false'."
    );
  }
  switch (GetDASSpeed()) {
    // For the DAS speeds that are in between whole number frames (e.g slower than 4F but faster than 5F),
    // give them a worse DAS charge on every wallcharge and piece spawn
    case DASSpeed.SLOW_MEDIUM:
      return GetDASChargedFloor() + 2;
    case DASSpeed.FAST:
      return GetDASChargedFloor();
    default:
      // All other speeds have DAS auto-wall-charged on piece spawn
      return GetDASTriggerThreshold();
  }
}

export function GetDASChargedFloor() {
  return 10;
}

export function GetDASUnchargedFloor() {
  return 0;
}

export function GetDASTriggerThreshold() {
  let ARR;
  const dasSpeed = GetDASSpeed();
  switch (dasSpeed) {
    case DASSpeed.STANDARD:
      ARR = 6;
      break;
    case DASSpeed.FAST:
    case DASSpeed.FASTDAS:
      ARR = 4;
      break;
    case DASSpeed.SLOW_MEDIUM:
    case DASSpeed.MEDIUM:
      ARR = 5;
      break;
    default:
      throw new Error("Unknown DAS speed: " + dasSpeed);
  }
  return GetDASChargedFloor() + ARR;
}
