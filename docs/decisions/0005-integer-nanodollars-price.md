# ADR 0005 — Price as integer nanodollars; no tick quantization in the converter

- **Status:** Accepted
- **Date:** 2026-06-27 (commit `48d7f9a`)
- **Component:** `core/include/trading_sim/event.hpp`,
  `core/include/trading_sim/dbn.hpp`

## Context

Price needs an in-memory representation. The order book will be **keyed on price**,
so prices must compare exactly. A tick is the minimum legal price increment; the
question of *where* prices get quantized to a tick also arises.

## Decision

Store `price` as `std::int64_t` **nanodollars** (1e-9 USD), exactly as on the DBN
wire. The converter copies it across **unquantized** — tick alignment is handled
later, at the book boundary.

## Consequences

**Positive**
- **Exact, comparable, hashable** integer prices — required for a price-keyed
  container; `double` cannot compare decimal money reliably.
- **Deterministic, reproducible** backtests (integer math is bit-stable across
  machines/compilers).
- **Lossless**: trade prints (T/F) can be genuinely sub-tick (e.g. midpoint
  fills); not rounding preserves them.

**Negative / cost**
- Must track the 1e-9 scale and format for display.

## Alternatives considered

- **`double` dollars.** Rejected: most decimal prices have no exact binary form,
  so equality breaks — fatal for a price-keyed book — and results aren't
  reproducible.
- **Quantize to a tick inside `to_event`.** Rejected on two counts: the converter
  doesn't *know* the per-instrument tick size (it's defined at the book), and
  rounding would **irreversibly destroy** sub-tick trade prints. Quantization
  belongs at the book, the only place that both knows the tick and can safely
  assume tick alignment (resting orders only).

## Related

- ADR 0006 — the largest-first layout `price` sits in.
