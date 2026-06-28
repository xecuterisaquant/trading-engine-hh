# ADR 0014 — MBO action→book mapping and the modify priority rule

- **Status:** Accepted — design (not yet implemented; author-written core)
- **Date:** 2026-06-27
- **Component:** order book / matching — `core/include/trading_sim/book.hpp`
  (planned); `Action` in `core/include/trading_sim/event.hpp`

## Context

Every MBO event carries an `action ∈ {A, C, M, T, F, R}` (ADR 0007). Each maps to
an operation on the book (ADR 0010–0012). Most are mechanical; **M (modify)** is
the one with a non-obvious rule because it interacts with **price-time
priority** — the fairness guarantee that your queue position is a reward for
*waiting*.

## Decision

| Action | Book operation |
|--------|----------------|
| **A** Add    | append a new order at the **tail** of its level's FIFO; insert into the `order_id`→node map |
| **C** Cancel | map lookup → **unlink** from the level's list → erase from map (level emptied → slot cleared) |
| **F** Fill   | decrement the resting order's size in place; if it reaches zero, unlink it (it was at/near the **head**) |
| **T** Trade  | informational print (often `side = N`); usually no resting-order mutation — the book change arrives via F/C |
| **R** Clear  | full structural reset: empty both rings' lists and the map **and reset each ring's `base`/window** |
| **M** Modify | **see rule below** |

**Modify priority rule:**

- **Size decrease, price unchanged → keep queue position.** Decrement size in
  place; the node does not move. Reducing your size cannot disadvantage anyone
  behind you, so the exchange lets you keep your place.
- **Size increase, OR any price change → lose priority.** Treated as
  cancel + re-add at the **tail**. A size increase would let you grab queue
  priority you did not wait for (unfair to those behind you); a price change moves
  you to a **different level = a different list**, where "keep your position" is
  not even meaningful — you are in a new line, at its back.

## Consequences

**Positive**
- Preserves **price-time fairness** exactly as a real venue does; the resulting
  fills are realistic.
- The rule operates directly on the intrusive list (ADR 0012): keep = no-op on
  links; lose = unlink + append at tail.

**Negative / cost**
- Modify handling branches on (price changed?) and (size up vs down).
- T semantics are feed-dependent; documented assumption is "informational, no
  resting mutation."

## Alternatives considered

- **Treat every modify as in-place (always keep position).** Rejected: lets a
  trader inflate size while keeping their spot — breaks time priority and
  produces dishonestly favourable fills.
- **Treat every modify as cancel + re-add (always lose position).** Rejected:
  needlessly strips priority on benign size-*downs*, which real venues preserve —
  so it would understate fills for passive size reductions.

## Related

- ADR 0007 — the `Action` enum these map from.
- ADR 0012 — the FIFO list the rule manipulates.
- ADR 0017 — the fill model that depends on this queue behaviour.
