function totalFrames(lockHeight, grav) {
  const are = 10 + Math.floor((lockHeight + 2) / 4) * 2;
  const drop = (18 - lockHeight) * grav;
  const reaction = 12;
  const frames = are + drop + reaction;
  console.log(`are ${are}, drop ${drop}, reaction ${reaction}`);
  return (frames * 16.666667) / 7;
}

const s = {
  1: 34,
  2: 10,
  3: 1,
};
// const s = {
//   1: 34,
//   2: 7,
//   3: 3,
// };

const e = {
  1: 34,
  2: 20,
  3: 10,
  4: 5,
};

// const G_time = 0.046;
const G_time = 0.1;
const E_time = 0.011;

function computeTime(depth) {
  switch (depth) {
    case 1:
      return G_time + e[1] * E_time;
    case 2:
      return (1 + s[1]) * G_time + (e[1] + s[1] * e[2]) * E_time;
    case 3:
      return (
        (1 + s[1] + 7 * s[2]) * G_time +
        (e[1] + s[1] * e[2] + 7 * s[2] * e[3]) * E_time
      );
    case 4:
      return (
        (1 + s[1] + 7 * s[2] + 49 * s[2] * s[3]) * G_time +
        (e[1] + s[1] * e[2] + 7 * s[2] * e[3] + 49 * s[2] * s[3] * e[3]) *
          E_time
      );
  }
}

console.log(
  `depth 2: ${computeTime(2)}, depth 3: ${computeTime(3)}, 4: ${computeTime(4)}`
);
