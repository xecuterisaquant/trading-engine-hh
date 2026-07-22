// Randomized ("fuzz") test for the framer and the assembler drain loop.
//
// frame_one and the reader's drain loop consume ARBITRARY bytes, so they must never
// read out of bounds, contradict their own contract, or (the nightmare) loop forever.
// The hand-written cases in test_dbn_reader.cpp cover the paths we THOUGHT of; this
// throws hundreds of thousands of random buffers to shake out the ones we didn't.
//
// It is DETERMINISTIC: a fixed PRNG seed, printed on failure, so any discovered
// counter-example reproduces exactly (reproducibility is the whole point of a
// regression fuzzer vs a one-shot random test). Conventions (ADR 0008): no framework,
// distinct non-zero exit codes. A coverage-guided libFuzzer harness (clang-only) lives
// in fuzz/; this portable version runs in ctest on every platform (ADR 0022).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

#include "trading_sim/dbn_reader.hpp"

namespace ts = trading_sim;

namespace {

// Assert frame_one's result is internally consistent for whatever bytes went in.
// Returns 0 on success, or a distinct code naming the violated invariant.
int check_frame_one(std::span<const std::byte> buf) {
    const ts::FrameResult r = ts::frame_one(buf);
    switch (r.status) {
        case ts::FrameStatus::Ok: {
            // A whole record is present: size is word-aligned, in [4, 255*4], equals
            // length*4, and NEVER exceeds the buffer (else it must be NeedMore).
            if (r.record_size < 4)          return 1;
            if (r.record_size % 4 != 0)     return 2;
            if (r.record_size > buf.size()) return 3;
            const auto len = std::to_integer<std::uint8_t>(buf[0]);
            if (r.record_size != static_cast<std::size_t>(len) * 4) return 4;
            break;
        }
        case ts::FrameStatus::NeedMore:
            // NeedMore is legal ONLY when fewer than 2 bytes are present, or the
            // framed size exceeds what we have. If a nonzero size fit, it should be Ok.
            if (buf.size() >= 2) {
                const auto len = std::to_integer<std::uint8_t>(buf[0]);
                const std::size_t sz = static_cast<std::size_t>(len) * 4;
                if (sz != 0 && sz <= buf.size()) return 5;
            }
            break;
        case ts::FrameStatus::Corrupt:
            // Corrupt is legal ONLY for a zero-length record (length byte == 0), which
            // requires the length byte to be present.
            if (buf.empty())                                 return 6;
            if (std::to_integer<std::uint8_t>(buf[0]) != 0)  return 7;
            break;
    }
    return 0;
}

}  // namespace

int main() {
    // Fixed seed: reproducible. If this test ever fails in CI, the same seed replays
    // the exact byte stream locally.
    std::mt19937_64 rng(0xC0FFEEULL);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    // --- Part 1: frame_one never contradicts its own contract on random input ---
    {
        std::uniform_int_distribution<std::size_t> len_dist(0, 600);
        std::vector<std::byte> buf;  // reused to avoid per-iter allocation churn
        for (int iter = 0; iter < 150'000; ++iter) {
            buf.resize(len_dist(rng));
            for (auto& b : buf) {
                b = static_cast<std::byte>(byte_dist(rng));
            }
            if (const int code = check_frame_one(buf); code != 0) {
                std::fprintf(stderr,
                             "frame_one invariant %d violated at iter %d (n=%zu)\n",
                             code, iter, buf.size());
                return code;  // 1..7
            }
        }
    }

    // --- Part 2: the assembler drain loop ALWAYS TERMINATES on random bytes ------
    // The Corrupt-on-length-0 guard under fire: feed a random chunk and drain it. The
    // loop MUST reach NeedMore/Corrupt in a bounded number of steps (each Record eats
    // >= 4 bytes), never spin, never read out of bounds. Blowing the cap == a hang.
    {
        std::uniform_int_distribution<std::size_t> size_dist(0, 4096);
        for (int trial = 0; trial < 5'000; ++trial) {
            std::vector<std::byte> bytes(size_dist(rng));
            for (auto& b : bytes) {
                b = static_cast<std::byte>(byte_dist(rng));
            }

            ts::RecordAssembler assembler;
            assembler.append(bytes);

            const std::size_t cap = bytes.size() + 16;  // >=4 bytes/record + slack
            std::size_t steps = 0;
            for (;;) {
                const ts::Pull p = assembler.next();
                if (p.status != ts::PullStatus::Record) {
                    break;  // NeedMore or Corrupt: cleanly done
                }
                (void)ts::route_record(p.record);  // exercise routing on random bytes
                if (++steps > cap) {
                    std::fprintf(stderr,
                                 "drain loop did not terminate (trial %d, n=%zu)\n",
                                 trial, bytes.size());
                    return 20;  // hang bug
                }
            }
        }
    }

    return 0;
}
