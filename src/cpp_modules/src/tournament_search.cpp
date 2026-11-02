#include "tournament_search.hpp"
#include "high_level_search.hpp"
#include "playout.hpp"
#include "formatting.hpp"
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>
using namespace std;

// Stage markers on stderr (unbuffered, so they are visible even if a later stage hangs)
#define TMT_TRACE(...) do { if (TMT_LOGGING_ENABLED) { fprintf(stderr, __VA_ARGS__); } } while (0)

/** One continuation (second placement) sampled for a candidate. Pools are trailing-aligned:
 * any two conts' last k scores cover the same sequence windows (paired-test invariant). */
struct TournamentCont {
  GameState resultingState;
  float immediateReward;     // Line clear reward from the first + second placements
  float evalScoreInclReward; // Fast eval of the pair, used for ordering/tie-breaking
  std::vector<float> seqScores; // Pooled per-sequence playout scores, in play order
  std::string contKey;       // encodeLockPosition of the second placement (debugging aid)
};

/** Mean of the pooled playout scores (excluding immediate reward). */
float getTournamentPlayoutMean(TournamentCont const& cont){
  int n = (int) cont.seqScores.size();
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
float getTournamentContStandardError(TournamentCont const& cont){
  int n = (int) cont.seqScores.size();
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
float getTournamentContScore(TournamentCont const& cont);
void logTournamentCont(std::string label, std::string key, TournamentCont const& cont, int windowStart){
  int n = (int) cont.seqScores.size();
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
  float windowStd = windowCount > 1 ? sqrtf(std::max(windowSumSquared - windowSum * windowSum / windowCount, 0.0f) / (windowCount - 1)) : 0;
  printf("  %s %-9s cont=%-9s rew=%6.2f eval=%8.2f playoutMean=%9.2f score=%9.2f n=%d windowMean=%9.2f windowStd=%8.2f windowMin=%9.2f windowMax=%8.2f\n",
    label.c_str(), key.c_str(), cont.contKey.c_str(), cont.immediateReward, cont.evalScoreInclReward,
    getTournamentPlayoutMean(cont), getTournamentContScore(cont), n, windowMean, windowStd, windowMin, windowMax);
}

/**
 * The score a cont may publish into the final map: a conservative lower confidence bound.
 * Playout scores are heavy-tailed and shallow pools rarely contain the tail, so
 * mean - tCrit*SE keeps shallow estimates from outranking deep, well-supported ones.
 */
float getTournamentContPublishScore(TournamentCont const& cont, OUT float *outRawScore = NULL){
  int n = (int) cont.seqScores.size();
  float rawScore = getTournamentContScore(cont);
  if (outRawScore != NULL) { *outRawScore = rawScore; }
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
float getTournamentContScore(TournamentCont const& cont){
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
int getTournamentCandidateBestContIndex(TournamentCandidate const& candidate){
  int bestIdx = -1;
  float bestScore = FLOAT_MIN;
  for (int i = 0; i < (int) candidate.conts.size(); i++) {
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
TournamentCont const& getTournamentBestCont(TournamentCandidate const& candidate){
  return candidate.conts[getTournamentCandidateBestContIndex(candidate)];
}

/** Non-const overload, for round 3 where the best continuation needs new samples appended. */
TournamentCont &getTournamentBestCont(TournamentCandidate &candidate){
  return candidate.conts[getTournamentCandidateBestContIndex(candidate)];
}

/** Returns the index of the best surviving candidate (the "leader"). */
int getTournamentLeaderIndex(std::vector<TournamentCandidate> const& candidates){
  int leaderIdx = -1;
  float leaderScore = FLOAT_MIN;
  for (int i = 0; i < (int) candidates.size(); i++) {
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
int countTournamentSurvivors(std::vector<TournamentCandidate> const& candidates){
  int count = 0;
  for (auto const& candidate : candidates) {
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
bool isTournamentCandidateSignificantlyWorse(TournamentCandidate const& challenger, TournamentCont const& leaderCont, float *outMeanDiff = NULL, float *outStandardError = NULL, float zThreshold = TMT_PAIRED_TEST_Z){
  const TournamentCont &challengerCont = getTournamentBestCont(challenger);
  int n = (int) std::min(leaderCont.seqScores.size(), challengerCont.seqScores.size());
  if (n < 2){
    if (outMeanDiff != NULL) { *outMeanDiff = 0; }
    if (outStandardError != NULL) { *outStandardError = 0; }
    return false; // Not enough paired samples to conclude anything
  }
  // Align from the end (see doc); paired diffs cover the full overall score
  int leaderStart = (int) leaderCont.seqScores.size() - n;
  int challengerStart = (int) challengerCont.seqScores.size() - n;
  float rewardDiff = leaderCont.immediateReward - challengerCont.immediateReward;
  float sumDiff = 0;
  float sumDiffSquared = 0;
  for (int i = 0; i < n; i++) {
    float diff = rewardDiff + leaderCont.seqScores[leaderStart + i] - challengerCont.seqScores[challengerStart + i];
    sumDiff += diff;
    sumDiffSquared += diff * diff;
  }
  float meanDiff = sumDiff / n;
  float variance = (sumDiffSquared - sumDiff * sumDiff / n) / (n - 1);
  float standardError = sqrtf(std::max(variance, 0.0f) / n);
  if (outMeanDiff != NULL) { *outMeanDiff = meanDiff; }
  if (outStandardError != NULL) { *outStandardError = standardError; }
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
void cutTournamentCandidatesByRank(OUT std::vector<TournamentCandidate> &candidates, int cap, TournamentRankMode rankMode){
  struct CandidateRank {
    float score;
    float evalScore;
    std::string key;
    int candidateIndex;
  };
  std::vector<CandidateRank> rankings;
  for (int i = 0; i < (int) candidates.size(); i++) {
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
    rankings.push_back({
      rankScore,
      bestCont.evalScoreInclReward,
      candidates[i].key,
      i
    });
  }
  std::sort(rankings.begin(), rankings.end(), [](CandidateRank const& a, CandidateRank const& b){
    if (a.score != b.score) {
      return a.score > b.score;
    }
    if (a.evalScore != b.evalScore) {
      return a.evalScore > b.evalScore;
    }
    return a.key < b.key;
  });
  for (int i = 0; i < (int) rankings.size(); i++) {
    if (i >= cap) {
      candidates[rankings[i].candidateIndex].eliminated = true;
    }
  }
}

/** Appends `sampleCount` playouts from the shared window at `batchOffset` to the cont's pool. */
void playTournamentSamples(OUT TournamentCont &cont, int sampleCount, int batchOffset, int playoutLength, const PieceRangeContext pieceRangeContextLookup[3], int secondPieceIndex, AggressionLevel aggressionLevel){
  cont.seqScores.reserve(cont.seqScores.size() + sampleCount);
  getPlayoutScore(cont.resultingState, sampleCount, playoutLength, pieceRangeContextLookup, secondPieceIndex, aggressionLevel, /* playoutDataList */ NULL, &cont.seqScores, batchOffset);
}

/** Runs the whole 3-round tournament. See the block comment above for the scheme. */
unordered_map<string, float> getLockValueLookupTournament(GameState gameState, const Piece *firstPiece, const Piece *secondPiece, int pruningBreadth, int playoutCount, int playoutLength, int secondPieceDelay, const EvalContext *evalContext, const PieceRangeContext pieceRangeContextLookup[3]){
  unordered_map<string, float> lockValueMap;

  // Enumerate every (first, second) pair with a fast eval (same first pass as legacy)
  list<Possibility> possibilityList;
  searchDepth2(gameState, firstPiece, secondPiece, secondPieceDelay, evalContext, possibilityList);
  if (possibilityList.size() == 0){
    return lockValueMap; // Topped out: no legal first placement
  }

  // Eval-only shortcut (DAS shallow maps pass playoutCount=0): max eval per first placement.
  if (playoutCount * playoutLength == 0){
    for (Possibility const& possibility : possibilityList) {
      string lockPosEncoded = encodeLockPosition(possibility.firstPlacement);
      float evalScore = possibility.evalScoreInclReward;
      if (lockValueMap.count(lockPosEncoded) == 0 || evalScore > lockValueMap[lockPosEncoded]) {
        lockValueMap[lockPosEncoded] = evalScore;
      }
    }
    return lockValueMap;
  }

  // Sort pairs by eval desc so the per-candidate cont cap keeps the eval-best conts
  // (searchDepth2's order is unsorted, tucks last).
  std::vector<Possibility> sortedPossibilities(possibilityList.begin(), possibilityList.end());
  std::stable_sort(sortedPossibilities.begin(), sortedPossibilities.end(), [](Possibility const& a, Possibility const& b){
    return a.evalScoreInclReward > b.evalScoreInclReward;
  });

  // Group pairs into candidates (one per distinct first placement), keeping eval-best conts.
  std::vector<TournamentCandidate> candidates;
  unordered_map<string, int> candidateIndexByKey;
  for (Possibility const& possibility : sortedPossibilities) {
    string key = encodeLockPosition(possibility.firstPlacement);
    int candidateIdx;
    auto found = candidateIndexByKey.find(key);
    if (found == candidateIndexByKey.end()){
      candidateIdx = (int) candidates.size();
      candidateIndexByKey[key] = candidateIdx;
      TournamentCandidate newCandidate = { key, {}, false };
      candidates.push_back(newCandidate);
    } else {
      candidateIdx = found->second;
    }
    if ((int) candidates[candidateIdx].conts.size() < TMT_CONTINUATIONS_PER_CANDIDATE) {
      TournamentCont newCont = {
        possibility.resultingState,
        possibility.immediateReward,
        possibility.evalScoreInclReward,
        {},
        encodeLockPosition(possibility.secondPlacement)
      };
      candidates[candidateIdx].conts.push_back(newCont);
    }
  }
  int candidateCount = (int) candidates.size();

  // ---- Budget. Tournament mode (pruningBreadth == -1): playoutCount is the total budget;
  // legacy-style: pruningBreadth * playoutCount. NB: the TMT_MIN_PLAYOUTS floor can push
  // total spend past the budget (round 3's share is floored so finalists stay meaningful).
  playoutCount = std::min(playoutCount, CANONICAL_SEQUENCE_BATCH_SIZE);
  long long totalBudget;
  if (pruningBreadth == -1) {
    totalBudget = std::min((long long) playoutCount, (long long) CANONICAL_SEQUENCE_BATCH_SIZE * 64);
  } else {
    totalBudget = std::min((long long) pruningBreadth * playoutCount, (long long) CANONICAL_SEQUENCE_BATCH_SIZE * 64);
  }
  auto getRoundSampleCount = [](long long budgetShare, int entryCount) {
    long long perEntry = budgetShare / std::max(entryCount, 1);
    return (int) std::min((long long) CANONICAL_SEQUENCE_BATCH_SIZE, std::max(perEntry, (long long) TMT_MIN_PLAYOUTS));
  };

  const int secondPieceIndex = secondPiece->index;
  // samplesSoFar doubles as the next round's batch offset (fresh window per round). NB: with
  // extreme budgets the window can wrap and overlap earlier rounds; within-round CRN holds.
  int samplesSoFar = 0;

  // ---- Round 1: screen every candidate on its eval-best continuation(s) (the wide net).
  TMT_TRACE("TRACE: entering R1 (n1 pending)\n");
  // r1Conts = largest cont count whose floor-level spend fits R1's share (see TMT_R1_MAX_CONTS).
  int r1MaxConts = std::min(TMT_R1_MAX_CONTS, TMT_CONTINUATIONS_PER_CANDIDATE);
  int r1Conts = 1;
  while (r1Conts < r1MaxConts
      && (long long) candidateCount * (r1Conts + 1) * TMT_MIN_PLAYOUTS <= (long long)(totalBudget * TMT_R1_BUDGET_SHARE)) {
    r1Conts++;
  }
  int n1 = getRoundSampleCount(totalBudget * TMT_R1_BUDGET_SHARE, candidateCount * r1Conts);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT setup: candidates=%d r1Conts=%d totalBudget=%lld playoutLength=%d\n", candidateCount, r1Conts, totalBudget, playoutLength);
    for (auto const& candidate : candidates) {
      for (auto const& cont : candidate.conts) {
        if (!cont.seqScores.empty()) {
          logTournamentCont("setup", candidate.key, cont, 0);
        }
      }
    }
  }
  for (auto &candidate : candidates) {
    int contsToPlay = std::min(r1Conts, (int) candidate.conts.size());
    for (int c = 0; c < contsToPlay; c++) {
      playTournamentSamples(candidate.conts[c], n1, samplesSoFar, playoutLength, pieceRangeContextLookup, secondPieceIndex, evalContext->aggressionLevel);
    }
  }
  samplesSoFar += n1;
  TMT_TRACE("TRACE: R1 done (n1=%d)\n", n1);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT R1 done: n1=%d window=[%d,%d)\n", n1, 0, n1);
    for (auto const& candidate : candidates) {
      for (auto const& cont : candidate.conts) {
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
    printf("TMT cut1: leader=%s score=%.2f\n", candidates[leaderIdx].key.c_str(), getTournamentContScore(leaderCont));
  }
  if (TMT_CUT1_VETO_Z > 0) {
    for (int i = 0; i < candidateCount; i++) {
      if (candidates[i].eliminated || i == leaderIdx) {
        continue;
      }
      float pairedMeanDiff, pairedStandardError;
      if (isTournamentCandidateSignificantlyWorse(candidates[i], leaderCont, &pairedMeanDiff, &pairedStandardError, TMT_CUT1_VETO_Z)) {
        candidates[i].eliminated = true;
        if (TMT_LOGGING_ENABLED) {
          printf("  cut1 VETO %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(), pairedMeanDiff, pairedStandardError, pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
        }
      } else if (TMT_LOGGING_ENABLED) {
        printf("  cut1 keep %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(), pairedMeanDiff, pairedStandardError, pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
      }
    }
  }
  int survivorsAfterVeto = countTournamentSurvivors(candidates);
  cutTournamentCandidatesByRank(candidates, std::min(TMT_FINALIST_COUNT, survivorsAfterVeto), TMT_RANK_UPPER_BOUND);
  int survivorsAfterCut1 = countTournamentSurvivors(candidates);
  int spentSoFar = 0;
  for (auto const& candidate : candidates) {
    spentSoFar += std::min(r1Conts, (int) candidate.conts.size()) * n1;
  }
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut1 done: survivors(%d)=", survivorsAfterCut1);
    for (auto const& candidate : candidates) {
      if (!candidate.eliminated) {
        printf(" %s", candidate.key.c_str());
      }
    }
    printf("\n");
  }

  // ---- Round 2: sample every cont of each survivor (discovery: eval-best isn't always best).
  int n2 = getRoundSampleCount(totalBudget * TMT_R2_BUDGET_SHARE, survivorsAfterCut1 * TMT_CONTINUATIONS_PER_CANDIDATE);
  for (auto &candidate : candidates) {
    if (candidate.eliminated) {
      continue;
    }
    for (auto &cont : candidate.conts) {
      playTournamentSamples(cont, n2, samplesSoFar, playoutLength, pieceRangeContextLookup, secondPieceIndex, evalContext->aggressionLevel);
    }
  }
  samplesSoFar += n2;
  spentSoFar += survivorsAfterCut1 * TMT_CONTINUATIONS_PER_CANDIDATE * n2;
  TMT_TRACE("TRACE: R2 done (n2=%d)\n", n2);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT R2 done: n2=%d window=[%d,%d)\n", n2, n1, n1 + n2);
    for (auto const& candidate : candidates) {
      if (candidate.eliminated) {
        continue;
      }
      for (auto const& cont : candidate.conts) {
        logTournamentCont("R2", candidate.key, cont, n1);
      }
    }
  }

  // ---- Cut 2: paired veto vs the leader, then cap at TMT_R3_COUNT by raw pooled mean.
  // False positives get corrected by round 3 and the LCBs; cut alternates never recover.
  leaderIdx = getTournamentLeaderIndex(candidates);
  const TournamentCont &leaderCont2 = getTournamentBestCont(candidates[leaderIdx]);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut2: leader=%s score=%.2f\n", candidates[leaderIdx].key.c_str(), getTournamentContScore(leaderCont2));
  }
  for (int i = 0; i < candidateCount; i++) {
    if (candidates[i].eliminated || i == leaderIdx) {
      continue;
    }
    float pairedMeanDiff, pairedStandardError;
    if (isTournamentCandidateSignificantlyWorse(candidates[i], leaderCont2, &pairedMeanDiff, &pairedStandardError)) {
      candidates[i].eliminated = true;
      if (TMT_LOGGING_ENABLED) {
        printf("  cut2 VETO %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(), pairedMeanDiff, pairedStandardError, pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
      }
    } else if (TMT_LOGGING_ENABLED) {
      printf("  cut2 keep %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(), pairedMeanDiff, pairedStandardError, pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
    }
  }
  int survivorsAfterCut2Veto = countTournamentSurvivors(candidates);
  cutTournamentCandidatesByRank(candidates, std::min(TMT_R3_COUNT, survivorsAfterCut2Veto), TMT_RANK_RAW_MEAN);
  int finalists = countTournamentSurvivors(candidates);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut2 done: finalists(%d)=", finalists);
    for (auto const& candidate : candidates) {
      if (!candidate.eliminated) {
        printf(" %s", candidate.key.c_str());
      }
    }
    printf("\n");
  }

  // ---- Round 3: the remaining budget goes to each finalist's best cont, deeply.
  int n3 = getRoundSampleCount(totalBudget - spentSoFar, finalists);
  TMT_TRACE("TRACE: entering R3 (n3=%d)\n", n3);
  if (PLAYOUT_LOGGING_ENABLED || TMT_LOGGING_ENABLED) {
    printf("Tournament: candidates=%d r1Conts=%d n1=%d survivors1=%d n2=%d finalists=%d n3=%d samplesSoFar=%d\n",
      candidateCount, r1Conts, n1, survivorsAfterCut1, n2, finalists, n3, samplesSoFar + n3);
  }
  for (auto &candidate : candidates) {
    if (candidate.eliminated) {
      continue;
    }
    int contIdx = getTournamentCandidateBestContIndex(candidate);
    TMT_TRACE("TRACE: R3 playing %s cont=%s\n", candidate.key.c_str(), candidate.conts[contIdx].contKey.c_str());
    playTournamentSamples(candidate.conts[contIdx], n3, samplesSoFar, playoutLength, pieceRangeContextLookup, secondPieceIndex, evalContext->aggressionLevel);
    if (TMT_LOGGING_ENABLED) {
      logTournamentCont("R3", candidate.key, candidate.conts[contIdx], n1 + n2);
    }
  }

  // ---- Build the map: each first placement publishes the best LCB over its sampled conts;
  // unproven keys may publish low -- correct and intended.
  for (auto const& candidate : candidates) {
    int bestContIdx = -1;
    float bestScore = FLOAT_MIN;
    for (int i = 0; i < (int) candidate.conts.size(); i++) {
      if (candidate.conts[i].seqScores.empty()) {
        continue; // Never sampled (cut before its round): no playout-backed estimate exists
      }
      float score = getTournamentContPublishScore(candidate.conts[i]);
      if (bestContIdx == -1 || score > bestScore) {
        bestScore = score;
        bestContIdx = i;
      }
    }
    lockValueMap[candidate.key] = bestScore;
    if (TMT_LOGGING_ENABLED) {
      float rawScore;
      getTournamentContPublishScore(candidate.conts[bestContIdx], &rawScore);
      printf("  final %-9s = %9.2f (cont=%-9s n=%d raw=%9.2f)\n",
        candidate.key.c_str(), bestScore, candidate.conts[bestContIdx].contKey.c_str(),
        (int) candidate.conts[bestContIdx].seqScores.size(), rawScore);
    }
  }

  if (TMT_LOGGING_ENABLED) {
    // Debug-only: re-estimate the top keys' winning conts legacy-style for direct comparison.
    std::vector<std::pair<float, std::string>> ranked;
    for (auto const& entry : lockValueMap) {
      ranked.push_back({entry.second, entry.first});
    }
    std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b){ return a.first > b.first; });
    int validateCount = std::min((int) ranked.size(), 3);
    printf("TMT validation: re-playing top %d keys' winning conts with %d playouts legacy-style\n", validateCount, playoutCount);
    for (int r = 0; r < validateCount; r++) {
      for (auto const& candidate : candidates) {
        if (candidate.key != ranked[r].second) {
          continue;
        }
        int contIdx = 0;
        float bestScore = FLOAT_MIN;
        for (int i = 0; i < (int) candidate.conts.size(); i++) {
          float score = getTournamentContScore(candidate.conts[i]);
          if (score > bestScore) {
            bestScore = score;
            contIdx = i;
          }
        }
        TournamentCont const& cont = candidate.conts[contIdx];
        std::vector<float> legacyScores;
        float legacyMean = getPlayoutScore(cont.resultingState, playoutCount, playoutLength, pieceRangeContextLookup, secondPieceIndex, evalContext->aggressionLevel, /* playoutDataList */ NULL, &legacyScores);
        float legacySum = 0;
        float legacySumSquared = 0;
        float legacyMin = 1e9;
        for (float score : legacyScores) {
          legacySum += score;
          legacySumSquared += score * score;
          legacyMin = std::min(legacyMin, score);
        }
        float legacyStd = sqrtf(std::max(legacySumSquared - legacySum * legacySum / legacyScores.size(), 0.0f) / (legacyScores.size() - 1));
        printf("  validate %-9s cont=%-9s tournament=%9.2f legacy(%d)=%9.2f rew=%.2f legacyStd=%.1f legacyMin=%.1f\n",
          candidate.key.c_str(), cont.contKey.c_str(), bestScore, playoutCount, cont.immediateReward + legacyMean, cont.immediateReward, legacyStd, legacyMin);
        // Print the round-1 window's individual scores for direct comparison with the legacy tail
        printf("    r1scores:");
        for (int i = 0; i < std::min((int) cont.seqScores.size(), n1); i++) {
          printf(" %.1f", cont.seqScores[i]);
        }
        printf("\n");
      }
    }
  }
  return lockValueMap;
}
