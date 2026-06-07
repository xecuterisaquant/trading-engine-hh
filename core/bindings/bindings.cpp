// pybind11 bindings — the ONLY file in the project allowed to include Python.
//
// This module deliberately exposes a single dummy function (`ping`) for the
// skeleton. Its job is to prove three things end to end:
//   1. the C++ <-> Python bridge is wired and importable, and
//   2. the binding links against the pure-C++ core library
//      (it calls trading_sim::version()), and
//   3. the build/packaging toolchain (CMake + scikit-build-core) works.
#include <pybind11/pybind11.h>

#include <string>

#include "trading_sim/version.hpp"

namespace py = pybind11;

namespace {
std::string ping() {
    return std::string("pong from trading_sim core v") + trading_sim::version();
}
}  // namespace

// Module name MUST match the import name and the CMake target name (trading_sim).
PYBIND11_MODULE(trading_sim, m) {
    m.doc() = "trading_sim — C++ core bindings (skeleton)";
    m.def("ping", &ping, "Liveness check; returns a string produced by the C++ core.");
}
