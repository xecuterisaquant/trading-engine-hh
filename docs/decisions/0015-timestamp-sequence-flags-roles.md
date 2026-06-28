# ADR 0015 — Roles of `ts_event`, `ts_recv`, `sequence`, and `flags`

- **Status:** Accepted
- **Date:** 2026-06-27
- **Component:** `core/include/trading_sim/event.hpp` (fields exist + commented);
  consumed by the replay loop / book (planned)

## Context

`Event` carries two timestamps (`ts_event`, `ts_recv`), a `sequence`, and
`flags`. The fields are stored and commented; this ADR pins down *how each is
used*, because using the wrong one for the wrong job is a classic backtest bug.

## Decision

- **`ts_event` orders the book replay.** It is the venue/matching-engine's own
  timestamp — the authoritative order in which the exchange processed events. The
  reconstructed book must follow it.
- **`ts_recv` gates strategy reaction.** It is when *our* consumer received the
  event — the earliest a strategy could legitimately act. It also models feed
  latency (`ts_recv − ts_event`) for later realism.
- **`sequence` is the gap-detection tripwire.** A jump (e.g. `100 → 103`) means
  records were **dropped**; the book has silently diverged from reality, so a gap
  must trigger reset/resync, never silent continuation.
- **`flags` / `F_LAST` marks event-group boundaries.** One matching-engine event
  can emit several MBO records (e.g. one aggressor producing multiple fills);
  `F_LAST` marks the last of the group, so the group can be applied **atomically**
  and the book published as consistent only at the boundary.

## Consequences

**Positive**
- **No lookahead bias** — reaction is gated on receive time, not venue time.
- **Correct ordering** — book follows venue time, immune to receive-side jitter.
- **Data-loss is detected**, not silently absorbed.
- **Consistent snapshots** — strategies never react to a mid-group, partially
  applied book.

**Negative / cost**
- Both timestamps must be carried (already in the 48-byte layout, ADR 0006).
- Group-atomic application needs buffering until `F_LAST`.

## Alternatives considered

- **Order the book by `ts_recv`.** Rejected: receive-side network/feed **jitter**
  would reorder events relative to how the venue actually processed them — a
  corrupted book.
- **Let strategies react on `ts_event`.** Rejected: that is acting on information
  *before it reached you* — **lookahead bias**, the most common backtest sin.
- **Ignore `sequence`.** Rejected: a dropped packet (e.g. a missed cancel leaving
  a ghost order) would corrupt the book with no warning.
- **Ignore `F_LAST`.** Rejected: a strategy could react to a partially applied
  sweep — a book state that never existed as a stable snapshot.

## Related

- ADR 0001 / 0002 — the ingest path these fields arrive through.
- ADR 0017 — the fill model that depends on honest receive-time gating.
