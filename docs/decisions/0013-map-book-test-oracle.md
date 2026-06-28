# ADR 0013 — Map-based order book as a differential-test oracle

- **Status:** Accepted — design (not yet implemented)
- **Component:** tests — `tests/cpp/` (planned)
- **Date:** 2026-06-27

## Context

The ring-buffer book (ADR 0010) is dense with invariants — range-check-before-
mask, clear-on-slide, gap→reset, the modify rule, queue ordering — each of which,
gotten slightly wrong, produces **silent corruption** (no crash, just wrong
fills). Hand-written unit tests cover the cases the author *thinks to write*; the
dangerous slide/reset/aliasing bugs only surface after **long sequences of real
events** that cross window edges and reuse slots in combinations no human
enumerates.

## Decision

Maintain a second, deliberately-simple **`std::map`-based order book** as a
**reference oracle**. Run the **same MBO stream** through both the fast ring book
and the map book, and after each event assert the two are **identical**.

The equivalence check is **deep** — comparing best bid/ask is *not* enough (two
books can share a top while differing in depth or queue order, which is exactly
the corruption being hunted). Full equivalence is:

1. the same set of price levels on each side,
2. the same total size at each level, and
3. the same orders in the same **FIFO sequence** within each level (same
   `order_id`s, same sizes, same order).

Item (3) matters most here: queue order determines *who fills next*, the basis of
the queue-aware fill model (ADR 0017).

## Consequences

**Positive**
- **Vast coverage for free** — millions of real events exercise the exact paths
  most likely to be silently wrong.
- The two books have **disjoint bug surfaces**; any divergence means the complex
  one is wrong and the simple one shows the right answer.
- The oracle is **trustworthy because it is simple** — no `base`, no mask, no
  slide, no reset — correct by inspection. Speed is traded for trustworthiness;
  it never ships.

**Negative / cost**
- A second book implementation to maintain.
- The equivalence check must be deep, or it gives false confidence.

## Alternatives considered

- **Unit tests only.** Rejected as *sufficient*: they miss the long-sequence
  state transitions. (Still written — this complements them.)
- **Compare best bid/ask only.** Rejected: passes on a book with corrupted depth
  or queue order — the very bugs the oracle exists to catch.

## Related

- ADR 0008 — the no-framework / exit-code test harness this runs under.
- ADR 0010 — the structure under test.
- ADR 0012 — the per-level queue whose order must match.
