#ifndef TOURNAMENT_SEARCH
#define TOURNAMENT_SEARCH

#include "types.hpp"
#include <unordered_map>
#include <vector>

/**
 * Tournament candidate selection for the lock value lookup. The legacy lookup spends its
 * budget on top pairs *by eval score*, starving moves eval underrates; here each first
 * placement is a candidate whose value is sampled through its continuations:
 *
 *   Round 1: every candidate plays its eval-best continuation(s) (wide screen).
 *   Cut 1:   cap at TMT_FINALIST_COUNT by optimistic rank (mean + z*SE); optional veto off.
 *   Round 2: finalists play ALL their conts on a fresh window (discovery: eval's best cont
 *            can be wrong; alternates need samples to prove themselves).
 *   Cut 2:   cap at TMT_R3_COUNT by raw pooled mean (keeps top runner-ups well-calibrated
 *            for multi-piece precompute averaging).
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

// Tiered Round-1 continuations per candidate (candidates are in descending fast-eval order).
// Top-ranked candidates screen extra continuations before Cut 1 when the R1 budget supports
// TMT_MIN_PLAYOUTS across them, so a single deceptive cont#0 does not knock out a strong move
// before Round 2 discovery. Tight budgets fall back to 1 continuation per candidate.
constexpr int TMT_R1_TIER1_CANDIDATES = 3;
constexpr int TMT_R1_TIER1_CONTS = 3;
constexpr int TMT_R1_TIER2_CANDIDATES = 5;
constexpr int TMT_R1_TIER2_CONTS = 2;

// The cap on how many candidates reach round 2 (discovery)
constexpr int TMT_FINALIST_COUNT = 8;

// The cap on how many candidates reach round 3 (deepening)
constexpr int TMT_R3_COUNT = 4;

// Floor on playouts per placement (below this, estimates are too noisy). NB: in fixed-playout
// mode this floor can push total spend past budget; in timed mode rounds below this floor are
// skipped (falling back to eval-only if Round 1 cannot reach it).
constexpr int TMT_MIN_PLAYOUTS = 30;

// Cap on Round-1 samples per entry so small-candidate pieces (e.g. O/I/S/Z) do not over-spend
// Round 1's share on coarse screening; saved budget flows into Round 3 deepening.
constexpr int TMT_R1_MAX_PLAYOUTS = 45;

// Safety margin (ms) reserved before the tournament deadline when sizing Rounds 1 and 2.
constexpr double TMT_CLOCK_SAFETY_MS = 8.0;

// Timed Round 3 runs in small paired chunks so it can fill remaining time up to the deadline and
// stop cleanly on partial R3 (or R2) results if execution slows down.
constexpr int TMT_R3_CHUNK_SAMPLES = 10;
constexpr double TMT_R3_FINISH_MARGIN_MS = 2.5;

// Shares of the total budget for rounds 1 and 2 (round 3 gets the remainder). When tiered R1
// is active (extra conts for top-eval candidates), R1 takes 45% and R2 takes 35% (R3: 20%).
// On tight budgets that fall back to 1 cont per candidate, keep the 38/37/25 split so R2/R3
// aren't starved without the extra R1 breadth.
constexpr float TMT_R1_BUDGET_SHARE = 0.45f;
constexpr float TMT_R2_BUDGET_SHARE = 0.35f;
constexpr float TMT_R1_FALLBACK_BUDGET_SHARE = 0.38f;
constexpr float TMT_R2_FALLBACK_BUDGET_SHARE = 0.37f;

// Eliminate a candidate if the leader's paired mean difference exceeds this many standard
// errors (1.96 ~= one-sided 97.5% confidence)
constexpr float TMT_PAIRED_TEST_Z = 1.96f;

// Cut-1 veto threshold (paired SEs vs the leader). Judges candidates on their eval-best
// cont alone; <= 0 disables the veto (the default).
constexpr float TMT_CUT1_VETO_Z = 0;

std::unordered_map<std::string, float> getLockValueLookupTournamentFromPossibilities(
    const std::vector<Possibility> &possibilities, const Piece *secondPiece, int playoutLength,
    const SearchBudget &budget, const EvalContext *evalContext,
    const PieceRangeContext pieceRangeContextLookup[3]);

std::unordered_map<std::string, float>
getLockValueLookupTournament(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                             int playoutLength, const SearchBudget &budget, int dasCharge,
                             int secondPieceDasCharge, char const *dasInputFrameTimeline,
                             const EvalContext *evalContext,
                             const PieceRangeContext pieceRangeContextLookup[3]);

#endif
