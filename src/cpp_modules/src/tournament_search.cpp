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

/** One continuation (second placement) sampled for a candidate. All conts that survive the
 * same rounds have seqScores vectors of identical length, aligned per sequence. */
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
 * The score a cont is allowed to publish into the final map: a conservative lower
 * confidence bound of its overall score.
 *
 * Playout scores are heavy-tailed (a bad board can score hundreds below the average), and a
 * shallow pool often contains no such draws, so its raw mean sits far above the cont's true
 * expected value. Publishing mean - tCrit * std / sqrt(n) keeps estimates with different
 * sample depths comparable: a lucky shallow mean can never outrank a deep, well-supported
 * one (a finalist's deepened cont beats its own shallow conts, and cut candidates' frozen
 * round-1 scores can't top the map), and the bound converges to the raw mean as the pool
 * deepens. The shape of tCrit reflects that: a wide margin for small pools that have not
 * sampled the tail yet, relaxing toward a ~98% one-sided bound as the pool deepens.
 */
float getTournamentContPublishScore(TournamentCont const& cont, OUT float *outRawScore = NULL){
  int n = (int) cont.seqScores.size();
  float rawScore = getTournamentContScore(cont);
  if (outRawScore != NULL) { *outRawScore = rawScore; }
  if (n < 2) {
    return rawScore;
  }
  float standardError = getTournamentContStandardError(cont);
  // ~98% one-sided t critical value (2.0 at the normal limit), widened by 4.5/df for small
  // pools -- matches the true t values within a few percent for df >= 9 (the smallest pool
  // is TMT_MIN_PLAYOUTS samples)
  float tCrit = 2.0f + 4.5f / (n - 1);
  return rawScore - tCrit * standardError;
}

/** One first placement ("candidate") and its continuations, stored in eval-ranked
 * order (possibilities are sorted by eval score desc before grouping). */
struct TournamentCandidate {
  std::string key; // encodeLockPosition of the first placement
  std::vector<TournamentCont> conts;
  bool eliminated;
};

/** Current overall-score estimate for a continuation (immediate reward + mean playout
 * score), matching how the legacy lookup combines them. Conts that were never sampled
 * (round 1 only plays the eval-best cont) have empty pools; they rank below any sampled cont. */
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

/** Returns the index of the candidate's best-scoring sampled continuation (first index on
 * ties). Unsampled conts are skipped -- they have no estimate yet. */
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
 * Paired-difference significance test: is the challenger's best continuation significantly
 * worse than the leader's best continuation across the shared sequence window? Every
 * participant's pool comes entirely from the shared screening window, so the per-sequence
 * scores align exactly (common random numbers) and the mean and variance of the paired
 * differences capture how much of the gap is real rather than piece luck.
 * @returns true when the leader's advantage exceeds TMT_PAIRED_TEST_Z standard errors.
 * @param outMeanDiff / outStandardError - optional debug outputs for the computed stats.
 */
bool isTournamentCandidateSignificantlyWorse(TournamentCandidate const& challenger, TournamentCont const& leaderCont, float *outMeanDiff = NULL, float *outStandardError = NULL){
  const TournamentCont &challengerCont = getTournamentBestCont(challenger);
  int n = (int) std::min(leaderCont.seqScores.size(), challengerCont.seqScores.size());
  if (n < 2){
    if (outMeanDiff != NULL) { *outMeanDiff = 0; }
    if (outStandardError != NULL) { *outStandardError = 0; }
    return false; // Not enough paired samples to conclude anything
  }
  // Align from the end: the trailing n scores of both pools cover the same sequence windows
  int leaderStart = (int) leaderCont.seqScores.size() - n;
  int challengerStart = (int) challengerCont.seqScores.size() - n;
  // Each paired difference covers the full overall score: immediate reward + playout score
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
  return meanDiff - TMT_PAIRED_TEST_Z * standardError > 0;
}

/** Sorts the surviving candidates by their best continuation's optimistic ranking score
 * (mean + z*SE, ties: eval score, then key) and marks every candidate beyond the cap as
 * eliminated. Already-eliminated candidates stay eliminated and are excluded from the
 * ranking (their scores are frozen at their small-n estimates). */
void cutTournamentCandidatesByRank(OUT std::vector<TournamentCandidate> &candidates, int cap){
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
    rankings.push_back({
      getTournamentContScore(bestCont) + TMT_PAIRED_TEST_Z * getTournamentContStandardError(bestCont),
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

/** Plays `sampleCount` more sequences on one continuation, appending the per-sequence
 * scores to its pool. `batchOffset` is the shared sequence window for this round. */
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

  // Eval-only shortcut (used by the DAS shallow maps, which pass playoutCount=0): max eval
  // per first placement over all of its pairs -- identical to the legacy lookup's eval-only path.
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

  // Sort the pairs by eval score desc before grouping, so that the per-candidate continuation
  // cap keeps the eval-best continuations. searchDepth2 returns an unsorted list (and
  // tucks are enumerated last), so enumeration order would keep the wrong conts.
  std::vector<Possibility> sortedPossibilities(possibilityList.begin(), possibilityList.end());
  std::stable_sort(sortedPossibilities.begin(), sortedPossibilities.end(), [](Possibility const& a, Possibility const& b){
    return a.evalScoreInclReward > b.evalScoreInclReward;
  });

  // Group the pairs into candidates (one per distinct first placement), keeping the eval-best
  // continuations of each candidate. Every first placement found by searchDepth2 gets a candidate.
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

  // ---- Budget. In tournament mode (pruningBreadth == -1), playoutCount is
  // the TOTAL playout budget. Otherwise (legacy-style params), it's the per-candidate
  // playout count and the total is pruningBreadth * playoutCount, matching the legacy lookup's
  // spend. Early rounds are cheap because the sample counts are small, not because the
  // playouts are short. NB: the TMT_MIN_PLAYOUTS floor means total spend can
  // slightly exceed the legacy budget when pruningBreadth is small (round 3's share is floored
  // so the finalists always get a meaningful sample count even if the budget is exhausted).
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
  // samplesSoFar doubles as the next round's batch offset (fresh sequence window per round).
  // NB: with extreme budgets the window can wrap past CANONICAL_SEQUENCE_BATCH_SIZE and
  // overlap round 1's window via the modulo in playout.cpp. Within-round CRN still holds;
  // only the cross-round freshness degrades.
  int samplesSoFar = 0;

  // ---- Round 1: screen every candidate on its eval-best continuation (the wide net).
  TMT_TRACE("TRACE: entering R1 (n1 pending)\n");
  // One cont per candidate keeps the screening sample count high enough to rank candidates
  // reliably; the remaining continuations are only sampled if a later stage ever needs them.
  int n1 = getRoundSampleCount(totalBudget * TMT_R1_BUDGET_SHARE, candidateCount);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT setup: candidates=%d totalBudget=%lld playoutLength=%d\n", candidateCount, totalBudget, playoutLength);
    for (auto const& candidate : candidates) {
      for (auto const& cont : candidate.conts) {
        if (!cont.seqScores.empty()) {
          logTournamentCont("setup", candidate.key, cont, 0);
        }
      }
    }
  }
  for (auto &candidate : candidates) {
    playTournamentSamples(candidate.conts[0], n1, samplesSoFar, playoutLength, pieceRangeContextLookup, secondPieceIndex, evalContext->aggressionLevel);
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

  // ---- Cut: candidates significantly worse than the leader are vetoed (paired difference
  // test on the shared screening window -- every candidate's pool is that window, so the
  // pairing is direct), then the field is capped at TMT_FINALIST_COUNT by optimistic rank
  // (mean + z*SE) so a candidate isn't cut on one unlucky sample.
  int leaderIdx = getTournamentLeaderIndex(candidates);
  const TournamentCont &leaderCont = getTournamentBestCont(candidates[leaderIdx]);
  if (TMT_LOGGING_ENABLED) {
    printf("TMT cut: leader=%s score=%.2f\n", candidates[leaderIdx].key.c_str(), getTournamentContScore(leaderCont));
  }
  for (int i = 0; i < candidateCount; i++) {
    if (candidates[i].eliminated || i == leaderIdx) {
      continue;
    }
    float pairedMeanDiff, pairedStandardError;
    if (isTournamentCandidateSignificantlyWorse(candidates[i], leaderCont, &pairedMeanDiff, &pairedStandardError)) {
      candidates[i].eliminated = true;
      if (TMT_LOGGING_ENABLED) {
        printf("  cut VETO %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(), pairedMeanDiff, pairedStandardError, pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
      }
    } else if (TMT_LOGGING_ENABLED) {
      printf("  cut keep  %s meanDiff=%.2f se=%.2f (z=%.2f)\n", candidates[i].key.c_str(), pairedMeanDiff, pairedStandardError, pairedMeanDiff / std::max(pairedStandardError, 0.0001f));
    }
  }
  int survivorsAfterVeto = countTournamentSurvivors(candidates);
  cutTournamentCandidatesByRank(candidates, std::min(TMT_FINALIST_COUNT, survivorsAfterVeto));
  int finalistCount = countTournamentSurvivors(candidates);
  int spentSoFar = candidateCount * n1;
  if (TMT_LOGGING_ENABLED) {
    printf("TMT finalists(%d)=", finalistCount);
    for (auto const& candidate : candidates) {
      if (!candidate.eliminated) {
        printf(" %s", candidate.key.c_str());
      }
    }
    printf("\n");
  }

  // ---- Finals: the remaining budget goes to the finalists' eval-best continuations, deeply.
  // One continuation per finalist (the screen only samples the eval-best cont).
  int n3 = getRoundSampleCount(totalBudget - spentSoFar, finalistCount);
  TMT_TRACE("TRACE: entering finals (n3=%d)\n", n3);
  if (PLAYOUT_LOGGING_ENABLED || TMT_LOGGING_ENABLED) {
    printf("Tournament: candidates=%d n1=%d vetoSurvivors=%d finalists=%d n3=%d samplesSoFar=%d\n",
      candidateCount, n1, survivorsAfterVeto, finalistCount, n3, samplesSoFar + n3);
  }
  for (auto &candidate : candidates) {
    if (candidate.eliminated) {
      continue;
    }
    int contIdx = getTournamentCandidateBestContIndex(candidate);
    TMT_TRACE("TRACE: finals playing %s cont=%s\n", candidate.key.c_str(), candidate.conts[contIdx].contKey.c_str());
    playTournamentSamples(candidate.conts[contIdx], n3, samplesSoFar, playoutLength, pieceRangeContextLookup, secondPieceIndex, evalContext->aggressionLevel);
    if (TMT_LOGGING_ENABLED) {
      logTournamentCont("finals", candidate.key, candidate.conts[contIdx], n1);
    }
  }

  // ---- Build the map: for every first placement, the best published score across its
  // sampled continuations. Every key gets a playout-backed estimate, published as a lower
  // confidence bound (see getTournamentContPublishScore) so estimates of very different
  // sample depths are comparable: eliminated candidates' frozen round-1 means and
  // a finalist's shallower non-best conts can't outrank the finalists' deep evidence.
  // NB: unproven keys can publish low (even negative) values; that is the correct
  // conservative outcome, not a bug.
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
    // Debug-only validation: re-estimate the top keys' winning conts with the full budget,
    // legacy-style (one big shared window starting at offset 0) for direct comparison.
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
