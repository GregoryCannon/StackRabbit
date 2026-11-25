// Phase 0 Feasibility Spike:
// Measures 20-row NES Tetris (10 Hz "X.....", Level 19 & Level 18) board revisit rates,
// out-of-sample TB hit-rate curves, candidate key-encoding coverage (Option 1 & Option 2),
// height/hole distributions, and fastMoveSearch throughput.
//
// Strictly read-only against src/backend/ (zero modifications to src/).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Unity includes from src/backend/ (read-only)
#include "../../src/data/tetrominoes.hpp"
#include "../../src/backend/eval.cpp"
#include "../../src/backend/eval_context.cpp"
#include "../../src/backend/move_result.cpp"
#include "../../src/backend/move_search.cpp"
#include "../../src/backend/piece_ranges.cpp"
#include "../../src/data/ranks_base_7.cpp"

namespace phase0 {

constexpr int NUM_PIECES = 7;

// Exact NES Tetris transition numerators out of 64, in I,O,L,J,T,S,Z order.
constexpr int NES_TRANSITION_NUM[NUM_PIECES][NUM_PIECES] = {
    {2, 10, 10, 10, 10, 12, 10}, // I
    {10, 4, 10, 10, 10, 10, 10}, // O
    {10, 10, 2, 10, 12, 10, 10}, // L
    {10, 10, 10, 2, 12, 10, 10}, // J
    {10, 10, 10, 10, 2, 10, 12}, // T
    {10, 10, 10, 10, 10, 4, 10}, // S
    {10, 10, 10, 12, 10, 10, 2}, // Z
};

constexpr uint64_t NES_STATIONARY_DENOM = 335916ULL;
constexpr uint64_t NES_STATIONARY_NUM[NUM_PIECES] = {
    46655ULL, 47988ULL, 46655ULL, 47989ULL, 49284ULL, 49321ULL, 48024ULL,
};

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed) {}
  uint64_t nextU64() noexcept {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  int sampleOpeningPiece() noexcept {
    uint64_t r = nextU64() % NES_STATIONARY_DENOM;
    uint64_t acc = 0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      acc += NES_STATIONARY_NUM[p];
      if (r < acc) {
        return p;
      }
    }
    return NUM_PIECES - 1;
  }
  int sampleNextPiece(int prev) noexcept {
    int r = static_cast<int>(nextU64() & 63ULL);
    int acc = 0;
    for (int p = 0; p < NUM_PIECES; ++p) {
      acc += NES_TRANSITION_NUM[prev][p];
      if (r < acc) {
        return p;
      }
    }
    return NUM_PIECES - 1;
  }
};

// Exact lossless 200-bit 20x10 board key (row 0 = top, row 19 = bottom; 10 bits/row).
// Packed as 6 rows per uint64_t in w[0..2] (18 rows = 180 bits) + 2 rows in w[3] (20 bits).
struct RawBoard20 {
  uint64_t w[4];

  bool operator==(const RawBoard20 &o) const noexcept {
    return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3];
  }
  bool operator!=(const RawBoard20 &o) const noexcept { return !(*this == o); }
  bool operator<(const RawBoard20 &o) const noexcept {
    for (int i = 0; i < 4; ++i) {
      if (w[i] != o.w[i]) {
        return w[i] < o.w[i];
      }
    }
    return false;
  }
};

constexpr RawBoard20 EMPTY_SENTINEL = {~0ULL, ~0ULL, ~0ULL, ~0ULL};

inline RawBoard20 packRawBoard20(const unsigned int board[20]) noexcept {
  RawBoard20 out = {{0, 0, 0, 0}};
  for (int r = 0; r < 6; ++r) {
    out.w[0] |= static_cast<uint64_t>(board[r] & 0x3FFU) << (r * 10);
  }
  for (int r = 0; r < 6; ++r) {
    out.w[1] |= static_cast<uint64_t>(board[6 + r] & 0x3FFU) << (r * 10);
  }
  for (int r = 0; r < 6; ++r) {
    out.w[2] |= static_cast<uint64_t>(board[12 + r] & 0x3FFU) << (r * 10);
  }
  for (int r = 0; r < 2; ++r) {
    out.w[3] |= static_cast<uint64_t>(board[18 + r] & 0x3FFU) << (r * 10);
  }
  return out;
}

