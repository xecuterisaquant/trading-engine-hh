# ADR 0011 — One ring per side, not one combined array

- **Status:** Accepted — design (not yet implemented; author-written core)
- **Date:** 2026-06-27
- **Component:** order book — `core/include/trading_sim/book.hpp` (planned)

## Context

Given the windowed ring-buffer book (ADR 0010), should bids and asks share **one
combined** ring spanning both sides, or get **two separate** rings, one per side?

The relevant fact: a price level is **either a bid or an ask, never both** — a
buy and a sell resting at the same price would cross and match. (They can be
momentarily crossed/locked *during* matching, but that resolves immediately.)

## Decision

**Two rings — one per side**, each with its own `base`, sliding independently.
Best bid = highest occupied slot in the bid ring; best ask = lowest occupied slot
in the ask ring. The spread between them occupies **zero slots** — it simply does
not exist in either array.

## Consequences

**Positive**
- **Independent slide.** Depth becomes asymmetric under one-way flow; two rings
  let each side slide on its own instead of a shared `base` fighting the
  asymmetry.
- **No crossed-book boundary to track** — each ring holds exactly one side, so
  the transient crossed/locked moment during matching needs no special boundary
  bookkeeping.
- **Spread costs nothing** — no wasted middle slots.

**Negative / cost**
- Two structures and two `base` values; touch tracking is per-side.

## Alternatives considered

- **One combined array spanning both sides.** Rejected. Its only real advantage
  is **contiguous traversal across the touch** — and matching never performs that
  traversal: an aggressive order walks **one side, outward from the touch**
  (a buy climbs asks upward from best ask; a sell walks bids downward), never
  bid→through-spread→ask in one sweep. So the contiguity is for a walk that never
  happens, while it forces a shared `base` (bad for asymmetric depth), wasted
  spread slots, and crossed-book boundary tracking. In a two-ring design, the
  side matching *does* walk is already contiguous within its own ring.

## Related

- ADR 0010 — the ring buffer these two instances are.
- ADR 0012 — the per-level queue each ring slot holds.
