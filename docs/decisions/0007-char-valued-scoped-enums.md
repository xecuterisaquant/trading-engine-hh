# ADR 0007 — `char`-valued `enum class : char` for `Action` and `Side`

- **Status:** Accepted
- **Date:** 2026-06-27 (commit `48d7f9a`)
- **Component:** `core/include/trading_sim/event.hpp`

## Context

`action` and `side` arrive as single wire bytes from a **closed alphabet**
(`action ∈ {A,C,M,T,F,R}`, `side ∈ {B,A,N}`). They need an in-memory type that is
safe, cheap to produce from the wire, and small.

## Decision

Model them as `enum class Action : char` / `enum class Side : char`, with each
enumerator set to its wire byte value (`Add = 'A'`, …).

## Consequences

**Positive**
- Wire→enum is a **zero-instruction `static_cast`** — the enumerator *is* the byte.
- **Scoped** (`enum class`): `Action` and `Side` are distinct types with no
  implicit `int` decay and no name leakage — you cannot mix them up.
- **`: char` base** pins each to **1 byte** (keeps `Event` at 48 — see ADR 0006)
  and makes the enum exactly wire-width.
- **Switch-exhaustiveness**: `switch` over a scoped enum with `-Wswitch` (and
  `/WX`) makes adding a new action a **build error** until every `switch` handles
  it.

**Negative / cost**
- The cast does **not** validate, so it must be paired with `is_valid_action` /
  `is_valid_side` at the conversion boundary (see ADR 0003).

## Alternatives considered

- **Raw `char`.** Rejected: no type safety — `action` and `side` become
  interchangeable, and no `switch` can be exhaustive.
- **Plain (unscoped) `enum`.** Rejected: implicit `int` conversion and enumerator
  name leakage into the enclosing scope.
- **Default (`int`) underlying type.** Rejected: 4 bytes per field would blow the
  48-byte `Event` layout.

## Related

- ADR 0003 — `is_valid_*` validation pairs with the non-validating cast.
- ADR 0006 — 1-byte enums are why the layout closes at 48.
