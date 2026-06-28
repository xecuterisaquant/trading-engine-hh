# ADR 0017 — Queue-position-aware fill model (design principles)

- **Status:** Accepted — design principles (implementation deferred; author-written core)
- **Date:** 2026-06-27
- **Component:** fill model / backtest — `core/include/trading_sim/` (planned)

## Context

This is the project's keystone differentiator. Most backtests use a **parametric**
fill: "price touched my limit → I'm filled," or "I get X% of volume at my price."
That assumes you were at the **front** of the queue (sometimes the *only* order
there) and filled instantly — a lie about what happened to *your specific* order,
which was really somewhere mid-queue and fills only after the orders ahead of it.

Market-**by-order** data is what makes an honest model possible: every add/cancel/
fill carries an `order_id`, so the exact queue is reconstructable (ADR 0010–0012).
You are not *estimating* queue position; you *track* it from ground truth.

## Decision (principles; mechanics deferred to build time)

- **Placement:** a simulated order is dropped at the **back** of its level's FIFO
  at submission (it arrived last). Pessimistic and honest.
- **Tracking:** maintain "quantity ahead of me," decremented as F/C events consume
  orders **ahead** of the simulated order (and per the modify rule, ADR 0014, on
  those orders). Because MBO identifies each order, consumption ahead vs behind is
  **known**, not guessed.
- **Fill:** the simulated order fills only **after** the quantity ahead is
  exhausted; incoming trade volume then reaches it.
- **Market impact:** modelled as **zero — passive-only**. The simulated order was
  never in the historical data, so no participant reacted to it; its true impact
  is unknowable without every participant's strategy and latency.
- **Limitations are stated, not hidden:** zero-impact and back-of-queue are
  declared as the model's boundaries.

## Consequences

**Positive**
- **Honest fills from ground truth** — driven by the real queue, not an
  assumption.
- **Captures adverse selection** for free: you fill when real volume arrives, and
  real volume correlates with informed flow moving against you — a parametric
  model misses this.
- **Conservative by construction:** back-of-queue + zero-impact **under**-counts
  fills, so results are a *floor*, not a hope — the credible direction for a track
  record meant to be believed.

**Negative / cost**
- **No counterfactual:** cannot model the simulated order's own impact or others'
  reactions — stated as a limitation.
- Back-of-queue may understate fills where better placement was achievable.
- Implementation mechanics (data structures, replay integration) are not yet
  designed.

## Alternatives considered

- **Parametric / price-touch fills.** Rejected: assumes front-of-queue, dishonest
  about who actually fills.
- **Pro-rata / cancel-from-back estimation.** Rejected: only needed for
  *aggregated* (L2) data where queue position is unknown; MBO removes the guess.
- **Modelling own market impact.** Rejected as infeasible: requires knowing every
  participant's strategy, submission and execution latency — impossible from a
  historical replay.

## Related

- ADR 0012 — the FIFO list whose positions are tracked.
- ADR 0014 — the modify rule applied to the queue-ahead count.
- ADR 0015 — receive-time gating that keeps the simulation honest.
- ADR 0016 — the MBO data slice that makes exact tracking possible.
