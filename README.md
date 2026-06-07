# trading-sim

A low-latency trading simulator: a **C++ core** (order book, matching, fills —
to be built) with a **Python research layer** (signals, strategies, backtests),
bridged by **pybind11**.

> Portfolio project built to be *defended in interviews* — every design choice
> is intended to be explainable cold. See `.claude/CLAUDE.md` for the working
> agreement.

## Status

Skeleton only — build plumbing in place, no trading logic yet.

## Architecture

<!-- PLACEHOLDER: replace with your own diagram. Starter view of the layout: -->

```
trading-sim/
├── core/                 # C++ core (pure C++, no Python)
│   ├── include/          #   public headers (trading_sim/…)
│   ├── src/              #   implementation
│   └── bindings/         #   pybind11 ONLY — the C++ <-> Python boundary
├── research/             # Python research layer
│   ├── strategies/       #   trading strategies
│   ├── signals/          #   signal / feature generation
│   └── backtest/         #   backtest harness
├── tests/
│   ├── cpp/              #   CTest
│   └── python/           #   pytest (imports the compiled binding)
├── data/                 # gitignored market data (see data/README.md)
└── docs/                 # design notes / ADRs
```

```
            ┌─────────────────────────────┐
            │   research/  (Python)       │
            │   signals · strategies ·    │
            │   backtest                  │
            └─────────────┬───────────────┘
                          │  import trading_sim
            ┌─────────────▼───────────────┐
            │   core/bindings (pybind11)  │   ← only place Python meets C++
            └─────────────┬───────────────┘
                          │  links
            ┌─────────────▼───────────────┐
            │   trading_sim_core (C++)    │   ← pure C++, unit-tested alone
            │   order book · matching ·   │
            │   fills  (TODO)             │
            └─────────────────────────────┘
```

## Build

Requires a C++23 compiler, CMake ≥ 3.20, and Python ≥ 3.10.

### C++ core + tests (standalone CMake)

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Python binding

```
pip install .            # builds the C++ extension via scikit-build-core
python -c "import trading_sim; print(trading_sim.ping())"
pytest tests/python
```

On Windows with MSVC, run the above from a *Developer* shell (or after
`vcvars64.bat`) so `cl`, `cmake`, and `ninja` are on PATH.

## How the build is wired (the 30-second interview answer)

- **One CMake source of truth.** `CMakeLists.txt` defines a pure-C++ static lib
  (`trading_sim_core`), a pybind11 module (`trading_sim`) that links it, and a
  CTest target. `scikit-build-core` (via `pyproject.toml`) drives that *same*
  CMake for `pip install`, so there's no second build description to maintain.
- **Clean Python boundary.** Only `core/bindings/` includes Python; the core
  has no interpreter dependency and is tested on its own.
- **Portable optimizer flags.** Release uses `-O3 -march=native` on GCC/Clang
  and the MSVC analogues (`/O2 /arch:AVX2`), selected in CMake.

## License

MIT — see [LICENSE](LICENSE).
