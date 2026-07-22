// bench_dbn — a throughput benchmark for the DBN ingest hot path.
//
// It exists to turn "feels fast" into a NUMBER we can defend and regress against.
// Every future ingest optimization (mmap to kill the kernel copy, a ring buffer to
// kill the compaction memmove) is justified or rejected by the deltas this prints.
//
//   bench_dbn <file.dbn> [passes=20] [chunk_bytes=65536]
//
// (Feed it a *decompressed* .dbn — zstd is a later layer, and we do not want the
//  decompressor in the timed region anyway.)
//
// ----------------------------------------------------------------------------
// Why TWO modes (the methodology; see docs/benchmarks and ADR 0019)
// ----------------------------------------------------------------------------
// A batch file reader's cost splits into pieces our two planned optimizations
// attack separately, so we measure them separately:
//
//   Mode A  END-TO-END, warm cache. Runs the real DbnReader over the file after
//           the OS page cache is warmed. Because reads hit RAM (not the SSD), what
//           varies is our CPU work PLUS the kernel->scratch_ copy (Copy 1). This is
//           the number mmap would improve, because mmap removes exactly Copy 1.
//
//   Mode B  ISOLATED CPU. Preloads the whole file into our own RAM buffer ONCE
//           (outside the timed region), then drives RecordAssembler + frame_one +
//           to_event over it in chunk-sized slices — the same shape as
//           DbnReader::next, but with NO per-pass kernel read. This isolates the
//           framing + the compaction memmove (Copy 2 + the erase) + the decode
//           (Copy 3). This is the domain a ring buffer would improve.
//
// The decomposition is the payoff: (A per-event ns) - (B per-event ns) is roughly
// the per-event cost of the kernel copy = the CEILING on what mmap can buy us. B's
// own cost is where the ring buffer plays. We decide with these two numbers, not a
// hunch. See docs/benchmarks/baseline.md for the recorded baseline.
//
// A note on the metric: we report THROUGHPUT (events/sec, MB/s) and amortized
// ns/event, plus the pass-to-pass distribution (min/median/max) for run stability.
// We deliberately do NOT report per-record latency percentiles: a single next() is
// ~100 ns, and a steady_clock read is ~20 ns, so per-call timing would measure the
// clock, not the code. Per-record tail latency is the right metric for the future
// LIVE tick path (records arriving one at a time), not for this batch reader.
#include <algorithm>   // std::sort, std::min
#include <chrono>      // steady_clock — the timing source
#include <cstddef>     // std::byte, std::size_t
#include <cstdint>
#include <cstdio>
#include <cstring>     // std::memcpy
#include <exception>
#include <fstream>     // slurp the file into RAM for Mode B
#include <span>
#include <string>
#include <vector>

#include "trading_sim/dbn.hpp"         // WireMbo, to_event
#include "trading_sim/dbn_reader.hpp"  // DbnReader, RecordAssembler, frame_one

