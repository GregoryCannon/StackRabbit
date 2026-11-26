#ifndef DAS_TABLEBASE_DAS_RT_MOVE_GEN_REF_HPP
#define DAS_TABLEBASE_DAS_RT_MOVE_GEN_REF_HPP

// Brute-force frame-by-frame reference implementation (sharing only ShiftSchedule /
// buildShiftSchedule with das_rt_move_gen.hpp) plus a completely independent frame-by-frame
// NES Tetris physics validator (validateNesInputSequence).
//
// Deliberately avoids ALL optimizations in DasRtMoveTables (no swept-path masks, no O(1) drop,
// no retroactive DAS formulas, no empty-board frame-RT tables, no 0-hole Phase C skip) so that
// 0-mismatch parity across diverse boards proves every optimization in DasRtMoveTables is lossless.

#include "das_rt_move_gen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace das {
namespace ref {

inline constexpr char INPUT_CHAR_TABLE[3][3] = {
    {'F', 'L', 'E'}, // shift -1: rot -1, 0, +1
    {'B', '.', 'A'}, // shift  0: rot -1, 0, +1
    {'G', 'R', 'I'}, // shift +1: rot -1, 0, +1
};

inline int charShiftDir(char c) noexcept {
  switch (c) {
  case 'L':
  case 'E':
  case 'F':
  case 'l':
  case 'e':
  case 'f':
    return -1;
  case 'R':
  case 'I':
  case 'G':
  case 'r':
  case 'i':
  case 'g':
    return 1;
  default:
    return 0;
  }
}

inline int charRotDelta(char c) noexcept {
  switch (c) {
  case 'A':
  case 'E':
  case 'I':
  case 'a':
  case 'e':
  case 'i':
    return 1;
  case 'B':
  case 'F':
  case 'G':
  case 'b':
  case 'f':
  case 'g':
    return -1;
  default:
    return 0;
  }
}

inline bool charIsLower(char c) noexcept { return c >= 'a' && c <= 'z'; }

inline char makeFrameChar(int shiftDir, int rotDelta, bool shifted) noexcept {
  char c = INPUT_CHAR_TABLE[shiftDir + 1][rotDelta + 1];
  if (shiftDir != 0 && !shifted && c != '.') {
    c = static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

struct NesValidationResult {
  bool ok = false;
  std::string error;
  int finalX = 0;
  int finalY = 0;
  int finalRot = 0;
  int finalDas = 0;
  int framesSimulated = 0;
};

// Completely independent frame-by-frame NES Tetris physics validator.
// Verifies collision-free flight, exact lock cell, exact ending DAS charge, and strict
// independent-thumb cooldowns (>= dpadGapFrames between D-pad down-presses; >= rotGapFrames
// between A/B down-presses; ARE hold '_' starts with dpadCooldown = 0).
inline NesValidationResult validateNesInputSequence(const Board20 &board, int pieceIdx,
                                                    const DasStartState &start,
                                                    const std::string &seq,
                                                    const MoveSearchConfig &cfg) {
  NesValidationResult res;
  const PieceDef20 &piece = PIECES_20[pieceIdx];
  const int numOrients = piece.numOrientations;
  const int rotMask = numOrients - 1;

  if (collides20(board, pieceIdx, start.x, start.y, start.rot)) {
    res.error = "immediate entry collision";
    return res;
  }

  size_t idx = 0;
  int x = start.x;
  int y = start.y;
  int rot = start.rot;
  int f = start.frame;
  int das = start.dasCharge;
  int heldDir = start.heldDir;
  int dpadCd = start.dpadCooldown;
  int rotCd = start.rotCooldown;
  bool holdsThroughAre = false;

  if (idx < seq.size() && seq[idx] == '_') {
    if (start.isAdjustment) {
      res.error = "'_' ARE marker illegal on adjustment";
      return res;
    }
    if (start.dasCharge <= DAS_RESET_THRESHOLD) {
      res.error = "'_' ARE marker illegal when dasCharge <= 5";
      return res;
    }
    holdsThroughAre = true;
    idx++;
  }

  bool locked = false;
  for (; idx < seq.size(); ++idx) {
    const char c = seq[idx];
    const int dir = charShiftDir(c);
    const int rDelta = charRotDelta(c);
    const bool isLower = charIsLower(c);
    const bool isGrav = detail::isGravityFrame(f, cfg.gravity);

    if (dir != 0) {
      const bool continuedHold =
          (dir == heldDir)
          || (holdsThroughAre && f == start.frame && start.dasCharge > DAS_RESET_THRESHOLD);
      if (!continuedHold) {
        if (dpadCd > 0) {
          res.error = "D-pad cooldown violation at frame " + std::to_string(f);
          return res;
        }
        dpadCd = cfg.dpadGapFrames;
        if (collides20(board, pieceIdx, x + dir, y, rot)) {
          if (!isLower) {
            res.error = "uppercase shift collided at frame " + std::to_string(f);
            return res;
          }
          das = DAS_MAX_CHARGE;
        } else {
          if (isLower) {
            res.error = "lowercase wall-charge did not collide at frame " + std::to_string(f);
            return res;
          }
          x += dir;
          das = 0;
        }
      } else {
        if (dpadCd > 0) {
          dpadCd--;
        }
        if (das >= DAS_FULL_THRESHOLD) {
          if (collides20(board, pieceIdx, x + dir, y, rot)) {
            if (!isLower) {
              res.error = "uppercase on blocked held shift at frame " + std::to_string(f);
              return res;
            }
            das = DAS_MAX_CHARGE;
          } else {
            if (isLower) {
              res.error = "lowercase on das>=15 shift frame " + std::to_string(f);
              return res;
            }
            x += dir;
            das = DAS_POST_SHIFT_CHARGE;
          }
        } else {
          if (!isLower) {
            res.error = "uppercase shift with das<15 while holding at frame " + std::to_string(f);
            return res;
          }
          das++;
        }
      }
      heldDir = dir;
    } else {
      heldDir = 0;
      if (dpadCd > 0) {
        dpadCd--;
      }
    }

    if (rDelta != 0) {
      if (rotCd > 0) {
        res.error = "rotate cooldown violation at frame " + std::to_string(f);
        return res;
      }
      if (numOrients == 1) {
        res.error = "rotation on O piece at frame " + std::to_string(f);
        return res;
      }
      const int nrot = (rot + rDelta + 4) & rotMask;
      if (collides20(board, pieceIdx, x, y, nrot)) {
        res.error = "rotation collision at frame " + std::to_string(f);
        return res;
      }
      rot = nrot;
      rotCd = cfg.rotGapFrames;
    } else if (rotCd > 0) {
      rotCd--;
    }

    if (isGrav) {
      if (collides20(board, pieceIdx, x, y + 1, rot)) {
        locked = true;
        f++;
        idx++;
        break;
      }
      y++;
    }
    f++;
  }

  if (!locked) {
    res.error = "sequence ended without locking";
    return res;
  }
  if (idx != seq.size()) {
    res.error = "trailing characters after lock";
    return res;
  }

  res.ok = true;
  res.finalX = x;
  res.finalY = y;
  res.finalRot = rot;
  res.finalDas = das;
  res.framesSimulated = f - start.frame;
  return res;
}

struct RefMove {
  int x = 0;
  int y = 0;
  int rot = 0;
  int dasAfter = 0;
  char tuckInput = '.';
  std::string seq;
};

struct RefTuckHover {
  int x = 0;
  int y = 0;
  int rot = 0;
  int f = 0;
  int das = 0;
  int dpadReadyFrame = 0;
  int tuckDpadReadyFrame = 0;
  int rotReadyFrame = 0;
  int heldDirPrev = 0;
  int firstFallFrame = 0;
  int prevPrevHeldDir = 0;
  bool prevWasDot = false;
  std::string seq;
};

// Complete brute-force frame-by-frame reference search for one DasStartState.
inline void referenceSearchMoves(const Board20 &board, int pieceIdx, const DasStartState &start,
                                 const MoveSearchConfig &cfg, bool trackSeq,
                                 std::vector<RefMove> &outMoves,
                                 std::vector<DasStartState> *outAdjStates = nullptr) {
  outMoves.clear();
  if (outAdjStates != nullptr) {
    outAdjStates->clear();
  }
  if (collides20(board, pieceIdx, start.x, start.y, start.rot)) {
    return;
  }

  const PieceDef20 &piece = PIECES_20[pieceIdx];
  const int numOrients = piece.numOrientations;
  const int rotMask = numOrients - 1;
  const int tracks = cfg.enableQuicktaps ? 3 : (cfg.enableSlowTaps ? 2 : 1);
  const int rt = (!start.isAdjustment && cfg.reactionTime > 0 && outAdjStates != nullptr)
                     ? cfg.reactionTime
                     : -1;

  int minGoalX = 99;
  int maxGoalX = -99;
  for (int rot = 0; rot < numOrients; ++rot) {
    minGoalX = std::min<int>(minGoalX, piece.minX[rot]);
    maxGoalX = std::max<int>(maxGoalX, rt_detail::pieceMaxX(piece, rot));
  }

  std::unordered_map<int, RefMove> bestByCell;
  int basicLockY[4][detail::PIECE_MASK_X_COUNT];
  for (int r = 0; r < 4; ++r) {
    for (int x = 0; x < detail::PIECE_MASK_X_COUNT; ++x) {
      basicLockY[r][x] = INVALID_ROW_Y;
    }
  }
  auto cellKey = [](int rot, int x, int y) { return ((rot * 16) + (x + 4)) * 32 + (y + 4); };
  auto recordMove = [&](const RefMove &m, bool isBasic) {
    if (isBasic) {
      const int xi = m.x + detail::PIECE_MASK_X_OFFSET;
      if (m.y > basicLockY[m.rot][xi]) {
        basicLockY[m.rot][xi] = m.y;
      }
    }
    const int k = cellKey(m.rot, m.x, m.y);
    auto it = bestByCell.find(k);
    if (it == bestByCell.end()
        || m.dasAfter > it->second.dasAfter
        || (m.dasAfter == it->second.dasAfter
            && it->second.tuckInput != '.'
            && m.tuckInput == '.')) {
      bestByCell[k] = m;
    }
  };

  std::vector<RefTuckHover> hoverStates;
  std::unordered_map<uint64_t, bool> seenAdj;

  auto simulateFallExact = [&](int x, int y, int rot, int fStart, int dasStart, int dpadReadyF,
                               int tuckDpadReadyF, int rotReadyF, bool holdActive, int holdDir,
                               int heldDirAtArrive, bool lockedOnArrive,
                               const std::string &prefixSeq) {
    auto runVar = [&](bool keepHold) {
      if (lockedOnArrive) {
        recordMove({x, y, rot, dasStart, '.', prefixSeq}, true);
        return;
      }
      int cy = y;
      int cf = fStart;
      int cdas = dasStart;
      int cDpadReadyF = dpadReadyF;
      int cHeldDir = heldDirAtArrive;
      std::string seq = trackSeq ? prefixSeq : std::string();

      int hvHeldDir = heldDirAtArrive;
      int hvPrevPrevHeldDir = 0;
      bool hvPrevWasDot = false;
      int hvFallStartF = fStart;
      int hvDas = dasStart;
      std::string hvSeq = trackSeq ? prefixSeq : std::string();

      while (cf < DAS_MAX_SIM_FRAMES) {
        const bool isGrav = detail::isGravityFrame(cf, cfg.gravity);
        if (keepHold && cdas < DAS_FULL_THRESHOLD) {
          cdas++;
          cHeldDir = holdDir;
          hvDas = cdas;
          hvPrevPrevHeldDir = hvHeldDir;
          hvHeldDir = holdDir;
          hvPrevWasDot = false;
          hvFallStartF = cf + 1;
          if (trackSeq) {
            const char hc = makeFrameChar(holdDir, 0, false);
            seq.push_back(hc);
            hvSeq.push_back(hc);
          }
        } else {
          if (cfg.enableTucks) {
            hoverStates.push_back({x, cy, rot, cf, hvDas, std::max(hvFallStartF, dpadReadyF),
                                   std::max(hvFallStartF, tuckDpadReadyF),
                                   std::max(hvFallStartF, rotReadyF), hvHeldDir, hvFallStartF,
                                   hvPrevPrevHeldDir, hvPrevWasDot,
                                   trackSeq ? hvSeq : std::string()});
            hvPrevPrevHeldDir = hvHeldDir;
            hvHeldDir = 0;
            hvPrevWasDot = true;
            if (trackSeq) {
              hvSeq.push_back('.');
            }
          }
          char wc = 0;
          bool wasHeldWc = false;
          if (cdas < DAS_MAX_CHARGE) {
            // Held wall-charge at das == 15 whenever cHeldDir is already held against a wall
            // (either on arrival at das == 15 or immediately after finishing a hold-to-15).
            if (cHeldDir != 0
                && cdas == DAS_FULL_THRESHOLD
                && collides20(board, pieceIdx, x + cHeldDir, cy, rot)) {
              wc = (cHeldDir < 0) ? 'l' : 'r';
              wasHeldWc = true;
            } else if (cf >= cDpadReadyF) {
              if (cHeldDir != -1 && collides20(board, pieceIdx, x - 1, cy, rot)) {
                wc = 'l';
              } else if (cHeldDir != 1 && collides20(board, pieceIdx, x + 1, cy, rot)) {
                wc = 'r';
              }
            }
          }
          if (wc != 0) {
            const int wcDir = (wc == 'l') ? -1 : 1;
            cdas = DAS_MAX_CHARGE;
            cHeldDir = wcDir;
            if (!wasHeldWc) {
              cDpadReadyF = cf + cfg.dpadGapFrames + 1;
            }
            if (trackSeq) {
              seq.push_back(wc);
            }
          } else {
            cHeldDir = 0;
            if (trackSeq) {
              seq.push_back('.');
            }
          }
        }

        if (isGrav) {
          if (collides20(board, pieceIdx, x, cy + 1, rot)) {
            recordMove({x, cy, rot, cdas, '.', seq}, true);
            return;
          }
          cy++;
        }
        cf++;
      }
    };

    runVar(false);
    if (holdActive && holdDir != 0 && dasStart < DAS_FULL_THRESHOLD) {
      runVar(true);
    }
  };

  for (int goalX = minGoalX; goalX <= maxGoalX; ++goalX) {
    const int dx = goalX - static_cast<int>(start.x);
    for (int t = 0; t < tracks; ++t) {
      rt_detail::ShiftSchedule sched;
      if (!rt_detail::buildShiftSchedule(start, dx, static_cast<rt_detail::TrackKind>(t), cfg,
                                         sched)) {
        continue;
      }

      if (cfg.enablePoleVaults) {
        uint8_t m[4] = {};
        m[start.rot] = static_cast<uint8_t>(1U << start.rotCooldown);

        int8_t prevState[DAS_MAX_SIM_FRAMES][32];
        int8_t prevRotAct[DAS_MAX_SIM_FRAMES][32];
        int8_t shiftDirLog[DAS_MAX_SIM_FRAMES];
        uint8_t shiftedLog[DAS_MAX_SIM_FRAMES];
        if (trackSeq) {
          std::memset(prevState, -1, sizeof(prevState));
          std::memset(prevRotAct, 0, sizeof(prevRotAct));
          std::memset(shiftDirLog, 0, sizeof(shiftDirLog));
          std::memset(shiftedLog, 0, sizeof(shiftedLog));
        }

        auto reconstructPrefix = [&](int endStep, int targetRot, int targetCd) -> std::string {
          if (!trackSeq) {
            return {};
          }
          std::string s;
          if (sched.holdsThroughAre) {
            s.push_back('_');
          }
          std::string rev;
          int st = (targetRot << 3) | targetCd;
          for (int step = endStep; step >= 0; --step) {
            const int rAct = prevRotAct[step][st];
            rev.push_back(makeFrameChar(shiftDirLog[step], rAct, shiftedLog[step] != 0U));
            st = prevState[step][st];
          }
          std::reverse(rev.begin(), rev.end());
          s += rev;
          return s;
        };

        uint8_t qtRtRotMask = ALL_ROTS_MASK;
        if (sched.track == rt_detail::TrackKind::Quicktap && rt > 0) {
          qtRtRotMask = 0U;
          for (int r = 0; r < numOrients; ++r) {
            if (rt_detail::inPieceXBounds(piece, r, goalX)) {
              qtRtRotMask |= static_cast<uint8_t>(1U << r);
            }
          }
        }

        int x = start.x;
        int y = start.y;
        int f = start.frame;
        int das = start.dasCharge;
        int dpadCd = start.dpadCooldown;
        int shiftCd = start.shiftCooldown;
        int heldDir = start.heldDir;
        int nextShiftIdx = 0;
        bool emittedAtGoal[4] = {};

        if (x == goalX) {
          emittedAtGoal[start.rot] = true;
          const bool keepHoldingStationary =
              (start.isAdjustment && start.heldDir != 0 && das < DAS_FULL_THRESHOLD);
          simulateFallExact(x, y, start.rot, f, das, f + dpadCd,
                            rt_detail::computeTuckDpadReadyFrame(f, dpadCd, shiftCd, cfg),
                            f + start.rotCooldown, keepHoldingStationary, start.heldDir,
                            start.heldDir, false, "");
        }

        int step = 0;
        while (f < DAS_MAX_SIM_FRAMES) {
          if ((m[0] | m[1] | m[2] | m[3]) == 0U) {
            break;
          }
          const bool isGrav = detail::isGravityFrame(f, cfg.gravity);
          const bool willShift =
              (nextShiftIdx < sched.numShifts && sched.shiftFrames[nextShiftIdx] == f);
          int holdDir = 0;
          const bool willHold =
              !willShift
              && rt_detail::shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, holdDir);

          if (trackSeq) {
            shiftDirLog[step] =
                willShift ? sched.dir : (willHold ? static_cast<int8_t>(holdDir) : 0);
            shiftedLog[step] = willShift ? 1U : 0U;
          }

          if (willShift) {
            x += sched.dir;
            for (int r = 0; r < numOrients; ++r) {
              if (m[r] != 0U && collides20(board, pieceIdx, x, y, r)) {
                m[r] = 0;
              }
            }
            if ((m[0] | m[1] | m[2] | m[3]) == 0U) {
              break;
            }
            if (sched.shiftResetsDas[nextShiftIdx] != 0U) {
              das = 0;
              dpadCd = cfg.dpadGapFrames;
            } else {
              das = DAS_POST_SHIFT_CHARGE;
              if (dpadCd > 0) {
                dpadCd--;
              }
            }
            shiftCd = cfg.dpadGapFrames;
            heldDir = sched.dir;
            nextShiftIdx++;
          } else if (willHold) {
            if (das < DAS_FULL_THRESHOLD) {
              das++;
            }
            heldDir = holdDir;
            if (dpadCd > 0) {
              dpadCd--;
            }
            if (shiftCd > 0) {
              shiftCd--;
            }
          } else {
            heldDir = 0;
            if (dpadCd > 0) {
              dpadCd--;
            }
            if (shiftCd > 0) {
              shiftCd--;
            }
          }

          uint8_t nextM[4] = {};
          for (int r = 0; r < numOrients; ++r) {
            if (m[r] == 0U) {
              continue;
            }
            for (int cd = 0; cd <= cfg.rotGapFrames; ++cd) {
              if ((m[r] & (1U << cd)) == 0U) {
                continue;
              }
              const int ncd = (cd > 0) ? (cd - 1) : 0;
              if ((nextM[r] & (1U << ncd)) == 0U) {
                nextM[r] |= static_cast<uint8_t>(1U << ncd);
                if (trackSeq) {
                  prevState[step][(r << 3) | ncd] = static_cast<int8_t>((r << 3) | cd);
                  prevRotAct[step][(r << 3) | ncd] = 0;
                }
              }
            }
          }
          for (int r = 0; r < numOrients; ++r) {
            if ((m[r] & 1U) == 0U || numOrients <= 1) {
              continue;
            }
            const int ncd = cfg.rotGapFrames;
            const int cw = (r + 1) & rotMask;
            if (!collides20(board, pieceIdx, x, y, cw) && (nextM[cw] & (1U << ncd)) == 0U) {
              nextM[cw] |= static_cast<uint8_t>(1U << ncd);
              if (trackSeq) {
                prevState[step][(cw << 3) | ncd] = static_cast<int8_t>((r << 3) | 0);
                prevRotAct[step][(cw << 3) | ncd] = 1;
              }
            }
            if (numOrients == 4) {
              const int ccw = (r + 3) & 3;
              if (!collides20(board, pieceIdx, x, y, ccw) && (nextM[ccw] & (1U << ncd)) == 0U) {
                nextM[ccw] |= static_cast<uint8_t>(1U << ncd);
                if (trackSeq) {
                  prevState[step][(ccw << 3) | ncd] = static_cast<int8_t>((r << 3) | 0);
                  prevRotAct[step][(ccw << 3) | ncd] = -1;
                }
              }
            }
          }
          for (int r = 0; r < numOrients; ++r) {
            m[r] = nextM[r];
          }

          uint8_t lockedHere = 0;
          if (isGrav) {
            for (int r = 0; r < numOrients; ++r) {
              if (m[r] != 0U && collides20(board, pieceIdx, x, y + 1, r)) {
                lockedHere |= static_cast<uint8_t>(1U << r);
              }
            }
          }

          if (x == goalX) {
            int nextHoldDir = 0;
            const bool holdActiveNow =
                rt_detail::shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, nextHoldDir);
            const int tuckDpadReady =
                rt_detail::computeTuckDpadReadyFrame(f + 1, dpadCd, shiftCd, cfg);
            for (int r = 0; r < numOrients; ++r) {
              if (m[r] == 0U) {
                continue;
              }
              const bool isLocked = ((lockedHere & (1U << r)) != 0U);
              const int bestCd = __builtin_ctz(static_cast<unsigned>(m[r]));
              if (!emittedAtGoal[r]) {
                emittedAtGoal[r] = true;
                simulateFallExact(x, isLocked ? y : (y + (isGrav ? 1 : 0)), r, f + 1, das,
                                  f + 1 + dpadCd, tuckDpadReady, f + 1 + bestCd,
                                  !isLocked && holdActiveNow, nextHoldDir, heldDir, isLocked,
                                  reconstructPrefix(step, r, bestCd));
              }
              if (isLocked) {
                m[r] = 0;
              }
            }
          } else if (isGrav) {
            for (int r = 0; r < numOrients; ++r) {
              if ((lockedHere & (1U << r)) != 0U) {
                m[r] = 0;
              }
            }
          }

          if (isGrav) {
            y++;
          }
          f++;
          step++;

          if (rt > 0 && f == rt) {
            const uint8_t allowedMask = (x == goalX) ? ALL_ROTS_MASK : qtRtRotMask;
            for (int r = 0; r < numOrients; ++r) {
              if (m[r] == 0U || ((allowedMask & (1U << r)) == 0U)) {
                continue;
              }
              const DasStartState st = DasStartState::makeAdjustment(
                  x, y, r, f, das, heldDir, dpadCd, __builtin_ctz(static_cast<unsigned>(m[r])),
                  shiftCd);
              const uint64_t k = st.packKey();
              if (seenAdj.emplace(k, true).second) {
                outAdjStates->push_back(st);
              }
            }
          }

          if (x == goalX && (rt <= 0 || f >= rt)) {
            bool allDone = true;
            for (int r = 0; r < numOrients; ++r) {
              if (!emittedAtGoal[r]) {
                allDone = false;
                break;
              }
            }
            if (allDone) {
              break;
            }
          }
        }
      } else {
        for (int goalRot = 0; goalRot < numOrients; ++goalRot) {
          if (!rt_detail::inPieceXBounds(piece, goalRot, goalX)) {
            continue;
          }
          int rotInc = 0;
          if (start.rot != goalRot) {
            rotInc = (numOrients == 2)
                         ? 1
                         : ((((static_cast<int>(start.rot) - 1) & 3) == goalRot) ? -1 : 1);
          }
          int x = start.x;
          int y = start.y;
          int rot = start.rot;
          int f = start.frame;
          int das = start.dasCharge;
          int dpadCd = start.dpadCooldown;
          int rotCd = start.rotCooldown;
          int shiftCd = start.shiftCooldown;
          int heldDir = start.heldDir;
          int nextShiftIdx = 0;
          bool arrived = (x == goalX && rot == goalRot);
          bool capturedRt = false;
          std::string seq;
          if (trackSeq && sched.holdsThroughAre) {
            seq.push_back('_');
          }

          if (arrived) {
            const bool keepHoldingStationary =
                (start.isAdjustment && start.heldDir != 0 && das < DAS_FULL_THRESHOLD);
            simulateFallExact(x, y, rot, f, das, f + dpadCd,
                              rt_detail::computeTuckDpadReadyFrame(f, dpadCd, shiftCd, cfg),
                              f + rotCd, keepHoldingStationary, start.heldDir, start.heldDir, false,
                              seq);
            if (rt <= 0) {
              continue;
            }
          }

          while (f < DAS_MAX_SIM_FRAMES) {
            const bool isGrav = detail::isGravityFrame(f, cfg.gravity);
            const bool willShift =
                (nextShiftIdx < sched.numShifts && sched.shiftFrames[nextShiftIdx] == f);
            int holdDir = 0;
            const bool willHold =
                !willShift
                && rt_detail::shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, holdDir);

            if (willShift) {
              if (collides20(board, pieceIdx, x + sched.dir, y, rot)) {
                break;
              }
              x += sched.dir;
              if (sched.shiftResetsDas[nextShiftIdx] != 0U) {
                das = 0;
                dpadCd = cfg.dpadGapFrames;
              } else {
                das = DAS_POST_SHIFT_CHARGE;
                if (dpadCd > 0) {
                  dpadCd--;
                }
              }
              shiftCd = cfg.dpadGapFrames;
              heldDir = sched.dir;
              nextShiftIdx++;
            } else if (willHold) {
              if (das < DAS_FULL_THRESHOLD) {
                das++;
              }
              heldDir = holdDir;
              if (dpadCd > 0) {
                dpadCd--;
              }
              if (shiftCd > 0) {
                shiftCd--;
              }
            } else {
              heldDir = 0;
              if (dpadCd > 0) {
                dpadCd--;
              }
              if (shiftCd > 0) {
                shiftCd--;
              }
            }

            int didRot = 0;
            if (rot != goalRot && rotCd == 0) {
              const int nrot = (rot + rotInc + 4) & rotMask;
              if (!rt_detail::inPieceXBounds(piece, nrot, x)) {
                if (x == goalX) {
                  break;
                }
              } else {
                if (collides20(board, pieceIdx, x, y, nrot)) {
                  break;
                }
                rot = nrot;
                rotCd = cfg.rotGapFrames;
                didRot = rotInc;
              }
            } else if (rotCd > 0) {
              rotCd--;
            }

            if (trackSeq) {
              const int seqDir = willShift ? sched.dir : (willHold ? holdDir : 0);
              seq.push_back(makeFrameChar(seqDir, didRot, willShift));
            }

            const bool locksHere = isGrav && collides20(board, pieceIdx, x, y + 1, rot);
            const bool justArrived = (!arrived && x == goalX && rot == goalRot);
            if (justArrived) {
              arrived = true;
              int nextHoldDir = 0;
              const bool holdActive =
                  rt_detail::shouldHoldDpad(sched, start, nextShiftIdx, heldDir, das, nextHoldDir);
              const int tuckDpadReady =
                  rt_detail::computeTuckDpadReadyFrame(f + 1, dpadCd, shiftCd, cfg);
              simulateFallExact(x, locksHere ? y : (y + (isGrav ? 1 : 0)), rot, f + 1, das,
                                f + 1 + dpadCd, tuckDpadReady, f + 1 + rotCd, holdActive,
                                nextHoldDir, heldDir, locksHere, seq);
            }

            if (locksHere) {
              break;
            }
            if (isGrav) {
              y++;
            }
            f++;

            if (rt > 0 && !capturedRt && f == rt) {
              capturedRt = true;
              const DasStartState st =
                  DasStartState::makeAdjustment(x, y, rot, f, das, heldDir, dpadCd, rotCd, shiftCd);
              const uint64_t k = st.packKey();
              if (seenAdj.emplace(k, true).second) {
                outAdjStates->push_back(st);
              }
            }

            if (arrived && (rt <= 0 || capturedRt)) {
              break;
            }
          }
        }
      }
    }
  }

