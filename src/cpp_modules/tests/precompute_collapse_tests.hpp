// Precompute collapse + formatter tests: synthetic states, exact pins.
//
// Drives collapseAndFormatPrecompute (src/precompute.hpp) directly with
// hand-built phantom sets and lock value maps, so the collapse math and the
// response formatting are pinned independently of the eval and the move
// search. The maps are pure inputs here: keys are built with encodeLockCell
// ("rot|x|y", visible in this unity TU) and must match the synthetic
// placements' absolute lock coordinates.
//
// Numerical conventions pinned here:
//   - maximisation starts at Number.MIN_SAFE_INTEGER and a phantom whose
//     total is NaN or below it is skipped entirely (the response stays
//     "none" only when NO phantom qualifies);
//   - phantom totals accumulate lock value + input cost + DAS penalty with
//     the multiply and the add in separate statements; the tie constants in
//     the first test case were searched so that contracting that expression
//     into a fused multiply-add flips the winner;
//   - ties: first phantom wins; the LATER adjustment wins within a piece.

#ifndef SR_PRECOMPUTE_COLLAPSE_TESTS
#define SR_PRECOMPUTE_COLLAPSE_TESTS

#include <stdexcept>
#include <string>
#include <vector>

// --- synthetic-state builders -------------------------------------------

static PrecomputePossibility makePrecomputePlacement(int relRot, int relX, int relY, int rot, int x,
                                                     int y, const std::string &seq, char tuckInput,
                                                     int dasChargeAfter, int numLinesCleared) {
  PrecomputePossibility p;
  p.relRot = relRot;
  p.relX = relX;
  p.relY = relY;
  p.rot = rot;
  p.x = x;
  p.y = y;
  p.inputSequence = seq;
  p.tuckInput = tuckInput;
  p.dasChargeAfter = dasChargeAfter;
  p.numLinesCleared = numLinesCleared;
  return p;
}

// Standard spawn-relative labels for a rot-0 O piece locked at (x, y=17):
// relRot 0, relX = x - 3, relY = 17 - (-1) = 18.
static PrecomputePossibility oPlacement(int x, const std::string &seq, char tuckInput,
                                        int dasChargeAfter, int numLinesCleared) {
  return makePrecomputePlacement(0, x - 3, 18, 0, x, 17, seq, tuckInput, dasChargeAfter,
                                 numLinesCleared);
}

static PrecomputePhantom defaultBranchPhantom(const PrecomputePossibility &initial) {
  // No adjustments: the collapse scores the phantom's initial placement.
  PrecomputePhantom phantom;
  phantom.hasInitialPlacement = true;
  phantom.initial = initial;
  return phantom;
}

static PrecomputePhantom adjustmentBranchPhantom(
    const PrecomputePossibility &initial, const std::vector<PrecomputePossibility> &adjustments) {
  PrecomputePhantom phantom;
  phantom.hasInitialPlacement = true;
  phantom.initial = initial;
  phantom.possibleAdjustments = adjustments;
  return phantom;
}

static PrecomputePhantom spawnPhantom(const std::vector<PrecomputePossibility> &adjustments) {
  // The reaction-time-zero phantom: no initial placement, adjustments only.
  PrecomputePhantom phantom;
  phantom.hasInitialPlacement = false;
  phantom.possibleAdjustments = adjustments;
  return phantom;
}

static PrecomputeState collapseState(std::vector<PrecomputePhantom> phantoms, bool useDAS,
                                     int reactionTime) {
  PrecomputeState state;
  state.phantoms = phantoms;
  state.useDAS = useDAS;
  state.reactionTime = reactionTime;
  return state;
}

static void putLockValueEverywhere(PrecomputeState &state, const std::string &key, double value) {
  for (int i = 0; i < 7; i++) {
    state.lockValues[i][key] = value;
  }
}

static void putLockValueOnPiece(PrecomputeState &state, int pieceIndex, const std::string &key,
                                double value) {
  state.lockValues[pieceIndex][key] = value;
}

static void putPenaltiesEverywhere(PrecomputeState &state, const std::string &key, double partial,
                                   double min) {
  for (int i = 0; i < 7; i++) {
    state.partialPenalties[i][key] = partial;
    state.minPenalties[i][key] = min;
  }
}

