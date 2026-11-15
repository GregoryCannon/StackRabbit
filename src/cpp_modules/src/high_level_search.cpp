#include "high_level_search.hpp"
#include "full_move_search.hpp"
#include "formatting.hpp"
#include "tournament_search.hpp"
#include <algorithm>
#include <list>
#include <string>
#include <unordered_map>
using namespace std;

#define MAP_OFFSET 5000 // An offset to make any placement better than the default 0 in the map

/** Returns pointers to possibilities sorted descending by eval score, preserving tie order. */
std::vector<const Possibility *>
sortPossibilitiesDesc(const std::vector<Possibility> &possibilities) {
  std::vector<const Possibility *> sorted;
  sorted.reserve(possibilities.size());
  for (Possibility const &p : possibilities) {
    sorted.push_back(&p);
  }
  std::stable_sort(sorted.begin(), sorted.end(), [](const Possibility *a, const Possibility *b) {
    return a->evalScoreInclReward > b->evalScoreInclReward;
  });
  return sorted;
}

/** Searches 1-ply from a starting state, and performs an eval on each resulting state.
 * @returns the number of possibilities found
 * @param generateExplanation - when true, captures a per-move evalExplanation string
 *   (only used by GET_TOP_MOVES / GET_TOP_MOVES_HYBRID / GET_MOVE_AB_TEST paths).
 */
int searchDepth1(GameState gameState, const Piece *firstPiece, const EvalContext *evalContext,
                 OUT std::vector<Possibility> &possibilities, bool generateExplanation) {
  vector<LockPlacement> firstLockPlacements;
  fastMoveSearch(gameState, firstPiece, evalContext->pieceRangeContext.inputFrameTimeline,
                 firstLockPlacements);
  possibilities.reserve(possibilities.size() + firstLockPlacements.size());
  for (auto it = begin(firstLockPlacements); it != end(firstLockPlacements); ++it) {
    LockPlacement firstPlacement = *it;

    GameState resultingState = advanceGameState(gameState, firstPlacement, evalContext);
    if (SHOULD_PLAY_PERFECT && ((resultingState.lines - gameState.lines) % 4) != 0) {
      continue; // While playing perfect, ignore any placements that burn lines
    }
    float reward = getLineClearFactor(resultingState.lines - gameState.lines, evalContext->weights,
                                      evalContext->shouldRewardLineClears);
    // Only allocate an explanation string when the caller requested it, to avoid overhead
    // otherwise.
    std::string explanation;
    std::string *explanationPtr = generateExplanation ? &explanation : nullptr;
    float evalScoreInclReward =
        fastEval(gameState, resultingState, firstPlacement, evalContext, explanationPtr);

    Possibility newPossibility = {
        {firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex},
        NULL_LOCK_LOCATION,
        resultingState,
        evalScoreInclReward,
        reward,
        std::move(explanation)};
    possibilities.push_back(std::move(newPossibility));
  }
  return (int)possibilities.size();
}

/** Precomputes first-piece placements, post-placement game states, and line-clear rewards. */
FirstMoveSearchResult buildFirstMoveBranches(const GameState &gameState, const Piece *firstPiece,
                                             int firstPieceDasCharge,
                                             char const *dasInputFrameTimeline,
                                             const EvalContext *evalContext) {
  FirstMoveSearchResult result;
  result.useDas = firstPieceDasCharge >= 0;

  auto appendBranch = [&](const LockPlacement &firstPlacement, int dasChargeAfter) {
    GameState afterFirstMove = advanceGameState(gameState, firstPlacement, evalContext);
    if (SHOULD_PLAY_PERFECT && ((afterFirstMove.lines - gameState.lines) % 4) != 0) {
      return; // While playing perfect, ignore any placements that burn lines
    }
    float firstMoveReward =
        getLineClearFactor(afterFirstMove.lines - gameState.lines, evalContext->weights,
                           evalContext->shouldRewardLineClears);
    result.branches.push_back({firstPlacement, dasChargeAfter, afterFirstMove, firstMoveReward});
  };

  // Get the placements of the first piece. In DAS mode the results come out of
  // fullMoveSearch as DasPossibilities and are converted BY VALUE into
  // LockPlacements -- LockPlacement itself is unchanged (a struct edit regressed
  // the playout benchmark once already; see TESTING.md's benchmarking methodology).
  if (result.useDas) {
    vector<FullMovePossibility> firstFullMovePossibilities;
    firstFullMovePossibilities.reserve(64);
    fullMoveSearch(gameState, firstPiece, dasInputFrameTimeline, firstPieceDasCharge,
                   /* trackInputSequences= */ false, firstFullMovePossibilities);
    result.branches.reserve(firstFullMovePossibilities.size());
    for (FullMovePossibility const &fullMovePossibility : firstFullMovePossibilities) {
      appendBranch({fullMovePossibility.x, fullMovePossibility.y, fullMovePossibility.rotationIndex,
                    -1, fullMovePossibility.tuckInput, firstPiece},
                   fullMovePossibility.dasChargeAfter);
    }
  } else {
    vector<LockPlacement> firstLockPlacements;
    firstLockPlacements.reserve(48);
    fastMoveSearch(gameState, firstPiece, evalContext->pieceRangeContext.inputFrameTimeline,
                   firstLockPlacements);
    result.branches.reserve(firstLockPlacements.size());
    for (LockPlacement const &firstPlacement : firstLockPlacements) {
      appendBranch(firstPlacement, -1);
    }
  }
  return result;
}

