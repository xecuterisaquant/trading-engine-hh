# trading-sim

A from-scratch trading-systems project: a **C++ limit-order-book matching engine
and market-data replay simulator**, with a **Python research layer** for strategy
development and validation, bridged by **pybind11**. Built for correctness and
**defensibility** — every component is designed to be explained and reasoned
about on a whiteboard, not just to run.

> **Status: v0.0.0 — skeleton.** Build system, C++↔Python binding, CI, and project
> structure are in place and green. Core components (order book, matching, fills,
> strategies) are not built yet — see the [Roadmap](#roadmap). The
> working agreement for how this project is developed is in `.claude/CLAUDE.md`.

## Vision

One keystone C++ core serves two stories, then forks:

```
                C++ CORE
   Limit Order Book + price-time matching engine
   + market-data replay + queue-aware fills
                        │
        ┌───────────────┴────────────────┐
        ▼                                 ▼
   HFT FORK                          SYSTEMATIC FORK
   latency + systems depth          research + validation on top
        │                                 │
        └───────────────┬────────────────┘
                        ▼
        Paper deployment + honest track record
```

## Design philosophy

- **Pure C++ core, isolated from Python.** The core is a standalone static
  library with *no* Python dependency, so it can be unit-tested and
  latency-benchmarked in isolation. The pybind11 module is a separate target that
  links it. (This boundary already exists in the skeleton.)
- **Realistic fills over convenient fills (planned).** Backtests will use
  queue-position-aware fill modeling driven by real market-by-order (MBO) data,
  not mid-price or parametric approximations.
- **No claim without a defense.** A component is "done" only when its design
  decisions can be explained end-to-end, cold.

## Architecture

```
trading-sim/
├── core/                 # C++ core (pure C++, no Python)
│   ├── include/          #   public headers (trading_sim/…)
│   ├── src/              #   implementation
│   └── bindings/         #   pybind11 ONLY — the C++↔Python boundary, isolated here
├── research/             # Python research layer
│   ├── strategies/       #   trading strategies
│   ├── signals/          #   signal / feature generation
│   └── backtest/         #   backtest harness that drives the core
├── tests/
│   ├── cpp/              #   CTest unit tests for the core
│   └── python/           #   pytest (imports the compiled binding)
├── data/                 # gitignored market data (see data/README.md to obtain)
├── docs/                 # design notes / ADRs
├── CMakeLists.txt        # single source of truth for the C++ build
├── pyproject.toml        # scikit-build-core drives CMake on `pip install`
└── .github/workflows/    # CI: build, ctest, pytest, clang-tidy, ruff, mypy
```

Build / dependency wiring:

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

**Requirements:** a C++23 toolchain, CMake ≥ 3.20 + Ninja, and Python ≥ 3.10
(developed on 3.14).

> **Windows/MSVC:** run every build from a Visual Studio developer shell
> (`vcvars64.bat`) so `cl`, `cmake`, and `ninja` are on `PATH`. The project is
> built and verified with MSVC 19.5x (VS Build Tools), which is ABI-matched to the
> MSVC-built CPython it binds against.

### C++ core + tests (standalone CMake)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Python package (builds the C++ core + binding via scikit-build-core)

```bash
pip install .
python -c "import trading_sim; print(trading_sim.ping())"
pytest tests/python
```

For an editable dev install (Python-side edits picked up immediately; re-run
after C++ changes to rebuild the extension):

```bash
pip install --no-build-isolation -Cbuild-dir=build/skbuild -e .
```

> Avoid scikit-build-core's `-Ceditable.rebuild=true` on Windows/MSVC: it shells
> out to `cmake` on *every import*, which fails unless every `python` invocation
> runs inside the VS developer shell. The plain editable install above imports
> from any shell.

### Lint + types

```bash
ruff check research tests
mypy research
```

## How the build is wired (the 30-second interview answer)

- **One CMake source of truth.** `CMakeLists.txt` defines a pure-C++ static lib
  (`trading_sim_core`), a pybind11 module (`trading_sim`) that links it, and a
  CTest target. `scikit-build-core` (via `pyproject.toml`) drives that *same*
  CMake for `pip install`, so there's no second build description to maintain.
- **Clean Python boundary.** Only `core/bindings/` includes Python; the core has
  no interpreter dependency and is tested on its own.
- **Portable optimizer flags.** Release uses `-O3 -march=native` on GCC/Clang and
  the MSVC analogues (`/O2 /arch:AVX2`), selected in CMake.

## Roadmap

| Phase | Component | Status |
|-------|-----------|--------|
| 0  | Repo skeleton, build system, CI, binding | ✅ done |
| 1  | Limit order book + matching engine | 🔲 planned |
| 1  | Market-data (MBO) replay | 🔲 planned |
| 1  | Queue-position-aware fill model | 🔲 planned |
| 2A | Latency benchmarking + optimization | 🔲 planned |
| 2B | Research framework (walk-forward, DSR) + a strategy | 🔲 planned |
| 3  | Paper deployment + track record | 🔲 planned |

## License

MIT — see [LICENSE](LICENSE).