static void putPartialPenaltyEverywhere(PrecomputeState &state, const std::string &key,
                                        double partial) {
  for (int i = 0; i < 7; i++) {
    state.partialPenalties[i][key] = partial;
  }
}

static void putMinPenaltyEverywhere(PrecomputeState &state, const std::string &key, double min) {
  for (int i = 0; i < 7; i++) {
    state.minPenalties[i][key] = min;
  }
}

// Runs the collapse on an empty 200-char board.
static std::string runCollapse(const PrecomputeState &state,
                               const Piece *piece = &PIECE_LIST[IDX_O], int level = 18,
                               int lines = 0, bool pushDown = false) {
  unsigned int board[20];
  encodeBoard(makeEmptyBoard().c_str(), board);
  return collapseAndFormatPrecompute(state, board, piece, level, lines, pushDown);
}

// Seven piece lines carrying the same fallback marker.
static std::string sevenPieceMarkers(const char *marker) {
  std::string out;
  for (int i = 0; i < 7; i++) {
    out += "\n";
    out += "IOLJTSZ"[i];
    out += ":";
    out += marker;
  }
  return out;
}

// Seven identical content piece lines.
static std::string sevenPieceContent(const std::string &line) {
  std::string out;
  for (int i = 0; i < 7; i++) {
    out += "\n";
    out += "IOLJTSZ"[i];
    out += ":";
    out += line;
  }
  return out;
}

// Bottom two rows of the after-lock board for a rot-0 O piece at (x, y=17):
// the O occupies rows 18 and 19, columns x+1 and x+2.
static std::string bottomRows(const std::string &row18, const std::string &row19) {
  std::string board(200, '0');
  for (int i = 0; i < 10; i++) {
    board[180 + i] = row18[i];
    board[190 + i] = row19[i];
  }
  return board;
}

static std::string boardOAt(int x) {
  std::string row(10, '0');
  row[x + 1] = '1';
  row[x + 2] = '1';
  return bottomRows(row, row);
}

// Bottom two rows for a rot-0 T piece at (x=4, y=17): row 18 columns 5,6,7
// and row 19 column 6.
static std::string boardT4() { return bottomRows("0000011100", "0000001000"); }

// Bottom two rows for a rot-0 T piece at (x=2, y=17): row 18 columns 3,4,5
// and row 19 column 4.
static std::string boardT2() { return bottomRows("0001110000", "0000100000"); }

// --- the tests -----------------------------------------------------------

TEST_CASE("collapse keeps the first phantom on an exact value tie (fusion guard)") {
  // The two phantoms' per-piece lock values are deliberately uneven 53-bit
  // decimals chosen by search: their two-rounding totals are BIT-EQUAL (so
  // the strict '>' comparison keeps the first phantom), while a fused
  // multiply-add accumulation over the same constants would give the second
  // phantom the strictly larger total (scratch-verified: two-rounding
  // 0x1.c8dcf34c929b9p-3 for both; fused 0x1.c8dcf34c929b8p-3 vs
  // 0x1.c8dcf34c929bap-3). The multiply/add split in the collapse is
  // therefore load-bearing and pinned here permanently. The values differ
  // on pieces L, S and Z, so the phantoms' per-piece terms are different
  // multisets, not mirror images.
  static const double LOCK_VALUES_A[7] = {1.4573946109665386, 1.3365907603007052,
                                          1.7297523944846018, 1.7634263285605516,
                                          1.7227518561075703, 1.741593702393514,
                                          1.3504765583646701};
  static const double LOCK_VALUES_B[7] = {1.4573946109665386, 1.3365907603007052,
                                          1.7297523944846096, 1.7634263285605516,
                                          1.7227518561075703, 1.7415937023935171,
                                          1.3504765583646623};
  CHECK(LOCK_VALUES_A[2] != LOCK_VALUES_B[2]);
  CHECK(LOCK_VALUES_A[5] != LOCK_VALUES_B[5]);
  CHECK(LOCK_VALUES_A[6] != LOCK_VALUES_B[6]);

  const PrecomputePossibility first =
      makePrecomputePlacement(0, 1, 18, 0, 4, 17, "RRRRRRRR", '.', -1, 0);
  const PrecomputePossibility second =
      makePrecomputePlacement(1, -1, 18, 0, 2, 17, "LLLLLLLL", '.', -1, 0);
  const std::string firstLine =
      "Default:0,1,18|RRRRRRRR~~|" + boardT4() + "|18|0|-1" + sevenPieceMarkers("none");
  const std::string secondLine =
      "Default:1,-1,18|LLLLLLLL~~|" + boardT2() + "|18|0|-1" + sevenPieceMarkers("none");

  PrecomputeState state = collapseState({defaultBranchPhantom(first), defaultBranchPhantom(second)},
                                        /* useDAS= */ false, /* reactionTime= */ 8);
  for (int i = 0; i < 7; i++) {
    putLockValueOnPiece(state, i, encodeLockCell(0, 4, 17), LOCK_VALUES_A[i]);
    putLockValueOnPiece(state, i, encodeLockCell(0, 2, 17), LOCK_VALUES_B[i]);
  }
  // The tie constants were searched against the T transition row
  // (P(T->Z)=12/64, P(T->T)=2/64, the rest 10/64), so the collapse must run
  // with the T piece for the tie to be exact.
  CHECK(runCollapse(state, &PIECE_LIST[IDX_T]) == firstLine);

  // Reversing the order must pick the (then-)first phantom instead: the tie
  // keeps whichever phantom comes first, not this one specifically.
  PrecomputeState reversed =
      collapseState({defaultBranchPhantom(second), defaultBranchPhantom(first)},
                    /* useDAS= */ false, /* reactionTime= */ 8);
  for (int i = 0; i < 7; i++) {
    putLockValueOnPiece(reversed, i, encodeLockCell(0, 4, 17), LOCK_VALUES_A[i]);
    putLockValueOnPiece(reversed, i, encodeLockCell(0, 2, 17), LOCK_VALUES_B[i]);
  }
  CHECK(runCollapse(reversed, &PIECE_LIST[IDX_T]) == secondLine);
}