/** Runs the second-ply move search and fast eval against precomputed first-piece branches. */
static int searchDepth2FromFirstMoves(const FirstMoveSearchResult &firstMoves,
                                      const Piece *secondPiece, int secondPieceDasCharge,
                                      char const *dasInputFrameTimeline,
                                      const EvalContext *evalContext,
                                      OUT std::vector<Possibility> &possibilities,
                                      bool generateExplanation = false) {
  if (!firstMoves.useDas && secondPieceDasCharge >= 0) {
    fprintf(stderr,
            "Error: searchDepth2FromFirstMoves received secondPieceDasCharge (%d) for tap-mode "
            "first branches.\n",
            secondPieceDasCharge);
    return 0;
  }

  possibilities.reserve(possibilities.size() + firstMoves.branches.size() * 32);

  // Hoist per-branch scratch vectors outside the loop to avoid repeated heap allocations.
  vector<FullMovePossibility> secondFullMovePossibilities;
  secondFullMovePossibilities.reserve(64);
  vector<LockPlacement> secondLockPlacements;
  secondLockPlacements.reserve(64);

  for (FirstMoveBranch const &branch : firstMoves.branches) {
    const LockPlacement &firstPlacement = branch.firstPlacement;
    const GameState &afterFirstMove = branch.afterFirstMove;
    float firstMoveReward = branch.firstMoveReward;
    maybePrint("\n\n\n\nNEW FIRST MOVE: rot=%d x=%d\n", firstPlacement.rotationIndex,
               firstPlacement.x);

    for (int i = 0; i < 19; i++) {
      maybePrint("%d ", (afterFirstMove.board[i] & ALL_TUCK_SETUP_BITS) >> 20);
    }
    maybePrint("%d end of post first move\n",
               (afterFirstMove.board[19] & ALL_TUCK_SETUP_BITS) >> 20);
    if (LOGGING_ENABLED) {
      printBoard(afterFirstMove.board);
    }

    auto appendSecondPlacement = [&](const LockPlacement &secondPlacement) {
      GameState resultingState = advanceGameState(afterFirstMove, secondPlacement, evalContext);
      if (SHOULD_PLAY_PERFECT && ((resultingState.lines - afterFirstMove.lines) % 4) != 0) {
        return; // While playing perfect, ignore any placements that burn lines
      }
      // Only allocate an explanation string when the caller requested it, to avoid overhead
      // otherwise.
      std::string explanation;
      std::string *explanationPtr = generateExplanation ? &explanation : nullptr;
      float evalScore =
          firstMoveReward
          + fastEval(afterFirstMove, resultingState, secondPlacement, evalContext, explanationPtr);
      float secondMoveReward =
          getLineClearFactor(resultingState.lines - afterFirstMove.lines, evalContext->weights,
                             evalContext->shouldRewardLineClears);

      possibilities.push_back(
          {{firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex},
           {secondPlacement.x, secondPlacement.y, secondPlacement.rotationIndex},
           resultingState,
           evalScore,
           firstMoveReward + secondMoveReward,
           std::move(explanation)});
    };

    // Get the placements of the second piece
    if (firstMoves.useDas) {
      // DAS mode: the second piece spawns with the first placement's leftover charge
      // by default (generic semantics), or with secondPieceDasCharge when the caller
      // prescribes it.
      secondFullMovePossibilities.clear();
      int effectiveSecondPieceDasCharge =
          secondPieceDasCharge >= 0 ? secondPieceDasCharge : branch.dasChargeAfter;
      fullMoveSearch(afterFirstMove, secondPiece, dasInputFrameTimeline,
                     effectiveSecondPieceDasCharge,
                     /* trackInputSequences= */ false, secondFullMovePossibilities);
      for (FullMovePossibility const &fullMovePossibility : secondFullMovePossibilities) {
        appendSecondPlacement({fullMovePossibility.x, fullMovePossibility.y,
                               fullMovePossibility.rotationIndex, -1, fullMovePossibility.tuckInput,
                               secondPiece});
      }
    } else {
      secondLockPlacements.clear();
      fastMoveSearch(afterFirstMove, secondPiece, evalContext->pieceRangeContext.inputFrameTimeline,
                     secondLockPlacements);
      for (LockPlacement const &secondPlacement : secondLockPlacements) {
        appendSecondPlacement(secondPlacement);
      }
    }
  }
  return (int)possibilities.size();
}

/** Searches 2-ply from a starting state, and performs a fast eval on each of the resulting states.
 * @returns the number of possibilities found (0 if the arguments are invalid)
 * @param dasCharge - the first piece's incoming DAS charge: -1 = tap mode, 0..16 = DAS
 *   mode (searches run through fullMoveSearch on dasInputFrameTimeline, the player's real
 *   timeline). In DAS mode the second piece inherits each first placement's dasChargeAfter.
 * @param secondPieceDasCharge - DAS bucket mode: prescribes the SECOND piece's starting
 *   charge instead of inheriting it (the first piece is then always searched from a full
 *   charge, DAS_CHARGE_MAX). Used by the DAS penalty buckets to evaluate the hypothesis
 *   "the player ends the first placement with Q left over" -- the inherited value must
 *   not be used there because it is path-dependent (the same lock cell can end at 16 via
 *   a wall-charged drop or at 0 via a tuck).
 *   Mutually exclusive with dasCharge: at most one of the two may be >= 0 (checked below).
 * @param generateExplanation - when true, captures a per-move evalExplanation string
 *   (only used by GET_TOP_MOVES / GET_TOP_MOVES_HYBRID / GET_MOVE_AB_TEST paths).
 */
