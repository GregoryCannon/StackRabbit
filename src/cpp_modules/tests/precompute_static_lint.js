/**
 * Crude, deterministic tripwire for thread-safety hygiene in the files reachable
 * from the precompute lookup entry points (getLockValueLookupMapped /
 * getLockValueLookupDasMaps in high_level_search.cpp, which precompute runs on
 * concurrent per-next-piece threads).
 *
 * It flags non-const, non-thread_local file-scope `static` VARIABLE declarations
 * in the audited files. Function declarations are excluded: when a `static`
 * declaration's joined text hits a `(` before any `=` or `;`, it is a function
 * (this joins continuation lines, so multi-line definitions like
 * `static unordered_map<string, float>\ngetLockValueLookupForDasBucket(` are
 * classified correctly). The heuristic is knowingly crude -- a false positive
 * is waived in the allowlist file, never silently worked around in the lint.
 *
 * Waivers: src/cpp_modules/tests/thread_static_allowlist.txt, one `file:line`
 * or bare `file` entry per line (`#` starts a comment).
 *
 * Usage: npm run precompute_static_lint
 */
const fs = require("fs");
const path = require("path");

const CPP_SRC_DIR = path.join(__dirname, "../src");
const ALLOWLIST_PATH = path.join(__dirname, "thread_static_allowlist.txt");

const AUDITED_FILES = [
  "high_level_search.cpp",
  "tournament_search.cpp",
  "playout.cpp",
  "eval.cpp",
  "eval_context.cpp",
  "piece_rng.cpp",
  "move_result.cpp",
  "move_search.cpp",
  "full_move_search.cpp",
  "piece_ranges.cpp",
];

const MAX_CONTINUATION_LINES = 20;

/** Removes // comments, /* block comments (persisting across lines), and string/char literals. */
function stripCommentsAndLiterals(line, state) {
  let out = "";
  let i = 0;
  while (i < line.length) {
    const two = line.slice(i, i + 2);
    if (state.inBlockComment) {
      if (two === "*/") {
        state.inBlockComment = false;
        i += 2;
      } else {
        i += 1;
      }
      out += " ";
      continue;
    }
    if (two === "/*") {
      state.inBlockComment = true;
      i += 2;
      out += " ";
      continue;
    }
    if (two === "//") {
      break;
    }
    const c = line[i];
    if (c === '"' || c === "'") {
      const quote = c;
      i += 1;
      while (i < line.length && line[i] !== quote) {
        i += line[i] === "\\" ? 2 : 1;
      }
      i += 1; // closing quote
      out += " ";
      continue;
    }
    out += c;
    i += 1;
  }
  return out;
}

/** Joins a `static` declaration's lines until the first `(`, `=`, or `;` decides its kind. */
function joinDeclaration(lines, startIdx) {
  let text = "";
  let i = startIdx;
  for (; i < lines.length && i - startIdx < MAX_CONTINUATION_LINES; i++) {
    text += " " + lines[i];
    const decider = Math.min(
      ...["(", "=", ";"].map((ch) => {
        const at = text.indexOf(ch);
        return at === -1 ? Infinity : at;
      })
    );
    if (decider !== Infinity) {
      return { text: text.slice(0, decider), hit: text[decider], endIdx: i };
    }
  }
  return { text, hit: null, endIdx: i };
}

function lintFile(filePath, fileName) {
  const state = { inBlockComment: false };
  const rawLines = fs.readFileSync(filePath, "utf8").split("\n");
  const stripped = rawLines.map((line) => stripCommentsAndLiterals(line, state));

  // Brace depth: only declarations made at file scope (depth 0) are in range.
  let depth = 0;
  const offenses = [];
  for (let i = 0; i < stripped.length; i++) {
    if (depth === 0 && /^\s*static\b/.test(stripped[i])) {
      const joined = joinDeclaration(stripped, i);
      if (joined.hit !== "(") {
        const declared = joined.text;
        if (!/\bconst\b/.test(declared) && !/\bconstexpr\b/.test(declared) && !/\bthread_local\b/.test(declared)) {
          offenses.push(`${fileName}:${i + 1}`);
        }
      }
    }
    for (const c of stripped[i]) {
      if (c === "{") depth++;
      else if (c === "}") depth--;
    }
  }
  return offenses;
}

function main() {
  const waivers = new Set(
    fs
      .readFileSync(ALLOWLIST_PATH, "utf8")
      .split("\n")
      .map((line) => line.replace(/#.*$/, "").trim())
      .filter((line) => line.length > 0)
  );

  const offenses = [];
  for (const fileName of AUDITED_FILES) {
    for (const offense of lintFile(path.join(CPP_SRC_DIR, fileName), fileName)) {
      if (!waivers.has(offense) && !waivers.has(offense.split(":")[0])) {
        offenses.push(offense);
      }
    }
  }

  if (offenses.length > 0) {
    console.error("Non-const file-scope statics in the precompute-thread-audited files:");
    for (const offense of offenses) {
      console.error(`  ${offense}`);
    }
    console.error(
      "These run under concurrent per-next-piece precompute threads (see AGENTS.md). " +
        "Fix the declaration, or add a waiver in src/cpp_modules/tests/thread_static_allowlist.txt " +
        "if it is deliberate -- a waiver is a reviewable act, not a rubber stamp."
    );
    process.exitCode = 1;
    return;
  }
  console.log(
    `precompute_static_lint: 0 non-const file-scope statics across ${AUDITED_FILES.length} audited files`
  );
}

if (require.main === module) {
  main();
}