TEST_CASE("adjustment tie-break resolves equal values toward the later adjustment") {
  // Both adjustments lock the same cell with equal cost and charge, so their
  // values tie exactly; the value >= maxValue comparison keeps scanning and
  // the later adjustment wins.
  PrecomputeState state = collapseState(
      {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                               {oPlacement(4, "abc", '.', -1, 0), oPlacement(4, "xyz", '.', -1, 0)})},
      /* useDAS= */ false, /* reactionTime= */ 8);
  putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
  const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                               + sevenPieceContent("0,1,18|xyz|" + boardOAt(4) + "|18|0|-1");
  CHECK(runCollapse(state) == expected);
}

TEST_CASE("adjustment selection keeps the earlier adjustment when strictly better") {
  PrecomputeState state = collapseState(
      {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                               {oPlacement(4, "abc", '.', -1, 0), oPlacement(2, "xyz", '.', -1, 0)})},
      /* useDAS= */ false, /* reactionTime= */ 8);
  putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
  putLockValueEverywhere(state, encodeLockCell(0, 2, 17), 5.0);
  const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                               + sevenPieceContent("0,1,18|abc|" + boardOAt(4) + "|18|0|-1");
  CHECK(runCollapse(state) == expected);
}

TEST_CASE("per-piece fallback markers follow the reaction-time rule") {
  // Even without adjustments the default placement's lock cell must exist in
  // every piece's map: a missing key poisons the phantom (tested separately),
  // so these sections give every map the key.
  SUBCASE("sequence longer than the reaction time continues") {
    PrecomputeState state =
        collapseState({defaultBranchPhantom(oPlacement(4, "RRRRRRRRR", '.', -1, 0))},
                      /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    const std::string expected =
        "Default:0,1,18|RRRRRRRR~~R|" + boardOAt(4) + "|18|0|-1" + sevenPieceMarkers("continue");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("sequence exactly as long as the reaction time does not continue") {
    PrecomputeState state =
        collapseState({defaultBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0))},
                      /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    const std::string expected =
        "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1" + sevenPieceMarkers("none");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("reaction time zero renders the N/A default line and content piece lines") {
    PrecomputeState state = collapseState({spawnPhantom({oPlacement(4, "....", '.', -1, 0)})},
                                          /* useDAS= */ false, /* reactionTime= */ 0);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
    const std::string expected = "Default:N/A (0 reaction time)"
                                 + sevenPieceContent("0,1,18|....|" + boardOAt(4) + "|18|0|-1");
    CHECK(runCollapse(state) == expected);
  }
}

TEST_CASE("reaction time zero with every adjustment missing the lookup falls back to none") {
  // The guarded degenerate corner: no default placement exists and no
  // adjustment hits any piece's lookup, so every piece line is the plain
  // none fallback. The response still formats: a missed piece contributes
  // the maximisation floor (not NaN), so the phantom's average sits above
  // the floor and wins over the empty starting response.
  PrecomputeState state = collapseState({spawnPhantom({oPlacement(6, "..", '.', -1, 0)})},
                                        /* useDAS= */ false, /* reactionTime= */ 0);
  const std::string expected =
      std::string("Default:N/A (0 reaction time)") + sevenPieceMarkers("none");
  CHECK(runCollapse(state) == expected);

  SUBCASE("a phantom with neither placement nor adjustments is a broken state") {
    PrecomputePhantom broken;
    broken.hasInitialPlacement = false;
    PrecomputeState invalid = collapseState({broken}, /* useDAS= */ false, /* reactionTime= */ 0);
    CHECK_THROWS_AS(runCollapse(invalid), std::runtime_error);
  }
}

TEST_CASE("default line slices the input sequence at the reaction time") {
  // The maps carry the key so the phantom qualifies; the pin is the slice.
  SUBCASE("plain sequence") {
    PrecomputeState state =
        collapseState({defaultBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0))},
                      /* useDAS= */ false, /* reactionTime= */ 4);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    const std::string expected =
        "Default:0,1,18|RRRR~~RRRR|" + boardOAt(4) + "|18|0|-1" + sevenPieceMarkers("continue");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("hold-through-ARE marker advances the slice by one") {
    PrecomputeState state =
        collapseState({defaultBranchPhantom(oPlacement(4, "RR_RRRRR", '.', -1, 0))},
                      /* useDAS= */ false, /* reactionTime= */ 4);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    const std::string expected =
        "Default:0,1,18|RR_RR~~RRR|" + boardOAt(4) + "|18|0|-1" + sevenPieceMarkers("continue");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("empty sequence renders none and is then still sliced and clamped") {
    // The quirk: "none" substitutes for the empty sequence BEFORE slicing,
    // so the four-character substitute is sliced at the reaction time and
    // the line reads "none~~" regardless of the reaction time (4 here).
    PrecomputeState state = collapseState(
        {defaultBranchPhantom(oPlacement(4, "", '.', -1, 0))}, /* useDAS= */ false,
        /* reactionTime= */ 4);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    const std::string expected =
        "Default:0,1,18|none~~|" + boardOAt(4) + "|18|0|-1" + sevenPieceMarkers("none");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("sequence shorter than the reaction time is clamped to its length") {
    PrecomputeState state = collapseState(
        {defaultBranchPhantom(oPlacement(4, "RR", '.', -1, 0))}, /* useDAS= */ false,
        /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    const std::string expected =
        "Default:0,1,18|RR~~|" + boardOAt(4) + "|18|0|-1" + sevenPieceMarkers("none");
    CHECK(runCollapse(state) == expected);
  }
}

TEST_CASE("pushDownSequence rewrites the pre-lock sequence with D fills") {
  // D-fill count = ceil(0.666 * fallen frames) + 2; trailing '.' frames
  // before the lock marker are the fallen frames. The character at index 0
  // is never counted as fallen, and a leading '.' therefore survives.
  CHECK(pushDownSequence("RRR...*") == "RRRDDDD");
  CHECK(pushDownSequence("R......*") == "RDDDDDD");
  CHECK(pushDownSequence("RR*") == "RRDD");
  CHECK(pushDownSequence("....*") == ".DDDD");
  CHECK(pushDownSequence("RRR..X..*") == "RRR..XDDDD");
  // No lock marker: the whole string is pre-lock and trailing dots fall off.
  CHECK(pushDownSequence("RRRR..") == "RRRRDDDD");
}

TEST_CASE("pushdown applies to piece lines only when enabled and the level stays 18") {
  PrecomputeState state = collapseState(
      {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                               {oPlacement(4, "RR.*", '.', -1, 0)})},
      /* useDAS= */ false, /* reactionTime= */ 8);
  putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);

  SUBCASE("enabled at level 18") {
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,1,18|RRDDD|" + boardOAt(4) + "|18|0|-1");
    CHECK(runCollapse(state, &PIECE_LIST[IDX_O], 18, 0, /* pushDown= */ true) == expected);
  }
  SUBCASE("disabled keeps the raw sequence") {
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,1,18|RR.*|" + boardOAt(4) + "|18|0|-1");
    CHECK(runCollapse(state, &PIECE_LIST[IDX_O], 18, 0, /* pushDown= */ false) == expected);
  }
  SUBCASE("level leaving 18 after the placement disables the rewrite") {
    // lines=126 + 4 clears reaches level 19, so the flag alone is not
    // enough; the default line keeps the raw sequence either way.
    PrecomputeState cleared = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(4, "RR.*", 0, -1, 4)})},
        /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(cleared, encodeLockCell(0, 4, 17), 5.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|126|-1"
                                 + sevenPieceContent("0,1,18|RR.*|" + boardOAt(4) + "|19|130|-1");
    CHECK(runCollapse(cleared, &PIECE_LIST[IDX_O], 18, 126, /* pushDown= */ true) == expected);
  }
}