int searchDepth2(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                 int dasCharge, int secondPieceDasCharge, char const *dasInputFrameTimeline,
                 const EvalContext *evalContext, OUT std::vector<Possibility> &possibilities,
                 bool generateExplanation) {
  // The two DAS modes are mutually exclusive: dasCharge describes the first piece's
  // incoming charge (second piece inherits), secondPieceDasCharge prescribes the second
  // piece's charge for the bucket hypothesis. Passing both is a programmer error.
  if (dasCharge >= 0 && secondPieceDasCharge >= 0) {
    // TODO: this logs and returns an empty search rather than throwing, so the
    // caller silently sees "no legal moves". Decide the contract: reject loudly
    // (throw, surfacing as a 500) or define a documented precedence.
    fprintf(stderr,
            "Error: searchDepth2 received both dasCharge (%d) and secondPieceDasCharge (%d) -- "
            "they are mutually exclusive. Pass dasCharge for a generic DAS search, or "
            "secondPieceDasCharge for the DAS bucket hypothesis.\n",
            dasCharge, secondPieceDasCharge);
    return 0;
  }
  const bool useDas = dasCharge >= 0 || secondPieceDasCharge >= 0;
  // Bucket mode (secondPieceDasCharge >= 0) always searches the first piece from a
  // full charge so every bucket/deep lookup enumerates the same set of lock cells.
  const int firstPieceDasCharge =
      useDas ? (secondPieceDasCharge >= 0 ? DAS_CHARGE_MAX : dasCharge) : -1;

  const FirstMoveSearchResult firstMoves = buildFirstMoveBranches(
      gameState, firstPiece, firstPieceDasCharge, dasInputFrameTimeline, evalContext);
  return searchDepth2FromFirstMoves(firstMoves, secondPiece, secondPieceDasCharge,
                                    dasInputFrameTimeline, evalContext, possibilities,
                                    generateExplanation);
}

/** Plays one move from a given state, with or without knowledge of the next box.
 * @param outExplanation - optional output for the chosen move's eval explanation.
 *   When non-null, explanations are captured during search (used by GET_MOVE_AB_TEST);
 *   when null, no explanation strings are allocated.
 */
LockLocation playOneMove(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                         int numCandidatesToPlayout, int playoutCount, int playoutLength,
                         const EvalContext *evalContext,
                         const PieceRangeContext pieceRangeContextLookup[3],
                         std::string *outExplanation) {
  std::vector<Possibility> possibilities;
  // Only capture explanations when the caller asked for them (AB-test path).
  bool generateExplanation = (outExplanation != nullptr);

  // Search depth either 1 or 2 depending on whether a next piece was provided
  const Piece *lastSeenPiece;
  if (secondPiece == NULL) {
    searchDepth1(gameState, firstPiece, evalContext, possibilities, generateExplanation);
    lastSeenPiece = firstPiece;
  } else {
    searchDepth2(gameState, firstPiece, secondPiece, /* dasCharge= */ -1,
                 /* secondPieceDasCharge= */ -1, evalContext->pieceRangeContext.inputFrameTimeline,
                 evalContext, possibilities, generateExplanation);
    lastSeenPiece = secondPiece;
  }

  if (possibilities.empty()) {
    if (outExplanation != nullptr) {
      *outExplanation = "";
    }
    return NULL_LOCK_LOCATION; // Return an invalid lock location to indicate the agent has topped
                               // out
  }
  const std::vector<const Possibility *> sorted = sortPossibilitiesDesc(possibilities);

  if (playoutCount * playoutLength == 0) {
    // Return the highest-eval possibility
    const Possibility &best = *sorted[0];
    if (outExplanation != nullptr) {
      *outExplanation = best.evalExplanation;
    }
    return best.firstPlacement;
  }

  LockLocation bestLockLocation = {NONE, NONE, NONE};
  float bestPossibilityScore = FLOAT_MIN;
  std::string bestExplanation;
  int numPlayedOut = 0;
  for (const Possibility *possibilityPtr : sorted) {
    if (numPlayedOut >= numCandidatesToPlayout) {
      break;
    }
    Possibility const &possibility = *possibilityPtr;
    float overallScore =
        possibility.immediateReward
        + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength,
                          pieceRangeContextLookup, lastSeenPiece->index,
                          evalContext->aggressionLevel, /* playoutDataList */ NULL);

    maybePrint("Possibility %d %d has overallscore %f %f\n",
               possibility.firstPlacement.rotationIndex, possibility.firstPlacement.x - 3,
               overallScore, possibility.evalScoreInclReward);

    // Potentially update the best possibility
    if (bestLockLocation.x == NONE || overallScore > bestPossibilityScore) {
      bestLockLocation = possibility.firstPlacement;
      bestPossibilityScore = overallScore;
      bestExplanation = possibility.evalExplanation;
    }
    numPlayedOut++;
  }

  if (SHOULD_PLAY_PERFECT && bestPossibilityScore < 0.0001) {
    // Game is over
    if (outExplanation != nullptr) {
      *outExplanation = "";
    }
    return {NONE, NONE, NONE};
  }
  if (outExplanation != nullptr) {
    *outExplanation = bestExplanation;
  }
  return bestLockLocation;
}

/**
 * Checks whether a game state's board matches a player-provided resulting board.
 * Only the cell bits are compared, since player boards carry none of the
 * auxiliary hole/tuck metadata.
 */
bool boardMatchesPlayerBoard(const GameState &gameState, unsigned int playerBoardAfter[20]) {
  for (int i = 19; i >= 0; i--) {
    if (playerBoardAfter[i] != (gameState.board[i] & FULL_ROW)) {
      return false;
    }
  }
  return true;
}

/**
 * Finds the possibility whose resulting board equals the player's resulting board.
 * @returns true and sets outPossibility on match, false otherwise.
 */
bool findPossibilityByPlayerBoard(std::vector<Possibility> const &possibilities,
                                  unsigned int playerBoardAfter[20],
                                  OUT Possibility &outPossibility) {
  for (auto const &possibility : possibilities) {
    if (boardMatchesPlayerBoard(possibility.resultingState, playerBoardAfter)) {
      outPossibility = possibility;
      return true;
    }
  }
  return false;
}

/**
 * Finds the move out of possibilities that has the resulting board equal to the player's
 * resulting board. NB: REMOVES THE ELEMENT IN-PLACE (to avoid re-running its playouts
 * later in rateMove()).
 */
