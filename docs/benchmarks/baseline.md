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

## After: mmap built + measured (Mode C)

`MmapDbnReader` (whole-file `MemoryMappedFile`) now frames directly over the mapped
file — Copy 1 removed at the source. Three-mode run, 20 passes, same file:

| Mode | ns/event (min\|med\|max) | throughput (typical) | note |
|---|---|---|---|
| **A — streaming e2e** | 38.0 \| 39.3 \| 42.5 | 25.5 M ev/s (~1.36 GB/s) | `DbnReader` (kernel read + framing + decode) |
| **C — mmap e2e** | 18.2 \| 18.8 \| 20.7 | **53.3 M ev/s (~2.85 GB/s)** | `MmapDbnReader` (no copy, no assembler) |
| **B — CPU floor** | 6.7 \| 6.9 \| 7.3 | 144 M ev/s (~7.7 GB/s) | framing + decode only |

```
A streaming e2e     = 39.3
C mmap e2e          = 18.8
B framing + decode  =  6.9   (CPU floor)
A - C (mmap win)    = 20.5   <- MEASURED
C - B (map residual)= 11.8   (page-fault / touch cost mmap cannot remove)
parity OK: stream and mmap agree on 3,520,518 events
```

**Outcome: mmap ~2.1× throughput** (25.5 → 53.3 M ev/s), and the mmap reader decodes
the **identical** event count as streaming (correctness cross-check, printed as
`parity OK`).

**The "ceiling vs measured" lesson, now empirical.** The predicted ceiling (`A − B`,
the whole copy bucket) was ~32 ns/event. The *measured* win (`A − C`) is **20.5** — mmap
removed the copy and the `ifstream` overhead but **not** the ~11.8 ns it still costs to
fault and touch the pages. Predicted ceiling > actual win, exactly as theory said: mmap
is a *ceiling on savings*, not a guarantee.

> Note on absolute numbers: streaming `A` here reads 39 ns/event vs 45 in the initial
> baseline above — run-to-run / thermal variance on the same machine. The **relative**
> claims (mmap ~2×, parity, the decomposition shape) are the robust, defensible ones;
> re-run before quoting absolutes.

## What this makes the next move

1. ~~Ring buffer for ingest~~ — **deprioritized by the data**, confirmed: even the full
   CPU floor (B) is 6.9 ns; the memmove is a slice of that. Not worth the wrap-around
   complexity for ingest. (Still the right tool for the order book, ADR 0010.)
2. ~~`mmap` byte source~~ — **done and measured: ~2.1× throughput, parity verified.**
3. **Observability** — replace silent skips with dropped/skipped-record counters.
4. **Fuzzing** — the framer (`frame_one`) is a textbook fuzz target.

_Re-run command:_
`build/Release/bench_dbn.exe data/MSFT/xnas-itch-20260511.mbo.dbn 20`