inline uint64_t mix64(uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

inline uint64_t hashRawBoard20(const RawBoard20 &k) noexcept {
  uint64_t h = mix64(k.w[0] ^ 0x243F6A8885A308D3ULL);
  h = mix64(h ^ k.w[1] ^ 0x13198A2E03707344ULL);
  h = mix64(h ^ k.w[2] ^ 0xA4093822299F31D0ULL);
  h = mix64(h ^ k.w[3] ^ 0x082EFA98EC4E6C89ULL);
  return h;
}

// Secondary uncorrelated hash for hash-table slot indexing (independent from shard bits).
inline uint64_t hashRawBoard20Slot(const RawBoard20 &k) noexcept {
  uint64_t h = mix64(k.w[3] ^ 0x452821E638D01377ULL);
  h = mix64(h ^ k.w[2] ^ 0xBE5466CF34E90C6CULL);
  h = mix64(h ^ k.w[1] ^ 0xC0AC29B7C97C50DDULL);
  h = mix64(h ^ k.w[0] ^ 0x3F84D5B5B5470917ULL);
  return h;
}

// Structural summary of a 20x10 board for encoding coverage & stratification.
struct BoardStats {
  int maxHeight;          // 0..20
  int totalHoles;         // exact empty cells strictly below column surface (sealed + overhangs)
  int rightWellBottomB;   // consecutive bottom rows == 0b1111111110 (0x3FE), capped at 15
  int rightWellActive;    // active rows above rightWellBottomB (0..20)
  int anyWellBottomB;     // consecutive bottom rows with same single empty col, capped at 15
  int anyWellActive;      // active rows above anyWellBottomB (0..20)

  bool fitsK8Opt2a() const noexcept { return maxHeight <= 15 && totalHoles <= 4; }
  bool fitsK8Opt2b() const noexcept { return maxHeight <= 16 && totalHoles <= 3; }
  bool fitsK8Opt1a() const noexcept { return rightWellActive <= 6; }
  bool fitsK16Opt2c() const noexcept { return maxHeight <= 20 && totalHoles <= 9; }
  bool fitsK16Opt1b() const noexcept { return anyWellActive <= 12; }
};

inline BoardStats analyzeBoard(const unsigned int board[20]) noexcept {
  BoardStats s = {};
  uint32_t covered = 0;
  int topNonEmpty = 20;
  for (int r = 0; r < 20; ++r) {
    uint32_t row = board[r] & 0x3FFU;
    if (row != 0 && topNonEmpty == 20) {
      topNonEmpty = r;
    }
    uint32_t holes = (~row) & covered;
    s.totalHoles += __builtin_popcount(holes);
    covered |= row;
  }
  s.maxHeight = 20 - topNonEmpty;

  // Option 1a: consecutive bottom rows == 1111111110 (col 9 open = bit 0 clear => 0x3FE).
  // Note: CELL_BIT(x) = 1 << (9 - x), so col 9 is bit 0 => 0x3FE.
  constexpr uint32_t RIGHT_WELL_ROW = 0x3FEU;
  int rwB = 0;
  for (int r = 19; r >= 0 && rwB < 15; --r) {
    if ((board[r] & 0x3FFU) == RIGHT_WELL_ROW) {
      rwB++;
    } else {
      break;
    }
  }
  s.rightWellBottomB = rwB;
  s.rightWellActive = std::max(0, s.maxHeight - rwB);

  // Option 1b: consecutive bottom rows sharing any identical single-column open well (popcount == 9).
  int awB = 0;
  uint32_t bottomRow = board[19] & 0x3FFU;
  if (__builtin_popcount(bottomRow) == 9) {
    for (int r = 19; r >= 0 && awB < 15; --r) {
      if ((board[r] & 0x3FFU) == bottomRow) {
        awB++;
      } else {
        break;
      }
    }
  }
  s.anyWellBottomB = awB;
  s.anyWellActive = std::max(0, s.maxHeight - awB);
  return s;
}

inline int heightBucket(int h) noexcept {
  if (h <= 8) return 0;
  if (h <= 12) return 1;
  if (h <= 15) return 2;
  if (h == 16) return 3;
  return 4;
}

inline int holeBucket(int holes) noexcept {
  if (holes <= 4) return holes;
  if (holes <= 9) return 5;
  return 6;
}

// Fast open-addressing map for RawBoard20 -> uint32_t
struct FlatBoardMap {
  struct Slot {
    RawBoard20 key;
    uint32_t val;
  };
  std::vector<Slot> slots;
  size_t mask = 0;
  size_t count = 0;

  explicit FlatBoardMap(size_t initialCap = 1024) { rehash(initialCap); }

  void rehash(size_t newCap) {
    size_t cap = 16;
    while (cap < newCap) {
      cap <<= 1;
    }
    std::vector<Slot> next(cap, {EMPTY_SENTINEL, 0});
    size_t nextMask = cap - 1;
    for (const auto &s : slots) {
      if (s.key != EMPTY_SENTINEL) {
        size_t idx = hashRawBoard20Slot(s.key) & nextMask;
        while (next[idx].key != EMPTY_SENTINEL) {
          idx = (idx + 1) & nextMask;
        }
        next[idx] = s;
      }
    }
    slots.swap(next);
    mask = nextMask;
  }

  void increment(const RawBoard20 &k, uint32_t delta = 1) {
    if ((count + 1) * 2 > slots.size()) {
      rehash(slots.size() * 2);
    }
    size_t idx = hashRawBoard20Slot(k) & mask;
    while (true) {
      if (slots[idx].key == EMPTY_SENTINEL) {
        slots[idx].key = k;
        slots[idx].val = delta;
        count++;
        return;
      }
      if (slots[idx].key == k) {
        slots[idx].val += delta;
        return;
      }
      idx = (idx + 1) & mask;
    }
  }

  void insertVal(const RawBoard20 &k, uint32_t v) {
    if ((count + 1) * 2 > slots.size()) {
      rehash(slots.size() * 2);
    }
    size_t idx = hashRawBoard20Slot(k) & mask;
    while (slots[idx].key != EMPTY_SENTINEL) {
      idx = (idx + 1) & mask;
    }
    slots[idx] = {k, v};
    count++;
  }

  uint32_t findRank(const RawBoard20 &k) const noexcept {
    if (slots.empty()) {
      return UINT32_MAX;
    }
    size_t idx = hashRawBoard20Slot(k) & mask;
    while (true) {
      if (slots[idx].key == EMPTY_SENTINEL) {
        return UINT32_MAX;
      }
      if (slots[idx].key == k) {
        return slots[idx].val;
      }
      idx = (idx + 1) & mask;
    }
  }
};

struct ShardedCounter {
  static constexpr size_t NUM_SHARDS = 64;
  struct alignas(128) Shard {
    std::mutex mu;
    FlatBoardMap map;
    Shard() : map(1 << 14) {}
  };
  std::array<Shard, NUM_SHARDS> shards;

  void addBatch(const std::vector<RawBoard20> &batch) {
    std::array<std::vector<RawBoard20>, NUM_SHARDS> byShard;
    for (const auto &k : batch) {
      size_t sh = hashRawBoard20(k) & (NUM_SHARDS - 1);
      byShard[sh].push_back(k);
    }
    for (size_t sh = 0; sh < NUM_SHARDS; ++sh) {
      if (byShard[sh].empty()) {
        continue;
      }
      std::lock_guard<std::mutex> lk(shards[sh].mu);
      for (const auto &k : byShard[sh]) {
        shards[sh].map.increment(k, 1);
      }
    }
  }
};

struct ShardedRankIndex {
  static constexpr size_t NUM_SHARDS = 64;
  std::array<FlatBoardMap, NUM_SHARDS> shards;

  void buildFromSorted(const std::vector<std::pair<RawBoard20, uint32_t>> &sorted) {
    std::array<size_t, NUM_SHARDS> counts = {};
    for (const auto &kv : sorted) {
      counts[hashRawBoard20(kv.first) & (NUM_SHARDS - 1)]++;
    }
    for (size_t sh = 0; sh < NUM_SHARDS; ++sh) {
      shards[sh] = FlatBoardMap(counts[sh] * 2 + 16);
    }
    for (size_t rank = 0; rank < sorted.size(); ++rank) {
      const RawBoard20 &k = sorted[rank].first;
      size_t sh = hashRawBoard20(k) & (NUM_SHARDS - 1);
      shards[sh].insertVal(k, static_cast<uint32_t>(rank));
    }
  }

  uint32_t findRank(const RawBoard20 &k) const noexcept {
    size_t sh = hashRawBoard20(k) & (NUM_SHARDS - 1);
    return shards[sh].findRank(k);
  }
};

// Choose 2-ply eval.cpp move (10 Hz "X.....", no playouts, exact StackRabbit fastEval + fastMoveSearch).
bool chooseEval2PlyMove(const GameState &state, int curPieceIdx, int nextPieceIdx,
                        const PieceRangeContext pieceRangeLookup[4], AggressionLevel aggro,
                        bool stationaryLevel, std::vector<LockPlacement> &firstMoves,
                        std::vector<LockPlacement> &secondMoves, LockPlacement &outBest,
                        GameState &outAfterBest) {
  const EvalContext ctx1 = getEvalContext(state, pieceRangeLookup, false, aggro);
  firstMoves.clear();
  fastMoveSearch(state, &PIECE_LIST[curPieceIdx], ctx1.pieceRangeContext.inputFrameTimeline,
                 firstMoves);
  if (firstMoves.empty()) {
    return false;
  }

  float bestTotal = FLOAT_MIN;
  bool foundAny = false;
  LockPlacement bestMove = firstMoves[0];
  GameState bestAfter = {};

  for (const LockPlacement &m1 : firstMoves) {
    GameState after1 = advanceGameState(state, m1, &ctx1);
    if (stationaryLevel) {
      after1.level = state.level;
    }
    const int cleared1 = after1.lines - state.lines;
    const float r1 = getLineClearFactor(cleared1, ctx1.weights, ctx1.shouldRewardLineClears,
                                        state.lines, ctx1.lineoutAt29);
    const EvalContext ctx2 = getEvalContext(after1, pieceRangeLookup, false, aggro);

    secondMoves.clear();
    fastMoveSearch(after1, &PIECE_LIST[nextPieceIdx], ctx2.pieceRangeContext.inputFrameTimeline,
                   secondMoves);

    float bestPairScore = FLOAT_MIN;
    if (secondMoves.empty()) {
      // Ply-2 top-out penalty
      bestPairScore = -1e6f + r1;
    } else {
      for (const LockPlacement &m2 : secondMoves) {
        GameState after2 = advanceGameState(after1, m2, &ctx2);
        if (stationaryLevel) {
          after2.level = state.level;
        }
        float e2 = fastEval(after1, after2, m2, &ctx2, nullptr);
        float total = r1 + e2;
        if (total > bestPairScore) {
          bestPairScore = total;
        }
      }
    }

    if (!foundAny || bestPairScore > bestTotal) {
      foundAny = true;
      bestTotal = bestPairScore;
      bestMove = m1;
      bestAfter = after1;
    }
  }

  outBest = bestMove;
  outAfterBest = bestAfter;
  return foundAny;
}

inline int nesLinePoints(int linesCleared, int levelAfter) noexcept {
  static constexpr int BASE[5] = {0, 40, 100, 300, 1200};
  if (linesCleared < 0 || linesCleared > 4) {
    return 0;
  }
  return BASE[linesCleared] * (levelAfter + 1);
}

struct TestAgg {
  static constexpr size_t NUM_CAPS = 7;
  static constexpr uint32_t CAPS[NUM_CAPS] = {
      100000U, 500000U, 1000000U, 2500000U, 5000000U, 10000000U, UINT32_MAX,
  };

  uint64_t games = 0;
  uint64_t totalMoves = 0;
  uint64_t totalLines = 0;
  uint64_t totalScore = 0;
  uint64_t capHits[NUM_CAPS] = {};

  // Stratified by heightBucket (5) and holeBucket (7)
  uint64_t hTotal[5] = {};
  uint64_t hHit25M[5] = {};
  uint64_t hHitAll[5] = {};
  uint64_t holeTotal[7] = {};
  uint64_t holeHit25M[7] = {};
  uint64_t holeHitAll[7] = {};

  // Encoding coverage on ALL test moves and on REVISITED (in-train-set) test moves
  uint64_t encAll[5] = {};      // K8-Opt2a, K8-Opt2b, K8-Opt1a, K16-Opt2c, K16-Opt1b
  uint64_t encHitAll[5] = {};   // among test moves hitting ANY train board
  uint64_t encHit25M[5] = {};   // among test moves hitting top 2.5M train boards

  void merge(const TestAgg &o) {
    games += o.games;
    totalMoves += o.totalMoves;
    totalLines += o.totalLines;
    totalScore += o.totalScore;
    for (size_t i = 0; i < NUM_CAPS; ++i) capHits[i] += o.capHits[i];
    for (int i = 0; i < 5; ++i) {
      hTotal[i] += o.hTotal[i];
      hHit25M[i] += o.hHit25M[i];
      hHitAll[i] += o.hHitAll[i];
      encAll[i] += o.encAll[i];
      encHitAll[i] += o.encHitAll[i];
      encHit25M[i] += o.encHit25M[i];
    }
    for (int i = 0; i < 7; ++i) {
      holeTotal[i] += o.holeTotal[i];
      holeHit25M[i] += o.holeHit25M[i];
      holeHitAll[i] += o.holeHitAll[i];
    }
  }
};

constexpr uint32_t TestAgg::CAPS[TestAgg::NUM_CAPS];

struct Config {
  int startLevel = 19;
  int startLines = 130;
  int maxLinesGain = 100; // 100 for L19->29, 130 for L18->19, 0 = uncapped (capped by maxPieces)
  int maxPieces = 1500;
  bool stationaryLevel = true;
  int trainGames = 500;
  int testGames = 500;
  int threads = 0;
  uint64_t trainSeed = 10000000ULL;
  uint64_t testSeed = 30000000ULL;
  AggressionLevel aggro = AGGRO;
  bool runMicroBench = true;
};

void recordEncodingFlags(const BoardStats &s, uint64_t enc[5]) noexcept {
  if (s.fitsK8Opt2a()) enc[0]++;
  if (s.fitsK8Opt2b()) enc[1]++;
  if (s.fitsK8Opt1a()) enc[2]++;
  if (s.fitsK16Opt2c()) enc[3]++;
  if (s.fitsK16Opt1b()) enc[4]++;
}

void runExperiment(const Config &cfg) {
  const int numThreads =
      cfg.threads > 0 ? cfg.threads : std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  const char *timeline = "X.....";
  const PieceRangeContext pieceRangeLookup[4] = {
      getPieceRangeContext(timeline, 1, true),
      getPieceRangeContext(timeline, 1, false),
      getPieceRangeContext(timeline, 2, false),
      getPieceRangeContext(timeline, 3, false),
  };

  printf("\n====================================================================\n");
  printf("Phase 0 Spike: Level %d (startLines=%d, maxLinesGain=%d, stationary=%d, 10Hz)\n",
         cfg.startLevel, cfg.startLines, cfg.maxLinesGain, cfg.stationaryLevel ? 1 : 0);
  printf("trainGames=%d, testGames=%d, threads=%d\n", cfg.trainGames, cfg.testGames, numThreads);
  printf("====================================================================\n");

  // --- Stage 1: Train games (visit counting) ---
  auto t0 = std::chrono::steady_clock::now();
  ShardedCounter counter;
  std::atomic<int> nextGame{0};
  std::atomic<uint64_t> trainMoves{0};
  std::atomic<uint64_t> trainLines{0};
  std::atomic<uint64_t> trainScore{0};

  {
    std::vector<std::thread> workers;
    workers.reserve(numThreads);
    for (int tid = 0; tid < numThreads; ++tid) {
      workers.emplace_back([&]() {
        std::vector<LockPlacement> m1, m2;
        m1.reserve(64);
        m2.reserve(64);
        std::vector<RawBoard20> batch;
        batch.reserve(16384);
        uint64_t localMoves = 0, localLines = 0, localScore = 0;

        while (true) {
          int g = nextGame.fetch_add(1, std::memory_order_relaxed);
          if (g >= cfg.trainGames) {
            break;
          }
          Rng rng(cfg.trainSeed + static_cast<uint64_t>(g) * 1009ULL);
          GameState st = {};
          st.level = cfg.startLevel;
          st.lines = cfg.startLines;

          int curP = rng.sampleOpeningPiece();
          int nextP = rng.sampleNextPiece(curP);
          int pieces = 0;

          while (pieces < cfg.maxPieces) {
            if (cfg.maxLinesGain > 0 && (st.lines - cfg.startLines) >= cfg.maxLinesGain) {
              break;
            }
            batch.push_back(packRawBoard20(st.board));
            if (batch.size() >= 16384) {
              counter.addBatch(batch);
              batch.clear();
            }
            localMoves++;

            LockPlacement best = {};
            GameState after = {};
            if (!chooseEval2PlyMove(st, curP, nextP, pieceRangeLookup, cfg.aggro,
                                    cfg.stationaryLevel, m1, m2, best, after)) {
              break;
            }
            int cleared = after.lines - st.lines;
            localScore += static_cast<uint64_t>(nesLinePoints(cleared, after.level));
            st = after;
            curP = nextP;
            nextP = rng.sampleNextPiece(curP);
            pieces++;
          }
          localLines += static_cast<uint64_t>(st.lines - cfg.startLines);
        }
        if (!batch.empty()) {
          counter.addBatch(batch);
        }
        trainMoves.fetch_add(localMoves, std::memory_order_relaxed);
        trainLines.fetch_add(localLines, std::memory_order_relaxed);
        trainScore.fetch_add(localScore, std::memory_order_relaxed);
      });
    }
    for (auto &w : workers) w.join();
  }
  auto t1 = std::chrono::steady_clock::now();
  double trainSec = std::chrono::duration<double>(t1 - t0).count();

  // Extract all unique boards + visits
  size_t totalUnique = 0;
  for (size_t sh = 0; sh < ShardedCounter::NUM_SHARDS; ++sh) {
    totalUnique += counter.shards[sh].map.count;
  }
  std::vector<std::pair<RawBoard20, uint32_t>> uniqueBoards;
  uniqueBoards.reserve(totalUnique);
  uint64_t visitsGe2Boards = 0;
  uint64_t visitsGe2Moves = 0;
  uint64_t visitsGe5Boards = 0;
  uint64_t visitsGe5Moves = 0;
  for (size_t sh = 0; sh < ShardedCounter::NUM_SHARDS; ++sh) {
    for (const auto &s : counter.shards[sh].map.slots) {
      if (s.key != EMPTY_SENTINEL) {
        uniqueBoards.emplace_back(s.key, s.val);
        if (s.val >= 2) {
          visitsGe2Boards++;
          visitsGe2Moves += s.val;
        }
        if (s.val >= 5) {
          visitsGe5Boards++;
          visitsGe5Moves += s.val;
        }
      }
    }
    // Free shard memory immediately
    std::vector<FlatBoardMap::Slot>().swap(counter.shards[sh].map.slots);
  }
  std::sort(uniqueBoards.begin(), uniqueBoards.end(),
            [](const auto &a, const auto &b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });

  const uint64_t tm = trainMoves.load();
  printf("Train: %llu moves in %.2fs (%.1fk mv/s) | avg %.1f pieces/game, %.1f lines/game, avg score %.0f\n",
         static_cast<unsigned long long>(tm), trainSec, tm / std::max(1e-6, trainSec) / 1e3,
         static_cast<double>(tm) / cfg.trainGames,
         static_cast<double>(trainLines.load()) / cfg.trainGames,
         static_cast<double>(trainScore.load()) / cfg.trainGames);
  printf("Unique boards: %zu (%.2f%% of moves) | in-sample revisit (v>=2): %.2f%% of moves (%llu boards), (v>=5): %.2f%% (%llu boards)\n",
         totalUnique, 100.0 * totalUnique / std::max<uint64_t>(1, tm),
         100.0 * visitsGe2Moves / std::max<uint64_t>(1, tm),
         static_cast<unsigned long long>(visitsGe2Boards),
         100.0 * visitsGe5Moves / std::max<uint64_t>(1, tm),
         static_cast<unsigned long long>(visitsGe5Boards));

  // Build rank index for out-of-sample test evaluation
  ShardedRankIndex rankIdx;
  rankIdx.buildFromSorted(uniqueBoards);
  // Free sorted vector to save RAM
  std::vector<std::pair<RawBoard20, uint32_t>>().swap(uniqueBoards);

  // --- Stage 2: Disjoint held-out test games ---
  auto t2 = std::chrono::steady_clock::now();
  std::atomic<int> nextTestGame{0};
  std::vector<TestAgg> perThread(numThreads);
  std::vector<GameState> sampleBoards;
  std::mutex sampleMu;

  {
    std::vector<std::thread> workers;
    workers.reserve(numThreads);
    for (int tid = 0; tid < numThreads; ++tid) {
      workers.emplace_back([&, tid]() {
        TestAgg &agg = perThread[tid];
        std::vector<LockPlacement> m1, m2;
        m1.reserve(64);
        m2.reserve(64);
        std::vector<GameState> localSamples;

        while (true) {
          int g = nextTestGame.fetch_add(1, std::memory_order_relaxed);
          if (g >= cfg.testGames) {
            break;
          }
          agg.games++;
          Rng rng(cfg.testSeed + static_cast<uint64_t>(g) * 1009ULL);
          GameState st = {};
          st.level = cfg.startLevel;
          st.lines = cfg.startLines;

          int curP = rng.sampleOpeningPiece();
          int nextP = rng.sampleNextPiece(curP);
          int pieces = 0;

          while (pieces < cfg.maxPieces) {
            if (cfg.maxLinesGain > 0 && (st.lines - cfg.startLines) >= cfg.maxLinesGain) {
              break;
            }
            if (cfg.runMicroBench && localSamples.size() < 1500 && (pieces % 3 == 0)) {
              localSamples.push_back(st);
            }

            RawBoard20 key = packRawBoard20(st.board);
            uint32_t rank = rankIdx.findRank(key);
            BoardStats bs = analyzeBoard(st.board);
            int hb = heightBucket(bs.maxHeight);
            int hob = holeBucket(bs.totalHoles);

            agg.totalMoves++;
            agg.hTotal[hb]++;
            agg.holeTotal[hob]++;
            recordEncodingFlags(bs, agg.encAll);

            if (rank != UINT32_MAX) {
              agg.hHitAll[hb]++;
              agg.holeHitAll[hob]++;
              recordEncodingFlags(bs, agg.encHitAll);
              for (size_t c = 0; c < TestAgg::NUM_CAPS; ++c) {
                if (rank < TestAgg::CAPS[c]) {
                  agg.capHits[c]++;
                }
              }
              if (rank < 2500000U) {
                agg.hHit25M[hb]++;
                agg.holeHit25M[hob]++;
                recordEncodingFlags(bs, agg.encHit25M);
              }
            }

            LockPlacement best = {};
            GameState after = {};
            if (!chooseEval2PlyMove(st, curP, nextP, pieceRangeLookup, cfg.aggro,
                                    cfg.stationaryLevel, m1, m2, best, after)) {
              break;
            }
            int cleared = after.lines - st.lines;
            agg.totalScore += static_cast<uint64_t>(nesLinePoints(cleared, after.level));
            st = after;
            curP = nextP;
            nextP = rng.sampleNextPiece(curP);
            pieces++;
          }
          agg.totalLines += static_cast<uint64_t>(st.lines - cfg.startLines);
        }

        if (!localSamples.empty()) {
          std::lock_guard<std::mutex> lk(sampleMu);
          for (const auto &gs : localSamples) {
            if (sampleBoards.size() < 10000) {
              sampleBoards.push_back(gs);
            }
          }
        }
      });
    }
    for (auto &w : workers) w.join();
  }
  auto t3 = std::chrono::steady_clock::now();
  double testSec = std::chrono::duration<double>(t3 - t2).count();

  TestAgg agg;
  for (const auto &p : perThread) agg.merge(p);

  const double invMoves = 100.0 / std::max<uint64_t>(1, agg.totalMoves);
  const double invHitAll = 100.0 / std::max<uint64_t>(1, agg.capHits[TestAgg::NUM_CAPS - 1]);
  const double invHit25M = 100.0 / std::max<uint64_t>(1, agg.capHits[3]);

  printf("\nHeld-Out Test (%llu games, %llu moves in %.2fs | avg %.1f lines, score %.0f):\n",
         static_cast<unsigned long long>(agg.games),
         static_cast<unsigned long long>(agg.totalMoves), testSec,
         static_cast<double>(agg.totalLines) / agg.games,
         static_cast<double>(agg.totalScore) / agg.games);

  printf("  Out-of-sample TB Hit Rate by Top-N Cap:\n");
  static constexpr const char *CAP_NAMES[TestAgg::NUM_CAPS] = {
      "100k", "500k", "1M", "2.5M", "5M", "10M", "ALL",
  };
  for (size_t c = 0; c < TestAgg::NUM_CAPS; ++c) {
    printf("    Top %-4s : %6.2f%% (%llu / %llu)\n", CAP_NAMES[c], agg.capHits[c] * invMoves,
           static_cast<unsigned long long>(agg.capHits[c]),
           static_cast<unsigned long long>(agg.totalMoves));
  }

  printf("\n  Stratified by maxHeight:\n");
  static constexpr const char *H_NAMES[5] = {"<=8  ", "9..12", "13..15", "16   ", "17+  "};
  for (int i = 0; i < 5; ++i) {
    double share = agg.hTotal[i] * invMoves;
    double hit25 = agg.hTotal[i] ? (100.0 * agg.hHit25M[i] / agg.hTotal[i]) : 0.0;
    double hitAll = agg.hTotal[i] ? (100.0 * agg.hHitAll[i] / agg.hTotal[i]) : 0.0;
    printf("    H=%s : %6.2f%% of moves | hit@2.5M=%6.2f%% | hit@ALL=%6.2f%%\n",
           H_NAMES[i], share, hit25, hitAll);
  }

  printf("\n  Stratified by totalHoles (sealed + overhangs):\n");
  static constexpr const char *HOLE_NAMES[7] = {"0   ", "1   ", "2   ", "3   ", "4   ", "5..9", "10+ "};
  for (int i = 0; i < 7; ++i) {
    double share = agg.holeTotal[i] * invMoves;
    double hit25 = agg.holeTotal[i] ? (100.0 * agg.holeHit25M[i] / agg.holeTotal[i]) : 0.0;
    double hitAll = agg.holeTotal[i] ? (100.0 * agg.holeHitAll[i] / agg.holeTotal[i]) : 0.0;
    printf("    holes=%s : %6.2f%% of moves | hit@2.5M=%6.2f%% | hit@ALL=%6.2f%%\n",
           HOLE_NAMES[i], share, hit25, hitAll);
  }

  printf("\n  Candidate Key-Encoding Coverage:\n");
  static constexpr const char *ENC_NAMES[5] = {
      "K8-Opt2a  (8B: H<=15, <=4 holes)    ",
      "K8-Opt2b  (8B: H<=16, <=3 holes)    ",
      "K8-Opt1a  (8B: right-well + <=6 act)",
      "K16-Opt2c (16B: H<=20, <=9 holes)   ",
      "K16-Opt1b (16B: any-well + <=12 act)",
  };
  for (int i = 0; i < 5; ++i) {
    printf("    %s : ALL moves=%6.2f%% | Hit@ALL=%6.2f%% | Hit@2.5M=%6.2f%%\n",
           ENC_NAMES[i], agg.encAll[i] * invMoves, agg.encHitAll[i] * invHitAll,
           agg.encHit25M[i] * invHit25M);
  }

  // --- Stage 3: Move-Gen Throughput Micro-Benchmark ---
  if (cfg.runMicroBench && !sampleBoards.empty()) {
    const int reps = 3;
    std::vector<LockPlacement> out;
    out.reserve(64);
    uint64_t totalPlacements = 0;
    uint64_t calls = static_cast<uint64_t>(sampleBoards.size()) * NUM_PIECES * reps;
    auto mb0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < reps; ++rep) {
      for (const GameState &gs : sampleBoards) {
        for (int p = 0; p < NUM_PIECES; ++p) {
          out.clear();
          fastMoveSearch(gs, &PIECE_LIST[p], timeline, out);
          totalPlacements += out.size();
        }
      }
    }
    auto mb1 = std::chrono::steady_clock::now();
    double mbSec = std::chrono::duration<double>(mb1 - mb0).count();
    double nsPerCall = (mbSec * 1e9) / std::max<uint64_t>(1, calls);
    double avgBranch = static_cast<double>(totalPlacements) / std::max<uint64_t>(1, calls);
    // Projected T_outer on 10 cores (assuming 0.85 parallel efficiency):
    // T_outer = N * 7 * nsPerCall / (10 * 0.85)
    double tOuter25M = (2.5e6 * 7.0 * nsPerCall * 1e-9) / (numThreads * 0.85);
    double tOuter5M = (5.0e6 * 7.0 * nsPerCall * 1e-9) / (numThreads * 0.85);
    double tOuter10M = (10.0e6 * 7.0 * nsPerCall * 1e-9) / (numThreads * 0.85);
    printf("\n  Move-Gen Micro-Benchmark (%zu live boards x 7 pieces x %d reps, 1T):\n",
           sampleBoards.size(), reps);
    printf("    fastMoveSearch (10Hz): %.0f ns/call (avg %.1f moves/call)\n", nsPerCall, avgBranch);
    printf("    Projected T_outer (%dT, 85%% eff): 2.5M=%.1fs | 5M=%.1fs | 10M=%.1fs (move-gen only)\n",
           numThreads, tOuter25M, tOuter5M, tOuter10M);
  }
}

} // namespace phase0

