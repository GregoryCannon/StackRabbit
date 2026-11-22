#include "tournament_search.hpp"
#include "formatting.hpp"
#include "high_level_search.hpp"
#include "playout.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <math.h>
#include <string>
#include <vector>
using namespace std;

using SteadyClock = std::chrono::steady_clock;

namespace {

double msBetween(SteadyClock::time_point start, SteadyClock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

/** Max fast-eval score per first placement, used for eval-only and expired-deadline fallbacks. */
unordered_map<string, float> buildEvalOnlyLookupMap(const std::vector<Possibility> &possibilities) {
  unordered_map<string, float> lockValueMap;
  for (Possibility const &possibility : possibilities) {
    string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
    float evalScore = possibility.evalScoreInclReward;
    if (lockValueMap.count(lockPosEncoded) == 0 || evalScore > lockValueMap[lockPosEncoded]) {
      lockValueMap[lockPosEncoded] = evalScore;
    }
  }
  return lockValueMap;
}

/** Per-entry sample count for timed mode that fits within `timeMs` without exceeding the batch. */
int getTimedRoundSampleCount(double timeMs, double movesPerMs, int entryCount, int playoutLength,
                             int remainingBatch) {
  if (timeMs <= 0.5 || entryCount <= 0 || remainingBatch < TMT_MIN_PLAYOUTS) {
    return 0;
  }
  long long capByTime = (long long)((timeMs * movesPerMs) / ((double)entryCount * playoutLength));
  int count = (int)std::min<long long>(remainingBatch, capByTime);
  return count < TMT_MIN_PLAYOUTS ? 0 : count;
}

uint64_t hashGameState(const GameState &gs) {
  uint64_t h = 14695981039346656037ULL;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };
  for (int r = 0; r < 20; r += 2) {
    mix((static_cast<uint64_t>(gs.board[r]) << 32) | gs.board[r + 1]);
  }
  mix((static_cast<uint64_t>(static_cast<uint32_t>(gs.lines)) << 32)
      | static_cast<uint32_t>(gs.level));
  uint32_t partialBits = 0;
  if (gs.numPartialHoles != 0.0f) {
    memcpy(&partialBits, &gs.numPartialHoles, sizeof(partialBits));
  }
  mix((static_cast<uint64_t>(static_cast<uint32_t>(gs.numTrueHoles)) << 32) | partialBits);
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return h;
}

bool gameStateEquals(const GameState &a, const GameState &b) {
  return a.lines == b.lines
         && a.level == b.level
         && a.numTrueHoles == b.numTrueHoles
         && a.numPartialHoles == b.numPartialHoles
         && memcmp(a.board, b.board, sizeof(a.board)) == 0
         && memcmp(a.surfaceArray, b.surfaceArray, sizeof(a.surfaceArray)) == 0;
}

struct GameStatePtrHash {
  size_t operator()(const GameState *gs) const { return static_cast<size_t>(hashGameState(*gs)); }
};

struct GameStatePtrEqual {
  bool operator()(const GameState *a, const GameState *b) const { return gameStateEquals(*a, *b); }
};

} // namespace

// Stage markers on stderr (unbuffered, so they are visible even if a later stage hangs)
#define TMT_TRACE(...)                                                                             \
  do {                                                                                             \
    if (TMT_LOGGING_ENABLED) {                                                                     \
      fprintf(stderr, __VA_ARGS__);                                                                \
    }                                                                                              \
  } while (0)

/** One continuation (second placement) sampled for a candidate. Pools are trailing-aligned:
 * any two conts' last k scores cover the same sequence windows (paired-test invariant). */
struct TournamentCont {
  GameState resultingState;
  float immediateReward;        // Line clear reward from the first + second placements
  float evalScoreInclReward;    // Fast eval of the pair, used for ordering/tie-breaking
  std::vector<float> seqScores; // Pooled per-sequence playout scores, in play order
  std::string contKey;          // encodeLockPosition of the second placement (debugging aid)
};

/** Mean of the pooled playout scores (excluding immediate reward). */
float getTournamentPlayoutMean(TournamentCont const &cont) {
  int n = (int)cont.seqScores.size();
  if (n == 0) {
    return 0;
  }
  float sum = 0;
  for (float score : cont.seqScores) {
    sum += score;
  }
  return sum / n;
}

/** Standard error of the cont's pooled playout mean (0 when the pool has fewer than 2 samples). */
float getTournamentContStandardError(TournamentCont const &cont) {
  int n = (int)cont.seqScores.size();
  if (n < 2) {
    return 0;
  }
  float sum = 0;
  float sumSquared = 0;
  for (float score : cont.seqScores) {
    sum += score;
    sumSquared += score * score;
  }
  float variance = std::max((sumSquared - sum * sum / n) / (n - 1), 0.0f);
  return sqrtf(variance / n);
}

