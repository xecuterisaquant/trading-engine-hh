# ADR 0019 — Ingest benchmark methodology (two-mode decomposition)

- **Status:** Accepted
- **Date:** 2026-07-21
- **Component:** ingest tooling — `bench/bench_dbn.cpp`, `docs/benchmarks/`

## Context

Before this ADR, ingest performance was a single eyeballed number ("~4M events/sec")
from one `replay_dbn` run. That is not defensible: it bundles disk I/O, the kernel
copy, framing, and decode into one figure, so it cannot tell us *where* the time goes
— and therefore cannot justify or reject a specific optimization. We are about to
consider two: `mmap` (to remove the kernel→buffer copy) and a ring buffer (to remove
the assembler's compaction `memmove`). We need a harness that attributes cost to each
so the decision is evidence-driven, not a hunch (the standing "measure, don't feel"
bar).

## Decision

A dedicated `bench_dbn` tool measures the ingest hot path in **two modes** over the
same real data, and reports the decomposition:

- **Mode A — end-to-end, warm cache.** Runs the real `DbnReader` over the file after
  the OS page cache is warmed by a pre-read. Reads hit RAM, so the variable cost is
  our CPU work **plus the kernel→`scratch_` copy (Copy 1)**. This is what `mmap`
  would improve.
- **Mode B — isolated CPU.** Preloads the whole file into our own buffer once,
  *outside* the timed region, then drives `RecordAssembler` + `frame_one` +
  `to_event` over it in chunk-sized slices — the same shape as `DbnReader::next` but
  with **no per-pass kernel read**. This isolates framing + the compaction `memmove`
  + the decode. This is what a ring buffer would improve.

**The metric is throughput** (events/sec, MB/s) and **amortized ns/event**, reported
as the min/median/max across N passes for run-to-run stability. We deliberately do
**not** report per-record latency percentiles here: one `next()` is ~tens of ns and a
`steady_clock` read is ~20 ns, so per-call timing would measure the clock, not the
code. Per-record tail latency is the correct metric for the future *live* tick path
(records arriving one at a time), not for this batch reader — recorded here so the
distinction is deliberate, not forgotten.

The headline decomposition is `(A − B)` per event ≈ the kernel-copy cost = the
**ceiling** on what `mmap` can buy. Baseline numbers and interpretation live in
`docs/benchmarks/baseline.md`, committed so every future "we made it faster" is a
diffable before/after.

## Consequences

**Positive**
- Optimizations are now justified by an attributed number, not a guess. The first run
  already reshaped priorities (see baseline): the kernel copy dominates; the
  `memmove` a ring buffer would remove is a small slice of an already-small Mode B.
- A committed baseline is a regression guard: a future change that slows ingest shows
  up as a worse ns/event in the same tool.
- The chunk-size argument lets us sweep and find the throughput knee empirically
  rather than asserting a "cache-sized" buffer is fastest.

**Negative / cost**
- Hand-rolled (no Google Benchmark dependency): we compute our own warmup, iteration,
  and percentiles. Chosen to match the project's no-framework test style (ADR 0008)
  and add no dependency; the rigor is sufficient for a throughput decision. If we
  later need statistically robust microbenchmarks (noise modeling, auto iteration
  counts), adopting Google Benchmark is the documented upgrade path.
- Mode B mirrors `DbnReader::next`'s routing by duplicating one constant
  (`kMboRtype`) and the route/decode shape. This is intentional decoupling (a
  benchmark reaching into the reader's privates would couple to internals), at the
  cost of a small, commented duplication to keep in sync.

## Alternatives considered

- **Single end-to-end number only.** Rejected: cannot attribute cost, so cannot
  justify a specific optimization — the exact gap that motivated this ADR.
- **Google Benchmark.** Deferred, not rejected: more rigorous, but a new dependency,
  and throughput on a 3.5M-event file is stable enough hand-rolled (see baseline
  min/median/max spread).
- **Per-record latency percentiles for the batch reader.** Rejected as misleading
  here (clock overhead dominates a sub-100 ns operation); reserved for the live path.

## Related

- ADR 0008 — no-framework tests / exit codes: the same "no dependency, roll it
  ourselves where the rigor is sufficient" philosophy.
- ADR 0010 — the windowed ring buffer (order book): the ring-buffer technique whose
  value for *ingest* this benchmark is meant to test before we build it here.