namespace {

using Clock = std::chrono::steady_clock;
using trading_sim::WireMbo;

// Mirrors DbnReader's private routing constant. Duplicated (not exposed) on purpose:
// the reader's kMboRtype is an implementation detail, and a benchmark that reaches
// into privates would couple to internals it should not. If dbn.hpp ever names this
// publicly, switch to it.
constexpr std::uint8_t kMboRtype = 0xA0;

// Read an entire file into a byte vector. Used BOTH to warm the page cache for Mode
// A and to provide the in-RAM source for Mode B. Throws on open failure.
std::vector<std::byte> slurp(const char* path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error(std::string("bench_dbn: cannot open ") + path);
    }
    const std::streamsize n = in.tellg();
    in.seekg(0);
    std::vector<std::byte> buf(static_cast<std::size_t>(n));
    in.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

// Compute the offset of the first record: skip the 8-byte DBN header + the metadata
// whose length the header states. Mirrors DbnReader::skip_prologue_ but over an
// in-memory buffer (Mode B has no byte source to read through).
std::size_t record_start(std::span<const std::byte> file) {
    if (file.size() < 8 || file[0] != std::byte{'D'} || file[1] != std::byte{'B'} ||
        file[2] != std::byte{'N'}) {
        throw std::runtime_error("bench_dbn: not a DBN stream (bad magic)");
    }
    std::uint32_t metadata_len = 0;
    std::memcpy(&metadata_len, file.data() + 4, sizeof(metadata_len));
    return std::size_t{8} + metadata_len;
}

struct Stats {
    std::size_t events = 0;
    std::size_t record_bytes = 0;         // payload bytes framed (excludes prologue)
    std::vector<double> pass_ns_per_event; // one entry per measured pass
};

// Summarize per-pass ns/event into min/median/max and print a labeled block.
void report(const char* label, const Stats& s) {
    std::vector<double> v = s.pass_ns_per_event;
    std::sort(v.begin(), v.end());
    const double best = v.front();
    const double med = v[v.size() / 2];
    const double worst = v.back();

    // Throughput derived from the BEST pass (peak) and the MEDIAN pass (typical).
    const double peak_evs = 1e9 / best;
    const double typ_evs = 1e9 / med;
    const double typ_mbps =
        (static_cast<double>(s.record_bytes) / (med * 1e-9 * static_cast<double>(s.events))) /
        (1024.0 * 1024.0);

    std::printf("  %-10s events=%zu  ns/event[min|med|max]= %6.1f | %6.1f | %6.1f\n",
                label, s.events, best, med, worst);
    std::printf("             peak= %6.2f M ev/s   typical= %6.2f M ev/s   ~%6.0f MB/s\n",
                peak_evs / 1e6, typ_evs / 1e6, typ_mbps);
}

// ---- Mode A: end-to-end DbnReader over the (warm) file ----------------------
Stats bench_end_to_end(const char* path, std::size_t chunk, int passes) {
    Stats s;
    for (int p = 0; p < passes; ++p) {
        trading_sim::DbnReader reader(path, chunk);
        std::size_t events = 0;
        const auto t0 = Clock::now();
        for (auto e = reader.next(); e.has_value(); e = reader.next()) {
            ++events;
        }
        const auto t1 = Clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
        s.events = events;
        s.pass_ns_per_event.push_back(ns / static_cast<double>(events));
    }
    return s;
}

// ---- Mode B: framing + decode over an in-RAM buffer (no per-pass kernel read)
// Drives the exact units DbnReader::next drives (RecordAssembler + frame_one +
// to_event), fed in chunk-sized slices to reproduce the compaction memmove, but
// with the file already in our address space so Copy 1 is out of the timed region.
Stats bench_in_memory(std::span<const std::byte> file, std::size_t chunk,
                      int passes) {
    Stats s;
    const std::size_t start = record_start(file);
    const std::span<const std::byte> records = file.subspan(start);

    for (int p = 0; p < passes; ++p) {
        trading_sim::RecordAssembler assembler;
        std::size_t events = 0;
        std::size_t rbytes = 0;
        std::size_t offset = 0;

        const auto t0 = Clock::now();
        for (;;) {
            const trading_sim::Pull pull = assembler.next();
            if (pull.status == trading_sim::PullStatus::Record) {
                const auto rtype = std::to_integer<std::uint8_t>(pull.record[1]);
                if (rtype == kMboRtype && pull.record.size() == sizeof(WireMbo)) {
                    WireMbo w;
                    std::memcpy(&w, pull.record.data(), sizeof(WireMbo));
                    if (trading_sim::to_event(w)) {
                        ++events;
                    }
                }
                rbytes += pull.record.size();
                continue;
            }
            if (pull.status == trading_sim::PullStatus::Corrupt) {
                break;
            }
            // NeedMore: feed the next slice, or stop when the buffer is drained.
            if (offset >= records.size()) {
                break;
            }
            const std::size_t n = std::min(chunk, records.size() - offset);
            assembler.append(records.subspan(offset, n));
            offset += n;
        }
        const auto t1 = Clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
        s.events = events;
        s.record_bytes = rbytes;
        s.pass_ns_per_event.push_back(ns / static_cast<double>(events));
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.dbn> [passes=20] [chunk_bytes=65536]\n",
                     argv[0]);
        return 2;
    }
    const char* path = argv[1];
    const int passes = (argc > 2) ? std::atoi(argv[2]) : 20;
    const std::size_t chunk =
        (argc > 3) ? static_cast<std::size_t>(std::atoll(argv[3]))
                   : std::size_t{64} * 1024;

    if (passes < 1) {
        std::fprintf(stderr, "bench_dbn: passes must be >= 1\n");
        return 2;
    }

    try {
        // Warm the OS page cache and provide Mode B's in-RAM source in one read.
        const std::vector<std::byte> file = slurp(path);
        // Record byte count for Mode A's MB/s (Mode B computes its own).
        std::printf("bench_dbn: file=%s  size=%.1f MB  chunk=%zu B  passes=%d\n",
                    path, static_cast<double>(file.size()) / (1024.0 * 1024.0),
                    chunk, passes);

        // A couple of untimed warmup passes settle CPU caches / branch predictors.
        (void)bench_end_to_end(path, chunk, 2);
        (void)bench_in_memory(file, chunk, 2);

        const Stats a = bench_end_to_end(path, chunk, passes);
        Stats b = bench_in_memory(file, chunk, passes);
        // Mode A does not track record_bytes; borrow Mode B's (same records) so its
        // MB/s line is populated too.
        Stats a_full = a;
        a_full.record_bytes = b.record_bytes;

        std::printf("\n");
        report("A e2e", a_full);
        report("B in-mem", b);

        // The decomposition: A - B per-event is roughly the kernel-copy cost, i.e.
        // the ceiling on what mmap can remove. B is where a ring buffer would play.
        std::vector<double> va = a.pass_ns_per_event, vb = b.pass_ns_per_event;
        std::sort(va.begin(), va.end());
        std::sort(vb.begin(), vb.end());
        const double a_med = va[va.size() / 2];
        const double b_med = vb[vb.size() / 2];
        std::printf("\n  decomposition (median ns/event):\n");
        std::printf("    A end-to-end        = %6.1f\n", a_med);
        std::printf("    B framing+decode    = %6.1f\n", b_med);
        std::printf("    A - B (kernel copy) = %6.1f  <- mmap ceiling\n",
                    a_med - b_med);
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bench_dbn error: %s\n", ex.what());
        return 1;
    }
}