/** Detailed one-line description of a cont's current estimate (guarded by TMT_LOGGING_ENABLED). */
float getTournamentContScore(TournamentCont const &cont);
void logTournamentCont(std::string label, std::string key, TournamentCont const &cont,
                       int windowStart) {
  int n = (int)cont.seqScores.size();
  // Window-local spread stats, to show how noisy small-sample means really are
  float windowSum = 0;
  float windowSumSquared = 0;
  float windowMin = 0;
  float windowMax = 0;
  int windowCount = 0;
  for (int i = windowStart; i < n; i++) {
    float score = cont.seqScores[i];
    if (windowCount == 0 || score < windowMin) {
      windowMin = score;
    }
    if (windowCount == 0 || score > windowMax) {
      windowMax = score;
    }
    windowSum += score;
    windowSumSquared += score * score;
    windowCount++;
  }
  float windowMean = windowCount > 0 ? windowSum / windowCount : 0;
  float windowStd =
      windowCount > 1 ? sqrtf(std::max(windowSumSquared - windowSum * windowSum / windowCount, 0.0f)
                              / (windowCount - 1))
                      : 0;
  printf("  %s %-9s cont=%-9s rew=%6.2f eval=%8.2f playoutMean=%9.2f score=%9.2f n=%d "
         "windowMean=%9.2f windowStd=%8.2f "
         "windowMin=%9.2f windowMax=%8.2f\n",
         label.c_str(), key.c_str(), cont.contKey.c_str(), cont.immediateReward,
         cont.evalScoreInclReward, getTournamentPlayoutMean(cont), getTournamentContScore(cont), n,
         windowMean, windowStd, windowMin, windowMax);
}

/**
 * The score a cont may publish into the final map: a conservative lower confidence bound.
 * Playout scores are heavy-tailed and shallow pools rarely contain the tail, so
 * mean - tCrit*SE keeps shallow estimates from outranking deep, well-supported ones.
 */
float getTournamentContPublishScore(TournamentCont const &cont, OUT float *outRawScore = NULL) {
  int n = (int)cont.seqScores.size();
  float rawScore = getTournamentContScore(cont);
  if (outRawScore != NULL) {
    *outRawScore = rawScore;
  }
  if (n < 2) {
    return rawScore;
  }
  float standardError = getTournamentContStandardError(cont);
  // ~98% one-sided t critical value, widened for small pools (exact within a few % for df >= 9)
  float tCrit = 2.0f + 4.5f / (n - 1);
  return rawScore - tCrit * standardError;
}

/** One first placement ("candidate") and its continuations, kept in eval-ranked order. */
struct TournamentCandidate {
  std::string key; // encodeLockPosition of the first placement
  std::vector<TournamentCont> conts;
  bool eliminated;
};

/** Overall-score estimate (reward + mean playout score); unsampled conts return the reward. */
float getTournamentContScore(TournamentCont const &cont) {
  if (cont.seqScores.empty()) {
    return cont.immediateReward;
  }
  float sum = 0;
  for (float score : cont.seqScores) {
    sum += score;
  }
  return cont.immediateReward + sum / cont.seqScores.size();
}

/** Index of the best-scoring sampled continuation (first index on ties; unsampled skipped). */
int getTournamentCandidateBestContIndex(TournamentCandidate const &candidate) {
  int bestIdx = -1;
  float bestScore = FLOAT_MIN;
  for (int i = 0; i < (int)candidate.conts.size(); i++) {
    if (candidate.conts[i].seqScores.empty()) {
      continue;
    }
    float score = getTournamentContScore(candidate.conts[i]);
    if (bestIdx == -1 || score > bestScore) {
      bestScore = score;
      bestIdx = i;
    }
  }
  return bestIdx == -1 ? 0 : bestIdx;
}

/** Returns the candidate's best-scoring continuation itself. */
TournamentCont const &getTournamentBestCont(TournamentCandidate const &candidate) {
  return candidate.conts[getTournamentCandidateBestContIndex(candidate)];
}

/** Non-const overload, for round 3 where the best continuation needs new samples appended. */
TournamentCont &getTournamentBestCont(TournamentCandidate &candidate) {
  return candidate.conts[getTournamentCandidateBestContIndex(candidate)];
}

/** Returns the index of the best surviving candidate (the "leader"). */
int getTournamentLeaderIndex(std::vector<TournamentCandidate> const &candidates) {
  int leaderIdx = -1;
  float leaderScore = FLOAT_MIN;
  for (int i = 0; i < (int)candidates.size(); i++) {
    if (candidates[i].eliminated) {
      continue;
    }
    float score = getTournamentContScore(getTournamentBestCont(candidates[i]));
    if (leaderIdx == -1 || score > leaderScore) {
      leaderScore = score;
      leaderIdx = i;
    }
  }
  return leaderIdx;
}

/** Number of candidates that have not been eliminated. */
int countTournamentSurvivors(std::vector<TournamentCandidate> const &candidates) {
  int count = 0;
  for (auto const &candidate : candidates) {
    if (!candidate.eliminated) {
      count++;
    }
  }
  return count;
}

/**
 * Paired-difference test vs the leader's best cont: is the challenger significantly worse?
 * Pools differ in length, but their trailing n scores share sequence windows (common random
 * numbers), so aligning from the end isolates the real gap.
 */
