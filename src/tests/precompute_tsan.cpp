// Replays representative production-param precompute requests under ThreadSanitizer.
//
// Input: the dump written by `node src/tests/precompute_verify.js
// --dump-inputs [path]` (default build/precompute_tsan_inputs.txt) -- a count
// line, then two lines per request: the PRECOMPUTE pipe string, then the
// golden output with every newline escaped to a \x01 sentinel. Pipe strings
// never contain newlines or \x01, so this is unambiguous.
//
// Each dumped request goes to mainProcess(pipeStr, PRECOMPUTE), whose seven
// per-next-piece lock lookups run on concurrent threads; that concurrency is
// what ThreadSanitizer checks. In addition to the deterministic golden replays
// (covering tap + DAS across reactionTime = 0, 8, 20), this harness derives and
// runs two timed tournament (`pruningBreadth == -2`, 150ms budget) DAS
// precompute requests: one standard 2-ply request and one no-next-box
// (`reactionTime = 60`, level >= 19) request whose 7-thread DAS penalty path is
// explicitly asserted via `PrecomputeState::usedNoNextBoxPath`.
//
// A detected race makes the process exit 66 by default and prints an
// unsuppressed TSan report. Every deterministic request is also byte-compared
// against its golden output, and the timed request must return a normal
// `Default:` precompute response (not `"none"` or `"Error:"`).
//
// Usage: ./build/precompute_tsan [dump-file]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../backend/main.cpp"

namespace {

std::string unescapeGoldenLine(const std::string &line) {
  std::string out;
  out.reserve(line.size());
  for (char c : line) {
    out.push_back(c == '\x01' ? '\n' : c);
  }
  return out;
}

std::vector<std::string> splitPipeFields(const std::string &pipe) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t end = pipe.find('|', start);
    if (end == std::string::npos) {
      break;
    }
    fields.push_back(pipe.substr(start, end - start));
    start = end + 1;
  }
  return fields;
}

std::string buildTimedDasPipe(const std::string &dasPipe, int budgetMs, int levelOverride = -1,
                              int reactionTimeOverride = -1) {
  std::vector<std::string> fields = splitPipeFields(dasPipe);
  if (fields.size() != 12) {
    return "";
  }
  if (levelOverride >= 18) {
    fields[1] = std::to_string(levelOverride);
  }
  fields[6] = std::to_string(budgetMs); // playoutCount = wall-clock budget in ms
  fields[7] = "2";                      // playoutLength
  fields[8] = "-2";                     // pruningBreadth = timed tournament mode
  if (reactionTimeOverride >= 0) {
    fields[11] = std::to_string(reactionTimeOverride);
  }
  std::string out;
  for (const std::string &field : fields) {
    out += field;
    out += '|';
  }
  return out;
}

bool runTimedPrecomputeCheck(const std::string &pipe, const char *label, bool requireNnb) {
  if (pipe.empty()) {
    fprintf(stderr, "precompute_tsan: failed to derive %s pipe string\n", label);
    return false;
  }
  try {
    if (requireNnb) {
      // Run through preparePrecomputeState + collapseAndFormatPrecompute so TSan exercises the
      // exact 7-thread DAS penalty + 1-ply timed tournament path AND we assert usedNoNextBoxPath.
      const std::vector<std::string> fields = splitPipeFields(pipe);
      if (fields.size() != 12) {
        fprintf(stderr, "precompute_tsan: %s pipe had %zu fields\n", label, fields.size());
        return false;
      }
      GameState gs = {};
      encodeBoard(fields[0].c_str(), gs.board);
      gs.level = atoi(fields[1].c_str());
      gs.lines = atoi(fields[2].c_str());
      getSurfaceArray(gs.board, gs.surfaceArray);
      auto h1 = updateSurfaceAndHoles(gs.surfaceArray, gs.board, 9, false);
      gs.numTrueHoles = h1.first;
      gs.numPartialHoles = h1.second;
      const int dasCharge = atoi(fields[10].c_str());
      const std::string eqTimeline = getDasEquivalentInputTimeline(fields[5].c_str());
      const PieceRangeContext contexts[4] = {
          getPieceRangeContext(eqTimeline.c_str(), 1, /* gravityDoubled= */ true),
          getPieceRangeContext(eqTimeline.c_str(), 1, /* gravityDoubled= */ false),
          getPieceRangeContext(eqTimeline.c_str(), 2, /* gravityDoubled= */ false),
          getPieceRangeContext(eqTimeline.c_str(), 3, /* gravityDoubled= */ false),
      };
      const EvalContext evalCtx = getEvalContext(gs, contexts, /* useTestWeights= */ false, SAFE);
      auto h2 = updateSurfaceAndHoles(gs.surfaceArray, gs.board,
                                      evalCtx.countWellHoles ? -1 : evalCtx.wellColumn,
                                      evalCtx.aiMode == DIG);
      gs.numTrueHoles = h2.first;
      gs.numPartialHoles = h2.second;
      const Piece *curPiece = &PIECE_LIST[atoi(fields[3].c_str())];
      const int budgetMs = atoi(fields[6].c_str());
      const SearchBudget budget = SearchBudget::fromDeadline(
          budgetMs, std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs));
      const PrecomputeState state = preparePrecomputeState(
          gs, curPiece, fields[5].c_str(), dasCharge, atoi(fields[11].c_str()),
          atoi(fields[8].c_str()), budgetMs, atoi(fields[7].c_str()), &evalCtx, contexts, &budget);
      if (!state.usedNoNextBoxPath) {
        fprintf(stderr, "precompute_tsan: %s did not take the no-next-box path\n", label);
        return false;
      }
      const std::string out = collapseAndFormatPrecompute(state, gs.board, curPiece, gs.level,
                                                          gs.lines, SHOULD_PUSHDOWN);
      if (out.rfind("Default:", 0) != 0) {
        fprintf(stderr, "precompute_tsan: %s did not return a 'Default:' response: %s\n", label,
                out.c_str());
        return false;
      }
      return true;
    }

    const std::string out = mainProcess(pipe.c_str(), PRECOMPUTE);
    if (out.rfind("Default:", 0) != 0) {
      fprintf(stderr, "precompute_tsan: %s did not return a 'Default:' response: %s\n", label,
              out.c_str());
      return false;
    }
    return true;
  } catch (std::exception const &err) {
    fprintf(stderr, "precompute_tsan: %s threw: %s\n", label, err.what());
    return false;
  }
}

} // namespace