TEST_CASE("level and lines fields cross the transition thresholds") {
  // getLevelAfterLineClears: an 18 start transitions at 126 lines, a 19
  // start at 136, a 29 start at 196 -- and only when the clear crosses a
  // multiple of ten. The piece line here carries the adjustment's clear
  // count; the default line the initial placement's.
  struct ThresholdCase {
    int level;
    int lines;
    int levelAfter;
    const char *description;
  };
  const ThresholdCase cases[] = {
      {18, 125, 18, "18 start below the transition stays"},
      {18, 126, 19, "18 start crossing 126 lines rises"},
      {19, 135, 19, "19 start below the transition stays"},
      {19, 136, 20, "19 start crossing 136 lines rises"},
      {29, 195, 29, "29 start below the transition stays"},
      {29, 196, 30, "29 start crossing 196 lines rises"},
  };
  for (const ThresholdCase &testCase : cases) {
    SUBCASE(testCase.description) {
      PrecomputeState state =
          collapseState({adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", 0, -1, 4),
                                                 {oPlacement(4, "....", 0, -1, 4)})},
                        /* useDAS= */ false, /* reactionTime= */ 8);
      putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
      char defaultTail[32];
      snprintf(defaultTail, sizeof(defaultTail), "|%d|%d|-1", testCase.levelAfter,
               testCase.lines + 4);
      const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + defaultTail
                                   + sevenPieceContent(
                                       std::string("0,1,18|....|") + boardOAt(4) + defaultTail);
      CHECK(runCollapse(state, &PIECE_LIST[IDX_O], testCase.level, testCase.lines) == expected);
    }
  }

  SUBCASE("staying on 18 without a crossing clear") {
    // lines=126 with zero clears does not cross a multiple of ten.
    PrecomputeState state =
        collapseState({adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                               {oPlacement(4, "....", '.', -1, 0)})},
                      /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|126|-1"
                                 + sevenPieceContent("0,1,18|....|" + boardOAt(4) + "|18|126|-1");
    CHECK(runCollapse(state, &PIECE_LIST[IDX_O], 18, 126) == expected);
  }
}

