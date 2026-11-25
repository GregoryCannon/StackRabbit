#ifndef DAS_TABLEBASE_EVAL_ADAPTER_HPP
#define DAS_TABLEBASE_EVAL_ADAPTER_HPP

// Read-only adapter bridging experimental/das_tablebase/ to src/backend/ for parity tests,
// baseline benchmarking, precompute evaluation, and eval-based state seeding.
// Never included by standalone core, solver, or NNUE headers.

#include "core_20row.hpp"

// Unity includes from src/backend/ (strictly read-only).
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "../../src/data/tetrominoes.hpp"
#include "../../src/data/canonical_sequences.cpp"
#include "../../src/backend/eval.cpp"
#include "../../src/backend/eval_context.cpp"
#include "../../src/backend/move_result.cpp"
#include "../../src/backend/move_search.cpp"
#include "../../src/backend/full_move_search.cpp"
#include "../../src/backend/piece_ranges.cpp"
#include "../../src/backend/playout.cpp"
#include "../../src/backend/high_level_search.cpp"
#include "../../src/backend/precompute.cpp"
#include "../../src/backend/tournament_search.cpp"
#include "../../src/backend/piece_rng.cpp"
#include "../../src/data/ranks_base_7.cpp"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <chrono>
#include <vector>

namespace das {

inline constexpr const char *TIMELINE_10HZ_STR = "X.....";

struct BackendContexts {
  PieceRangeContext lookup[4];