int main(int argc, char **argv) {
  const char *path = argc >= 2 ? argv[1] : "build/precompute_tsan_inputs.txt";
  std::ifstream dumpFile(path);
  if (!dumpFile) {
    fprintf(stderr, "precompute_tsan: cannot open dump file %s\n", path);
    return 2;
  }

  size_t requestCount = 0;
  dumpFile >> requestCount;
  dumpFile.ignore(); // newline after the count line

  size_t matches = 0;
  std::string pipeLine;
  std::string goldenLine;
  std::string firstDasPipe;
  for (size_t i = 0; i < requestCount; i++) {
    if (!std::getline(dumpFile, pipeLine) || !std::getline(dumpFile, goldenLine)) {
      fprintf(stderr, "precompute_tsan: dump file ended at request %zu\n", i);
      return 2;
    }
    if (firstDasPipe.empty() && goldenLine != "none") {
      const std::vector<std::string> fields = splitPipeFields(pipeLine);
      if (fields.size() == 12 && atoi(fields[10].c_str()) >= 0) {
        firstDasPipe = pipeLine;
      }
    }
    bool requestMatched = false;
    try {
      requestMatched = mainProcess(pipeLine.c_str(), PRECOMPUTE) == unescapeGoldenLine(goldenLine);
    } catch (std::exception const &err) {
      fprintf(stderr, "precompute_tsan: request %zu threw: %s\n", i, err.what());
    }
    if (requestMatched) {
      matches++;
    } else {
      fprintf(stderr, "precompute_tsan: request %zu output differs from golden\n", i);
    }
  }

  // Exercise both the standard 2-ply timed tournament and the no-next-box (reactionTime = 60)
  // 7-thread DAS penalty + 1-ply timed tournament code paths.
  // Under TSan slowdown, pre-tournament DAS setup takes ~75ms, so use a 150ms
  // budget to ensure the timed tournament rounds actually run.
  bool timedOk = false;
  bool nnbTimedOk = false;
  if (firstDasPipe.empty()) {
    fprintf(
        stderr,
        "precompute_tsan: no non-'none' DAS fixture found in dump to derive timed -2 request\n");
  } else {
    const std::string timedPipe = buildTimedDasPipe(firstDasPipe, /* budgetMs= */ 150);
    timedOk = runTimedPrecomputeCheck(timedPipe, "timed -2 request", /* requireNnb= */ false);

    // Force level 19 (gravity 2: max fall time ~40 frames << reactionTime 60) so every phantom
    // is guaranteed to lock before reactionTime regardless of fixture piece/board.
    const std::string nnbTimedPipe = buildTimedDasPipe(
        firstDasPipe, /* budgetMs= */ 150, /* levelOverride= */ 19, /* reactionTimeOverride= */ 60);
    nnbTimedOk =
        runTimedPrecomputeCheck(nnbTimedPipe, "NNB timed -2 request", /* requireNnb= */ true);
  }

  printf("%zu/%zu byte-identical; timed -2 request %s; NNB timed -2 request %s\n", matches,
         requestCount, timedOk ? "OK" : "FAILED", nnbTimedOk ? "OK" : "FAILED");
  return (matches == requestCount && timedOk && nnbTimedOk) ? 0 : 1;
}
