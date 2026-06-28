# ADR 0012 — Intrusive FIFO list per level + `order_id`→node hash map

- **Status:** Accepted — design (not yet implemented; author-written core)
- **Date:** 2026-06-27
- **Component:** order book — `core/include/trading_sim/book.hpp` (planned)

## Context

Each price level (one ring slot, ADR 0010) holds not one order but a **queue** of
orders. Price-time priority means: better price first (the ring), and within a
price, **earlier arrival first**. So a level is a FIFO queue — **head = oldest**
(fills first), new orders **append at the tail**.

The workload fact that drives the design: in real MBO data **cancels vastly
outnumber trades**, so "remove an arbitrary order from the middle of a level's
queue" is a hottest-path operation and must be **O(1)** — and a cancel arrives
carrying only an `order_id`, not a pointer.

## Decision

Two structures, working together:

- **Per-level intrusive doubly-linked list.** The `prev`/`next` links live
  *inside* the `Order` object (intrusive), and orders live in a **pre-allocated
  pool**. Head is oldest, tail is newest; unlink is O(1) pointer surgery.
- **`order_id → node` hash map.** Turns the id on a cancel/modify into the node
  address in O(1).

A cancel is then: map lookup (O(1)) → intrusive unlink (O(1)). The order object
is allocated **once** and has **two views**: a node in its level's list (its
queue position) and the value the map points to (findable by id).

## Consequences

**Positive**
- **O(1) cancel/modify** by `order_id` — the dominant operation.
- **No per-node heap allocation** (pool + intrusive links); the order's data and
  its links share a cache line, so walking a queue stays cache-warm.
- Clean separation of duties: the **list owns ordering** (time priority), the
  **map owns lookup**.

**Negative / cost**
- Two structures to keep mutually consistent (every add/cancel touches both).
- Pool sizing must bound the live-order count.

## Alternatives considered

- **`std::vector<Order>` per level.** Rejected: middle-removal (a cancel) is
  O(n) shift — disqualifying given cancel frequency.
- **`std::list<Order>`.** Rejected: a separate heap allocation per order,
  scattered across memory (pointer-chasing, allocator overhead per add *and*
  cancel) — the intrusive list keeps O(1) unlink without the allocation.
- **Hash map only.** Rejected: a map has **no order**, so it cannot represent the
  FIFO queue or answer "who is first in line" — i.e. cannot encode time priority.
- **List only.** Rejected: finding an order by `order_id` would be an O(n) scan.

The map and list are complementary halves of one O(1) cancel — not a trade-off
where one is sacrificed for the other.

## Related

- ADR 0010 / 0011 — the rings whose slots hold these lists.
- ADR 0014 — the modify rule, which manipulates queue position in this list.
- ADR 0017 — queue-aware fills track a simulated order's position in this list.