TEST_CASE("probability weighting can reverse the raw per-piece value order") {
  // Current piece T: the transition table gives P(T->Z) = 12/64 and
  // P(T->T) = 2/64. Phantom A is raw-better (200 vs 118 summed over the two
  // pieces) but probability-worse (100*12 + 100*2 = 1400 vs 118*12 = 1416),
  // so B must win the collapse. All arithmetic here is exact dyadic.
  PrecomputeState state = collapseState(
      {defaultBranchPhantom(makePrecomputePlacement(0, 1, 18, 0, 4, 17, "AAAAAAAA", '.', -1, 0)),
       defaultBranchPhantom(makePrecomputePlacement(1, -1, 18, 0, 2, 17, "BBBBBBBB", '.', -1, 0))},
      /* useDAS= */ false, /* reactionTime= */ 8);
  putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 0.0); // A's cell
  putLockValueEverywhere(state, encodeLockCell(0, 2, 17), 0.0); // B's cell
  putLockValueOnPiece(state, IDX_Z, encodeLockCell(0, 4, 17), 100.0);
  putLockValueOnPiece(state, IDX_T, encodeLockCell(0, 4, 17), 100.0);
  putLockValueOnPiece(state, IDX_Z, encodeLockCell(0, 2, 17), 118.0);
  // B's T value stays 0.0: a present zero, not a missing key.
  const std::string expected = "Default:1,-1,18|BBBBBBBB~~|" + boardT2() + "|18|0|-1"
                               + sevenPieceMarkers("none");
  CHECK(runCollapse(state, &PIECE_LIST[IDX_T]) == expected);
}