Possibility findPlayerMove(std::vector<Possibility> &possibilities,
                           unsigned int playerBoardAfter[20]) {
  for (auto iter = possibilities.begin(); iter != possibilities.end(); iter++) {
    if (boardMatchesPlayerBoard(iter->resultingState, playerBoardAfter)) {
      Possibility match = *iter;
      possibilities.erase(iter);
      return match;
    }
  }
  // Error out
  return {NULL_LOCK_LOCATION, NULL_LOCK_LOCATION, {}, -1, -1, ""};
}

std::string rateMove(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                     unsigned int playerBoardAfter[20], int numCandidatesToPlayout,
                     int playoutCount, int playoutLength, const EvalContext *evalContext,
                     const PieceRangeContext pieceRangeContextLookup[3]) {
  std::vector<Possibility> possibilitiesD1;
  std::vector<Possibility> possibilitiesD2;
  bool hasNb = secondPiece != NULL;

  // Search depth 1
  searchDepth1(gameState, firstPiece, evalContext, possibilitiesD1);
  if (hasNb) {
    searchDepth2(gameState, firstPiece, secondPiece, /* dasCharge= */ -1,
                 /* secondPieceDasCharge= */ -1, evalContext->pieceRangeContext.inputFrameTimeline,
                 evalContext, possibilitiesD2);
  }
  if (possibilitiesD1.empty() || (hasNb && possibilitiesD2.empty())) {
    return std::string("Error: no legal moves found");
  }

  // Find the player move (and remove it from the D1 possibility vector)
  Possibility playerMove = findPlayerMove(possibilitiesD1, playerBoardAfter);
  if (playerMove.firstPlacement.x
      == NONE) { // Check for the particular error value supplied by the function
    return std::string("Error: player move not found");
  }

  // Sort the rest of the possibilities
  const std::vector<const Possibility *> sortedD1 =
      sortPossibilitiesDesc(possibilitiesD1); // Does not include player move
  const std::vector<const Possibility *> sortedD2 =
      hasNb ? sortPossibilitiesDesc(possibilitiesD2)
            : std::vector<const Possibility *>{}; // Includes player move

  float playerValNoAdj = FLOAT_MIN;
  float bestValNoAdj = FLOAT_MIN;
  float playerValAfterAdj = FLOAT_MIN;
  float bestValAfterAdj = FLOAT_MIN;

  // NO PLAYOUTS NEEDED
  if (playoutCount * playoutLength == 0) {
    // NNB
    // If no playouts are requested, add the NNB values based on the already sorted list
    playerValNoAdj = playerMove.evalScoreInclReward;
    bestValNoAdj = playerValNoAdj;
    if (!sortedD1.empty()) {
      float bestOtherVal = sortedD1[0]->evalScoreInclReward;
      bestValNoAdj = std::max(playerValNoAdj, bestOtherVal);
    }

    if (hasNb) {
      // WITH NB
      // Find the best NB values, as well as the best NB value that uses the player move for the
      // first move
      bestValAfterAdj = sortedD2[0]->evalScoreInclReward;
      for (const Possibility *possibilityPtr : sortedD2) {
        if (lockLocationEquals(possibilityPtr->firstPlacement, playerMove.firstPlacement)) {
          playerValAfterAdj = possibilityPtr->evalScoreInclReward;
          break;
        }
      }
    }
  }
  // PLAYOUTS NEEDED
  else {
    // NNB Playouts (first on the player move, then on the rest)
    playerValNoAdj = playerMove.immediateReward
                     + getPlayoutScore(playerMove.resultingState, playoutCount, playoutLength,
                                       pieceRangeContextLookup, firstPiece->index,
                                       evalContext->aggressionLevel, /* playoutDataList */ NULL);

    bestValNoAdj = playerValNoAdj;
    int numPlayedOut = 0;
    for (const Possibility *possibilityPtr : sortedD1) {
      if (numPlayedOut >= numCandidatesToPlayout) {
        break;
      }
      Possibility const &possibility = *possibilityPtr;
      float overallScore =
          possibility.immediateReward
          + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength,
                            pieceRangeContextLookup, firstPiece->index,
                            evalContext->aggressionLevel, /* playoutDataList */ NULL);
      if (overallScore > bestValNoAdj) {
        bestValNoAdj = overallScore;
      }
      numPlayedOut++;
    }

    if (hasNb) {
      // NB Playouts
      bestValAfterAdj = FLOAT_MIN;
      playerValAfterAdj = FLOAT_MIN;
      numPlayedOut = 0;
      // Play out the top `numCandidatesToPlayout` continuations, plus any continuations
      // whose first placement matches the player's (the sorted list is fully scanned
      // to find them, but scanning without playouts is cheap). The player's move can
      // rank arbitrarily low in this list, so it must never gate early termination.
      for (const Possibility *possibilityPtr : sortedD2) {
        Possibility const &possibility = *possibilityPtr;
        bool inTopSet = numPlayedOut < numCandidatesToPlayout;
        bool isPlayerSibling =
            lockLocationEquals(playerMove.firstPlacement, possibility.firstPlacement);
        if (inTopSet || isPlayerSibling) {
          float overallScore =
              possibility.immediateReward
              + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength,
                                pieceRangeContextLookup, secondPiece->index,
                                evalContext->aggressionLevel, /* playoutDataList */ NULL);
          if (overallScore > bestValAfterAdj) {
            bestValAfterAdj = overallScore;
          }
          if (isPlayerSibling && overallScore > playerValAfterAdj) {
            playerValAfterAdj = overallScore;
          }
          numPlayedOut++;
        }
      }
    }
  }

  return formatRateMove(playerValNoAdj, bestValNoAdj, playerValAfterAdj, bestValAfterAdj, hasNb);
}

/**
 * Runs full playouts for a single possibility and inserts the result into the
 * sorted move list (ordered by playout score).
 * @returns true if the move had legal playouts and was added, false otherwise.
 */
