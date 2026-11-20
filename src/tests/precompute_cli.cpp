// Batch CLI runner for the C++ PRECOMPUTE pipeline.
//
// Reads PRECOMPUTE pipe strings from stdin, one per line (skipping empty lines
// and stripping trailing '\r'), executes mainProcess(line.c_str(), PRECOMPUTE)
// for each request, and writes one '\n'-terminated output line per request to
// stdout with every '\n' in the response escaped to '\x01'. Pipe strings and
// precompute outputs never contain '\x01', so the framing is unambiguous.
//
// Usage: ./build/precompute_cli < inputs.txt
#include <algorithm>
#include <exception>
#include <iostream>
#include <string>

#include "../backend/main.cpp"

int main() {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }

    std::string output;
    try {
      output = mainProcess(line.c_str(), PRECOMPUTE);
    } catch (const std::exception &e) {
      output = std::string("Error: ") + e.what();
    } catch (...) {
      output = "Error: unknown engine exception";
    }

    std::replace(output.begin(), output.end(), '\n', '\x01');
    std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
    std::cout.put('\n');
    if (!std::cout) {
      return 1;
    }
  }

  std::cout.flush();
  if (std::cin.bad() || !std::cout) {
    return 1;
  }
  return 0;
}
