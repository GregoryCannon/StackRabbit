// benchmark.cpp — Performance benchmark for the GET_MOVE path.
//
// Build (from repo root):
//   g++ -O2 -std=c++17 -o benchmark benchmark.cpp
//
// Run:
//   ./benchmark

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

// Pull in all the engine code the same way entrypoint.cpp does.
#include "src/cpp_modules/src/main.cpp"


// ---------------------------------------------------------------------------
// Board helpers
// ---------------------------------------------------------------------------

// Build a 200-char board string where the bottom `filledRows` rows each have
// every cell filled except column `openCol` (0-indexed from the left).
// Rows above `filledRows` are all empty.
// Board encoding: row 0 = top of the playfield, row 19 = bottom.
static std::string makeBoard(int filledRows, int openCol = 9) {
  std::string board(200, '0');
  for (int row = 20 - filledRows; row < 20; ++row) {
    for (int col = 0; col < 10; ++col) {
      board[row * 10 + col] = (col == openCol) ? '0' : '1';
    }
  }
  return board;
}

// Completely empty board.
static std::string emptyBoard() {
  return std::string(200, '0');
}

// A board with a more complex, jagged surface to stress the evaluator.
// Heights (from left, col 0..9): 3 5 2 4 6 1 3 5 2 4
static std::string jaggedBoard() {
  int heights[10] = {3, 5, 2, 4, 6, 1, 3, 5, 2, 4};
  std::string board(200, '0');
  for (int col = 0; col < 10; ++col) {
    int h = heights[col];
    for (int row = 20 - h; row < 20; ++row) {
      board[row * 10 + col] = '1';
    }
  }
  return board;
}

// A board that is nearly topped-out: the top 14 rows all have a full row except
// column 9, giving the AI a tall, dangerous board to reason about.
static std::string nearlyToppedOut() {
  return makeBoard(14, 9);
}

// ---------------------------------------------------------------------------
// Benchmark case definition
// ---------------------------------------------------------------------------

struct BenchmarkCase {
  std::string name;
  std::string inputStr; // Full input string passed to mainProcess()
};

// Build the non-board portion of the input string.
// Parameters mirror the ones parsed in mainProcess():
//   level | lines | curPiece | nextPiece | inputFrameTimeline | [playoutCount | playoutLength | pruningBreadth |]
static std::string makeArgs(
    int level, int lines,
    int curPiece, int nextPiece,
    const std::string& timeline,
    int playoutCount = DEFAULT_PLAYOUT_COUNT,
    int playoutLength = DEFAULT_PLAYOUT_LENGTH,
    int pruningBreadth = DEFAULT_PRUNING_BREADTH)
{
  return std::to_string(level)    + "|" +
         std::to_string(lines)    + "|" +
         std::to_string(curPiece) + "|" +
         std::to_string(nextPiece)+ "|" +
         timeline                 + "|" +
         std::to_string(playoutCount)   + "|" +
         std::to_string(playoutLength)  + "|" +
         std::to_string(pruningBreadth) + "|";
}

// Assemble a full GET_MOVE input string from a board string + arg string.
static std::string makeInput(const std::string& board, const std::string& args) {
  return board + "|" + args;
}

// ---------------------------------------------------------------------------
// Build all benchmark cases
// ---------------------------------------------------------------------------

