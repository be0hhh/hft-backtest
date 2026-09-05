# HFT Backtest

[![cpp](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml/badge.svg?branch=main)](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml?query=branch%3Amain)

HFT Backtest is the deterministic replay/simulation consumer of canonical
CXET events, recorder corpus contracts and trading-core semantics. It builds
only as part of the CXET direct-source root graph.

## Build

From this directory:

```bash
./compile.sh --force all low p
```

`all` builds the shared backtest library, CLI and every Backtest-owned test
executable. `--force` rebuilds the selected target closure from a clean shared
root `build/`; `p` enables all available processors. Backtest has no runtime
metrics instrumentation, so the common `low` profile leaves it uninstrumented.
The wrapper does not run tests. Run the built Backtest suite with:

```bash
ctest --test-dir ../../build -L hft-backtest --output-on-failure
```

## CI

The `cpp` workflow overlays the triggering Backtest revision into the private
CXETCPP root checkout, builds the selected `hft-backtest` target closure and
runs the offline CTest suite labeled `hft-backtest`. The root checkout requires
the repository secret `CI_CXETCPP_SSH_KEY`; fork pull requests without that
secret fail explicitly before checkout and do not provide a passing CI result.