bool playoutAndInsert(Possibility const &possibility, int playoutCount, int playoutLength,
                      const PieceRangeContext pieceRangeContextLookup[3], int lastSeenPieceIndex,
                      AggressionLevel aggressionLevel, bool generateExplanation, bool isPlayerMove,
                      OUT list<EngineMoveData> &sortedList) {
  vector<PlayoutData> playoutDataList = {};
  float overallScore = possibility.immediateReward
                       + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength,
                                         pieceRangeContextLookup, lastSeenPieceIndex,
                                         aggressionLevel, &playoutDataList);

  // If this position has no legal playouts, ignore it
  if (playoutDataList.size() == 0) {
    return false;
  }
  // Pick 7 playouts from the sorted playout list
  int len = (int)playoutDataList.size();
  EngineMoveData newMoveData = {
      possibility.firstPlacement,
      possibility.secondPlacement,
      /* playoutScore */ overallScore,
      /* shallowEvalScore */ possibility.evalScoreInclReward,
      /* resultingBoard */ formatBoard(possibility.resultingState.board),
      /* playout1 (best case) */ playoutDataList.at(0),
      /* playout2 (83 %ile case) */ playoutDataList.at(len / 6), // Fractions are "backwards"
                                                                 // because moves are ordered best
                                                                 // (100%ile) to worst (0%ile).
      /* playout3 (66 %ile case) */ playoutDataList.at(len / 3),
      /* playout4 (median case) */ playoutDataList.at(len / 2),
      /* playout5 (33 %ile case) */ playoutDataList.at(len * 2 / 3),
      /* playout6 (16 %ile case) */ playoutDataList.at(len * 5 / 6),
      /* playout7 (worst case) */ playoutDataList.at(len - 1),
      /* evalExplanation */ generateExplanation ? possibility.evalExplanation : std::string(""),
      /* isPlayerMove */ isPlayerMove,
  };
  insertIntoList(newMoveData, sortedList);
  return true;
}

/**
 * Gets a list of the top moves, formatted as a JSON string. (See formatting.hpp for exact format
 * details).
 * @param generateExplanation - when true, each EngineMoveData includes an evalExplanation
 *   string (used by GET_TOP_MOVES / GET_TOP_MOVES_HYBRID); when false, no explanation
 *   strings are allocated.
 * @param playerBoardAfter - optional board resulting from the player's move (i.e. secondBoard).
 *   When provided, the returned list is guaranteed to include the player's move
 *   (appended in sorted order, usually last) even if it wouldn't otherwise make the top N.
 *   Included moves have isPlayerMove set to true in EngineMoveData and in the JSON output.
 */
std::string getTopMoveList(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                           int pruningBreadth, int playoutCount, int playoutLength,
                           const EvalContext *evalContext,
                           const PieceRangeContext pieceRangeContextLookup[3],
                           bool generateExplanation, unsigned int *playerBoardAfter) {
  std::vector<Possibility> possibilities;
  list<EngineMoveData> sortedList;

  // Search depth either 1 or 2 depending on whether a next piece was provided
  const Piece *lastSeenPiece;
  if (secondPiece == NULL) {
    searchDepth1(gameState, firstPiece, evalContext, possibilities, generateExplanation);
    lastSeenPiece = firstPiece;
  } else {
    searchDepth2(gameState, firstPiece, secondPiece, /* dasCharge= */ -1,
                 /* secondPieceDasCharge= */ -1, evalContext->pieceRangeContext.inputFrameTimeline,
                 evalContext, possibilities, generateExplanation);
    lastSeenPiece = secondPiece;
  }

  if (possibilities.empty()) {
    return "No legal moves";
  }
  const std::vector<const Possibility *> initiallySorted = sortPossibilitiesDesc(possibilities);

  // Perform playouts on the promising possibilities
  int numAdded = 0;
  for (const Possibility *possibilityPtr : initiallySorted) {
    if (numAdded >= pruningBreadth) {
      break;
    }
    Possibility const &possibility = *possibilityPtr;
    // printf("Doing playout for: %s %s\n", encodeLockPosition(possibility.firstPlacement).c_str(),
    // encodeLockPosition(possibility.secondPlacement).c_str());
    if (playoutAndInsert(possibility, playoutCount, playoutLength, pieceRangeContextLookup,
                         lastSeenPiece->index, evalContext->aggressionLevel, generateExplanation,
                         /* isPlayerMove= */ false, sortedList)) {
      numAdded++;
    }
  }

  // If a player board was provided, ensure the player's move is included even if it
  // didn't make the top N (common RATE_MOVE -> GET_TOP_MOVES flow).
  if (playerBoardAfter != nullptr) {
    bool foundPlayer = false;
    Possibility playerPossibility = {NULL_LOCK_LOCATION, NULL_LOCK_LOCATION, {}, 0, 0, ""};
    if (secondPiece == NULL) {
      // Depth-1: match the resulting board directly.
      foundPlayer =
          findPossibilityByPlayerBoard(possibilities, playerBoardAfter, playerPossibility);
    } else {
      // Depth-2: secondBoard only describes the board after the *first* piece.
      // First resolve the player's first placement via a depth-1 search, then pick
      // the best depth-2 continuation sharing that first placement.
      std::vector<Possibility> depth1Possibilities;
      searchDepth1(gameState, firstPiece, evalContext, depth1Possibilities, generateExplanation);
      Possibility depth1PlayerMove = {};
      LockLocation playerFirstPlacement = NULL_LOCK_LOCATION;
      if (findPossibilityByPlayerBoard(depth1Possibilities, playerBoardAfter, depth1PlayerMove)) {
        playerFirstPlacement = depth1PlayerMove.firstPlacement;
      }
      if (playerFirstPlacement.x != NONE) {
        bool bestUnset = true;
        float bestEval = FLOAT_MIN;
        for (auto const &possibility : possibilities) {
          if (lockLocationEquals(possibility.firstPlacement, playerFirstPlacement)) {
            if (bestUnset || possibility.evalScoreInclReward > bestEval) {
              bestUnset = false;
              bestEval = possibility.evalScoreInclReward;
              playerPossibility = possibility;
              foundPlayer = true;
            }
          }
        }
      }
    }

    if (foundPlayer) {
      // Check if the player move is already in the top N.
      bool playerMoveInTopN = false;
      for (auto &move : sortedList) {
        if (lockLocationEquals(move.firstPlacement, playerPossibility.firstPlacement)) {
          move.isPlayerMove = true;
          playerMoveInTopN = true;
          break;
        }
      }
      // If the player move wasn't already played out, do the playouts now
      if (!playerMoveInTopN) {
        playoutAndInsert(playerPossibility, playoutCount, playoutLength, pieceRangeContextLookup,
                         lastSeenPiece->index, evalContext->aggressionLevel, generateExplanation,
                         /* isPlayerMove= */ true, sortedList);
      }
    }
  }

  return formatEngineMoveList(sortedList, firstPiece, secondPiece);
}

