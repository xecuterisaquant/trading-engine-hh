# ADR 0021 — Modular ingest: one framing/routing core, per-source readers

- **Status:** Accepted
- **Date:** 2026-07-22
- **Component:** ingest — `core/include/trading_sim/dbn_reader.hpp`,
  `memory_mapped_file.hpp`

## Context

Ingest must eventually consume DBN from two structurally different kinds of source:

- **A seekable file** — fixed size, mappable whole into memory. The fast path is a
  memory map: the whole file is one contiguous region, so records never straddle a
  boundary and no copy into a user buffer is needed (mmap removes ~85% of per-event
  cost per ADR 0019).
- **A non-seekable stream** — e.g. a live socket. It cannot be mapped or seeked; it
  must be read in chunks, and a record split across two reads (the *straddle*) has to
  be stitched back together before it can be framed.

The naive design writes two monolithic readers, each re-implementing framing, routing,
decoding, and counting. That duplicates the hard, bug-prone logic (the framer, the
size guard, the routing policy) in two places that will inevitably drift.

## Decision

Layer ingest so that **only the source-specific step differs per reader**, and
everything else is shared, pure, and source-agnostic:

**Shared core (knows nothing about files or sockets):**
- `frame_one(span) -> FrameResult` — the per-record framing decision (Ok / NeedMore /
  Corrupt). Pure, no I/O, no state.
- `route_record(span) -> Routed` — route by rtype + decode one framed record. Pure.
- `tally(IngestStats&, Routed)` + `IngestStats` — counting/observability (ADR on
  observability).
- `kSupportedDbnVersion` / `kMboRtype`, `WireMbo` / `to_event` — the protocol/decode
  contract and version guard (ADR 0020), defined once.

**Per-source layer (the only part that differs):**
- **Streaming** — `FileByteSource` (a `read(span) -> size_t` byte pump) feeds a
  `RecordAssembler` (stitches straddles) inside `DbnReader`. The `read(span)->size_t`
  shape is deliberately generic: a socket is a drop-in replacement, and the assembler
  literally cannot tell a file from a socket.
- **File (mmap)** — `MemoryMappedFile` gives `MmapDbnReader` a whole-file span; with
  no chunk boundaries there is no `RecordAssembler`, no scratch buffer, and no
  dangling-view contract.

Both readers expose the **same contract** — `next() -> optional<Event>`, `version()`,
`failed()`, `stats()` — so a consumer is agnostic to which source produced the events.

We keep the readers as **two concrete classes** (ADR-fork "A") rather than unifying
them behind one interface, because the two access patterns genuinely differ
(chunked-pull vs whole-region) and forcing them together would be premature.

## Consequences

**Positive**
- **New source = one small class.** Adding a live socket means implementing a single
  `read(span) -> size_t`; framing, routing, decoding, the version guard, and the
  counters are reused unchanged. mmap is a file-only specialization that does not
  compromise the streaming path.
- **Policy lives in one place.** Framing, routing (ADR 0003), the version guard (ADR
  0020), and the corrupt-frame rule (ADR 0018) are defined once, so both readers agree
  *by construction* — proven by the parity test (mmap and streaming decode the same
  3,520,518 events and tally identical stats).
- **The hard logic is unit-tested once.** The straddle/framing bug surface lives in
  `frame_one` + `RecordAssembler`, tested with hand-built byte streams independent of
  any real source.

**Negative / cost**
- **Two reader classes**, with some surface duplication in the pull loop and the
  prologue parse (one reads through a source, the other indexes a span). We accept a
  little duplication to avoid a leaky abstraction.
- **No single unified reader type.** A consumer that wants to be source-generic at
  compile time would template over the reader or need a common base; deferred until a
  real requirement (a third source) justifies it.

## Alternatives considered

- **One monolithic reader with a `file`/`socket` mode flag.** Rejected: branches the
  hot path and couples file and socket concerns in one class — the opposite of
  modular.
- **A runtime-polymorphic `ByteSource` base with `virtual read()`, one reader.**
  Rejected *for mmap*: mmap does not fit a `read(dst) -> count` shape at all — it is
  whole-region, not fill-my-buffer. Forcing it behind that interface would reintroduce
  the very copy mmap exists to remove. (The virtual call itself is cheap — once per
  chunk, not per record — so this remains viable for *streaming* sources later.)
- **Templating the reader on the source type.** Deferred: premature unification; the
  two readers are small and clear as-is. Revisit if a third source shape appears.

## Related

- ADR 0001 — separate wire and event types (the decode contract reused by both).
- ADR 0003 — routing (rtype) vs validation split — the shared routing policy.
- ADR 0018 — corrupt-frame stop policy — shared, so both readers behave identically.
- ADR 0019 — benchmark: why the mmap file path exists (kernel-copy cost).
- ADR 0020 — supported-version guard — defined once, enforced by both readers.