bool isTournamentCandidateSignificantlyWorse(TournamentCandidate const &challenger,
                                             TournamentCont const &leaderCont,
                                             float *outMeanDiff = NULL,
                                             float *outStandardError = NULL,
                                             float zThreshold = TMT_PAIRED_TEST_Z) {
  const TournamentCont &challengerCont = getTournamentBestCont(challenger);
  int n = (int)std::min(leaderCont.seqScores.size(), challengerCont.seqScores.size());
  if (n < 2) {
    if (outMeanDiff != NULL) {
      *outMeanDiff = 0;
    }
    if (outStandardError != NULL) {
      *outStandardError = 0;
    }
    return false; // Not enough paired samples to conclude anything
  }
  // Align from the end (see doc); paired diffs cover the full overall score
  int leaderStart = (int)leaderCont.seqScores.size() - n;
  int challengerStart = (int)challengerCont.seqScores.size() - n;
  float rewardDiff = leaderCont.immediateReward - challengerCont.immediateReward;
  float sumDiff = 0;
  float sumDiffSquared = 0;
  for (int i = 0; i < n; i++) {
    float diff = rewardDiff
                 + leaderCont.seqScores[leaderStart + i]
                 - challengerCont.seqScores[challengerStart + i];
    sumDiff += diff;
    sumDiffSquared += diff * diff;
  }
  float meanDiff = sumDiff / n;
  float variance = (sumDiffSquared - sumDiff * sumDiff / n) / (n - 1);
  float standardError = sqrtf(std::max(variance, 0.0f) / n);
  if (outMeanDiff != NULL) {
    *outMeanDiff = meanDiff;
  }
  if (outStandardError != NULL) {
    *outStandardError = standardError;
  }
  return meanDiff - zThreshold * standardError > 0;
}

// Rank modes for cutTournamentCandidatesByRank.
enum TournamentRankMode {
  TMT_RANK_UPPER_BOUND,  // mean + z*SE: optimistic about under-measured pools
  TMT_RANK_RAW_MEAN,     // plain mean: neutral selection; later evidence corrects mistakes
  TMT_RANK_PUBLISH_SCORE // mean - tCrit*SE: conservative, the proven value
};

/** Eliminates surviving candidates ranked below `cap` by their best cont's rank-mode score
 * (ties: eval, then key). Cut 1 uses TMT_RANK_UPPER_BOUND (tolerate small-sample noise);
 * cut 2 uses TMT_RANK_RAW_MEAN (false positives get corrected by round 3 and the LCBs). */
void cutTournamentCandidatesByRank(OUT std::vector<TournamentCandidate> &candidates, int cap,
                                   TournamentRankMode rankMode) {
  struct CandidateRank {
    float score;
    float evalScore;
    std::string key;
    int candidateIndex;
  };
  std::vector<CandidateRank> rankings;
  for (int i = 0; i < (int)candidates.size(); i++) {
    if (candidates[i].eliminated) {
      continue;
    }
    const TournamentCont &bestCont = getTournamentBestCont(candidates[i]);
    float rawScore = getTournamentContScore(bestCont);
    float rankScore = rawScore;
    if (rankMode == TMT_RANK_UPPER_BOUND) {
      rankScore += TMT_PAIRED_TEST_Z * getTournamentContStandardError(bestCont);
    } else if (rankMode == TMT_RANK_PUBLISH_SCORE) {
      rankScore = getTournamentContPublishScore(bestCont);
    }
    rankings.push_back({rankScore, bestCont.evalScoreInclReward, candidates[i].key, i});
  }
  std::sort(rankings.begin(), rankings.end(), [](CandidateRank const &a, CandidateRank const &b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    if (a.evalScore != b.evalScore) {
      return a.evalScore > b.evalScore;
    }
    return a.key < b.key;
  });
  for (int i = 0; i < (int)rankings.size(); i++) {
    if (i >= cap) {
      candidates[rankings[i].candidateIndex].eliminated = true;
    }
  }
}

/** Appends `sampleCount` playouts from the shared window at `batchOffset` to the cont's pool.
 * @returns the number of individual playout moves (plies) executed.
 */
int playTournamentSamples(OUT TournamentCont &cont, int sampleCount, int batchOffset,
                          int playoutLength, const PieceRangeContext pieceRangeContextLookup[3],
                          int secondPieceIndex, AggressionLevel aggressionLevel,
                          AiMode originalAiMode, const std::atomic<bool> *cancelFlag = nullptr) {
  cont.seqScores.reserve(cont.seqScores.size() + sampleCount);
  int movesExecuted = 0;
  getPlayoutScore(cont.resultingState, sampleCount, playoutLength, pieceRangeContextLookup,
                  secondPieceIndex, aggressionLevel, originalAiMode, /* playoutDataList */ NULL,
                  &cont.seqScores, batchOffset, &movesExecuted, cancelFlag);
  return movesExecuted;
}