TEST_CASE("DAS penalty rules choose between zero, the partial map and the min map") {
  // Every section scores one phantom with two same-piece adjustments whose
  // only difference is the cell, the sequence and the DAS charge; the maps
  // are identical across the seven pieces, so all piece lines agree.
  SUBCASE("tap mode ignores the penalty maps entirely") {
    // X: charge 5 at a cell whose maps would penalise; Y: charge 9. In tap
    // mode both penalties vanish, X wins (10 > 9.5); if the tap-mode guard
    // ever leaked, X's -2 penalty would hand the win to Y.
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(4, "....", '.', 5, 0), oPlacement(6, "..", '.', 9, 0)})},
        /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    putLockValueEverywhere(state, encodeLockCell(0, 6, 17), 9.5);
    putPenaltiesEverywhere(state, encodeLockCell(0, 4, 17), -2.0, -2.0);
    putPenaltiesEverywhere(state, encodeLockCell(0, 6, 17), -2.0, -2.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,1,18|....|" + boardOAt(4) + "|18|0|5");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("a charge of 15 or more carries no penalty") {
    // KA would suffer -3.5 from either map if the charge exemption were
    // broken, which would hand the win to KB (9.5 - 2.0 = 7.5).
    for (int exemptCharge = 15; exemptCharge <= 16; exemptCharge++) {
      PrecomputeState state = collapseState(
          {adjustmentBranchPhantom(
              oPlacement(4, "RRRRRRRR", '.', -1, 0),
              {oPlacement(2, "..", '.', 9, 0), oPlacement(4, "....", 0, exemptCharge, 0)})},
          /* useDAS= */ true, /* reactionTime= */ 8);
      putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
      putLockValueEverywhere(state, encodeLockCell(0, 2, 17), 9.5);
      putPenaltiesEverywhere(state, encodeLockCell(0, 4, 17), -3.5, -3.5);
      putPenaltiesEverywhere(state, encodeLockCell(0, 2, 17), -2.0, -2.0);
      const std::string tail = "|18|0|" + std::to_string(exemptCharge);
      const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                   + sevenPieceContent("0,1,18|....|" + boardOAt(4) + tail);
      CHECK(runCollapse(state) == expected);
    }
  }
  SUBCASE("charges 10-14 read the partial map") {
    // KP's partial penalty (-3.5) is harsher than its min penalty (-0.5), so
    // the charge-14 adjustment loses to the charge-5 one exactly when the
    // partial map is consulted; reading the min map instead would flip it.
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(4, "....", '.', 14, 0), oPlacement(2, "..", '.', 5, 0)})},
        /* useDAS= */ true, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    putLockValueEverywhere(state, encodeLockCell(0, 2, 17), 10.0);
    putPenaltiesEverywhere(state, encodeLockCell(0, 4, 17), -3.5, -0.5);
    putPenaltiesEverywhere(state, encodeLockCell(0, 2, 17), -2.0, -1.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,-1,18|..|" + boardOAt(2) + "|18|0|5");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("charges below 10 read the min map") {
    // Mirror of the section above: the charge-9 adjustment must enjoy KP's
    // mild min penalty (-0.5), not its partial penalty (-3.5).
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(4, "....", '.', 9, 0), oPlacement(2, "..", '.', 14, 0)})},
        /* useDAS= */ true, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    putLockValueEverywhere(state, encodeLockCell(0, 2, 17), 10.0);
    putPenaltiesEverywhere(state, encodeLockCell(0, 4, 17), -3.5, -0.5);
    putPenaltiesEverywhere(state, encodeLockCell(0, 2, 17), -2.0, -1.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,1,18|....|" + boardOAt(4) + "|18|0|9");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("a key missing from the partial map zeroes the penalty") {
    // KM has a brutal min penalty but no partial entry: the guard must treat
    // the penalty as zero (KM wins by its raw 10.0), not fall back to the
    // min map (which would score 1.0 and lose to KN's 7.5).
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(4, "....", '.', 5, 0), oPlacement(6, "..", '.', 5, 0)})},
        /* useDAS= */ true, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 10.0);
    putLockValueEverywhere(state, encodeLockCell(0, 6, 17), 9.5);
    putMinPenaltyEverywhere(state, encodeLockCell(0, 4, 17), -9.0);
    putPenaltiesEverywhere(state, encodeLockCell(0, 6, 17), -3.5, -2.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,1,18|....|" + boardOAt(4) + "|18|0|5");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("a key missing from the min map zeroes the penalty") {
    // KO has a mild partial entry but no min entry: the guard must zero the
    // penalty (KO's raw 8.0 beats KN's 9.9 - 2.0 = 7.9); a partial-map
    // fallback would score 7.6 and lose.
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(2, "..", '.', 5, 0), oPlacement(6, "....", '.', 5, 0)})},
        /* useDAS= */ true, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 2, 17), 8.0);
    putLockValueEverywhere(state, encodeLockCell(0, 6, 17), 9.9);
    putPartialPenaltyEverywhere(state, encodeLockCell(0, 2, 17), -0.4);
    putPenaltiesEverywhere(state, encodeLockCell(0, 6, 17), -3.5, -2.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,-1,18|..|" + boardOAt(2) + "|18|0|5");
    CHECK(runCollapse(state) == expected);
  }
}

