# ADR 0022 — Fuzzing the framer: portable property test + libFuzzer harness

- **Status:** Accepted
- **Date:** 2026-07-22
- **Component:** ingest tests — `tests/cpp/test_frame_fuzz.cpp`,
  `fuzz/frame_fuzz_libfuzzer.cpp`

## Context

`frame_one` and the `RecordAssembler` drain loop consume **arbitrary, untrusted
bytes** — the exact profile of code that fuzzing is built for. Our hand-written cases
(test_dbn_reader.cpp) verify the paths we *thought of*: the straddle, the length-0
corrupt frame, back-to-back records. They cannot cover the inputs we did not imagine,
and binary framing is precisely where the unimagined input bites: an out-of-bounds
read, a contract violation, or — worst — an **infinite loop** (the failure the
length-0 `Corrupt` guard exists to prevent).

The standard tool is libFuzzer/AFL: coverage-guided, in-process, mutation-based. But
this project builds primarily with **MSVC on Windows**, and libFuzzer needs clang with
`-fsanitize=fuzzer`. Making the whole test suite depend on that toolchain would be a
regression in portability (ADR 0008's "runs everywhere, no framework" philosophy).

## Decision

Two layers, so we get portable regression coverage *and* deep coverage-guided fuzzing:

1. **Portable randomized property test** (`test_frame_fuzz.cpp`, a normal ctest
   target on every platform):
   - A **fixed-seed** PRNG throws random buffers at `frame_one`, asserting its contract
     holds for *any* input: an `Ok` size is word-aligned, in `[4, 255*4]`, equals
     `length*4`, and never exceeds the buffer; `NeedMore`/`Corrupt` only occur under
     their defined conditions.
   - A bounded-iteration check drains random byte streams through the assembler and
     asserts the loop **always terminates** (each `Record` consumes ≥ 4 bytes, so the
     step count is capped) — the infinite-loop guard under fire.
   - Fixed seed ⇒ **reproducible**: a CI failure replays the exact bytes locally. This
     is the property a one-shot `rand()` test lacks and the reason a *regression*
     fuzzer must be deterministic.

2. **libFuzzer harness** (`fuzz/frame_fuzz_libfuzzer.cpp`, opt-in): the same target
   surface behind `LLVMFuzzerTestOneInput`, built only when `-DTRADING_SIM_FUZZER=ON`
   under clang with `-fsanitize=fuzzer,address,undefined`. Coverage-guided mutation
   explores framing paths the random test would hit only by luck; ASan/UBSan catch
   OOB/UB and the timeout catches hangs. **OFF by default**, so the normal build is
   untouched (the guarded CMake block is never evaluated).

## Consequences

**Positive**
- **Every CI run fuzzes** the framer (portable test), on all platforms, with no extra
  toolchain — catching contract and termination regressions immediately.
- **Deep, coverage-guided fuzzing is one flag away** for anyone with clang, without
  imposing that toolchain on everyone.
- The termination property is now *tested*, not merely argued — the length-0 guard has
  adversarial evidence behind it.

**Negative / cost**
- The portable test is random, not coverage-guided: it explores by volume, not by
  program structure, so it is weaker than libFuzzer at reaching deep states. That is
  exactly the gap the opt-in harness fills.
- Iteration counts are tuned so the ctest stays a few seconds under MSVC Debug's
  checked-iterator overhead; the heavy, long-running fuzzing is the libFuzzer job, not
  the ctest.
- The libFuzzer target is not built in our default MSVC CI, so it can bit-rot; the
  shared target surface (it calls the same `frame_one`/`route_record`) keeps that risk
  low.

## Alternatives considered

- **libFuzzer only.** Rejected as the sole mechanism: it would make the framer's fuzz
  coverage depend on a clang+sanitizer build we do not run by default, leaving MSVC CI
  with no fuzzing at all.
- **Portable random test only.** Rejected as *insufficient* for the institutional bar:
  random buffers rarely reach deep framing states; coverage guidance is what finds the
  subtle ones, so the harness is worth having even if opt-in.
- **A third-party property-testing framework (RapidCheck, etc.).** Rejected: a new
  dependency for what a seeded `mt19937_64` and a handful of invariant checks already
  deliver (consistent with ADR 0008).

## Related

- ADR 0008 — no-framework tests / exit codes: the style and "no dependency where a
  little code suffices" philosophy this follows.
- ADR 0018 — the corrupt-frame (length-0) policy whose termination guarantee this
  fuzzes.