  static BackendContexts make10Hz() {
    BackendContexts bc = {};
    bc.lookup[0] = getPieceRangeContext(TIMELINE_10HZ_STR, 1, /* gravityDoubled= */ true);
    bc.lookup[1] = getPieceRangeContext(TIMELINE_10HZ_STR, 1, /* gravityDoubled= */ false);
    bc.lookup[2] = getPieceRangeContext(TIMELINE_10HZ_STR, 2, /* gravityDoubled= */ false);
    bc.lookup[3] = getPieceRangeContext(TIMELINE_10HZ_STR, 3, /* gravityDoubled= */ false);
    return bc;
  }
};

// Reconstructs a canonical GameState from clean 20x10 occupancy (matching src/backend/main.cpp).
inline GameState board20ToGameState(const Board20 &b, int level, int lines,
                                    const BackendContexts &bc, AggressionLevel aggro) {
  GameState gs = {};
  gs.level = level;
  gs.lines = lines;
  const uint16_t *rPtr = b.rows();
  for (int r = 0; r < BOARD_ROWS; ++r) {
    gs.board[r] = static_cast<unsigned int>(rPtr[r] & FULL_ROW_MASK);
  }
  getSurfaceArray(gs.board, gs.surfaceArray);
  auto initHoles = updateSurfaceAndHoles(gs.surfaceArray, gs.board, WELL_COLUMN, false);
  gs.numTrueHoles = initHoles.first;
  gs.numPartialHoles = initHoles.second;

  const EvalContext ctx = getEvalContext(gs, bc.lookup, /* useTestWeights= */ false, aggro);
  auto holes2 = updateSurfaceAndHoles(gs.surfaceArray, gs.board,
                                      ctx.countWellHoles ? -1 : ctx.wellColumn, ctx.aiMode == DIG);
  gs.numTrueHoles = holes2.first;
  gs.numPartialHoles = holes2.second;
  return gs;
}

inline Board20 gameStateToBoard20(const GameState &gs) noexcept {
  return Board20::fromRows(gs.board);
}

struct Eval2PlyResult {
  bool hasMove = false;
  LockPlacement bestMove = {};
  int linesCleared = 0;
  GameState afterState = {};
};

// Fast 2-ply eval.cpp move selector (10 Hz, 0-frame, no playouts).
// Returns canonicalized post-move GameState (matching production main.cpp per-turn state rebuild).
// Ply-2 top-outs receive -1e6f + r1 so the current piece still gets placed if legal.
inline Eval2PlyResult chooseEval2PlyMove10Hz(const GameState &state, int curPieceIdx,
                                             int nextPieceIdx, const BackendContexts &bc,
                                             AggressionLevel aggro, bool stationaryLevel,
                                             std::vector<LockPlacement> &firstMoves,
                                             std::vector<LockPlacement> &secondMoves) {
  Eval2PlyResult res = {};
  const EvalContext ctx1 = getEvalContext(state, bc.lookup, false, aggro);
  firstMoves.clear();
  fastMoveSearch(state, &PIECE_LIST[curPieceIdx], ctx1.pieceRangeContext.inputFrameTimeline,
                 firstMoves);
  if (firstMoves.empty()) {
    return res;
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
    const EvalContext ctx2 = getEvalContext(after1, bc.lookup, false, aggro);

    secondMoves.clear();
    fastMoveSearch(after1, &PIECE_LIST[nextPieceIdx], ctx2.pieceRangeContext.inputFrameTimeline,
                   secondMoves);

    float bestPairScore = FLOAT_MIN;
    if (secondMoves.empty()) {
      bestPairScore = -1e6f + r1;
    } else {
      for (const LockPlacement &m2 : secondMoves) {
        GameState after2 = advanceGameState(after1, m2, &ctx2);
        if (stationaryLevel) {
          after2.level = state.level;
        }
        const float e2 = fastEval(after1, after2, m2, &ctx2, nullptr);
        const float total = r1 + e2;
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

  if (!foundAny) {
    return res;
  }

  const int cleared = bestAfter.lines - state.lines;
  const int nextLines = stationaryLevel ? state.lines : bestAfter.lines;
  const int nextLevel = stationaryLevel ? state.level : bestAfter.level;
  res.hasMove = true;
  res.bestMove = bestMove;
  res.linesCleared = cleared;
  res.afterState =
      board20ToGameState(gameStateToBoard20(bestAfter), nextLevel, nextLines, bc, aggro);
  return res;
}

struct PrecomputeOptions {
  int dasCharge = DAS_CHARGE_MAX; // -1 = tap mode, 0..16 = DAS mode
  int reactionTime = 18;          // 0..60 frames
  int playoutCount = 0;
  int playoutLength = 0;
  int pruningBreadth = 12;
  double calibratedMovesPerMs7T = 0.0; // Used when pruningBreadth == -2
  AggressionLevel aggro = AGGRO;
  bool stationaryLevel = false;
};

// Startup 7-thread precompute speed calibration matching src/backend/live_game_server.cpp,
// ensuring timed (-2) requests accurately budget Round 1 on the host CPU.
inline double calibratePrecomputeSpeed7T(const BackendContexts &bc) {
  const auto started = std::chrono::steady_clock::now();
  constexpr int CALIB_PLAYOUTS = 1200;
  constexpr int CALIB_LENGTH = 8;

  Board20 flatB = Board20::empty();
  uint16_t flatRows[BOARD_ROWS] = {};
  for (int r = 14; r < BOARD_ROWS; ++r) {
    flatRows[r] = 0x3FEU;
  }
  flatB = Board20::fromRows(flatRows);

  uint16_t overRows[BOARD_ROWS] = {};
  for (int r = 8; r < BOARD_ROWS; ++r) {
    overRows[r] = 0x3FEU;
  }
  overRows[9] &= static_cast<uint16_t>(~colBitMask(2));
  overRows[10] &= static_cast<uint16_t>(~colBitMask(5));
  overRows[11] &= static_cast<uint16_t>(~colBitMask(7));
  const Board20 overB = Board20::fromRows(overRows);

  auto measureBoard = [&](const Board20 &b) {
    const GameState gs = board20ToGameState(b, 18, 20, bc, SAFE);
    const EvalContext ctx = getEvalContext(gs, bc.lookup, false, SAFE);
    const Piece *curPiece = &PIECE_LIST[4]; // T piece, matching live_game_server.cpp
    const char *calibTimeline = "X..";

    auto t0 = std::chrono::steady_clock::now();
    (void)preparePrecomputeState(gs, curPiece, calibTimeline, DAS_CHARGE_MAX, 18, -1, 0,
                                 CALIB_LENGTH, &ctx, bc.lookup, nullptr);
    const double baseMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    auto t1 = std::chrono::steady_clock::now();
    (void)preparePrecomputeState(gs, curPiece, calibTimeline, DAS_CHARGE_MAX, 18, -1,
                                 CALIB_PLAYOUTS, CALIB_LENGTH, &ctx, bc.lookup, nullptr);
    const double workMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();

    const double playoutMs = std::max(1.0, workMs - baseMs);
    return (static_cast<double>(CALIB_PLAYOUTS) * static_cast<double>(CALIB_LENGTH)) / playoutMs;
  };

  const double flatRate = std::max(measureBoard(flatB), measureBoard(flatB));
  const double overRate = std::max(measureBoard(overB), measureBoard(overB));
  const double blended = (2.0 * flatRate * overRate) / std::max(1e-6, flatRate + overRate);
  const double totalMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  fprintf(stderr,
          "Calibrated 7T precompute speed=%.1f moves/ms (flat=%.1f, overhang=%.1f) in %.1fms\n",
          blended, flatRate, overRate, totalMs);
  return blended;
}

struct PrecomputeMoveResult {
  bool hasMove = false;
  int rot = 0;
  int x = 0;
  int y = 0;
  int dasChargeAfter = DAS_CHARGE_MAX;
  int linesCleared = 0;
  GameState afterState = {};
};

// Runs StackRabbit's production precompute pipeline (fullMoveSearch + reactionTime mid-air
// adjustment search + dasPenaltyLookupMap + playouts/tournament) and executes the chosen move for
// the actual revealed `nextPieceIdx`. Matches collapseAndFormatPrecompute's "none" semantics.
inline PrecomputeMoveResult chooseProductionPrecomputeMove(const GameState &state, int curPieceIdx,
                                                           int nextPieceIdx,
                                                           const PrecomputeOptions &opts,
                                                           const BackendContexts &bc) {
  PrecomputeMoveResult res = {};
  const EvalContext ctx = getEvalContext(state, bc.lookup, false, opts.aggro);
  const Piece *curPiece = &PIECE_LIST[curPieceIdx];

  SearchBudget timedBudget{};
  const SearchBudget *budgetPtr = nullptr;
  if (opts.pruningBreadth == -2) {
    timedBudget = SearchBudget::fromDeadline(opts.playoutCount,
                                             std::chrono::steady_clock::now()
                                                 + std::chrono::milliseconds(opts.playoutCount),
                                             opts.calibratedMovesPerMs7T);
    budgetPtr = &timedBudget;
  }

  PrecomputeState pState = preparePrecomputeState(
      state, curPiece, TIMELINE_10HZ_STR, opts.dasCharge, opts.reactionTime, opts.pruningBreadth,
      opts.playoutCount, opts.playoutLength, &ctx, bc.lookup, budgetPtr);
  if (pState.phantoms.empty()) {
    return res;
  }

  std::vector<PrecomputePhantomBreakdown> ranked = analyzePrecomputeCollapse(pState, curPiece);
  if (ranked.empty() || !ranked[0].isWinner) {
    return res;
  }

  const PrecomputePhantomBreakdown &winner = ranked[0];
  const PrecomputePieceBreakdown &pb = winner.byPiece[nextPieceIdx];

  int chosenRot = 0;
  int chosenX = 0;
  int chosenY = 0;
  int chosenDas = DAS_CHARGE_MAX;
  if (pb.valid) {
    chosenRot = pb.rot;
    chosenX = pb.x;
    chosenY = pb.y;
    chosenDas = pb.dasChargeAfter;
  } else if (winner.hasInitialPlacement) {
    chosenRot = winner.rot;
    chosenX = winner.x;
    chosenY = winner.y;
    chosenDas = winner.dasChargeAfter;
  } else {
    return res;
  }

  LockPlacement lp = {chosenX, chosenY, chosenRot, -1, NO_TUCK_NOTATION, curPiece};
  GameState after = advanceGameState(state, lp, &ctx);
  const int cleared = after.lines - state.lines;
  const int nextLines = opts.stationaryLevel ? state.lines : after.lines;
  const int nextLevel = opts.stationaryLevel ? state.level : after.level;

  res.hasMove = true;
  res.rot = chosenRot;
  res.x = chosenX;
  res.y = chosenY;
  res.dasChargeAfter = chosenDas;
  res.linesCleared = cleared;
  res.afterState =
      board20ToGameState(gameStateToBoard20(after), nextLevel, nextLines, bc, opts.aggro);
  return res;
}

} // namespace das

#endif // DAS_TABLEBASE_EVAL_ADAPTER_HPP