static unordered_map<string, float>
getLockValueLookupFromPossibilities(const std::vector<Possibility> &possibilities,
                                    int pruningBreadth, int playoutCount, int playoutLength,
                                    const Piece *secondPiece, const EvalContext *evalContext,
                                    const PieceRangeContext pieceRangeContextLookup[3]) {
  unordered_map<string, float> lockValueMap;

  // If no playouts, just use the eval
  if (playoutCount * playoutLength == 0) {
    for (Possibility const &possibility : possibilities) {
      string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
      float overallScore = possibility.evalScoreInclReward;
      if (lockValueMap.count(lockPosEncoded) == 0 || overallScore > lockValueMap[lockPosEncoded]) {
        lockValueMap[lockPosEncoded] = overallScore;
      }
    }
  } else {
    unordered_map<string, int> lockValueRepeatMap;
    int numSorted = 99999;

    const std::vector<const Possibility *> sortedPtrs = sortPossibilitiesDesc(possibilities);

    // Perform playouts on the promising possibilities
    int i = 0;
    int numPlayedOut = 0;
    // The repeat cap is a fraction of the breadth, but a cap of 0 would disqualify every
    // possibility from playouts entirely (leaving the whole map at the deathCoef sentinel),
    // so small breadths always allow at least one pair per lock position.
    for (const Possibility *possibilityPtr : sortedPtrs) {
      Possibility const &possibility = *possibilityPtr;
      string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
      // Cap the number of times a lock position can be repeated (despite differing second
      // placements)
      int shouldPlayout = i < numSorted
                          && numPlayedOut < pruningBreadth
                          && lockValueRepeatMap[lockPosEncoded] < LOCK_POSITION_REPEAT_CAP;
      if (PLAYOUT_LOGGING_ENABLED) {
        printf("\n----%s, repeats %d, willPlay %d\n", lockPosEncoded.c_str(),
               lockValueRepeatMap[lockPosEncoded], shouldPlayout);
      }
      lockValueRepeatMap[lockPosEncoded] += 1;

      float overallScore =
          (shouldPlayout
               ? possibility.immediateReward
                     + getPlayoutScore(possibility.resultingState, playoutCount, playoutLength,
                                       pieceRangeContextLookup, secondPiece->index,
                                       evalContext->aggressionLevel, /* playoutDataList */ NULL)
               : (SHOULD_PLAY_PERFECT ? 0 : evalContext->weights.deathCoef));

      if (lockValueMap.count(lockPosEncoded) == 0 || overallScore > lockValueMap[lockPosEncoded]) {
        if (PLAYOUT_LOGGING_ENABLED || PLAYOUT_RESULT_LOGGING_ENABLED) {
          if (shouldPlayout) {
            printf("Adding to map: %s %f (%f + %f)\n", lockPosEncoded.c_str(), overallScore,
                   possibility.immediateReward, overallScore - possibility.immediateReward);
          }
        }
        lockValueMap[lockPosEncoded] = overallScore;
      } else if (PLAYOUT_LOGGING_ENABLED || PLAYOUT_RESULT_LOGGING_ENABLED) {
        if (shouldPlayout) {
          printf("Score of %.1f is worse than existing move %.1f\n", overallScore,
                 lockValueMap[lockPosEncoded]);
        }
      }
      i++;
      if (shouldPlayout) {
        numPlayedOut++;
      }
    }
  }

  return lockValueMap;
}

/** Calculates the valuation of every possible terminal position for a given piece on a given board,
 * and stores it in a map.
 * @param dasCharge - First piece's incoming DAS charge: -1 = tap mode, 0..16 = DAS mode (see
 * searchDepth2). Mutually exclusive with secondPieceDasCharge (searchDepth2 errors out if both are
 * >= 0).
 * @param secondPieceDasCharge - DAS bucket mode: forces the second piece's starting charge (the
 * bucket hypothesis) instead of inheriting the first placement's dasChargeAfter; the first piece is
 * then searched from DAS_CHARGE_MAX. -1 = not in bucket mode.
 * @param dasInputFrameTimeline - The player's real input frame timeline, consumed by the DAS move
 * search in DAS mode.
 */
unordered_map<string, float>
getLockValueLookup(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                   int pruningBreadth, int playoutCount, int playoutLength, int dasCharge,
                   int secondPieceDasCharge, char const *dasInputFrameTimeline,
                   const EvalContext *evalContext,
                   const PieceRangeContext pieceRangeContextLookup[3]) {
  std::vector<Possibility> possibilities;
  searchDepth2(gameState, firstPiece, secondPiece, dasCharge, secondPieceDasCharge,
               dasInputFrameTimeline, evalContext, possibilities);
  return getLockValueLookupFromPossibilities(possibilities, pruningBreadth, playoutCount,
                                             playoutLength, secondPiece, evalContext,
                                             pieceRangeContextLookup);
}

