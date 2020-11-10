export function IsDASAlwaysCharged() {
  return false;
}

function UseKillscreenDAS12Hz() {
  return false;
}

export function ShouldTransitionEvery10Lines() {
  return false;
}

export function ShouldTransitionEveryLine() {
  return false;
}

export function ShouldShowDiggingHints() {
  return true;
}

export function GetDASUnchargedFloor() {
  if (IsDASAlwaysCharged()) {
    return 10;
  }
  return 0;
}

export function GetDASChargedFloor() {
  return 10;
}

export function GetDASTriggerThreshold() {
  if (UseKillscreenDAS12Hz()) {
    return 22;
  }
  return 16;
}
