cmake_minimum_required(VERSION 3.20)

project(trading_sim
    VERSION 0.0.0
    DESCRIPTION "Low-latency trading simulator: C++ core + Python research layer"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++23, not -std=gnu++23

# Default to Release for single-config generators (Ninja/Make) if unset.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# compile_commands.json for clang-tidy / editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TRADING_SIM_WERROR       "Treat warnings as errors on first-party code" ON)
option(TRADING_SIM_BUILD_TESTS  "Build the C++ tests"                          ON)
option(TRADING_SIM_BUILD_PYTHON "Build the pybind11 module"                    ON)

# ---------------------------------------------------------------------------
# Portable optimization + warning flags (INTERFACE target)
#
# Attached ONLY to first-party targets (core lib, C++ tests) — never to the
# pybind11 module, whose translation unit pulls in third-party (pybind11/Python)
# headers that can legitimately warn and would break a -Werror build.
#
# -O3 / -march=native are GCC/Clang spellings; MSVC has no equivalent literal,
# so we map to its nearest analogues (/O2, /arch:AVX2). This keeps the optimizer
# request portable across the toolchains this project targets.
# ---------------------------------------------------------------------------
add_library(trading_sim_flags INTERFACE)

if(MSVC)
    target_compile_options(trading_sim_flags INTERFACE
        /W4
        $<$<CONFIG:Release>:/O2>          # MSVC max-speed; there is no /O3
        $<$<CONFIG:Release>:/arch:AVX2>   # closest portable analogue to -march=native
    )
    if(TRADING_SIM_WERROR)
        target_compile_options(trading_sim_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(trading_sim_flags INTERFACE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-march=native>
    )
    if(TRADING_SIM_WERROR)
        target_compile_options(trading_sim_flags INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library — PURE C++, zero Python dependency
# ---------------------------------------------------------------------------
add_library(trading_sim_core STATIC
    core/src/version.cpp
    core/src/memory_mapped_file.cpp   # platform mmap/MapViewOfFile behind an #ifdef
)
target_include_directories(trading_sim_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)
target_link_libraries(trading_sim_core PRIVATE trading_sim_flags)
# PIC so the static lib can be linked into the shared pybind11 module.
set_target_properties(trading_sim_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Python binding — pybind11 ONLY here
# ---------------------------------------------------------------------------
if(TRADING_SIM_BUILD_PYTHON)
    find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

    # Locate the pybind11 CMake config shipped with the active interpreter so a
    # plain `cmake ...` works the same way scikit-build-core does under the hood.
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
        OUTPUT_VARIABLE _pybind11_cmakedir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _pybind11_result)
    if(_pybind11_result EQUAL 0)
        list(APPEND CMAKE_PREFIX_PATH "${_pybind11_cmakedir}")
    endif()
    find_package(pybind11 CONFIG REQUIRED)

    # Target name == module name == import name (trading_sim).
    pybind11_add_module(trading_sim core/bindings/bindings.cpp)
    target_link_libraries(trading_sim PRIVATE trading_sim_core)

    # Install rule lets scikit-build-core pack the .pyd into the wheel at the
    # top level, so `import trading_sim` works after `pip install .`.
    install(TARGETS trading_sim LIBRARY DESTINATION .)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TRADING_SIM_BUILD_TESTS)
    enable_testing()
    add_executable(cpp_smoke_test tests/cpp/test_smoke.cpp)
    target_link_libraries(cpp_smoke_test PRIVATE trading_sim_core trading_sim_flags)
    add_test(NAME cpp_smoke_test COMMAND cpp_smoke_test)

    # event.hpp is header-only; linking trading_sim_core provides the include
    # path and compiles its static_asserts (sizeof==48 etc.) into a real TU.
    add_executable(cpp_event_test tests/cpp/test_event.cpp)
    target_link_libraries(cpp_event_test PRIVATE trading_sim_core trading_sim_flags)
    add_test(NAME cpp_event_test COMMAND cpp_event_test)

    # dbn.hpp is header-only too; this TU compiles its wire-layout static_asserts
    # (sizeof==56, every offsetof==N, little-endian host) and round-trips a record.
    add_executable(cpp_dbn_test tests/cpp/test_dbn.cpp)
    target_link_libraries(cpp_dbn_test PRIVATE trading_sim_core trading_sim_flags)
    add_test(NAME cpp_dbn_test COMMAND cpp_dbn_test)

    # dbn_reader.hpp is header-only; this TU exercises frame_one's framing decision
    # (Ok / NeedMore / Corrupt) against hand-built buffers, including the straddle.
    add_executable(cpp_dbn_reader_test tests/cpp/test_dbn_reader.cpp)
    target_link_libraries(cpp_dbn_reader_test PRIVATE trading_sim_core trading_sim_flags)
    add_test(NAME cpp_dbn_reader_test COMMAND cpp_dbn_reader_test)

    # MemoryMappedFile: RAII whole-file mmap (POSIX) / MapViewOfFile (Windows). The
    # test maps real temp files, so it links the core lib for the .cpp platform impl.
    add_executable(cpp_memory_mapped_file_test tests/cpp/test_memory_mapped_file.cpp)
    target_link_libraries(cpp_memory_mapped_file_test PRIVATE trading_sim_core trading_sim_flags)
    add_test(NAME cpp_memory_mapped_file_test COMMAND cpp_memory_mapped_file_test)
endif()

# ---------------------------------------------------------------------------
# Examples / tools
# ---------------------------------------------------------------------------
# replay_dbn: end-to-end driver over DbnReader — streams a real .dbn file and
# reports version, first event, event count, and clean/corrupt end. Not a test
# (it takes a data-file argument); build it to run the ingest stack on real data.
add_executable(replay_dbn examples/replay_dbn.cpp)
target_link_libraries(replay_dbn PRIVATE trading_sim_core trading_sim_flags)

# bench_dbn: throughput benchmark for the ingest hot path. Two modes (end-to-end
# warm-cache vs isolated in-memory framing+decode) whose delta bounds what mmap and
# a ring buffer could each buy us. Build in Release for meaningful numbers; results
# and methodology live in docs/benchmarks/ (ADR 0019). Not a test (takes a data arg).
add_executable(bench_dbn bench/bench_dbn.cpp)
target_link_libraries(bench_dbn PRIVATE trading_sim_core trading_sim_flags)
