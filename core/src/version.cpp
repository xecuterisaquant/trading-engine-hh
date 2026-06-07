#include "trading_sim/version.hpp"

// Trivial stub so the core library target has a translation unit to compile and
// link. Real components land here later (order book, matching, etc.).
namespace trading_sim {

const char* version() noexcept { return "0.0.0"; }

}  // namespace trading_sim
