# ADR 0003 — Routing (`rtype`/`length`) in the loop; field validation in `to_event`

- **Status:** Accepted
- **Date:** 2026-06-27
- **Component:** market-data ingest — streaming decode loop (planned) +
  `core/include/trading_sim/dbn.hpp`

## Context

A DBN stream is **mixed**: MBO records are interleaved with symbol-mapping,
system, and error records of different types and lengths. Two distinct kinds of
checking exist:

1. **Routing** — "is this even an MBO record?" (`rtype`), and "where does the next
   record start?" (`length`). This is framing over raw bytes.
2. **Field validation** — "are this record's `action`/`side` in the legal
   alphabet?" before they are `static_cast` into enums.

The question: do both live in one place, or are they split?

## Decision

- **Routing lives in the streaming decode loop.** It is the only stage that sees
  raw framing, so it alone can skip non-MBO records (advance `length × 4`) and
  decide which records reach the converter.
- **Field validation lives in `to_event`.** It validates exactly the fields it
  *transforms* (`action`, `side`), at the single conversion choke point.

## Consequences

**Positive**
- Each stage validates exactly what it uniquely sees: the loop sees framing; the
  converter sees a committed typed record and the fields it consumes.
- The trust boundary stays **single and un-split** — "is every `Event`
  validated?" depends on one function, not two files staying in sync.
- No redundant re-checking.

**Negative / cost**
- Validation-adjacent responsibility is split across two stages, so the division
  must be documented (this ADR + comments in `dbn.hpp`).

## Alternatives considered

- **All checks in `to_event`.** Impossible: by the time `to_event` runs it has a
  typed `WireMbo`, not a byte stream — it structurally cannot see framing or route
  other record types.
- **All checks in the loop (including `action`/`side`).** Rejected: it duplicates
  the enum-legality logic and splits the trust boundary across files
  (belt-and-suspenders), weakening the "one door" guarantee from ADR 0001.

## Related

- ADR 0001 — the single-door trust boundary.
- ADR 0002 — how `to_event` reports a validation failure.
- ADR 0007 — why `action`/`side` (closed alphabet) are the validatable fields.
