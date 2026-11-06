#include "high_level_search.hpp"
#include "das_move_search.hpp"
#include "formatting.hpp"
#include "params.hpp"
#include "tournament_search.hpp"
#include <limits>
#include <math.h>
#include <string>
#include <unordered_map>
using namespace std;

#define MAP_OFFSET 5000 // An offset to make any placement better than the default 0 in the map

/**
 * Performs a partial insertion sort such that the highest N elements of the list are guaranteed to
 * be sorted and kept at the front of the list. The other elements can be anywhere.
 */
void partiallySortPossibilityList(list<Possibility> &possibilityList, int pruningBreadth,
                                  OUT list<Possibility> &sortedList) {
  auto cutoffPossibility =
      possibilityList
          .begin(); // The node on the "cutoff" between being in the top N placements and not
  int size = 0;     // Tracking manually is cheaper than doing the O(n) operation each iteration

  for (auto it = begin(possibilityList); it != end(possibilityList); it++) {
    Possibility newPossibility = *it;
    if (size < pruningBreadth
        || newPossibility.evalScoreInclReward > cutoffPossibility->evalScoreInclReward) {
      // Insert into the list in its correct sorted place
      for (auto it2 = sortedList.begin(); true; it2++) {
        if (it2 == sortedList.end()) {
          sortedList.push_back(newPossibility);
          size++;
          break;
        }
        if (newPossibility.evalScoreInclReward > it2->evalScoreInclReward) {
          sortedList.insert(it2, newPossibility);
          size++;
          break;
        }
      }

      // Update the cutoff node
      if (size == pruningBreadth) {
        // Get the Nth elt
        auto ptr = sortedList.begin();
        advance(ptr, pruningBreadth - 1);
        cutoffPossibility = ptr;
      }
      if (size > pruningBreadth) {
        // Use the predecessor of the one that just got pushed out
        cutoffPossibility = prev(cutoffPossibility);
      }

    } else {
      // Add it at the end of the list (sorting not important)
      sortedList.push_back(newPossibility);
    }
  }
}

/** Searches 1-ply from a starting state, and performs an eval on each resulting state.
 * @returns an UNSORTED list of evaluated possibilities
 * @param generateExplanation - when true, captures a per-move evalExplanation string
 *   (only used by GET_TOP_MOVES / GET_TOP_MOVES_HYBRID / GET_MOVE_AB_TEST paths).
 */
int searchDepth1(GameState gameState, const Piece *firstPiece, const EvalContext *evalContext,
                 OUT list<Possibility> &possibilityList, bool generateExplanation) {
  vector<LockPlacement> firstLockPlacements;
  moveSearch(gameState, firstPiece, evalContext->pieceRangeContext.inputFrameTimeline,
             firstLockPlacements);
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
        explanation};
    possibilityList.push_back(newPossibility);
  }
  return (int)possibilityList.size();
}

