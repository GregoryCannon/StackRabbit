#ifndef TABLEBASE_CLI_UTILS_HPP
#define TABLEBASE_CLI_UTILS_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace tb::cli {

inline constexpr double MAX_PASSING_Z_SCORE = 3.5;

// Strict unsigned base-10 integer parser with overflow/underflow safety across [0, UINT64_MAX].
inline bool parseStrictU64(std::string_view sub, uint64_t minVal, uint64_t maxVal, uint64_t &out) {
  if (sub.empty())
    return false;
  uint64_t v = 0;
  for (char c : sub) {
    if (c < '0' || c > '9')
      return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (digit > maxVal || v > (maxVal - digit) / 10ULL)
      return false;
    v = v * 10ULL + digit;
  }
  if (v < minVal || v > maxVal)
    return false;
  out = v;
  return true;
}

// Strict positive decimal/scientific float parser (e.g. "1e-7", "0.00001"); rejects whitespace,
// signs before mantissa, hex floats, NaN, Inf, or junk suffixes.
inline bool parseStrictPositiveDouble(const std::string &sub, double minVal, double maxVal,
                                      double &out) {
  if (sub.empty())
    return false;
  size_t i = 0;
  bool seenMantissaDigit = false;
  bool seenDot = false;
  while (i < sub.size()) {
    const char c = sub[i];
    if (c >= '0' && c <= '9') {
      seenMantissaDigit = true;
      ++i;
    } else if (c == '.' && !seenDot) {
      seenDot = true;
      ++i;
    } else {
      break;
    }
  }
  if (!seenMantissaDigit)
    return false;

  if (i < sub.size()) {
    if (sub[i] != 'e' && sub[i] != 'E')
      return false;
    ++i;
    if (i < sub.size() && (sub[i] == '+' || sub[i] == '-'))
      ++i;
    bool seenExpDigit = false;
    while (i < sub.size() && sub[i] >= '0' && sub[i] <= '9') {
      seenExpDigit = true;
      ++i;
    }
    if (!seenExpDigit || i != sub.size())
      return false;
  }

  char *endPtr = nullptr;
  const double v = std::strtod(sub.c_str(), &endPtr);
  if (endPtr == sub.c_str() || *endPtr != '\0' || !std::isfinite(v) || v < minVal || v > maxVal) {
    return false;
  }
  out = v;
  return true;
}

// Creates parent directories for outPath when needed; returns false and prints to stderr on error.
inline bool ensureParentDirectory(const std::string &outPath) {
  const std::filesystem::path p(outPath);
  if (!p.has_parent_path()) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  if (ec) {
    std::fprintf(stderr, "Error creating parent directory '%s': %s\n",
                 p.parent_path().string().c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

} // namespace tb::cli

#endif // TABLEBASE_CLI_UTILS_HPP
