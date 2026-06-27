# ADR 0002 — `std::expected<Event, ParseError>` as the converter return type

- **Status:** Accepted
- **Date:** 2026-06-27 (decided ~2026-06-26, commit `93d35d9`)
- **Component:** market-data ingest — `core/include/trading_sim/dbn.hpp`

## Context

`to_event` can fail: a wire byte for `action`/`side` may be outside the legal
alphabet. On a real feed, **malformed records are routine, not exceptional**. The
converter is on the ingest hot path and is marked `noexcept`. It needs to report
*either* a converted `Event` *or* a typed reason for failure.

## Decision

Return `std::expected<Event, ParseError>` (C++23). On success it holds the
`Event`; on failure it holds a `ParseError`. Callers test `if (!ev)`, read the
value via `*ev` / `ev->field`, and the reason via `ev.error()`.

## Consequences

**Positive**
- Value-or-typed-error in a single return type; the failure mode is **in the
  signature**, and `[[nodiscard]]` makes ignoring it a warning.
- No hidden control flow — nothing unwinds; the optimizer sees a plain return.
- Keeps the `noexcept` promise.

**Negative / cost**
- Requires C++23. Caller must branch on every call (this is the point, not a
  drawback).

## Alternatives considered

- **Throw an exception.** Rejected: malformed records are routine, so exceptions
  are a category error here; throwing adds hidden unwinding edges at every call
  site that constrain the optimizer on a hot path; the failure is invisible in the
  signature; and it would break `noexcept`.
- **`std::optional<Event>`.** Rejected: it signals *that* parsing failed but not
  *why* — it throws away the reason, so you can't log, route, or measure
  `BadAction` vs `BadSide`. `expected` is `optional` with the error kept.
- **`bool` + out-parameter.** Rejected: out-params invite uninitialized reads,
  carry no typed reason, and are easy to misuse.

## Related

- ADR 0001 — the two-type trust boundary that `to_event` bridges.
- ADR 0003 — which fields `to_event` validates, and where routing lives.
