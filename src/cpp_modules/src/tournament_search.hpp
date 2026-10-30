#ifndef TOURNAMENT_SEARCH
#define TOURNAMENT_SEARCH

#include "types.hpp"
#include "utils.hpp"
#include "config.hpp"
#include <list>
#include <unordered_map>

/**
 * Tournament-based candidate selection for the lock value lookup.
 *
 * The legacy lookup spends its playout budget on the top pairs *by eval score*, wasting
 * playouts on dominated continuations of the same first move and starving moves the eval
 * underrates (eval blind spots). The tournament instead treats each first placement as a
 * candidate and its continuations as samples of that candidate's value, in 3 rounds:
 *
 *   Round 1: every candidate plays its eval-best continuation with the screening budget
 *            (wide net, one cont per candidate so the sample count stays meaningful).
 *   Cut:     candidates significantly worse than the leader are vetoed (paired difference
 *            test on the shared round-1 window), then the field is capped at
 *            TMT_FINALIST_COUNT candidates (ranked optimistically: mean + z*SE).
 *   Finals:  the finalists' eval-best continuations split the remaining budget (deep samples).
 *
 * In tournament mode (pruningBreadth == -1 from the request) playoutCount is used as the total budget, 
 * to be divided up dynamically between all the possible candidates
 *
 * Noise: entries in a round share one window of the canonical sequence batch,, so comparisons 
 * are paired and playout luck mostly cancels. Scores accumulate cumulatively across rounds.
 *
 * The result is a map of first placement -> best overall score, where every first placement
 * searchDepth2 finds gets a playout-backed estimate published as a conservative lower
 * confidence bound (see getTournamentContPublishScore in tournament_search.cpp). This keeps
 * shallow round-1/round-2 estimates comparable with the finalists' deep round-3 estimates:
 * a lucky small-sample mean can never publish as a great score, so the map's ranking is
 * trustworthy even though most candidates only get a handful of playouts.
 */


// ----- Config for tournament-style playouts -----

// Max second-piece placements (continuations) that are tested per first-piece placement.
constexpr int TMT_CONTINUATIONS_PER_CANDIDATE = 3;

// The cap on how many candidates reach the finals
constexpr int TMT_FINALIST_COUNT = 8;

// Even if low budgets are specified, no placement can get less than 10 playouts (otherwise that would be too much noise)
constexpr int TMT_MIN_PLAYOUTS = 30;

// Share of the total playout budget spent on the initial screen (the finals get the remainder)
// NB: the screen plays one continuation per candidate, so its per-entry sample count is
// totalBudget * TMT_R1_BUDGET_SHARE / candidateCount.
constexpr float TMT_R1_BUDGET_SHARE = 0.5f;

// Paired-difference test: eliminate a candidate if the mean difference vs the leader exceeds
// this many standard errors (1.96 ~= one-sided 97.5% confidence)
constexpr float TMT_PAIRED_TEST_Z = 1.96f;




std::unordered_map<std::string, float> getLockValueLookupTournament(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int pruningBreadth, int playoutCount, int playoutLength, int secondPieceDelay, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]);

#endif
