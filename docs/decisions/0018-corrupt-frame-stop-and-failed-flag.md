# ADR 0018 — Corrupt frame stops the stream and sets `failed()`

- **Status:** Accepted
- **Date:** 2026-07-21
- **Component:** ingest — `core/include/trading_sim/dbn_reader.hpp`

## Context

The framer (`frame_one`) reads a record's `length` byte and reports one of three
outcomes: `Ok`, `NeedMore`, or `Corrupt`. `Corrupt` fires on `length == 0` — a
zero-byte record. This is not a hypothetical: without the guard, a zero-size record
makes the caller's advance-by-size cursor move by 0 and loop forever (see ADR on the
framer). But once we detect it, we must decide what the *reader* does with it, and
crucially how that differs from the OTHER kind of bad record we already handle.

There are two distinct classes of "bad record", and conflating them would be wrong:

1. **A malformed but well-framed record** — e.g. an MBO whose `action` byte is not a
   valid action. Its `length` is sane, so framing is intact: we know exactly where it
   ends and where the next record begins. We can skip it and keep going (ADR 0002:
   malformed records are routine on the parse path).

2. **A corrupt frame** — `length == 0`. The framing itself is gone. A zero-length
   record gives us no way to know where the next record starts. There is no marker to
   resynchronize to in a flat, self-delimiting stream: once the length prefix is
   untrustworthy, every byte after it is untrustworthy too.

## Decision

On a `Corrupt` frame, `DbnReader::next()` **stops the stream**: it sets a private
`failed_` flag, returns `std::nullopt`, and returns `nullopt` on every subsequent
call. The consumer distinguishes this from a clean end-of-file by querying
`failed()`:

- `next()` returns `nullopt` **and** `failed() == false` → clean EOF.
- `next()` returns `nullopt` **and** `failed() == true` → stopped on a corrupt frame.

A malformed-but-framed record (class 1) is handled differently and deliberately: it
is **skipped**, and iteration continues (ADR 0002 / ADR 0003).

## Consequences

**Positive**
- **No infinite loop, no guessing.** We never try to "resync" past a lost frame by
  scanning for a plausible next record — that would be inventing structure we cannot
  verify and would risk emitting garbage Events.
- **The failure is observable, not silent.** Collapsing "ran out cleanly" and "gave
  up on corruption" into a bare `nullopt` would hide a data-integrity problem. The
  `failed()` flag makes it queryable, which is the seed of the observability work
  (dropped/failed counters) tracked for the hardening pass.
- **The two error classes stay separated**, each with the response its structure
  warrants: skip what we can still frame, stop when framing is lost.

**Negative / cost**
- We stop at the FIRST corrupt frame even if valid records might follow. For a
  historical file that is the honest choice (a corrupt frame means the file is
  damaged or our understanding of the format is wrong — both warrant a stop). A live
  feed that can genuinely resynchronize (e.g. a framed protocol with sync markers)
  would revisit this; DBN's flat record stream has no such marker.

## Alternatives considered

- **Skip the corrupt frame and continue** (treat it like a bad-action record).
  Rejected: there is no safe number of bytes to skip. `length == 0` gives no record
  size to advance by, so "continue" has no defined starting point — any choice is a
  guess that risks framing garbage as records.
- **Throw on corruption.** Rejected: corruption is discovered mid-iteration, on the
  hot pull path, after the reader has already yielded valid Events. A thrown
  exception would discard the run's partial, valid output and force every caller into
  a try/catch around a loop. A queryable flag lets the caller keep what it got and
  decide.
- **Collapse into a bare `nullopt`** (no `failed()`). Rejected: silently
  indistinguishable from EOF — exactly the silent-failure mode this project treats as
  worse than a loud one.

## Related

- ADR 0002 — malformed records return a value (routine), not an exception.
- ADR 0003 — routing (rtype) vs validation (action/side) split; non-MBO records skip.
- ADR 0007 — `-Wswitch` with no `default` so a new `FrameStatus`/`PullStatus` cannot
  be silently swallowed.
