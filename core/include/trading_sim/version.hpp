#pragma once

// Public C++ API surface for the trading_sim core.
//
// DESIGN RULE: this header (and everything under core/include + core/src) is
// PURE C++ with no Python/pybind11 dependency. The only place Python is allowed
// to appear is core/bindings/. Keeping that boundary clean means the core can be
// unit-tested, benchmarked, and reused with zero interpreter in the loop.
namespace trading_sim {

// Semantic version string of the core library. Stub for the skeleton.
[[nodiscard]] const char* version() noexcept;

}  // namespace trading_sim
