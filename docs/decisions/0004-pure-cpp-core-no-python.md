# ADR 0004 — Pure C++ core, zero Python dependency

- **Status:** Accepted
- **Date:** 2026-06-27 (Phase 0)
- **Component:** build — `CMakeLists.txt`, `core/`, `core/bindings/`

## Context

The project is a C++ core plus a Python research layer bridged by pybind11. Python
could be allowed to leak into the core, or the core could be kept strictly
interpreter-free with Python confined to one boundary file.

## Decision

`core/include/` and `core/src/` are **pure C++ with no Python dependency**. The
*only* file allowed to include pybind11/Python is `core/bindings/bindings.cpp`.
Enforced structurally by CMake targets: `trading_sim_core` (static lib, no Python)
is linked by the separate `trading_sim` pybind module.

## Consequences

**Positive**
- The core can be **unit-tested and latency-benchmarked with no interpreter in
  the loop** — measurements reflect the engine, not Python.
- The core **builds without a Python dev environment** at all.
- Clear, single C++↔Python seam.

**Negative / cost**
- One extra build target and a boundary rule to maintain.

## Alternatives considered

- **Binding logic inside the core library.** Rejected: it forces a `libpython`
  link dependency onto *every* build, CI job, and benchmark harness — even ones
  that never touch Python — and any hot-path code touching a pybind object
  inherits the **GIL**, a latency and correctness hazard. The clean boundary means
  the book never knows the interpreter exists.

## Related

- `docs/interview-cheatsheet.md` — the boundary one-liner.