int main(int argc, char **argv) {
  phase0::Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto parseVal = [&](const char *prefix, auto &dst) -> bool {
      size_t len = strlen(prefix);
      if (arg.compare(0, len, prefix) == 0) {
        dst = static_cast<std::decay_t<decltype(dst)>>(std::stoll(arg.substr(len)));
        return true;
      }
      return false;
    };
    if (parseVal("--level=", cfg.startLevel)) continue;
    if (parseVal("--start-lines=", cfg.startLines)) continue;
    if (parseVal("--max-lines=", cfg.maxLinesGain)) continue;
    if (parseVal("--max-pieces=", cfg.maxPieces)) continue;
    if (parseVal("--train-games=", cfg.trainGames)) continue;
    if (parseVal("--test-games=", cfg.testGames)) continue;
    if (parseVal("--threads=", cfg.threads)) continue;
    if (arg == "--stationary=0") { cfg.stationaryLevel = false; continue; }
    if (arg == "--stationary=1") { cfg.stationaryLevel = true; continue; }
    if (arg == "--no-bench") { cfg.runMicroBench = false; continue; }
    if (arg == "--aggro=safe") { cfg.aggro = SAFE; continue; }
    if (arg == "--aggro=aggro") { cfg.aggro = AGGRO; continue; }
    if (arg == "--aggro=hyper") { cfg.aggro = HYPER; continue; }
    fprintf(stderr, "Unknown arg: %s\n", arg.c_str());
    return 1;
  }

  phase0::runExperiment(cfg);
  return 0;
}