TEST_CASE("a missing default lock value poisons the phantom total") {
  SUBCASE("the poisoned phantom alone produces none") {
    PrecomputeState state =
        collapseState({defaultBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0))},
                      /* useDAS= */ false, /* reactionTime= */ 8);
    // Piece I's map lacks the key: the NaN total can never beat the floor.
    putLockValueOnPiece(state, IDX_O, encodeLockCell(0, 4, 17), 100.0);
    putLockValueOnPiece(state, IDX_L, encodeLockCell(0, 4, 17), 100.0);
    putLockValueOnPiece(state, IDX_J, encodeLockCell(0, 4, 17), 100.0);
    putLockValueOnPiece(state, IDX_T, encodeLockCell(0, 4, 17), 100.0);
    putLockValueOnPiece(state, IDX_S, encodeLockCell(0, 4, 17), 100.0);
    putLockValueOnPiece(state, IDX_Z, encodeLockCell(0, 4, 17), 100.0);
    CHECK(runCollapse(state) == "none");
  }
  SUBCASE("a healthy competitor wins instead") {
    PrecomputeState state = collapseState(
        {defaultBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0)),
         defaultBranchPhantom(oPlacement(2, "LLLLLLLL", '.', -1, 0))},
        /* useDAS= */ false, /* reactionTime= */ 8);
    // Piece I's map (index 0) is left without the key: A's total is NaN.
    putLockValueOnPiece(state, IDX_O, encodeLockCell(0, 4, 17), 100.0);
    putLockValueEverywhere(state, encodeLockCell(0, 2, 17), 5.0);
    for (int i = IDX_L; i <= IDX_Z; i++) {
      putLockValueOnPiece(state, i, encodeLockCell(0, 4, 17), 100.0);
    }
    const std::string expected = "Default:0,-1,18|LLLLLLLL~~|" + boardOAt(2) + "|18|0|-1"
                                 + sevenPieceMarkers("none");
    CHECK(runCollapse(state) == expected);
  }
}

