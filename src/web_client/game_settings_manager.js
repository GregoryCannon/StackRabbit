import { DASSpeed, DASBehavior } from "./constants";

const Ui = require("./game_settings_ui_manager");

/* ------ LEVEL TRANSITIONS -------- */

export function shouldTransitionEvery10Lines() {
  return Ui.getTransition10Lines();
}

export function shouldTransitionEveryLine() {
  return false;
}

export function getStartingLevel() {
  return Ui.getStartingLevel();
}

/* -------- GAMEPLAY --------- */

export function isAIPlaying() {
  return Ui.getAIPlayerEnabled();
}

export function shouldShowDiggingHints() {
  return Ui.getDiggingHintsEnabled();
}

export function shouldShowParityHints() {
  return Ui.getParityHintsEnabled();
}

export function isNoAdjustmentMode() {
  return Ui.getNoAdjustmentsModeEnabled();
}

export function getGameSpeedMultiplier() {
  return Ui.getGameSpeedMultiplier();
}

// e.g. if game at half speed, skip every other frame
export function getFrameSkipCount() {
  return Math.round(1 / Ui.getGameSpeedMultiplier());
}

export function shouldReduceLongBars() {
  return Ui.getDroughtModeEnabled();
}

export function getPieceSequence() {
  return Ui.getPieceSequence();
}

export function getStartingBoardType() {
  return Ui.getStartingBoardType();
}

/* --------- DAS --------- */

export function shouldSetDASChargeOnPieceStart() {
  const dasBehavior = Ui.getDASBehavior();
  return (
    dasBehavior == DASBehavior.ALWAYS_CHARGED ||
    dasBehavior == DASBehavior.CHARGE_ON_PIECE_SPAWN
  );
}

export function isDASAlwaysCharged() {
  return Ui.getDASBehavior() == DASBehavior.ALWAYS_CHARGED;
}

export function getDASChargeAfterTap() {
  // If DAS is set to 'always charged', set it to the charged floor (to avoid double shifts)
  if (Ui.getDASBehavior() == DASBehavior.ALWAYS_CHARGED) {
    return getDASChargedFloor();
  }
  // Otherwise, DAS loses its charge on tap
  else {
    return GetDASUnchargedFloor();
  }
}

/** Gets the DAS value set on wall charge, or maybe on piece spawn (depending on the DAS behavior setting) */
export function getDASWallChargeAmount() {
  switch (Ui.getDASSpeed()) {
    // For the DAS speeds that are in between whole number frames (e.g slower than 4F but faster than 5F),
    // give them a worse DAS charge on every wallcharge and piece spawn
    case DASSpeed.SLOW_MEDIUM:
      return getDASChargedFloor() + 2;
    case DASSpeed.FAST:
      return getDASChargedFloor();
    default:
      // All other speeds have DAS auto-wall-charged on piece spawn
      return getDASTriggerThreshold();
  }
}

export function getDASChargedFloor() {
  return 10;
}

export function GetDASUnchargedFloor() {
  return 0;
}

export function getDASTriggerThreshold() {
  let ARR;
  const dasSpeed = Ui.getDASSpeed();
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
  return getDASChargedFloor() + ARR;
}
