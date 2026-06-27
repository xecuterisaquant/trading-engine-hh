# ADR 0009 — Little-endian `static_assert`: the memcpy byte-order contract

- **Status:** Accepted
- **Date:** 2026-06-27 (commit `93d35d9`)
- **Component:** `core/include/trading_sim/dbn.hpp`

## Context

The parser fast path `memcpy`s raw wire bytes straight into multi-byte integer
fields (`ts_event`, `price`, `order_id`, …). **`memcpy` does not byte-swap** — it
is correct only if the host's byte order matches the wire's. DBN is little-endian;
our targets (x86-64, ARM64) are little-endian.

## Decision

Add `static_assert(std::endian::native == std::endian::little, ...)` in `dbn.hpp`,
making the host-endianness assumption a compile-time contract.

## Consequences

**Positive**
- A big-endian build **fails to compile** instead of silently scrambling every
  price and timestamp at runtime (a bug with no crash to warn you).
- **Zero runtime cost** — endianness is a compile-time constant, so there is no
  reason to spend a runtime branch on it.

**Negative / cost**
- The code is intentionally **not portable to big-endian** without adding
  byte-swaps; the assert documents and enforces that scope.

## Alternatives considered

- **Runtime endianness check.** Rejected: it burns a branch on a value that can
  never change during execution, and only fails *after* shipping to the wrong
  host.
- **A comment only.** Rejected: a comment cannot fail the build; the assumption
  would be unenforced.
- **A byte-swap abstraction now.** Deferred: unnecessary complexity for
  little-endian-only targets.

## Related

- ADR 0006 — the trivially-copyable layout the `memcpy` fast path relies on.