std::string encodeMapToJSON(unordered_map<string, float> map) {
  // Encode lookup to JSON
  std::string mapEncoded = std::string("{");
  // float globalMax = 0; // Only used for perfect play
  for (const auto &n : map) {
    char mapEntryBuf[30];
    snprintf(mapEntryBuf, 30, "\"%s\":%.2f,", n.first.c_str(), n.second);
    mapEncoded.append(mapEntryBuf);
    // if (SHOULD_PLAY_PERFECT){
    //   globalMax = std::max(globalMax, n.second);
    // }
  }
  // if (SHOULD_PLAY_PERFECT && globalMax < FLOAT_EPSILON){
  //   return "{\"abort\": true}";
  // }
  if (map.size() > 0) {
    mapEncoded.pop_back(); // Remove the last comma
  }
  mapEncoded.append("}");
  return mapEncoded;
}

/** Calculates the valuation of every possible terminal position for a given piece on a given board,
 * and returns it as a map (see getLockValueLookupEncoded, its JSON-encoding wrapper).
 * @param pruningBreadth - -1 = tournament mode: playoutCount is interpreted as the TOTAL playout
 * budget instead.
 *
 * Thread-safety contract: precompute calls this once per next piece on concurrent threads. It must
 * remain thread-safe, along with everything reachable from it (enforced by `npm run
 * precompute_tsan`; see AGENTS.md's rule against new non-const file-scope statics and
 * thread-unsafe libc calls on this path).
 */
unordered_map<string, float>
getLockValueLookupMapped(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                         int pruningBreadth, int playoutCount, int playoutLength,
                         const EvalContext *evalContext,
                         const PieceRangeContext pieceRangeContextLookup[3]) {
  const FirstMoveSearchResult firstMoves =
      buildFirstMoveBranches(gameState, firstPiece, /* firstPieceDasCharge= */ -1,
                             evalContext->pieceRangeContext.inputFrameTimeline, evalContext);
  return getLockValueLookupMappedWithFirstMoves(firstMoves, secondPiece, pruningBreadth,
                                                playoutCount, playoutLength, evalContext,
                                                pieceRangeContextLookup);
}

static unordered_map<string, float>
dispatchLookupFromPossibilities(const std::vector<Possibility> &possibilities,
                                const Piece *secondPiece, int pruningBreadth, int playoutCount,
                                int playoutLength, const EvalContext *evalContext,
                                const PieceRangeContext pieceRangeContextLookup[3]) {
  if (pruningBreadth == -1) {
    return getLockValueLookupTournamentFromPossibilities(possibilities, secondPiece, pruningBreadth,
                                                         playoutCount, playoutLength, evalContext,
                                                         pieceRangeContextLookup);
  }
  return getLockValueLookupFromPossibilities(possibilities, pruningBreadth, playoutCount,
                                             playoutLength, secondPiece, evalContext,
                                             pieceRangeContextLookup);
}

/** Variant of getLockValueLookupMapped using precomputed tap-mode firstMoves shared read-only
 * across concurrent per-next-piece precompute threads (see thread-safety contract above).
 */
unordered_map<string, float> getLockValueLookupMappedWithFirstMoves(
    const FirstMoveSearchResult &firstMoves, const Piece *secondPiece, int pruningBreadth,
    int playoutCount, int playoutLength, const EvalContext *evalContext,
    const PieceRangeContext pieceRangeContextLookup[3]) {
  if (firstMoves.useDas) {
    fprintf(stderr,
            "Error: getLockValueLookupMappedWithFirstMoves expects tap-mode first moves.\n");
    return {};
  }
  std::vector<Possibility> possibilities;
  searchDepth2FromFirstMoves(firstMoves, secondPiece, /* secondPieceDasCharge= */ -1,
                             evalContext->pieceRangeContext.inputFrameTimeline, evalContext,
                             possibilities);
  return dispatchLookupFromPossibilities(possibilities, secondPiece, pruningBreadth, playoutCount,
                                         playoutLength, evalContext, pieceRangeContextLookup);
}

std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece,
                                      const Piece *secondPiece, int pruningBreadth,
                                      int playoutCount, int playoutLength,
                                      const EvalContext *evalContext,
                                      const PieceRangeContext pieceRangeContextLookup[3]) {
  return encodeMapToJSON(getLockValueLookupMapped(gameState, firstPiece, secondPiece,
                                                  pruningBreadth, playoutCount, playoutLength,
                                                  evalContext, pieceRangeContextLookup));
}

/** Runs the full DAS-penalty lookup for one next piece: three low-depth bucket lookups (full /
 * partial / minimum second-piece charge) converted to relative penalties, plus one deep lookup for
 * the raw values (see getLockValueLookupDas, its JSON-encoding wrapper).
 *
 * Thread-safety contract: precompute calls this once per next piece on concurrent threads. It must
 * remain thread-safe, along with everything reachable from it (enforced by `npm run
 * precompute_tsan`; see AGENTS.md's rule against new non-const file-scope statics and
 * thread-unsafe libc calls on this path).
 */
DasLockValueMaps getLockValueLookupDasMaps(GameState gameState, const Piece *firstPiece,
                                           const Piece *secondPiece, int pruningBreadth,
                                           int playoutCount, int playoutLength,
                                           char const *dasInputFrameTimeline,
                                           const EvalContext *evalContext,
                                           const PieceRangeContext pieceRangeContextLookup[3]) {
  const FirstMoveSearchResult firstMoves = buildFirstMoveBranches(
      gameState, firstPiece, DAS_CHARGE_MAX, dasInputFrameTimeline, evalContext);
  return getLockValueLookupDasMapsWithFirstMoves(firstMoves, secondPiece, pruningBreadth,
                                                 playoutCount, playoutLength, dasInputFrameTimeline,
                                                 evalContext, pieceRangeContextLookup);
}

/** Variant of getLockValueLookupDasMaps using precomputed full-charge firstMoves shared read-only
 * across concurrent per-next-piece precompute threads (see thread-safety contract above), reusing
 * the full-charge (15) possibility vector for the deep playouts when playouts are enabled.
 */
