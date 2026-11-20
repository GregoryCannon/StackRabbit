// Replays representative production-param precompute requests under ThreadSanitizer.
//
// Input: the dump written by `node src/cpp_modules/tests/precompute_verify.js
// --dump-inputs [path]` (default build/precompute_tsan_inputs.txt) -- a count
// line, then two lines per request: the PRECOMPUTE pipe string, then the
// golden output with every newline escaped to a \x01 sentinel. Pipe strings
// never contain newlines or \x01, so this is unambiguous.
//
// Each dumped request goes to mainProcess(pipeStr, PRECOMPUTE), whose seven
// per-next-piece lock lookups run on concurrent threads; that concurrency is
// what ThreadSanitizer checks. In addition to the deterministic golden replays
// (covering tap + DAS across reactionTime = 0, 8, 20), this harness derives and runs one
// timed tournament (`pruningBreadth == -2`, 150ms budget) DAS precompute request
// so TSan also exercises the timed 7-thread tournament loop.
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

#include "../src/main.cpp"

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

std::string buildTimedDasPipe(const std::string &dasPipe, int budgetMs) {
  std::vector<std::string> fields = splitPipeFields(dasPipe);
  if (fields.size() != 12) {
    return "";
  }
  fields[6] = std::to_string(budgetMs); // playoutCount = wall-clock budget in ms
  fields[7] = "2";                      // playoutLength
  fields[8] = "-2";                     // pruningBreadth = timed tournament mode
  std::string out;
  for (const std::string &field : fields) {
    out += field;
    out += '|';
  }
  return out;
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

  // Exercise the timed tournament (`pruningBreadth == -2`) 7-thread code path.
  // Under TSan slowdown, pre-tournament DAS setup takes ~75ms, so use a 150ms
  // budget to ensure the timed tournament rounds actually run on all 7 threads.
  bool timedOk = false;
  if (firstDasPipe.empty()) {
    fprintf(stderr,
            "precompute_tsan: no non-'none' DAS fixture found in dump to derive timed -2 request\n");
  } else {
    const std::string timedPipe = buildTimedDasPipe(firstDasPipe, /* budgetMs= */ 150);
    if (timedPipe.empty()) {
      fprintf(stderr, "precompute_tsan: failed to derive timed -2 pipe string\n");
    } else {
      try {
        const std::string timedOutput = mainProcess(timedPipe.c_str(), PRECOMPUTE);
        if (timedOutput.rfind("Default:", 0) != 0) {
          fprintf(stderr,
                  "precompute_tsan: timed -2 request did not return a 'Default:' response: %s\n",
                  timedOutput.c_str());
        } else {
          timedOk = true;
        }
      } catch (std::exception const &err) {
        fprintf(stderr, "precompute_tsan: timed -2 request threw: %s\n", err.what());
      }
    }
  }

  printf("%zu/%zu byte-identical; timed -2 request %s\n", matches, requestCount,
         timedOk ? "OK" : "FAILED");
  return (matches == requestCount && timedOk) ? 0 : 1;
}
