# Ingest benchmark — baseline

Methodology: **ADR 0019**. Tool: `bench/bench_dbn.cpp` (`bench_dbn <file> [passes]
[chunk_bytes]`). Build **Release** (`/O2 /arch:AVX2`) for these numbers.

## Setup

| | |
|---|---|
| Data | `data/MSFT/xnas-itch-20260511.mbo.dbn` (XNAS-ITCH MBO, DBN v1) |
| Size | 188.0 MB |
| Events | 3,520,518 MBO records |
| Machine | Windows 11, MSVC Release, `/O2 /arch:AVX2`, C++23 |
| Passes | 20 (warm page cache; 2 untimed warmup passes discarded) |
| Default chunk | 65,536 B |

Numbers are **amortized ns per event**, reported min / median / max across passes.
Re-run on your machine before trusting absolutes — the *shape* (the decomposition) is
what generalizes, not the exact nanoseconds.

## Result (chunk = 64 KiB, 20 passes)

| Mode | ns/event (min\|med\|max) | throughput (typical) | what it measures |
|---|---|---|---|
| **A — end-to-end (warm cache)** | 43.8 \| 45.0 \| 47.1 | **22.2 M ev/s (~1.19 GB/s)** | real `DbnReader`: kernel read + framing + memmove + decode |
| **B — isolated CPU (in-RAM)** | 6.7 \| 6.9 \| 7.4 | **145 M ev/s (~7.7 GB/s)** | framing + compaction memmove + decode only |

**Decomposition (median ns/event):**

```
A end-to-end        = 45.0
B framing + decode  =  6.9
A − B (kernel copy) = 38.1   <- ceiling on what mmap can remove
```

## Headline finding — this reshaped the optimization plan

**~85% of ingest time (38 of 45 ns/event) is the file-read path (Copy 1: the
kernel→`scratch_` copy through `std::ifstream`), NOT framing, the memmove, or the
decode.** The entire framing + compaction-memmove + decode pipeline is 6.9 ns/event.

We had two candidate optimizations. The data ranks them decisively:

- **`mmap` (removes the kernel copy)** targets the **38 ns** bucket → the real lever.
  Realistic ceiling: ~85% of per-event cost. Mapping the page cache into our address
  space lets `frame_one` read the file bytes directly, deleting Copy 1 (and the
  `std::ifstream` read-path overhead, which likely explains why Mode A moves only
  ~1.2 GB/s — far below RAM bandwidth — so mmap may beat even the naive copy cost).
- **Ring buffer (removes the compaction `memmove`)** targets a *fraction* of the
  **6.9 ns** Mode B bucket. Even eliminating it entirely could not move end-to-end
  throughput more than a rounding error. **For ingest throughput, it is not worth
  the wrap-around complexity** (a ring buffer makes a straddling record
  non-contiguous, so `next()` can no longer hand out a single span — real cost, ~zero
  benefit *here*).

> The ring-buffer technique still earns its place in the **order book** (ADR 0010),
> where the access pattern and reuse are different. This benchmark only says it is the
> wrong tool for the *ingest* hot path — which is exactly the kind of claim that
> should rest on a measurement, not intuition.

## Chunk-size sweep (12 passes each) — testing the "fit in cache" hypothesis

| chunk | A e2e ns/event (med) | B in-mem ns/event (med) |
|---|---|---|
| 4 KiB | 45.7 | 7.0 |
| 16 KiB | 45.9 | 7.0 |
| 64 KiB | 45.4 | 6.8 |
| 256 KiB | 45.7 | 6.9 |
| 1 MiB | 48.4 | 8.6 |
| 4 MiB | 49.9 | 9.7 |

**Reading it:**
- **4 KiB → 256 KiB is essentially flat.** There is no single "fastest" chunk; there
  is a broad optimal *plateau*. The syscall-amortization argument for a bigger buffer
  saturates almost immediately here (the page cache read is cheap and `ifstream`
  buffers internally), so 64 KiB is a fine, defensible default — it sits in the middle
  of the flat region.
- **1 MiB and 4 MiB get measurably *worse*** (most visibly in Mode B: 6.8 → 9.7
  ns/event). This is the **producer-consumer cache-locality** effect: a chunk larger
  than L2 means that by the time framing walks to the end of the just-appended chunk,
  its front has been evicted, so the framer reloads it from L3/RAM. Bigger is not
  better — past the cache-resident point it regresses.
- So the earlier intuition ("size the buffer to fit in cache") is *half* right: there
  is an upper bound where exceeding cache hurts — but the win is a broad plateau, not
  a knife-edge L1 fit, and the dominant cost (the 38 ns read path) is independent of
  chunk size entirely.

## What this makes the next move

1. ~~Ring buffer for ingest~~ — **deprioritized by the data.** Revisit only if a
   profile ever shows the memmove mattering (it doesn't at 56-byte records).
2. **`mmap` byte source** — the evidence-backed win. Design fork to decide together
   (author): cross-platform now (`CreateFileMapping`/`MapViewOfFile` on Windows vs
   `mmap` on POSIX) vs POSIX-first; and lifetime/ownership of the mapping. Benchmark
   the same two modes after, and this table becomes the before/after.

_Re-run command:_
`build/Release/bench_dbn.exe data/MSFT/xnas-itch-20260511.mbo.dbn 20`