DasLockValueMaps getLockValueLookupDasMapsWithFirstMoves(
    const FirstMoveSearchResult &firstMoves, const Piece *secondPiece, int pruningBreadth,
    int playoutCount, int playoutLength, char const *dasInputFrameTimeline,
    const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]) {
  if (!firstMoves.useDas) {
    fprintf(stderr,
            "Error: getLockValueLookupDasMapsWithFirstMoves expects DAS-mode first moves.\n");
    return {};
  }
  DasLockValueMaps maps;
  // First, do multiple shallow searches at low depth to check if finishing the first placement with
  // partial or no DAS charge sabotages the next piece's placements. E.g. if the next piece will
  // need full DAS (i.e. an I piece going all the way to the right), losing DAS during the first
  // piece would be bad, and the penalty should be calculated to be high. However, if the best move
  // for the next piece can still be done with a DAS charge of 0, then the penalties should all be
  // zero and the agent has free reign to lose DAS during the first piece.
  //
  // These method calls pass in values for the SECOND piece DAS charge since we're concerned with
  // how bad the DAS can be *after* the first piece without sacrificing eval quality.
  // Full charge baseline that the penalties are measured against. (Low-depth to be a fair
  // comparison). Also reused below for the deep playouts when enabled.
  std::vector<Possibility> fullChargePossibilities;
  searchDepth2FromFirstMoves(firstMoves, secondPiece, /* secondPieceDasCharge= */ 15,
                             dasInputFrameTimeline, evalContext, fullChargePossibilities);
  maps.lockValue = dispatchLookupFromPossibilities(
      fullChargePossibilities, secondPiece, pruningBreadth, /* playoutCount= */ 0,
      /* playoutLength= */ 0, evalContext, pieceRangeContextLookup);

  // Partial charge: 10 is the post-shift charge of a standard DAS shift (5 held
  // frames away from the next shift).
  std::vector<Possibility> bucketPossibilities;
  searchDepth2FromFirstMoves(firstMoves, secondPiece, /* secondPieceDasCharge= */ 10,
                             dasInputFrameTimeline, evalContext, bucketPossibilities);
  maps.partialPenalty = dispatchLookupFromPossibilities(
      bucketPossibilities, secondPiece, pruningBreadth, /* playoutCount= */ 0,
      /* playoutLength= */ 0, evalContext, pieceRangeContextLookup);

  // Minimum charge: 5 is the worst useful charge, since even with 0 charge you can
  // just first-frame tap without holding during ARE, and it ends up being exactly equivalent:
  // Charge < 5 : shift->0  1  2  3   4  5  6  7  8  9    10
  // Charge = 5:   6       7  8  9  10 11 12 13 14 15 shift->10
  bucketPossibilities.clear();
  searchDepth2FromFirstMoves(firstMoves, secondPiece, /* secondPieceDasCharge= */ 5,
                             dasInputFrameTimeline, evalContext, bucketPossibilities);
  maps.minPenalty = dispatchLookupFromPossibilities(
      bucketPossibilities, secondPiece, pruningBreadth, /* playoutCount= */ 0,
      /* playoutLength= */ 0, evalContext, pieceRangeContextLookup);

  // Replace the raw value maps with maps of the penalty relative to the best placement (as negative
  // numbers). NB: all four lookups share one first-piece search, so their key sets match; the guard
  // below is a safety net. Skipped keys carry no penalty, matching the
  // consumer's missing-key -> no-penalty rule.
  for (const auto &pair : maps.lockValue) {
    string key = pair.first;
    float value = pair.second;
    if (maps.partialPenalty.count(key) == 0 || maps.minPenalty.count(key) == 0) {
      continue;
    }
    float penaltyPartial = maps.partialPenalty.at(key) - value;
    float penaltyMin = maps.minPenalty.at(key) - value;

    // A positive penalty means a lower-charge bucket scored the first placement HIGHER
    // than full charge. This happens when the low-charge search finds maneuvers that
    // are out of the engine's scope (e.g. double-tuck chains, which are deliberately
    // omitted for complexity) -- the maneuver is real but the engine must not reward
    // it, so clamp the delta at zero. The consumer adds these values to lock
    // values, so clamping keeps "no advantage from losing DAS" instead of inflating
    // the low-charge continuation.
    maps.partialPenalty[key] = std::min(penaltyPartial, 0.0f);
    maps.minPenalty[key] = std::min(penaltyMin, 0.0f);
  }

  // Then, do one deep search to see what the actual best move is, irrespective of DAS.
  if (playoutCount * playoutLength != 0) {
    maps.lockValue = dispatchLookupFromPossibilities(fullChargePossibilities, secondPiece,
                                                     pruningBreadth, playoutCount, playoutLength,
                                                     evalContext, pieceRangeContextLookup);
  }

  return maps;
}

std::string getLockValueLookupDas(GameState gameState, const Piece *firstPiece,
                                  const Piece *secondPiece, int pruningBreadth, int playoutCount,
                                  int playoutLength, char const *dasInputFrameTimeline,
                                  const EvalContext *evalContext,
                                  const PieceRangeContext pieceRangeContextLookup[3]) {
  const DasLockValueMaps maps = getLockValueLookupDasMaps(
      gameState, firstPiece, secondPiece, pruningBreadth, playoutCount, playoutLength,
      dasInputFrameTimeline, evalContext, pieceRangeContextLookup);

  // Compile result as array of JSONs
  std::string resultStr = std::string("[\n");
  resultStr.append(encodeMapToJSON(maps.partialPenalty));
  resultStr.append(",\n");
  resultStr.append(encodeMapToJSON(maps.minPenalty));
  resultStr.append(",\n");
  resultStr.append(encodeMapToJSON(maps.lockValue));
  resultStr.append("\n]");
  return resultStr;
}

// void evaluatePossibilitiesWithPlayouts(int timeoutMs){
//   auto millisec_since_epoch =
//   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
//
// }
