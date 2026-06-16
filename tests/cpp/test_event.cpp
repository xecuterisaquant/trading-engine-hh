// CTest for the typed Event representation (event.hpp). Same conventions as
// test_smoke.cpp: no framework, no assert() (compiled out under NDEBUG), each
// failure returns a distinct exit code so a red CTest names the broken check.
//
// The layout guarantees (sizeof == 48, trivially copyable, no padding between
// fields) are static_asserts inside event.hpp itself — *including this header
// compiles them*, so this TU is what makes those checks actually run in CI.
#include <cstring>
#include <initializer_list>  // range-for over {'A','C',...}; MSVC does not provide it transitively

#include "trading_sim/event.hpp"

namespace ts = trading_sim;

int main() {
    // --- boundary validation accepts exactly the wire alphabet -------------
    for (char c : {'A', 'C', 'M', 'T', 'F', 'R'}) {
        if (!ts::is_valid_action(c)) return 1;
    }
    for (char c : {'B', 'A', 'N'}) {
        if (!ts::is_valid_side(c)) return 2;
    }
    // ...and rejects bytes outside it (lowercase, NUL, arbitrary garbage).
    for (char c : {'a', 'c', 'Z', 'X', '\0', ' ', '1'}) {
        if (ts::is_valid_action(c)) return 3;
    }
    for (char c : {'b', 'S', 'Z', '\0'}) {
        if (ts::is_valid_side(c)) return 4;
    }

    // --- the zero-cost cast preserves the wire byte exactly ----------------
    // (char-valued enum: the enumerator IS the byte; no remapping anywhere)
    if (static_cast<char>(ts::Action::Add) != 'A') return 5;
    if (static_cast<char>(ts::Action::Clear) != 'R') return 6;
    if (static_cast<char>(ts::Side::Bid) != 'B') return 7;
    char raw = 'M';
    if (static_cast<ts::Action>(raw) != ts::Action::Modify) return 8;

    // --- memcpy round-trip: the property the parser fast path relies on ----
    ts::Event e{};
    e.ts_event = 1'748'955'600'000'000'000ULL;  // realistic ns-since-epoch
    e.order_id = 123'456'789ULL;
    e.price    = 500'000'000'000LL;             // $500.00 in nanodollars
    e.size     = 100;
    e.sequence = 42;
    e.action   = ts::Action::Add;
    e.side     = ts::Side::Bid;

    unsigned char buf[sizeof(ts::Event)];
    std::memcpy(buf, &e, sizeof e);
    ts::Event back{};
    std::memcpy(&back, buf, sizeof back);
    if (back.price != e.price || back.order_id != e.order_id) return 9;
    if (back.action != ts::Action::Add || back.side != ts::Side::Bid) return 10;

    return 0;
}
