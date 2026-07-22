# ADR 0020 — Reject unvalidated DBN versions at construction

- **Status:** Accepted
- **Date:** 2026-07-21
- **Component:** ingest — `core/include/trading_sim/dbn_reader.hpp`

## Context

The DBN prologue carries a one-byte format `version`. `DbnReader` read it into
`version_` and exposed it via `version()`, but **did nothing with it** — it decoded
every stream through the same `WireMbo` layout regardless of version.

That layout is the DBN **v1** MBO record: every field offset is pinned by `offsetof`
static_asserts in `dbn.hpp`. Databento has published v2 and v3, and record layouts
can move fields between versions. Feeding a v2/v3 file to the old reader would
`memcpy` bytes into the v1 offsets and emit **structurally valid but semantically
wrong Events** — wrong price, wrong size, wrong order_id — with **no crash and no
error**. That is the silent-corruption failure this project treats as the worst kind:
a backtest would run on garbage and report believable-looking, false results.

We have only ever validated the v1 layout (it is the format of our XNAS-ITCH MSFT
slice, ADR 0016). We have not verified v2/v3 field offsets against real data.

## Decision

`skip_prologue_` **enforces** the version immediately after reading it, before
skipping the metadata or framing a single record:

```
if (version_ != kSupportedDbnVersion)   // kSupportedDbnVersion == 1
    throw std::runtime_error("unsupported DBN version ...");
```

- **Reject, don't reinterpret.** We refuse any version whose byte layout we have not
  validated, rather than decode it through offsets we cannot vouch for.
- **Throw, don't flag.** A wrong-version file is unusable from byte one, so we fail
  fast at construction — consistent with the existing bad-magic throw two lines above,
  and unlike a malformed *record* (routine, mid-stream) which returns a value / skips.
- **`kSupportedDbnVersion` is a single named constant.** Widening support is a
  deliberate act: bump it only *after* adding and validating the new layout, never
  before.

## Consequences

**Positive**
- Closes a real correctness hole: a non-v1 file now fails loudly instead of producing
  silent garbage Events.
- The guard runs before any record is framed, so a wrong version can never reach the
  `memcpy` in `next()`.
- Rejects nothing we actually use — our data slice is v1 (ADR 0016).

**Negative / cost**
- We cannot read v2/v3 files at all until their layouts are validated. That is the
  intended trade: correctness over reach. When we need a newer version, the work is
  explicit — add the layout, its static_asserts, and a version→layout branch, then
  widen the guard.
- A single supported version means the check is an equality, not a range. If we later
  support several, this becomes a set/range membership test; the ADR is the marker for
  that change.

## Alternatives considered

- **Keep exposing `version()` but do nothing (status quo).** Rejected: the getter
  gave false comfort — "we read the header" — while the value was never used to
  protect decoding.
- **Best-effort decode with a warning.** Rejected: emitting Events we know may be
  wrong, even with a log line, invites exactly the silent-bad-data outcome; downstream
  a warning is easily missed while the garbage flows on.
- **A queryable `failed()` flag instead of a throw** (as for corrupt frames, ADR
  0018). Rejected here: a corrupt frame is discovered mid-stream after valid output
  exists (a flag preserves that output); an unsupported version is known at
  construction with zero valid output possible, so failing fast is cleaner and forces
  the caller to handle it.

## Related

- ADR 0001 — separate wire and event types: the `WireMbo` layout this guard protects.
- ADR 0016 — the XNAS-ITCH MSFT (DBN v1) data slice this validates against.
- ADR 0018 — corrupt-frame policy: the other ingest failure mode, handled with a
  flag rather than a throw, for the reasons contrasted above.
