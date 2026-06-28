# ADR 0010 — Windowed ring-buffer order book (tick-indexed) + slide mechanics

- **Status:** Accepted — design (not yet implemented; author-written core)
- **Date:** 2026-06-27
- **Component:** order book — `core/include/trading_sim/book.hpp` (planned)

## Context

The book maintains every resting order, organised so the hot operations are
fast: read **best bid/ask** (top of book), **insert** at a price level,
**cancel**, and **walk levels outward** for matching. The price representation
is integer ticks (see ADR 0005).

The structure choice turns on one observation about real single-name data:

- the price **level drifts** a lot over a session (MSFT can roam a \$15–20 band
  ≈ 1,500–2,000 penny-ticks), but
- the book **depth is bounded** — essentially all resting liquidity sits within a
  few hundred ticks of the touch at any instant.

So the thing that drifts (absolute level) and the thing that is bounded (depth)
are different numbers. The data structure should be sized to the bounded one.

## Decision

A **per-side windowed ring buffer**: a fixed-width array of price levels covering
`[base, base + WIDTH)` ticks around the current touch, indexed by

```
slot = (tick − base) & (WIDTH − 1)      // WIDTH a power of two → mask, not divide
```

`WIDTH` is a power of two so the modulo is a single-cycle bitmask rather than a
~20-cycle division on the hot path. As the market moves and the touch nears an
edge, **slide** the window by advancing `base` and reusing the slots that scroll
off the far side — circular reuse is what makes it a ring rather than a `vector`
we shift.

**Invariants (each guards against *silent* corruption — no crash, just wrong
fills):**

1. **Range-check before the mask.** Verify `base ≤ tick < base + WIDTH` *before*
   computing `slot`. The mask alone is not a bounds check — it wraps. A tick
   `WIDTH + 5` above `base` masks to slot `5`, the physical home of `base + 5`, a
   different legitimate price → **aliasing**: one order's data stomps another
   level's slot.
2. **Clear-on-slide.** When `base` advances by `k`, the `k` slots that leave the
   window must be cleared. The ring **reuses** physical storage, so an uncleared
   slot is read later as a **ghost order** from a scrolled-off price.
3. **Gap > WIDTH → full reset.** A move (halt/gap) larger than `WIDTH` cannot be
   represented relative to any single `base` (old resting book and new price
   cannot coexist in one window) → clear everything and rebuild. Direction is
   irrelevant; only magnitude relative to `WIDTH` matters.

Slide cost is **O(levels vacated)**, not O(WIDTH): only the slots that actually
leave the window are touched.

**Tick mapping.** Prices are stored as integer nanodollars (ADR 0005) and
converted to a dense integer tick for indexing under a **constant-penny tick**
assumption (valid for large-cap equities; ADR 0016). Sub-tick trade prints (T/F)
never index the price array — they are handled outside it (ADR 0005).

## Consequences

**Positive**
- **O(1), cache-contiguous** level access; matching walks a contiguous array, not
  a pointer chase.
- **Bounded memory** — sized to depth, not to the full price range.
- **No per-operation heap allocation** on the hot path.

**Negative / cost**
- Windowing machinery (slide + the three invariants) is where the subtle bugs
  live; mitigated by the differential-test oracle (ADR 0013).
- Assumes liquidity never spreads wider than `WIDTH`; halts/large gaps pay a
  reset.
- Assumes a constant per-instrument tick (fine for the large-cap dev slice).

## Alternatives considered

- **`std::map<price, level>`.** Rejected for the hot path: pointer-chasing across
  scattered cache lines, O(log n) per op, a heap allocation per node. Its only
  win — free handling of drift and sparsity — is one this design does not need.
  (Kept, deliberately, as the *test oracle* — ADR 0013.)
- **One giant flat array over the full price range.** Rejected: O(1) but most
  slots are empty almost always (liquidity hugs the touch), wasting memory/cache,
  and a price outside the fixed range corrupts or crashes.
- **`memmove`-based sliding array.** Rejected: shifts every element on each move —
  O(WIDTH) per tick of drift vs the ring's O(levels vacated).

## Related

- ADR 0005 — integer nanodollars / where tick quantization happens.
- ADR 0006 — the cache-conscious layout philosophy.
- ADR 0011 — one ring per side (not a combined array).
- ADR 0012 — the intrusive FIFO list living inside each level.
- ADR 0013 — the map oracle that validates this structure.
