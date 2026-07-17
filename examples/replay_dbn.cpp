// replay_dbn — a tiny driver over DbnReader: point it at a plain .dbn file and it
// streams the whole thing through the ingest stack, reporting the DBN version, the
// first decoded Event, the total MBO event count, and whether the stream ended
// cleanly or on a corrupt frame.
//
// This is the end-to-end proof that the ingest units compose on REAL data (not the
// hand-built byte streams the unit tests use). It doubles as an inspection tool.
//
//   replay_dbn <path/to/file.dbn>
//
// (Feed it a *decompressed* .dbn; zstd handling is a later layer.)
#include <cstdint>
#include <cstdio>
#include <exception>

#include "trading_sim/dbn_reader.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.dbn>\n", argv[0]);
        return 2;
    }

    try {
        trading_sim::DbnReader reader(argv[1]);
        std::printf("DBN version : %u\n", reader.version());

        std::size_t count = 0;
        for (auto e = reader.next(); e.has_value(); e = reader.next()) {
            if (count == 0) {  // report the first decoded event verbatim
                std::printf(
                    "first event : action=%c side=%c order_id=%llu price=%lld "
                    "size=%u seq=%u ts_event=%llu\n",
                    static_cast<char>(e->action), static_cast<char>(e->side),
                    static_cast<unsigned long long>(e->order_id),
                    static_cast<long long>(e->price), e->size, e->sequence,
                    static_cast<unsigned long long>(e->ts_event));
            }
            ++count;
        }

        std::printf("total events: %zu\n", count);
        std::printf("ended        : %s\n",
                    reader.failed() ? "CORRUPT (stopped on bad frame)"
                                    : "clean EOF");
        return reader.failed() ? 1 : 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "replay_dbn error: %s\n", ex.what());
        return 1;
    }
}
