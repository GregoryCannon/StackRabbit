#ifndef TOURNAMENT_SEARCH
#define TOURNAMENT_SEARCH

#include "types.hpp"
#include "utils.hpp"
#include "config.hpp"
#include <list>
#include <unordered_map>

/**
 * Tournament candidate selection for the lock value lookup. The legacy lookup spends its
 * budget on top pairs *by eval score*, starving moves eval underrates; here each first
 * placement is a candidate whose value is sampled through its continuations:
 *
 *   Round 1: every candidate plays its eval-best continuation(s) (wide screen).
 *   Cut 1:   cap at TMT_FINALIST_COUNT by optimistic rank (mean + z*SE); optional veto off.
 *   Round 2: finalists play ALL their conts on a fresh window (discovery: eval's best cont
 *            can be wrong; alternates need samples to prove themselves).
 *   Cut 2:   paired veto vs the leader, then cap at TMT_R3_COUNT by raw pooled mean.
 *   Round 3: each survivor's best cont gets the remaining budget (deep samples).
 *
 * Tournament mode (pruningBreadth == -1) treats playoutCount as the total budget. Entries in
 * a round share one sequence window, so comparisons are paired and playout luck cancels.
 * Result: first placement -> best LCB over its sampled conts (see getTournamentContPublishScore).
 */


// ----- Config for tournament-style playouts -----

// Max continuations kept per candidate, in eval-ranked order. Discovery can only rescue a
// cont it samples, so this must reach past eval's mis-rankings while keeping round 2's
// per-cont sample count meaningful.
constexpr int TMT_CONTINUATIONS_PER_CANDIDATE = 5;

// Max conts the round-1 screen plays per candidate (adaptive). Cut 1 ranks candidates on
// one estimate each, so extra breadth dilutes the depth that cut needs; 1 is the default.
constexpr int TMT_R1_MAX_CONTS = 1;

// The cap on how many candidates reach round 2 (discovery)
constexpr int TMT_FINALIST_COUNT = 8;

// The cap on how many candidates reach round 3 (deepening)
constexpr int TMT_R3_COUNT = 4;

// Floor on playouts per placement (below this, estimates are too noisy). NB: flooring can
// make a round overspend its share and starve later rounds.
constexpr int TMT_MIN_PLAYOUTS = 30;

// Shares of the total budget for rounds 1 and 2 (round 3 gets the remainder). R1 plays
// r1Conts conts per candidate; R2 plays all of a finalist's conts.
constexpr float TMT_R1_BUDGET_SHARE = 0.38f;
constexpr float TMT_R2_BUDGET_SHARE = 0.37f;

// Eliminate a candidate if the leader's paired mean difference exceeds this many standard
// errors (1.96 ~= one-sided 97.5% confidence)
constexpr float TMT_PAIRED_TEST_Z = 1.96f;

// Cut-1 veto threshold (paired SEs vs the leader). Judges candidates on their eval-best
// cont alone; <= 0 disables the veto (the default).
constexpr float TMT_CUT1_VETO_Z = 0;




std::unordered_map<std::string, float> getLockValueLookupTournament(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int pruningBreadth, int playoutCount, int playoutLength, int secondPieceDelay, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]);

#endif
