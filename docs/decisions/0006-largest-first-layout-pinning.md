# ADR 0006 — Largest-first struct layout + compile-time layout pinning

- **Status:** Accepted
- **Date:** 2026-06-27 (commits `48d7f9a`, `93d35d9`)
- **Component:** `core/include/trading_sim/event.hpp`,
  `core/include/trading_sim/dbn.hpp`

## Context

`Event` is read and copied on **every replayed event** — the replay path is
memory-bandwidth-bound, so the struct's size matters. C++ cannot reorder fields,
so layout (and therefore padding) is determined by declaration order. Each scalar
must sit at an offset that is a multiple of its alignment, or the compiler inserts
padding.

## Decision

- Declare `Event` fields **largest-first** (`u64`s → `u32`s → 1-byte
  enums/`flags`), so the running offset always already satisfies the next field's
  alignment → **no internal padding** (48 bytes; 43 used + 5 tail).
- **Pin the layout** with `static_assert(sizeof == 48)`, `alignof == 8`, and
  `is_trivially_copyable_v`.
- `WireMbo` mirrors **wire order** (not largest-first) but is also padding-free —
  Databento engineered the format so small fields fill alignment gaps (the four
  1-byte fields at offsets 36–39 plug the gap before `ts_recv` at 40). Proven with
  a `static_assert(offsetof == N)` for every field.

## Consequences

**Positive**
- Minimal cache footprint / bandwidth on the replay hot path (48 B, not 56 B).
- The `static_assert`s are a **regression tripwire**: a careless reorder or field
  addition fails the build instead of silently slowing replay.
- Trivially copyable → the `memcpy` parser fast path is legal.

**Negative / cost**
- Field order follows size, not logical grouping — a readability trade-off,
  mitigated by comments.

## Alternatives considered

- **Logical/grouped field order.** Rejected: a small field before a larger-aligned
  one punches an alignment hole (e.g. a 1-byte field before a `u64` → 7-byte gap),
  bloating the struct.
- **`#pragma pack(1)` on `WireMbo`.** Rejected: DBN already aligns every field, so
  packing saves zero bytes while stripping alignment and inviting misaligned-
  pointer UB. The `offsetof` proofs give the wire match without packing.

## Related

- ADR 0005 — `price` is one of the `u64`-class fields.
- ADR 0007 — `: char` enums keep `action`/`side` at 1 byte each.
- ADR 0009 — the little-endian contract behind the `memcpy` fast path.