/** Searches 2-ply from a starting state, and performs a fast eval on each of the resulting states.
 * @returns an UNSORTED list of evaluated possibilities (0 if the arguments are invalid)
 * @param dasCharge - the first piece's incoming DAS charge: -1 = tap mode, 0..16 = DAS
 *   mode (searches run through dasMoveSearch on dasInputFrameTimeline, the player's real
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
                 const EvalContext *evalContext, OUT list<Possibility> &possibilityList,
                 bool generateExplanation) {

  // The two DAS modes are mutually exclusive: dasCharge describes the first piece's
  // incoming charge (second piece inherits), secondPieceDasCharge prescribes the second
  // piece's charge for the bucket hypothesis. Passing both is a programmer error.
  if (dasCharge >= 0 && secondPieceDasCharge >= 0) {
    fprintf(stderr,
            "Error: searchDepth2 received both dasCharge (%d) and secondPieceDasCharge (%d) -- "
            "they are mutually "
            "exclusive. Pass dasCharge for a generic DAS search, or secondPieceDasCharge for the "
            "DAS bucket hypothesis.\n",
            dasCharge, secondPieceDasCharge);
    return 0;
  }
  const bool useDas = dasCharge >= 0 || secondPieceDasCharge >= 0;

  // Get the placements of the first piece. In DAS mode the results come out of
  // dasMoveSearch as DasPossibilities and are converted BY VALUE into
  // LockPlacements -- LockPlacement itself is unchanged (a struct edit regressed
  // the playout benchmark once already; see TESTING.md's benchmarking methodology).
  vector<LockPlacement> firstLockPlacements;
  // DAS mode only: the charge left over after each first placement, index-aligned
  // with firstLockPlacements. By default the second piece spawns with it; the
  // secondPieceDasCharge override (bucket searches) replaces it.
  vector<int> firstDasChargeAfters;
  if (useDas) {
    // Bucket mode (secondPieceDasCharge >= 0) always searches the first piece from a
    // full charge so every bucket/deep lookup enumerates the same set of lock cells.
    int firstPieceDasCharge = secondPieceDasCharge >= 0 ? DAS_CHARGE_MAX : dasCharge;
    vector<DasPossibility> firstDasPossibilities;
    dasMoveSearch(gameState, firstPiece, dasInputFrameTimeline, firstPieceDasCharge,
                  /* trackInputSequences= */ false, firstDasPossibilities);
    for (DasPossibility const &dasPossibility : firstDasPossibilities) {
      firstLockPlacements.push_back({dasPossibility.x, dasPossibility.y,
                                     dasPossibility.rotationIndex, -1, dasPossibility.tuckInput,
                                     firstPiece});
      firstDasChargeAfters.push_back(dasPossibility.dasChargeAfter);
    }
  } else {
    moveSearch(gameState, firstPiece, evalContext->pieceRangeContext.inputFrameTimeline,
               firstLockPlacements);
  }
  int firstPlacementIndex = -1;
  for (auto it = begin(firstLockPlacements); it != end(firstLockPlacements); ++it) {
    firstPlacementIndex++;
    LockPlacement firstPlacement = *it;
    maybePrint("\n\n\n\nNEW FIRST MOVE: rot=%d x=%d\n", firstPlacement.rotationIndex,
               firstPlacement.x);

    GameState afterFirstMove = advanceGameState(gameState, firstPlacement, evalContext);
    if (SHOULD_PLAY_PERFECT && ((afterFirstMove.lines - gameState.lines) % 4) != 0) {
      continue; // While playing perfect, ignore any placements that burn lines
    }
    for (int i = 0; i < 19; i++) {
      maybePrint("%d ", (afterFirstMove.board[i] & ALL_TUCK_SETUP_BITS) >> 20);
    }
    maybePrint("%d end of post first move\n",
               (afterFirstMove.board[19] & ALL_TUCK_SETUP_BITS) >> 20);
    if (LOGGING_ENABLED) {
      printBoard(afterFirstMove.board);
    }

    float firstMoveReward =
        getLineClearFactor(afterFirstMove.lines - gameState.lines, evalContext->weights,
                           evalContext->shouldRewardLineClears);

    // Get the placements of the second piece
    vector<LockPlacement> secondLockPlacements;
    if (useDas) {
      // DAS mode: the second piece spawns with the first placement's leftover charge
      // by default (generic semantics), or with secondPieceDasCharge when the caller
      // prescribes it.
      vector<DasPossibility> secondDasPossibilities;
      int effectiveSecondPieceDasCharge = secondPieceDasCharge >= 0
                                              ? secondPieceDasCharge
                                              : firstDasChargeAfters[firstPlacementIndex];
      dasMoveSearch(afterFirstMove, secondPiece, dasInputFrameTimeline,
                    effectiveSecondPieceDasCharge,
                    /* trackInputSequences= */ false, secondDasPossibilities);
      for (DasPossibility const &dasPossibility : secondDasPossibilities) {
        secondLockPlacements.push_back({dasPossibility.x, dasPossibility.y,
                                        dasPossibility.rotationIndex, -1, dasPossibility.tuckInput,
                                        secondPiece});
      }
    } else {
      moveSearch(afterFirstMove, secondPiece, evalContext->pieceRangeContext.inputFrameTimeline,
                 secondLockPlacements);
    }

    for (auto secondPlacement : secondLockPlacements) {
      GameState resultingState = advanceGameState(afterFirstMove, secondPlacement, evalContext);
      if (SHOULD_PLAY_PERFECT && ((resultingState.lines - afterFirstMove.lines) % 4) != 0) {
        continue; // While playing perfect, ignore any placements that burn lines
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

      Possibility newPossibility = {
          {firstPlacement.x, firstPlacement.y, firstPlacement.rotationIndex},
          {secondPlacement.x, secondPlacement.y, secondPlacement.rotationIndex},
          resultingState,
          evalScore,
          firstMoveReward + secondMoveReward,
          explanation};

      possibilityList.push_back(newPossibility);
    }
  }
  return (int)possibilityList.size();
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
  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> sortedList;
  // Only capture explanations when the caller asked for them (AB-test path).
  bool generateExplanation = (outExplanation != nullptr);

  // Search depth either 1 or 2 depending on whether a next piece was provided
  const Piece *lastSeenPiece;
  if (secondPiece == NULL) {
    searchDepth1(gameState, firstPiece, evalContext, possibilityList, generateExplanation);
    lastSeenPiece = firstPiece;
  } else {
    searchDepth2(gameState, firstPiece, secondPiece, /* dasCharge= */ -1,
                 /* secondPieceDasCharge= */ -1, evalContext->pieceRangeContext.inputFrameTimeline,
                 evalContext, possibilityList, generateExplanation);
    lastSeenPiece = secondPiece;
  }

  if (possibilityList.size() == 0) {
    if (outExplanation != nullptr) {
      *outExplanation = "";
    }
    return NULL_LOCK_LOCATION; // Return an invalid lock location to indicate the agent has topped
                               // out
  }
  partiallySortPossibilityList(possibilityList, numCandidatesToPlayout, sortedList);

  if (playoutCount * playoutLength == 0) {
    // Return the first element in the preliminary sorted list
    const Possibility &best = *sortedList.begin();
    if (outExplanation != nullptr) {
      *outExplanation = best.evalExplanation;
    }
    return best.firstPlacement;
  }

  LockLocation bestLockLocation = {NONE, NONE, NONE};
  float bestPossibilityScore = FLOAT_MIN;
  std::string bestExplanation;
  int numPlayedOut = 0;
  for (auto possibility : sortedList) {
    if (numPlayedOut >= numCandidatesToPlayout) {
      break;
    }
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
bool findPossibilityByPlayerBoard(list<Possibility> const &possibilityList,
                                  unsigned int playerBoardAfter[20],
                                  OUT Possibility &outPossibility) {
  for (auto const &possibility : possibilityList) {
    if (boardMatchesPlayerBoard(possibility.resultingState, playerBoardAfter)) {
      outPossibility = possibility;
      return true;
    }
  }
  return false;
}

/**
 * Finds the move out of a list of possibilities that has the resulting board equal to the player's
 * resulting board. NB: REMOVES THE ELEMENT FROM THE LIST IN-PLACE (to avoid re-running its playouts
 * later in rateMove()).
 */
Possibility findPlayerMove(list<Possibility> &possibilityList, unsigned int playerBoardAfter[20]) {
  for (auto iter = possibilityList.begin(); iter != possibilityList.end(); iter++) {
    if (boardMatchesPlayerBoard(iter->resultingState, playerBoardAfter)) {
      Possibility match = *iter;
      possibilityList.erase(iter);
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
  list<Possibility> possibilityListD1;
  list<Possibility> possibilityListD2;
  list<Possibility> sortedListD1; // Does not include player move
  list<Possibility> sortedListD2; // Includes player move
  bool hasNb = secondPiece != NULL;

  // Search depth 1
  searchDepth1(gameState, firstPiece, evalContext, possibilityListD1);
  if (hasNb) {
    searchDepth2(gameState, firstPiece, secondPiece, /* dasCharge= */ -1,
                 /* secondPieceDasCharge= */ -1, evalContext->pieceRangeContext.inputFrameTimeline,
                 evalContext, possibilityListD2);
  }
  if (possibilityListD1.size() == 0 || (hasNb && possibilityListD2.size() == 0)) {
    return std::string("Error: no legal moves found");
  }

  // Find the player move (and remove it from the D1 possibility list)
  Possibility playerMove = findPlayerMove(possibilityListD1, playerBoardAfter);
  if (playerMove.firstPlacement.x
      == NONE) { // Check for the particular error value supplied by the function
    return std::string("Error: player move not found");
  }

  // Sort the rest of the possibilities
  partiallySortPossibilityList(possibilityListD1, numCandidatesToPlayout, sortedListD1);
  if (hasNb) {
    partiallySortPossibilityList(possibilityListD2, 999999,
                                 sortedListD2); // Large numCandidatesToPlayout = full sort
  }

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
    if (sortedListD1.size() > 0) {
      float bestOtherVal = (*sortedListD1.begin()).evalScoreInclReward;
      bestValNoAdj = std::max(playerValNoAdj, bestOtherVal);
    }

    if (hasNb) {
      // WITH NB
      // Find the best NB values, as well as the best NB value that uses the player move for the
      // first move
      bestValAfterAdj = (*sortedListD2.begin()).evalScoreInclReward;
      for (auto possibility : sortedListD2) {
        if (lockLocationEquals(possibility.firstPlacement, playerMove.firstPlacement)) {
          playerValAfterAdj = possibility.evalScoreInclReward;
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
    for (auto possibility : sortedListD1) {
      if (numPlayedOut >= numCandidatesToPlayout) {
        break;
      }
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
      for (auto possibility : sortedListD2) {
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
  // Keep a running list of the top X possibilities as the move search is happening.
  // Keep twice as many as we'll eventually need, since some duplicates may be removed before
  // playouts start
  int numSorted = pruningBreadth * 2;

  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> initiallySortedList;
  list<EngineMoveData> sortedList;

  // Search depth either 1 or 2 depending on whether a next piece was provided
  const Piece *lastSeenPiece;
  if (secondPiece == NULL) {
    searchDepth1(gameState, firstPiece, evalContext, possibilityList, generateExplanation);
    lastSeenPiece = firstPiece;
  } else {
    searchDepth2(gameState, firstPiece, secondPiece, /* dasCharge= */ -1,
                 /* secondPieceDasCharge= */ -1, evalContext->pieceRangeContext.inputFrameTimeline,
                 evalContext, possibilityList, generateExplanation);
    lastSeenPiece = secondPiece;
  }

  if (possibilityList.size() == 0) {
    return "No legal moves";
  }
  partiallySortPossibilityList(possibilityList, numSorted, initiallySortedList);

  // Perform playouts on the promising possibilities
  int numAdded = 0;
  for (Possibility const &possibility : initiallySortedList) {
    if (numAdded >= pruningBreadth) {
      break;
    }
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
          findPossibilityByPlayerBoard(possibilityList, playerBoardAfter, playerPossibility);
    } else {
      // Depth-2: secondBoard only describes the board after the *first* piece.
      // First resolve the player's first placement via a depth-1 search, then pick
      // the best depth-2 continuation sharing that first placement.
      list<Possibility> depth1List;
      searchDepth1(gameState, firstPiece, evalContext, depth1List, generateExplanation);
      Possibility depth1PlayerMove = {};
      LockLocation playerFirstPlacement = NULL_LOCK_LOCATION;
      if (findPossibilityByPlayerBoard(depth1List, playerBoardAfter, depth1PlayerMove)) {
        playerFirstPlacement = depth1PlayerMove.firstPlacement;
      }
      if (playerFirstPlacement.x != NONE) {
        bool bestUnset = true;
        float bestEval = FLOAT_MIN;
        for (auto const &possibility : possibilityList) {
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

/** Calculates the valuation of every possible terminal position for a given piece on a given board,
 * and stores it in a map.
 * @param pruningBreadth - How many possibilities to evaluate via a full set of playouts, as opposed
 * to just the eval function.
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
  unordered_map<string, float> lockValueMap;
  unordered_map<string, int> lockValueRepeatMap;

  // Keep a running list of the top X possibilities as the move search is happening.
  // Keep twice as many as we'll eventually need, since some duplicates may be removed before
  // playouts start
  int numSorted = 99999;

  // Get the list of evaluated possibilities
  list<Possibility> possibilityList;
  list<Possibility> sortedList;
  searchDepth2(gameState, firstPiece, secondPiece, dasCharge, secondPieceDasCharge,
               dasInputFrameTimeline, evalContext, possibilityList);
  partiallySortPossibilityList(possibilityList, numSorted, sortedList);

  // If no playouts, just use the eval
  if (playoutCount * playoutLength == 0) {
    for (Possibility const &possibility : sortedList) {
      string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
      float overallScore = possibility.evalScoreInclReward;
      if (lockValueMap.count(lockPosEncoded) == 0 || overallScore > lockValueMap[lockPosEncoded]) {
        lockValueMap[lockPosEncoded] = overallScore;
      }
    }
  } else {
    // Perform playouts on the promising possibilities
    int i = 0;
    int numPlayedOut = 0;
    // The repeat cap is a fraction of the breadth, but a cap of 0 would disqualify every
    // possibility from playouts entirely (leaving the whole map at the deathCoef sentinel),
    // so small breadths always allow at least one pair per lock position.
    for (Possibility const &possibility : sortedList) {
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
 * and stores it in a map.
 * @param pruningBreadth - How many possibilities to evaluate via a full set of playouts, as opposed
 * to just the eval function. -1 = tournament mode: playoutCount is interpreted as the TOTAL playout
 * budget instead.
 */
std::string getLockValueLookupEncoded(GameState gameState, const Piece *firstPiece,
                                      const Piece *secondPiece, int pruningBreadth,
                                      int playoutCount, int playoutLength,
                                      const EvalContext *evalContext,
                                      const PieceRangeContext pieceRangeContextLookup[3]) {
  unordered_map<string, float> lockValueMap;
  if (pruningBreadth == -1) {
    lockValueMap = getLockValueLookupTournament(
        gameState, firstPiece, secondPiece, pruningBreadth, playoutCount, playoutLength,
        /* dasCharge= */ -1, /* secondPieceDasCharge= */ -1,
        evalContext->pieceRangeContext.inputFrameTimeline, evalContext, pieceRangeContextLookup);
  } else {
    lockValueMap = getLockValueLookup(
        gameState, firstPiece, secondPiece, pruningBreadth, playoutCount, playoutLength,
        /* dasCharge= */ -1, /* secondPieceDasCharge= */ -1,
        evalContext->pieceRangeContext.inputFrameTimeline, evalContext, pieceRangeContextLookup);
  }
  return encodeMapToJSON(lockValueMap);
}

/**
 * Runs one lock value lookup for a DAS bucket, in either the tournament or legacy style
 * depending on pruningBreadth (see getLockValueLookupDas). The bucket prescribes the
 * second piece's starting charge; searchDepth2 then searches the first piece from a full
 * charge (DAS_CHARGE_MAX), so every bucket/deep lookup enumerates the same lock cells.
 *
 * These method calls pass in values for the SECOND piece DAS charge since we're concerned
 * with how bad the DAS can be *after* the first piece without sacrificing eval quality.
 */
static unordered_map<string, float>
getLockValueLookupForDasBucket(GameState gameState, const Piece *firstPiece,
                               const Piece *secondPiece, int pruningBreadth, int playoutCount,
                               int playoutLength, int secondPieceDasCharge,
                               char const *dasInputFrameTimeline, const EvalContext *evalContext,
                               const PieceRangeContext pieceRangeContextLookup[3]) {
  if (pruningBreadth == -1) {
    return getLockValueLookupTournament(
        gameState, firstPiece, secondPiece, pruningBreadth, playoutCount, playoutLength,
        /* dasCharge= */ -1, secondPieceDasCharge, dasInputFrameTimeline, evalContext,
        pieceRangeContextLookup);
  }
  return getLockValueLookup(gameState, firstPiece, secondPiece, pruningBreadth, playoutCount,
                            playoutLength,
                            /* dasCharge= */ -1, secondPieceDasCharge, dasInputFrameTimeline,
                            evalContext, pieceRangeContextLookup);
}

std::string getLockValueLookupDas(GameState gameState, const Piece *firstPiece,
                                  const Piece *secondPiece, int pruningBreadth, int playoutCount,
                                  int playoutLength, char const *dasInputFrameTimeline,
                                  const EvalContext *evalContext,
                                  const PieceRangeContext pieceRangeContextLookup[3]) {
  // First, do multiple shallow searches at low depth to check if finishing the first placement with
  // partial or no DAS charge sabotages the next piece's placements. E.g. if the next piece will
  // need full DAS (i.e. an I piece going all the way to the right), losing DAS during the first
  // piece would be bad, and the penalty should be calculated to be high. However, if the best move
  // for the next piece can still be done with a DAS charge of 0, then the penalties should all be
  // zero and the agent has free reign to lose DAS during the first piece.
  //
  // These method calls pass in values for the SECOND piece DAS charge since we're concerned with
  // how bad the DAS can be *after* the first piece without sacrificing eval quality.
  unordered_map<string, float> shallowMapFullCharge;
  unordered_map<string, float> shallowMapPartialCharge;
  unordered_map<string, float> shallowMapMinCharge;
  // Full charge baseline that the penalties are measured against. (Low-depth to be a fair
  // comparison).
  shallowMapFullCharge = getLockValueLookupForDasBucket(
      gameState, firstPiece, secondPiece, pruningBreadth, /* playoutCount= */ 0,
      /* playoutLength= */ 0,
      /* secondPieceDasCharge= */ 15, dasInputFrameTimeline, evalContext, pieceRangeContextLookup);

  // Partial charge: 10 is the post-shift charge of a standard DAS shift (5 held
  // frames away from the next shift).
  shallowMapPartialCharge = getLockValueLookupForDasBucket(
      gameState, firstPiece, secondPiece, pruningBreadth, /* playoutCount= */ 0,
      /* playoutLength= */ 0,
      /* secondPieceDasCharge= */ 10, dasInputFrameTimeline, evalContext, pieceRangeContextLookup);

  // Minimum charge: 5 is the worst useful charge, since even with 0 charge you can
  // just first-frame tap without holding during ARE, and it ends up being exactly equivalent:
  // Charge < 5 : shift->0  1  2  3   4  5  6  7  8  9    10
  // Charge = 5:   6       7  8  9  10 11 12 13 14 15 shift->10
  shallowMapMinCharge = getLockValueLookupForDasBucket(
      gameState, firstPiece, secondPiece, pruningBreadth, /* playoutCount= */ 0,
      /* playoutLength= */ 0,
      /* secondPieceDasCharge= */ 5, dasInputFrameTimeline, evalContext, pieceRangeContextLookup);

  // Replace the raw value maps with maps of the penalty relative to the best placement (as negative
  // numbers). NB: all four lookups share one first-piece search, so their key sets match; the guard
  // below is a safety net. Skipped keys match TS's getDasPenalty missing-key -> no-penalty
  // behavior.
  for (const auto &pair : shallowMapFullCharge) {
    string key = pair.first;
    float value = pair.second;
    if (shallowMapPartialCharge.count(key) == 0 || shallowMapMinCharge.count(key) == 0) {
      continue;
    }
    float penaltyPartial = shallowMapPartialCharge.at(key) - value;
    float penaltyMin = shallowMapMinCharge.at(key) - value;
    shallowMapPartialCharge[key] = penaltyPartial;
    shallowMapMinCharge[key] = penaltyMin;
  }

  // Then, do one deep search to see what the actual best move is, irrespective of DAS.
  unordered_map<string, float> deepValue = getLockValueLookupForDasBucket(
      gameState, firstPiece, secondPiece, pruningBreadth, playoutCount, playoutLength,
      /* secondPieceDasCharge= */ 15, dasInputFrameTimeline, evalContext, pieceRangeContextLookup);

  // Compile result as array of JSONs
  std::string resultStr = std::string("[\n");
  resultStr.append(encodeMapToJSON(shallowMapPartialCharge));
  resultStr.append(",\n");
  resultStr.append(encodeMapToJSON(shallowMapMinCharge));
  resultStr.append(",\n");
  resultStr.append(encodeMapToJSON(deepValue));
  resultStr.append("\n]");
  return resultStr;
}

// void evaluatePossibilitiesWithPlayouts(int timeoutMs){
//   auto millisec_since_epoch =
//   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
//
// }
