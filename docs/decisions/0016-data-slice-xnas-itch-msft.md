# ADR 0016 — Data slice: XNAS.ITCH MBO via DBN, single large-cap, one session

- **Status:** Accepted
- **Date:** 2026-06-27
- **Component:** data — `data/` (gitignored; see `data/README.md`)

## Context

The engine needs a concrete development data slice to build and test against. The
axes: which feed/encoding, which instrument(s), and how much.

## Decision

- **Feed/encoding:** NASDAQ **ITCH** market-by-order, delivered as **Databento
  DBN** (the `XNAS.ITCH` dataset). The parser (`WireMbo`, ADR 0001) targets DBN.
- **Instrument:** a single **liquid large-cap** (**MSFT**) first.
- **Size:** **one trading session** as the dev slice; scale to more
  names/sessions once the engine is proven.

## Consequences

**Positive**
- **One normalized schema.** DBN gives a single, well-documented record format
  instead of the raw exchange feed's quirks — one `WireMbo` to maintain.
- **Constant-penny tick** on a large-cap validates the ring book's
  constant-tick assumption (ADR 0010).
- **Deep, busy book** exercises every code path — many levels, large queues,
  heavy cancel flow — within a predictable depth.
- **Millions of events/day** is ample for differential testing (ADR 0013), yet a
  single session is small enough to **iterate fast and store locally** on
  non-institutional hardware.
- Practical: DBN is **accessible and affordable** for an independent/student
  developer.

**Negative / cost**
- One name / one session is **not representative** of all regimes (open/close,
  volatility, halts); broader validation is later work.
- The constant-penny assumption does not generalise to sub-penny or
  variable-tick instruments without revisiting ADR 0010.

## Alternatives considered

- **Raw ITCH (parse the exchange feed directly).** Rejected for now: venue-
  specific parsing and no cross-venue normalization, for no benefit at this stage.
- **A thin or penny-stock name first.** Rejected: low volume and data
  pathologies would obscure *engine* bugs — the goal is to debug the engine, not
  fight the data.
- **Pull a month up front.** Rejected: slower local iteration and premature
  before the engine is proven on one session.

## Related

- ADR 0001 — `WireMbo` / the DBN wire layer.
- ADR 0005 — integer nanodollars / tick handling.
- ADR 0010 — the ring book whose tick assumption this slice satisfies.
