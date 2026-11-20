// Unit tests for the StackRabbit C++ engine (see TESTING.md for the plan).
//
// Mirrors the unity-build pattern of src/main.cpp: instead of listing sources in
// a makefile, we include main.cpp, which includes every engine source file.
// Deliberately NOT included: the other main() definers (entrypoint.cpp,
// fixture_dump.cpp, precompute_tsan.cpp, src/benchmark/*.cpp) and
// module.cpp (nan glue) / wasm.cpp (emscripten glue).
//
// The test cases themselves live in per-category headers, included below:
//   helpers.hpp                 shared board builders, InputBuilder, assertions
//   input_validation_tests.hpp  SECTION A: input parsing & validation
//   request_type_tests.hpp      SECTION B: one test per request type
//   board_format_tests.hpp      formatBoard/parseCompressedBoard round trips
//   precompute_collapse_tests.hpp  synthetic-state collapse/formatter pins
//   precompute_property_tests.hpp  pipeline invariants on inline cases

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../vendor/doctest.h"

#include "../src/main.cpp"

#include "helpers.hpp"
#include "board_format_tests.hpp"
#include "input_validation_tests.hpp"
#include "move_search_fuzz_tests.hpp"
#include "precompute_collapse_tests.hpp"
#include "precompute_property_tests.hpp"
#include "request_type_tests.hpp"