static std::vector<BenchmarkCase> buildCases() {
  std::vector<BenchmarkCase> cases;

  // Piece indices: I=0, O=1, L=2, J=3, T=4, S=5, Z=6

  // --- Empty board, various pieces and speeds ---
  cases.push_back({"Empty board | level 18 | I piece | 20hz",
    makeInput(emptyBoard(), makeArgs(18, 0, 0 /*I*/, 4 /*T*/, "X.....", 30, 2, 20))});

  cases.push_back({"Empty board | level 18 | T piece | 20hz",
    makeInput(emptyBoard(), makeArgs(18, 0, 4 /*T*/, 2 /*L*/, "X.....", 30, 2, 20))});

  cases.push_back({"Empty board | level 18 | S piece | 20hz",
    makeInput(emptyBoard(), makeArgs(18, 0, 5 /*S*/, 6 /*Z*/, "X.....", 30, 2, 20))});

  // --- 4 rows filled (tetris-ready), right well open ---
  cases.push_back({"4-row fill | level 18 | I piece | 20hz",
    makeInput(makeBoard(4, 9), makeArgs(18, 0, 0 /*I*/, 3 /*J*/, "X.....", 30, 2, 20))});

  cases.push_back({"4-row fill | level 19 | L piece | 13hz",
    makeInput(makeBoard(4, 9), makeArgs(19, 0, 2 /*L*/, 0 /*I*/, "X..X..X..X..", 30, 2, 20))});

  cases.push_back({"4-row fill | level 19 | O piece | 13hz",
    makeInput(makeBoard(4, 9), makeArgs(19, 0, 1 /*O*/, 5 /*S*/, "X..X..X..X..", 30, 2, 20))});

  // --- 8 rows filled, building toward tetris ---
  cases.push_back({"8-row fill | level 18 | J piece | 20hz",
    makeInput(makeBoard(8, 9), makeArgs(18, 40, 3 /*J*/, 4 /*T*/, "X.....", 30, 2, 20))});

  cases.push_back({"8-row fill | level 18 | Z piece | 20hz",
    makeInput(makeBoard(8, 9), makeArgs(18, 40, 6 /*Z*/, 1 /*O*/, "X.....", 30, 2, 20))});

  cases.push_back({"8-row fill | level 19 | T piece | 13hz",
    makeInput(makeBoard(8, 9), makeArgs(19, 40, 4 /*T*/, 2 /*L*/, "X..X..X..X..", 30, 2, 20))});

  // --- Jagged board — forces the AI to think harder about surface ---
  cases.push_back({"Jagged board | level 18 | T piece | 20hz",
    makeInput(jaggedBoard(), makeArgs(18, 20, 4 /*T*/, 3 /*J*/, "X.....", 30, 2, 20))});

  cases.push_back({"Jagged board | level 18 | S piece | 20hz",
    makeInput(jaggedBoard(), makeArgs(18, 20, 5 /*S*/, 6 /*Z*/, "X.....", 30, 2, 20))});

  cases.push_back({"Jagged board | level 19 | I piece | 13hz",
    makeInput(jaggedBoard(), makeArgs(19, 20, 0 /*I*/, 2 /*L*/, "X..X..X..X..", 30, 2, 20))});

  // --- Nearly topped-out (high-stress, danger scenario) ---
  cases.push_back({"Near top-out | level 18 | I piece | 20hz",
    makeInput(nearlyToppedOut(), makeArgs(18, 120, 0 /*I*/, 4 /*T*/, "X.....", 30, 2, 20))});

  cases.push_back({"Near top-out | level 19 | L piece | 13hz",
    makeInput(nearlyToppedOut(), makeArgs(19, 120, 2 /*L*/, 0 /*I*/, "X..X..X..X..", 30, 2, 20))});

  cases.push_back({"Near top-out | level 29 | T piece | 30hz (killscreen)",
    makeInput(nearlyToppedOut(), makeArgs(29, 220, 4 /*T*/, 5 /*S*/, "X.....X.....X..X.....", 30, 2, 20))});

  // --- High line counts (near-killscreen, different AI mode thresholds) ---
  cases.push_back({"4-row fill | level 19 | lines=190 | I piece | 13hz",
    makeInput(makeBoard(4, 9), makeArgs(19, 190, 0 /*I*/, 3 /*J*/, "X..X..X..X..", 30, 2, 20))});

  cases.push_back({"8-row fill | level 19 | lines=200 | Z piece | 13hz",
    makeInput(makeBoard(8, 9), makeArgs(19, 200, 6 /*Z*/, 1 /*O*/, "X..X..X..X..", 30, 2, 20))});

  // --- No next piece (-1) — forces shallower search ---
  cases.push_back({"Empty board | level 18 | I piece | no next | 20hz",
    makeInput(emptyBoard(), makeArgs(18, 0, 0 /*I*/, -1, "X.....", 30, 2, 20))});

  cases.push_back({"8-row fill | level 19 | T piece | no next | 13hz",
    makeInput(makeBoard(8, 9), makeArgs(19, 40, 4 /*T*/, -1, "X..X..X..X..", 30, 2, 20))});

  // --- Wider pruning breadth — stresses the search tree more ---
  cases.push_back({"4-row fill | level 18 | T piece | breadth=33",
    makeInput(makeBoard(4, 9), makeArgs(18, 0, 4 /*T*/, 0 /*I*/, "X.....", 30, 2, 33))});

  cases.push_back({"Jagged board | level 18 | J piece | breadth=33",
    makeInput(jaggedBoard(), makeArgs(18, 20, 3 /*J*/, 5 /*S*/, "X.....", 30, 2, 33))});

  // --- Deeper playouts ---
  cases.push_back({"4-row fill | level 18 | I piece | playoutLen=3",
    makeInput(makeBoard(4, 9), makeArgs(18, 0, 0 /*I*/, 4 /*T*/, "X.....", 30, 3, 20))});

  cases.push_back({"8-row fill | level 19 | L piece | playoutLen=3",
    makeInput(makeBoard(8, 9), makeArgs(19, 40, 2 /*L*/, 3 /*J*/, "X..X..X..X..", 30, 3, 20))});

  return cases;
}

