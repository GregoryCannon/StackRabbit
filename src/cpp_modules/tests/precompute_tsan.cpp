// Replays production-param precompute requests under ThreadSanitizer.
//
// Input: the dump written by `node src/server/precompute_verify.js
// --dump-inputs [path]` (default build/precompute_tsan_inputs.txt) -- a count
// line, then two lines per request: the PRECOMPUTE pipe string, then the
// golden output with every newline escaped to a \x01 sentinel. Pipe strings
// never contain newlines or \x01, so this is unambiguous.
//
// Each request goes to mainProcess(pipeStr, PRECOMPUTE), whose seven
// per-next-piece lock lookups run on concurrent threads; that concurrency is
// what ThreadSanitizer checks. The race detection itself is ThreadSanitizer's
// job -- a detected race makes the process exit 66 by default and its report
// is printed unsuppressed. This main additionally byte-compares every result
// against the golden output so result-changing divergence fails loudly too.
//
// Usage: ./build/precompute_tsan [dump-file]
#include <cstdio>
#include <fstream>
#include <string>

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
  for (size_t i = 0; i < requestCount; i++) {
    if (!std::getline(dumpFile, pipeLine) || !std::getline(dumpFile, goldenLine)) {
      fprintf(stderr, "precompute_tsan: dump file ended at request %zu\n", i);
      return 2;
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

  printf("%zu/%zu byte-identical\n", matches, requestCount);
  return matches == requestCount ? 0 : 1;
}
