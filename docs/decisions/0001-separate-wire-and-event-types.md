# ADR 0001 — Separate `WireMbo` (wire) and `Event` (engine) types

- **Status:** Accepted
- **Date:** 2026-06-27 (decided ~2026-06-26, commit `93d35d9`)
- **Component:** market-data ingest — `core/include/trading_sim/dbn.hpp`,
  `core/include/trading_sim/event.hpp`

## Context

The DBN (Databento Binary Encoding) market-by-order feed delivers fixed
**56-byte little-endian** records in a *mixed* stream (MBO records interleaved
with symbol-mapping, system, and error records). The matching engine needs an
in-memory event to consume. The question: do we read the wire bytes into the
**same** struct the engine uses, or into a separate one we then convert?

A single shared struct is tempting — it avoids a copy and there's only one type
to maintain.

## Decision

Keep **two distinct types**, and make conversion the only bridge between them:

- **`WireMbo`** — a byte-for-byte mirror of the 56-byte DBN record, in **wire
  field order**, treated as **untrusted** input. Its layout is pinned to the wire
  by `static_assert(sizeof == 56)` plus an `offsetof` proof for every field.
- **`Event`** — the **48-byte** internal event the engine consumes, fields
  ordered **largest-first** (no internal padding), treated as **validated /
  trusted**.

The *only* way to obtain an `Event` from wire bytes is
`to_event(const WireMbo&) -> std::expected<Event, ParseError>`, which validates
the fields it transforms (`action`, `side`) and converts **field by field**.

## Consequences

**Positive**
- **Compiler-enforced trust boundary.** Because the engine's functions take
  `Event`, and the only producer of an `Event` from the wire is `to_event`, *every
  `Event` in the system is validated by construction.* You cannot hand the book
  unvalidated bytes — that code does not compile. The type boundary **is** the
  trust boundary.
- **Layout freedom.** `WireMbo` is free to match the wire; `Event` is free to be
  cache-optimal. Neither compromises for the other.
- **Change isolation.** A wire-format change touches only `WireMbo` and the
  converter — never the book, never replay.

**Negative / cost**
- **One translation (copy) step per event.** This is paid deliberately: the
  benefit is safety, not speed. (Common misconception to avoid in defense: the two
  types are *not* faster — they cost a copy. They are *safer*.)

## Alternatives considered

- **Single shared struct (read wire bytes straight into the engine type).**
  Rejected: nothing would force validation before the book sees the bytes, and the
  wire's field order would force the engine type into a cache-suboptimal layout.
- **`memcpy(WireMbo → Event)` as a shortcut.** Impossible by construction: the two
  types differ in **field set** (Event drops `rtype`, `publisher_id`,
  `instrument_id`, `channel_id`, `ts_in_delta`) **and field order**, so a byte
  copy would land bytes in the wrong fields. The conversion *must* be field-by-
  field — which is exactly where validation naturally lives.

> Interview one-liner: see `docs/interview-cheatsheet.md`.

## Related

- ADR 0002 — `std::expected<Event, ParseError>` as the converter return type.
- ADR 0003 — routing (`rtype`/`length`) in the streaming loop vs field validation
  in `to_event`.
