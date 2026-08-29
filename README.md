# HFT Backtest

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
