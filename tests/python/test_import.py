"""Trivial pytest: the compiled pybind11 binding imports and its dummy works.

This is the Python-side counterpart to tests/cpp/test_smoke.cpp. It proves the
C++ extension built and installed correctly and that the C++<->Python bridge is
live.
"""


def test_import_and_ping() -> None:
    import trading_sim  # the compiled C++ extension module

    result = trading_sim.ping()
    assert isinstance(result, str)
    assert "pong" in result