  // Phase C: Brute-force frame-by-frame tuck/spin DFS across ALL hover states (even on 0-hole
  // boards) so parity tests also verify the 0-hole skip is 100% lossless.
  if (cfg.enableTucks) {
    static constexpr int INPUT_COUNT_BY_ORIENT[5] = {0, 2, 5, 0, 8};
    static constexpr int INPUT_IDX_2[5] = {0, 1, 2, 4, 5};
    const int nInputs = INPUT_COUNT_BY_ORIENT[numOrients];

    for (const RefTuckHover &hv : hoverStates) {
      for (int tiIdx = 0; tiIdx < nInputs; ++tiIdx) {
        const TuckInput20 &ti = TUCK_INPUTS_20[(numOrients == 2) ? INPUT_IDX_2[tiIdx] : tiIdx];
        if (ti.xChange != 0 && (hv.f < hv.tuckDpadReadyFrame || hv.heldDirPrev == ti.xChange)) {
          continue;
        }
        if (ti.rotChange != 0 && hv.f < hv.rotReadyFrame) {
          continue;
        }
        const int newX = hv.x + ti.xChange;
        const int newRot = (numOrients == 1) ? hv.rot : ((hv.rot + ti.rotChange + 4) & rotMask);
        if (!rt_detail::inPieceXBounds(piece, newRot, newX)) {
          continue;
        }
        const int baseY = basicLockY[newRot][newX + detail::PIECE_MASK_X_OFFSET];
        if (hv.y < baseY) {
          continue;
        }
        if (hv.y == baseY) {
          const bool overhangAbove =
              (hv.y > MIN_PIECE_Y)
              && ((ti.xChange != 0) ? collides20(board, pieceIdx, newX, hv.y - 1, hv.rot)
                                    : collides20(board, pieceIdx, newX, hv.y - 1, newRot));
          if (!overhangAbove) {
            continue;
          }
        }
        if (ti.xChange != 0 && collides20(board, pieceIdx, newX, hv.y, hv.rot)) {
          continue;
        }
        if (ti.rotChange != 0 && collides20(board, pieceIdx, newX, hv.y, newRot)) {
          continue;
        }

        int cx = newX;
        int cy = hv.y;
        int crot = newRot;
        int cf = hv.f;
        int cdas = hv.das;
        int cDpadReadyF = hv.dpadReadyFrame;
        int cHeldDir = (ti.xChange != 0) ? ti.xChange : 0;
        bool isWallChargeTuck = false;
        char wcChar = 0;
        std::string seq = trackSeq ? hv.seq : std::string();

        if (ti.xChange != 0) {
          const bool prevWasGrav =
              (cf > start.frame) && detail::isGravityFrame(cf - 1, cfg.gravity);
          const bool prevDpadReady = hv.prevWasDot
                                     && (cf > hv.firstFallFrame)
                                     && (hv.prevPrevHeldDir != ti.xChange)
                                     && (cf - 1 >= hv.dpadReadyFrame);
          const bool blockedAbove =
              (cy > MIN_PIECE_Y) && collides20(board, pieceIdx, newX, cy - 1, hv.rot);
          if (prevWasGrav && prevDpadReady && blockedAbove) {
            isWallChargeTuck = true;
            wcChar = (ti.xChange > 0) ? 'r' : 'l';
            cdas = DAS_POST_SHIFT_CHARGE;
            cDpadReadyF = (cf - 1) + cfg.dpadGapFrames + 1;
            if (trackSeq && !seq.empty()) {
              seq.back() = wcChar;
            }
          } else {
            if (hv.y == baseY) {
              continue;
            }
            cdas = 0;
            cDpadReadyF = cf + cfg.dpadGapFrames + 1;
          }
        }

        if (trackSeq) {
          seq.push_back(ti.notation);
        }

        if (detail::isGravityFrame(cf, cfg.gravity)) {
          if (collides20(board, pieceIdx, cx, cy + 1, crot)) {
            recordMove({cx, cy, crot, cdas, ti.notation, seq}, false);
            continue;
          }
          cy++;
        }
        cf++;

        auto runPostTuckFall = [&](bool holdWallCharge) {
          int py = cy;
          int pf = cf;
          int pdas = cdas;
          int pDpadReadyF = cDpadReadyF;
          int pHeldDir = cHeldDir;
          std::string pseq = trackSeq ? seq : std::string();

          while (pf < DAS_MAX_SIM_FRAMES) {
            if (holdWallCharge && pdas < DAS_FULL_THRESHOLD) {
              pdas++;
              pHeldDir = ti.xChange;
              if (trackSeq) {
                pseq.push_back(wcChar);
              }
            } else {
              char wc = 0;
              bool wasHeldWc = false;
              if (pdas < DAS_MAX_CHARGE) {
                if (holdWallCharge
                    && pHeldDir != 0
                    && pdas == DAS_FULL_THRESHOLD
                    && collides20(board, pieceIdx, cx + pHeldDir, py, crot)) {
                  wc = (pHeldDir < 0) ? 'l' : 'r';
                  wasHeldWc = true;
                } else if (pf >= pDpadReadyF) {
                  if (pHeldDir != -1 && collides20(board, pieceIdx, cx - 1, py, crot)) {
                    wc = 'l';
                  } else if (pHeldDir != 1 && collides20(board, pieceIdx, cx + 1, py, crot)) {
                    wc = 'r';
                  }
                }
              }
              if (wc != 0) {
                const int wcDir = (wc == 'l') ? -1 : 1;
                pdas = DAS_MAX_CHARGE;
                pHeldDir = wcDir;
                if (!wasHeldWc) {
                  pDpadReadyF = pf + cfg.dpadGapFrames + 1;
                }
                if (trackSeq) {
                  pseq.push_back(wc);
                }
              } else {
                pHeldDir = 0;
                if (trackSeq) {
                  pseq.push_back('.');
                }
              }
            }

            if (detail::isGravityFrame(pf, cfg.gravity)) {
              if (collides20(board, pieceIdx, cx, py + 1, crot)) {
                recordMove({cx, py, crot, pdas, ti.notation, pseq}, false);
                break;
              }
              py++;
            }
            pf++;
          }
        };

        if (isWallChargeTuck) {
          runPostTuckFall(true);
          runPostTuckFall(false);
        } else {
          runPostTuckFall(false);
        }
      }
    }
  }

  outMoves.reserve(bestByCell.size());
  for (auto &kv : bestByCell) {
    outMoves.push_back(std::move(kv.second));
  }
}

} // namespace ref
} // namespace das

#endif // DAS_TABLEBASE_DAS_RT_MOVE_GEN_REF_HPP
