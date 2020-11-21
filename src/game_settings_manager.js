import { DASSpeed, DASBehavior } from "./constants";

const Ui = require("./game_settings_ui_manager");

/* ------ LEVEL TRANSITIONS -------- */

export function ShouldTransitionEvery10Lines() {
  return Ui.GetTransition10Lines();
}

export function ShouldTransitionEveryLine() {
  return false;
}

/* -------- GAMEPLAY --------- */

export function ShouldShowDiggingHints() {
  return Ui.GetDiggingHintsEnabled();
}

export function GetGameSpeedMultiplier() {
  return Ui.GetGameSpeedMultiplier();
}

/* --------- DAS --------- */

export function ShouldSetDASChargeOnPieceStart() {
  const dasBehavior = Ui.GetDASBehavior();
  return (
    dasBehavior == DASBehavior.ALWAYS_CHARGED ||
    dasBehavior == DASBehavior.CHARGE_ON_PIECE_SPAWN
  );
}

export function IsDASAlwaysCharged() {
  return Ui.GetDASBehavior() == DASBehavior.ALWAYS_CHARGED;
}

export function GetDASChargeAfterTap() {
  // If DAS is set to 'always charged', set it to the charged floor (to avoid double shifts)
  if (Ui.GetDASBehavior() == DASBehavior.ALWAYS_CHARGED) {
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
  switch (Ui.GetDASSpeed()) {
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
  const dasSpeed = Ui.GetDASSpeed();
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
