// Coverage-guided libFuzzer harness for the framer + assembler drain loop.
//
// Clang-only: build with -fsanitize=fuzzer (address,undefined) via the CMake option
// TRADING_SIM_FUZZER=ON. It is NOT part of the default build — the portable,
// every-platform regression fuzzer is tests/cpp/test_frame_fuzz.cpp (see ADR 0022).
//
// libFuzzer drives this with mutated inputs while ASan/UBSan watch for out-of-bounds
// reads or UB, and its timeout catches a hang. Same target surface as the portable
// test, but coverage-guided so it explores framing paths hand-written cases miss.
//
// Run (clang):
//   cmake -S . -B build-fuzz -DTRADING_SIM_FUZZER=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target frame_fuzz
//   ./build-fuzz/frame_fuzz -max_total_time=60
#include <cstddef>
#include <cstdint>
#include <span>

#include "trading_sim/dbn_reader.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(data), size);

    // Frame + route every record the mutator produces. Any OOB/UB trips the sanitizer;
    // a non-terminating drain trips libFuzzer's timeout.
    trading_sim::RecordAssembler assembler;
    assembler.append(bytes);
    for (;;) {
        const trading_sim::Pull p = assembler.next();
        if (p.status != trading_sim::PullStatus::Record) {
            break;
        }
        (void)trading_sim::route_record(p.record);
    }
    return 0;
}
