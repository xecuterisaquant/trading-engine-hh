# ADR 0008 — No-framework tests: distinct exit codes, never `assert()`

- **Status:** Accepted
- **Date:** 2026-06-27 (Phase 0)
- **Component:** `tests/cpp/*`, `CMakeLists.txt`

## Context

The C++ core needs unit tests in CI. In the early phases there is little logic, so
a full test framework would be dependency overhead before it earns its keep.

## Decision

Write tests as a bare `int main()` returning `0` for pass and a **distinct
non-zero code per check** (`return 1`, `return 2`, …), registered with CTest. **Do
not use `assert()`.** Add a real framework (Catch2/GoogleTest) only when there's
enough logic to justify it.

## Consequences

**Positive**
- **Zero test dependency** to build; CTest already interprets exit codes
  (0 = pass).
- Distinct codes **pinpoint the failed check** without a debugger or prints.
- Explicit `if (...) return N;` checks **run in every build type**, Debug and
  Release.

**Negative / cost**
- Manual and verbose; no fixtures/matchers. Acceptable until logic volume grows.

## Why not `assert()`

`assert()` is **compiled out under `NDEBUG`**, which Release defines. An
assert-based test in a Release build becomes a **no-op that always passes** — a
green suite that tested nothing in the configuration we actually ship. Same family
of failure as a silently-dropped check anywhere else in the codebase.

## Alternatives considered

- **Catch2 / GoogleTest now.** Deferred (not rejected forever): added when there's
  logic worth the dependency.
- **`assert()`-based tests.** Rejected: false green in Release.

## Related

- `docs/interview-cheatsheet.md` — the `assert`/`NDEBUG` one-liner.