// ---------------------------------------------------------------------------
// Timing utility
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::duration<double, std::milli>;

static double timeMsOneCall(const std::string& inputStr) {
  auto t0 = Clock::now();
  std::string result = mainProcess(inputStr.c_str(), GET_MOVE);
  auto t1 = Clock::now();
  // Use the result so the compiler cannot optimize the call away.
  if (result.empty()) {
    printf("(warning: empty result)\n");
  }
  return std::chrono::duration_cast<Ms>(t1 - t0).count();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  auto cases = buildCases();
  const int REPS = 10; // Repeat each case this many times to smooth noise.

  printf("StackRabbit GET_MOVE benchmark\n");
  printf("  %d cases x %d repetitions each\n\n", (int)cases.size(), REPS);

  std::vector<double> caseMeans;

  for (const auto& bc : cases) {
    std::vector<double> times;
    times.reserve(REPS);
    for (int r = 0; r < REPS; ++r) {
      times.push_back(timeMsOneCall(bc.inputStr));
    }
    double sum  = std::accumulate(times.begin(), times.end(), 0.0);
    double mean = sum / REPS;
    double mn   = *std::min_element(times.begin(), times.end());
    double mx   = *std::max_element(times.begin(), times.end());
    caseMeans.push_back(mean);

    printf("%-60s  avg %7.2f ms  [min %7.2f  max %7.2f]\n",
           bc.name.c_str(), mean, mn, mx);
  }

  double totalMean = std::accumulate(caseMeans.begin(), caseMeans.end(), 0.0);
  double overallMin = *std::min_element(caseMeans.begin(), caseMeans.end());
  double overallMax = *std::max_element(caseMeans.begin(), caseMeans.end());

  printf("\n--- Summary ---\n");
  printf("  Cases:        %d\n", (int)cases.size());
  printf("  Total time:   %.2f ms\n", totalMean * REPS);
  printf("  Mean / call:  %.2f ms\n", totalMean / (int)cases.size());
  printf("  Fastest case: %.2f ms\n", overallMin);
  printf("  Slowest case: %.2f ms\n", overallMax);

  return 0;
}