TEST_CASE("missing keys in the adjustment branch are skipped, not poisoning") {
  // The high-value adjustment misses piece J's lookup only; the collapse
  // must fall back to the low-value adjustment FOR THAT PIECE while the
  // other pieces still take the high-value one.
  PrecomputeState state = collapseState(
      {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRRR", '.', -1, 0),
                               {oPlacement(6, "....", '.', -1, 0), oPlacement(0, "..", '.', -1, 0)})},
      /* useDAS= */ false, /* reactionTime= */ 8);
  putLockValueEverywhere(state, encodeLockCell(0, 0, 17), 5.0);
  for (int i = 0; i < 7; i++) {
    if (i != IDX_J) {
      putLockValueOnPiece(state, i, encodeLockCell(0, 6, 17), 10.0);
    }
  }
  std::string expected = "Default:0,1,18|RRRRRRRR~~R|" + boardOAt(4) + "|18|0|-1";
  for (int i = 0; i < 7; i++) {
    expected += "\n";
    expected += "IOLJTSZ"[i];
    expected += ":";
    if (i == IDX_J) {
      expected += "0,-3,18|..|" + boardOAt(0) + "|18|0|-1";
    } else {
      expected += "0,3,18|....|" + boardOAt(6) + "|18|0|-1";
    }
  }
  CHECK(runCollapse(state) == expected);

  SUBCASE("a piece whose adjustments all miss falls back to its marker") {
    PrecomputeState single = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRRR", '.', -1, 0),
                                 {oPlacement(6, "....", '.', -1, 0)})},
        /* useDAS= */ false, /* reactionTime= */ 8);
    for (int i = 0; i < 7; i++) {
      if (i != IDX_J) {
        putLockValueOnPiece(single, i, encodeLockCell(0, 6, 17), 10.0);
      }
    }
    std::string expected2 = "Default:0,1,18|RRRRRRRR~~R|" + boardOAt(4) + "|18|0|-1";
    for (int i = 0; i < 7; i++) {
      expected2 += "\n";
      expected2 += "IOLJTSZ"[i];
      expected2 += ":";
      expected2 += (i == IDX_J) ? std::string("continue")
                                : "0,3,18|....|" + boardOAt(6) + "|18|0|-1";
    }
    CHECK(runCollapse(single) == expected2);
  }
}

TEST_CASE("tuck and spin input cost is counted twice, as legacy behaviour") {
  // Deliberate behaviour (see adjustmentInputCost): the special
  // character's cost lands once in the per-character sum and once through
  // its tuckInput's inputCostX10ForChar entry. The sections pit a
  // double-counted special adjustment
  // against a plain one whose cost sits between the single- and
  // double-counted readings, so the winning line distinguishes them.
  SUBCASE("spin") {
    // "RA": per-character -0.3, plus -0.2 via the tuck char -> -0.5 total,
    // versus "RRB"'s plain -0.4. The plain adjustment wins; a single-count
    // reading would score the spin -0.3 and flip the winner.
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(4, "RA", 'A', -1, 0), oPlacement(4, "RRB", '.', -1, 0)})},
        /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,1,18|RRB|" + boardOAt(4) + "|18|0|-1");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("spin-tuck") {
    // "EFGI": per-character -1.2, plus -0.3 via the tuck char -> -1.5 total,
    // versus thirteen R shifts' plain -1.3. The plain adjustment wins; a
    // single-count reading would tie at -1.2 and the later adjustment (the
    // spin-tuck) would win instead.
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(
            oPlacement(4, "RRRRRRRR", '.', -1, 0),
            {oPlacement(4, "RRRRRRRRRRRRR", '.', -1, 0), oPlacement(4, "EFGI", 'E', -1, 0)})},
        /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
    const std::string expected =
        "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
        + sevenPieceContent("0,1,18|RRRRRRRRRRRRR|" + boardOAt(4) + "|18|0|-1");
    CHECK(runCollapse(state) == expected);
  }
}

TEST_CASE("dasChargeAfter passes through to the formatted das field") {
  SUBCASE("tap mode carries -1 verbatim") {
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', -1, 0),
                                 {oPlacement(4, "....", '.', -1, 0)})},
        /* useDAS= */ false, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|-1"
                                 + sevenPieceContent("0,1,18|....|" + boardOAt(4) + "|18|0|-1");
    CHECK(runCollapse(state) == expected);
  }
  SUBCASE("DAS mode shows the charge") {
    PrecomputeState state = collapseState(
        {adjustmentBranchPhantom(oPlacement(4, "RRRRRRRR", '.', 7, 0),
                                 {oPlacement(4, "....", '.', 7, 0)})},
        /* useDAS= */ true, /* reactionTime= */ 8);
    putLockValueEverywhere(state, encodeLockCell(0, 4, 17), 5.0);
    putMinPenaltyEverywhere(state, encodeLockCell(0, 4, 17), -2.0);
    // No partial entries: the missing-key guard zeroes the penalty, and the
    // formatted field still shows the charge itself.
    const std::string expected = "Default:0,1,18|RRRRRRRR~~|" + boardOAt(4) + "|18|0|7"
                                 + sevenPieceContent("0,1,18|....|" + boardOAt(4) + "|18|0|7");
    CHECK(runCollapse(state) == expected);
  }
}

#endif // SR_PRECOMPUTE_COLLAPSE_TESTS