/** Runs the 3-round tournament over precomputed 2-ply possibilities. See block comment above. */
unordered_map<string, float> getLockValueLookupTournamentFromPossibilities(
    const std::vector<Possibility> &possibilities, const Piece *secondPiece, int playoutLength,
    const SearchBudget &budget, const EvalContext *evalContext,
    const PieceRangeContext pieceRangeContextLookup[3]) {
  unordered_map<string, float> lockValueMap;

  if (possibilities.empty()) {
    return lockValueMap; // Topped out: no legal first placement
  }

  const bool isTimed = (budget.mode == BUDGET_TIME_MS);
  const bool evalOnly =
      playoutLength == 0 || (isTimed ? budget.timeBudgetMs <= 0 : budget.playoutCount == 0);
  if (evalOnly || budget.isCancelled()) {
    return buildEvalOnlyLookupMap(possibilities);
  }

  const double clockSafetyMs =
      isTimed ? std::min<double>(TMT_CLOCK_SAFETY_MS, std::max(0.0, budget.timeBudgetMs * 0.15))
              : 0.0;
  const SteadyClock::time_point deadline =
      isTimed ? (budget.hasDeadline
                     ? budget.deadline
                     : SteadyClock::now() + std::chrono::milliseconds(budget.timeBudgetMs))
              : SteadyClock::time_point{};
  if (isTimed && msBetween(SteadyClock::now(), deadline) - clockSafetyMs <= 0.5) {
    return buildEvalOnlyLookupMap(possibilities);
  }

  // Sort pair pointers by eval desc so the per-candidate cont cap keeps the eval-best conts
  // (searchDepth2's order is unsorted, tucks last) without copying the full Possibility structs.
  const std::vector<const Possibility *> sortedPossibilities = sortPossibilitiesDesc(possibilities);

  // Group pairs into candidates (one per distinct first placement), keeping eval-best conts.
  std::vector<TournamentCandidate> candidates;
  unordered_map<string, int> candidateIndexByKey;
  for (const Possibility *possibilityPtr : sortedPossibilities) {
    Possibility const &possibility = *possibilityPtr;
    string key = encodeLockPosition(possibility.firstPlacement);
    int candidateIdx;
    auto found = candidateIndexByKey.find(key);
    if (found == candidateIndexByKey.end()) {
      candidateIdx = (int)candidates.size();
      candidateIndexByKey[key] = candidateIdx;
      TournamentCandidate newCandidate = {key, {}, false};
      candidates.push_back(newCandidate);
    } else {
      candidateIdx = found->second;
    }
    if ((int)candidates[candidateIdx].conts.size() < TMT_CONTINUATIONS_PER_CANDIDATE) {
      TournamentCont newCont = {possibility.resultingState,
                                possibility.immediateReward,
                                possibility.evalScoreInclReward,
                                {},
                                encodeLockPosition(possibility.secondPlacement)};
      candidates[candidateIdx].conts.push_back(newCont);
    }
  }
  int candidateCount = (int)candidates.size();

  // ---- Budget. In playout-count mode (-1 or >= 0), compute the total playout count directly.
  // In timed mode (-2), convert remaining wall-clock time via calibratedMovesPerMs for R1, then
  // re-measure actual moves/ms after R1 and R2 against the real-time deadline.
  double initMovesPerMs = DEFAULT_CALIBRATED_MOVES_PER_MS;
  long long totalBudget = 0;
  if (isTimed) {
    initMovesPerMs = budget.calibratedMovesPerMs > 0.0 ? budget.calibratedMovesPerMs
                                                       : DEFAULT_CALIBRATED_MOVES_PER_MS;
    double remMs = msBetween(SteadyClock::now(), deadline) - clockSafetyMs;
    totalBudget = std::min((long long)(remMs * initMovesPerMs / playoutLength),
                           (long long)CANONICAL_SEQUENCE_BATCH_SIZE * 64);
  } else {
    int clampedPlayoutCount = std::min(budget.playoutCount, CANONICAL_SEQUENCE_BATCH_SIZE);
    if (budget.pruningBreadth == -1) {
      totalBudget =
          std::min((long long)clampedPlayoutCount, (long long)CANONICAL_SEQUENCE_BATCH_SIZE * 64);
    } else {
      totalBudget = std::min((long long)budget.pruningBreadth * clampedPlayoutCount,
                             (long long)CANONICAL_SEQUENCE_BATCH_SIZE * 64);
    }
  }
  auto getRoundSampleCount = [](long long budgetShare, int entryCount) {
    long long perEntry = budgetShare / std::max(entryCount, 1);
    return (int)std::min((long long)CANONICAL_SEQUENCE_BATCH_SIZE,
                         std::max(perEntry, (long long)TMT_MIN_PLAYOUTS));
  };

  const int secondPieceIndex = secondPiece->index;
  // samplesSoFar doubles as the next round's batch offset (fresh window per round). NB: with
  // extreme budgets the window can wrap and overlap earlier rounds; within-round CRN holds.
  int samplesSoFar = 0;

  // ---- Round 1: screen every candidate on its eval-best continuation(s) (the wide net).
  // Candidates are ordered descending by fast-eval score; top-eval candidates screen extra
  // continuations when R1's budget share covers TMT_MIN_PLAYOUTS across them, falling back to
  // 1 continuation per candidate on tight budgets.
  TMT_TRACE("TRACE: entering R1 (n1 pending)\n");
  auto getTieredR1Conts = [](int candidateIdx, int availConts) {
    int limit = 1;
    if (candidateIdx < TMT_R1_TIER1_CANDIDATES) {
      limit = TMT_R1_TIER1_CONTS;
    } else if (candidateIdx < TMT_R1_TIER1_CANDIDATES + TMT_R1_TIER2_CANDIDATES) {
      limit = TMT_R1_TIER2_CONTS;
    }
    return std::min(std::min(limit, TMT_CONTINUATIONS_PER_CANDIDATE), availConts);
  };
  int tieredR1Entries = 0;
  int baseR1Entries = 0;
  for (int i = 0; i < candidateCount; i++) {
    int avail = (int)candidates[i].conts.size();
    tieredR1Entries += getTieredR1Conts(i, avail);
    baseR1Entries += std::min(1, avail);
  }
  const bool useTieredR1 = (long long)tieredR1Entries * TMT_MIN_PLAYOUTS
                           <= (long long)(totalBudget * TMT_R1_BUDGET_SHARE);
  const float r1BudgetShare = useTieredR1 ? TMT_R1_BUDGET_SHARE : TMT_R1_FALLBACK_BUDGET_SHARE;
  const float r2BudgetShare = useTieredR1 ? TMT_R2_BUDGET_SHARE : TMT_R2_FALLBACK_BUDGET_SHARE;
  const int r1Entries = useTieredR1 ? tieredR1Entries : baseR1Entries;
  auto getR1ContsForCandidate = [&](int candidateIdx) {
    int avail = (int)candidates[candidateIdx].conts.size();
    return useTieredR1 ? getTieredR1Conts(candidateIdx, avail) : std::min(1, avail);
  };
  int n1 = 0;
  if (isTimed) {
    double remMs = msBetween(SteadyClock::now(), deadline) - clockSafetyMs;
    n1 = getTimedRoundSampleCount(remMs * r1BudgetShare, initMovesPerMs, r1Entries, playoutLength,
                                  CANONICAL_SEQUENCE_BATCH_SIZE);
    if (n1 == 0) {
      // Very tight deadlines can fail R1's share threshold yet still fit TMT_MIN_PLAYOUTS
      // inside the remaining window; otherwise return eval-only rather than noisy samples.
      if (getTimedRoundSampleCount(remMs, initMovesPerMs, r1Entries, playoutLength,
                                   CANONICAL_SEQUENCE_BATCH_SIZE)
          >= TMT_MIN_PLAYOUTS) {
        n1 = TMT_MIN_PLAYOUTS;
      } else {
        return buildEvalOnlyLookupMap(possibilities);
      }
    }
  } else {
    n1 = getRoundSampleCount(totalBudget * r1BudgetShare, r1Entries);
  }
  n1 = std::min(n1, TMT_R1_MAX_PLAYOUTS);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT setup: candidates=%d r1Entries=%d tiered=%d totalBudget=%lld playoutLength=%d\n",
           candidateCount, r1Entries, (int)useTieredR1, totalBudget, playoutLength);
    for (auto const &candidate : candidates) {
      for (auto const &cont : candidate.conts) {
        if (!cont.seqScores.empty()) {
          logTournamentCont("setup", candidate.key, cont, 0);
        }
      }
    }
  }
  SteadyClock::time_point r1Start = SteadyClock::now();
  long long totalPlayoutMovesRun = 0;
  for (int i = 0; i < candidateCount; i++) {
    if (budget.isCancelled()) {
      return lockValueMap;
    }
    TournamentCandidate &candidate = candidates[i];
    int contsToPlay = getR1ContsForCandidate(i);
    for (int c = 0; c < contsToPlay; c++) {
      totalPlayoutMovesRun += playTournamentSamples(
          candidate.conts[c], n1, samplesSoFar, playoutLength, pieceRangeContextLookup,
          secondPieceIndex, evalContext->aggressionLevel, evalContext->aiMode, budget.cancelFlag);
    }
  }
  double playoutMsAccum = isTimed ? msBetween(r1Start, SteadyClock::now()) : 0.0;
  samplesSoFar += n1;
  TMT_TRACE("TRACE: R1 done (n1=%d)\n", n1);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT R1 done: n1=%d window=[%d,%d)\n", n1, 0, n1);
    for (auto const &candidate : candidates) {
      for (auto const &cont : candidate.conts) {
        if (!cont.seqScores.empty()) {
          logTournamentCont("R1", candidate.key, cont, 0);
        }
      }
    }
  }

  // ---- Cut 1: cap the field at TMT_FINALIST_COUNT by optimistic rank (mean + z*SE). The
  // veto is off by default (TMT_CUT1_VETO_Z): single-cont evidence can't justify a veto.
  int leaderIdx = getTournamentLeaderIndex(candidates);
  const TournamentCont &leaderCont = getTournamentBestCont(candidates[leaderIdx]);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut1: leader=%s score=%.2f\n", candidates[leaderIdx].key.c_str(),
           getTournamentContScore(leaderCont));
  }
  if (TMT_CUT1_VETO_Z > 0) {
    for (int i = 0; i < candidateCount; i++) {
      if (candidates[i].eliminated || i == leaderIdx) {
        continue;
      }
      float pairedMeanDiff, pairedStandardError;
      if (isTournamentCandidateSignificantlyWorse(candidates[i], leaderCont, &pairedMeanDiff,
                                                  &pairedStandardError, TMT_CUT1_VETO_Z)) {
        candidates[i].eliminated = true;
        if (TMT_LOGGING_ENABLED) {
          printf("  cut1 VETO %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(),
                 pairedMeanDiff, pairedStandardError,
                 pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
        }
      } else if (TMT_LOGGING_ENABLED) {
        printf("  cut1 keep %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(),
               pairedMeanDiff, pairedStandardError,
               pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
      }
    }
  }
  int survivorsAfterVeto = countTournamentSurvivors(candidates);
  cutTournamentCandidatesByRank(candidates, std::min(TMT_FINALIST_COUNT, survivorsAfterVeto),
                                TMT_RANK_UPPER_BOUND);
  int survivorsAfterCut1 = countTournamentSurvivors(candidates);
  int spentSoFar = r1Entries * n1;
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut1 done: survivors(%d)=", survivorsAfterCut1);
    for (auto const &candidate : candidates) {
      if (!candidate.eliminated) {
        printf(" %s", candidate.key.c_str());
      }
    }
    printf("\n");
  }

  auto getObservedMovesPerMs = [&]() {
    if (totalPlayoutMovesRun == 0) {
      return initMovesPerMs;
    }
    double raw = (double)totalPlayoutMovesRun / std::max(0.05, playoutMsAccum);
    // Avoid micro-timer upward inflation when elapsed wall time is under 2ms.
    if (playoutMsAccum < 2.0) {
      return std::min(initMovesPerMs, raw);
    }
    return raw;
  };

  // ---- Round 2: sample every cont of each survivor (discovery: eval-best isn't always best).
  int actualR2Entries = 0;
  for (auto const &candidate : candidates) {
    if (!candidate.eliminated) {
      actualR2Entries += (int)candidate.conts.size();
    }
  }
  int n2 = 0;
  if (isTimed) {
    double remAfterR1 = msBetween(SteadyClock::now(), deadline) - clockSafetyMs;
    const double r2ShareOfRemaining = r2BudgetShare / (1.0f - r1BudgetShare);
    n2 = getTimedRoundSampleCount(remAfterR1 * r2ShareOfRemaining, getObservedMovesPerMs(),
                                  actualR2Entries, playoutLength,
                                  CANONICAL_SEQUENCE_BATCH_SIZE - samplesSoFar);
    if (n2 == 0
        && getTimedRoundSampleCount(remAfterR1, getObservedMovesPerMs(), actualR2Entries,
                                    playoutLength, CANONICAL_SEQUENCE_BATCH_SIZE - samplesSoFar)
               >= TMT_MIN_PLAYOUTS) {
      // Avoid skipping discovery completely when R2's share lands just shy of TMT_MIN_PLAYOUTS
      // while the remaining window still comfortably fits the minimum sample floor.
      n2 = TMT_MIN_PLAYOUTS;
    }
  } else {
    n2 = getRoundSampleCount(totalBudget * r2BudgetShare, actualR2Entries);
  }
  if (n2 > 0) {
    SteadyClock::time_point r2Start = SteadyClock::now();
    long long r2MovesRun = 0;
    for (auto &candidate : candidates) {
      if (budget.isCancelled()) {
        return lockValueMap;
      }
      if (candidate.eliminated) {
        continue;
      }
      for (auto &cont : candidate.conts) {
        r2MovesRun += playTournamentSamples(
            cont, n2, samplesSoFar, playoutLength, pieceRangeContextLookup, secondPieceIndex,
            evalContext->aggressionLevel, evalContext->aiMode, budget.cancelFlag);
      }
    }
    if (isTimed) {
      playoutMsAccum += msBetween(r2Start, SteadyClock::now());
      totalPlayoutMovesRun += r2MovesRun;
    }
    samplesSoFar += n2;
    spentSoFar += actualR2Entries * n2;
  }
  TMT_TRACE("TRACE: R2 done (n2=%d)\n", n2);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT R2 done: n2=%d window=[%d,%d)\n", n2, n1, n1 + n2);
    for (auto const &candidate : candidates) {
      if (candidate.eliminated) {
        continue;
      }
      for (auto const &cont : candidate.conts) {
        logTournamentCont("R2", candidate.key, cont, n1);
      }
    }
  }

  // ---- Cut 2: keep the top TMT_R3_COUNT survivors by raw pooled mean for Round 3 deepening.
  // Keeping all TMT_R3_COUNT finalists (rather than vetoing down to 1) ensures the top few
  // placements in each lock-value map stay well-calibrated with tight LCBs for 7-piece precompute
  // averaging.
  leaderIdx = getTournamentLeaderIndex(candidates);
  const TournamentCont &leaderCont2 = getTournamentBestCont(candidates[leaderIdx]);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut2: leader=%s score=%.2f\n", candidates[leaderIdx].key.c_str(),
           getTournamentContScore(leaderCont2));
  }
  cutTournamentCandidatesByRank(candidates, std::min(TMT_R3_COUNT, survivorsAfterCut1),
                                TMT_RANK_RAW_MEAN);
  int finalists = countTournamentSurvivors(candidates);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut2 done: finalists(%d)=", finalists);
    for (auto const &candidate : candidates) {
      if (!candidate.eliminated) {
        printf(" %s", candidate.key.c_str());
      }
    }
    printf("\n");
  }

  // ---- Round 3: the remaining budget goes to each finalist's best cont, deeply.
  int n3 = 0;
  std::vector<int> finalistIndices;
  std::vector<int> finalistBestContIndices;
  for (int i = 0; i < candidateCount; i++) {
    if (!candidates[i].eliminated) {
      finalistIndices.push_back(i);
      finalistBestContIndices.push_back(getTournamentCandidateBestContIndex(candidates[i]));
    }
  }
  if (isTimed) {
    // Run timed Round 3 in small horizontal chunks so fast boards fill remaining time and slow
    // runs stop cleanly before the deadline (falling back to R2 or partial R3 results).
    double lastChunkMs = 0.0;
    const double finishMarginMs =
        std::min<double>(TMT_R3_FINISH_MARGIN_MS, std::max(0.0, budget.timeBudgetMs * 0.05));
    while (samplesSoFar < CANONICAL_SEQUENCE_BATCH_SIZE && !finalistIndices.empty()) {
      int remainingBatch = CANONICAL_SEQUENCE_BATCH_SIZE - samplesSoFar;
      int chunkSize = std::min(TMT_R3_CHUNK_SAMPLES, remainingBatch);
      if (chunkSize == 7 && playoutLength == 1) {
        chunkSize = 6; // Avoid playout.cpp's exhaustive-sequence special case mid-window.
      }
      if (chunkSize <= 0) {
        break;
      }
      double estChunkMs = ((double)finalists * chunkSize * playoutLength) / getObservedMovesPerMs();
      double guardMs = std::max(estChunkMs * 1.15, lastChunkMs * 1.05);
      double msLeft = msBetween(SteadyClock::now(), deadline) - finishMarginMs;
      if (msLeft < guardMs) {
        break;
      }

      SteadyClock::time_point chunkStart = SteadyClock::now();
      long long chunkMovesRun = 0;
      for (size_t f = 0; f < finalistIndices.size(); f++) {
        if (budget.isCancelled()) {
          return lockValueMap;
        }
        TournamentCandidate &candidate = candidates[finalistIndices[f]];
        int contIdx = finalistBestContIndices[f];
        TMT_TRACE("TRACE: R3 chunk playing %s cont=%s\n", candidate.key.c_str(),
                  candidate.conts[contIdx].contKey.c_str());
        chunkMovesRun += playTournamentSamples(candidate.conts[contIdx], chunkSize, samplesSoFar,
                                               playoutLength, pieceRangeContextLookup,
                                               secondPieceIndex, evalContext->aggressionLevel,
                                               evalContext->aiMode, budget.cancelFlag);
      }
      lastChunkMs = msBetween(chunkStart, SteadyClock::now());
      playoutMsAccum += lastChunkMs;
      totalPlayoutMovesRun += chunkMovesRun;
      samplesSoFar += chunkSize;
      n3 += chunkSize;
    }
    if (TMT_LOGGING_ENABLED && n3 > 0) {
      for (size_t f = 0; f < finalistIndices.size(); f++) {
        TournamentCandidate const &candidate = candidates[finalistIndices[f]];
        logTournamentCont("R3", candidate.key, candidate.conts[finalistBestContIndices[f]],
                          n1 + n2);
      }
    }
  } else {
    n3 = getRoundSampleCount(totalBudget - spentSoFar, finalists);
    if (n3 > 0) {
      for (size_t f = 0; f < finalistIndices.size(); f++) {
        if (budget.isCancelled()) {
          return lockValueMap;
        }
        TournamentCandidate &candidate = candidates[finalistIndices[f]];
        int contIdx = finalistBestContIndices[f];
        TMT_TRACE("TRACE: R3 playing %s cont=%s\n", candidate.key.c_str(),
                  candidate.conts[contIdx].contKey.c_str());
        playTournamentSamples(candidate.conts[contIdx], n3, samplesSoFar, playoutLength,
                              pieceRangeContextLookup, secondPieceIndex,
                              evalContext->aggressionLevel, evalContext->aiMode, budget.cancelFlag);
        if (TMT_LOGGING_ENABLED) {
          logTournamentCont("R3", candidate.key, candidate.conts[contIdx], n1 + n2);
        }
      }
      samplesSoFar += n3;
    }
  }
  TMT_TRACE("TRACE: R3 done (n3=%d)\n", n3);
  if (PLAYOUT_LOGGING_ENABLED || TMT_LOGGING_ENABLED) {
    printf("Tournament: candidates=%d r1Entries=%d n1=%d survivors1=%d n2=%d finalists=%d n3=%d "
           "samplesSoFar=%d\n",
           candidateCount, r1Entries, n1, survivorsAfterCut1, n2, finalists, n3, samplesSoFar);
  }

  // ---- Build the map: record the highest-sample-count playout estimate per distinct
  // post-2-piece GameState so transposed positions replace lower-sample estimates with their
  // deeper counterparts, then publish each first placement's best LCB across all possibilities
  // whose resulting state was sampled.
  struct StatePlayoutEstimate {
    int count = 0;
    float mean = 0.0f;
    float lcbPenalty = 0.0f;
  };
  std::unordered_map<const GameState *, StatePlayoutEstimate, GameStatePtrHash, GameStatePtrEqual>
      stateEstimates;
  stateEstimates.reserve(static_cast<size_t>(candidateCount * TMT_CONTINUATIONS_PER_CANDIDATE));
  for (auto const &candidate : candidates) {
    for (auto const &cont : candidate.conts) {
      const int n = static_cast<int>(cont.seqScores.size());
      if (n == 0) {
        continue; // Never sampled (cut before its round): no playout-backed estimate exists
      }
      const float mean = getTournamentPlayoutMean(cont);
      const float lcbPenalty =
          n < 2 ? 0.0f : (2.0f + 4.5f / (n - 1)) * getTournamentContStandardError(cont);
      const auto it = stateEstimates.find(&cont.resultingState);
      if (it == stateEstimates.end()
          || n > it->second.count
          || (n == it->second.count
              && (mean - lcbPenalty) > (it->second.mean - it->second.lcbPenalty))) {
        stateEstimates[&cont.resultingState] = {n, mean, lcbPenalty};
      }
    }
  }

  for (auto const &candidate : candidates) {
    lockValueMap[candidate.key] = FLOAT_MIN;
  }
  for (const Possibility *possibilityPtr : sortedPossibilities) {
    const Possibility &possibility = *possibilityPtr;
    const auto estIt = stateEstimates.find(&possibility.resultingState);
    if (estIt == stateEstimates.end()) {
      continue;
    }
    const StatePlayoutEstimate &est = estIt->second;
    const float rawScore = possibility.immediateReward + est.mean;
    const float score = est.count < 2 ? rawScore : rawScore - est.lcbPenalty;
    const std::string key = encodeLockPosition(possibility.firstPlacement);
    float &bestScore = lockValueMap[key];
    if (score > bestScore) {
      bestScore = score;
    }
  }
  if (TMT_LOGGING_ENABLED) {
    for (auto const &candidate : candidates) {
      printf("  final %-9s = %9.2f\n", candidate.key.c_str(), lockValueMap[candidate.key]);
    }
  }

  if (TMT_LOGGING_ENABLED) {
    // Debug-only: re-estimate the top keys' winning conts legacy-style for direct comparison.
    int validatePlayoutCount = isTimed ? (int)totalBudget : budget.playoutCount;
    std::vector<std::pair<float, std::string>> ranked;
    for (auto const &entry : lockValueMap) {
      ranked.push_back({entry.second, entry.first});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](auto const &a, auto const &b) { return a.first > b.first; });
    int validateCount = std::min((int)ranked.size(), 3);
    printf("TMT validation: re-playing top %d keys' winning conts with %d playouts legacy-style\n",
           validateCount, validatePlayoutCount);
    for (int r = 0; r < validateCount; r++) {
      for (auto const &candidate : candidates) {
        if (candidate.key != ranked[r].second) {
          continue;
        }
        int contIdx = 0;
        float bestScore = FLOAT_MIN;
        for (int i = 0; i < (int)candidate.conts.size(); i++) {
          float score = getTournamentContScore(candidate.conts[i]);
          if (score > bestScore) {
            bestScore = score;
            contIdx = i;
          }
        }
        TournamentCont const &cont = candidate.conts[contIdx];
        std::vector<float> legacyScores;
        float legacyMean =
            getPlayoutScore(cont.resultingState, validatePlayoutCount, playoutLength,
                            pieceRangeContextLookup, secondPieceIndex, evalContext->aggressionLevel,
                            evalContext->aiMode, /* playoutDataList */ NULL, &legacyScores);
        float legacySum = 0;
        float legacySumSquared = 0;
        float legacyMin = 1e9;
        for (float score : legacyScores) {
          legacySum += score;
          legacySumSquared += score * score;
          legacyMin = std::min(legacyMin, score);
        }
        float legacyStd =
            sqrtf(std::max(legacySumSquared - legacySum * legacySum / legacyScores.size(), 0.0f)
                  / (legacyScores.size() - 1));
        printf("  validate %-9s cont=%-9s tournament=%9.2f legacy(%d)=%9.2f rew=%.2f "
               "legacyStd=%.1f legacyMin=%.1f\n",
               candidate.key.c_str(), cont.contKey.c_str(), bestScore, validatePlayoutCount,
               cont.immediateReward + legacyMean, cont.immediateReward, legacyStd, legacyMin);
        // Print the round-1 window's individual scores for direct comparison with the legacy tail
        printf("    r1scores:");
        for (int i = 0; i < std::min((int)cont.seqScores.size(), n1); i++) {
          printf(" %.1f", cont.seqScores[i]);
        }
        printf("\n");
      }
    }
  }
  return lockValueMap;
}

/** Runs the whole 3-round tournament. See the block comment above for the scheme. */
unordered_map<string, float>
getLockValueLookupTournament(GameState gameState, const Piece *firstPiece, const Piece *secondPiece,
                             int playoutLength, const SearchBudget &budget, int firstPieceDasCharge,
                             int secondPieceDasCharge, char const *dasInputFrameTimeline,
                             const EvalContext *evalContext,
                             const PieceRangeContext pieceRangeContextLookup[3]) {
  // Enumerate every (first, second) pair with a fast eval (same first pass as legacy).
  // In DAS mode (firstPieceDasCharge >= 0), pair enumeration runs on dasInputFrameTimeline
  // (the player's real timeline) with secondPieceDasCharge inherited (-1) or prescribed (>= 0);
  // the tournament rounds/playouts themselves stay tap-based.
  std::vector<Possibility> possibilities;
  searchDepth2(gameState, firstPiece, secondPiece, firstPieceDasCharge, secondPieceDasCharge,
               dasInputFrameTimeline, evalContext, possibilities);
  return getLockValueLookupTournamentFromPossibilities(
      possibilities, secondPiece, playoutLength, budget, evalContext, pieceRangeContextLookup);
}
