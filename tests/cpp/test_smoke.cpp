// Trivial CTest smoke test for the PURE C++ core (no test framework dependency
// yet — a real framework like Catch2/GoogleTest gets added when there's logic
// worth testing). Returns a non-zero exit code on failure.
//
// NOTE: we intentionally do NOT use assert() here. assert is compiled out under
// NDEBUG (our Release config), so an assert-based test would silently always
// pass in Release. Explicit checks + return codes work in every build type.
#include <cstring>

#include "trading_sim/version.hpp"

int main() {
    const char* v = trading_sim::version();
    if (v == nullptr) return 1;
    if (std::strlen(v) == 0) return 2;
    return 0;
}
