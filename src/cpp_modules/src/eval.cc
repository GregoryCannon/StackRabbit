#include "../include/eval.h"

#include <math.h>
#include <vector>
// #include "data/ranksOutput.cc"
using namespace std;

const int USE_RANKS = 0;
const int WELL_COLUMN = 9;

float calculateFlatness(int surfaceArray[10]) {
  float score = 30;
  for (int i = 0; i < 8; i++) {
    int diff = surfaceArray[i + 1] - surfaceArray[i];
    score -= pow(abs(diff), 2.5);
  }
  return score;
}

float lookupSurface(int surfaceArray[10]) {
  // Backup option in case ranks aren't loaded
  if (!USE_RANKS) {
    return calculateFlatness(surfaceArray);
  }
  int index = 0;
  for (int i = 0; i < 8; i++) {
    int diff = surfaceArray[i + 1] - surfaceArray[i];
    index *= 9;
    index += diff + 3;
  }
  printf("index: %d\n", index);
  return 1.0;
  // return surfaceRanksRaw[index];
}

float getAverageHeightFactor(int surfaceArray[10], int wellColumn) {
  int totalHeight = 0;
  float weight = wellColumn >= 0 ? 0.1 : 0.111111;
  for (int i = 0; i < 10; i++) {
    if (i == wellColumn) {
      continue;
    }
    totalHeight += surfaceArray[i] * weight;
  }
  return totalHeight;
}

float fastEval(int board[20], int surfaceArray[10], vector<SimState> &lockPlacements, int wellColumn) {
  float surfaceFactor = lookupSurface(surfaceArray);
  float avgHeightFactor = getAverageHeightFactor(surfaceArray, wellColumn);
  return 1.0;
}
