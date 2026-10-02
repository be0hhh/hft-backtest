# HFT Backtest

[![cpp](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml/badge.svg?branch=main)](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml?query=branch%3Amain)

HFT Backtest is the deterministic replay/simulation consumer of canonical
CXET events, recorder corpus contracts and trading-core semantics. It builds
only as part of the CXET direct-source root graph.

## Build

From this directory in the canonical CXET checkout:

```bash
./compile.sh p
./compile.sh --force p
./compile.sh all p
./compile.sh --force all portable p
```

The default builds only this repository's production targets and requires ready
prerequisites. `--force` cleans and rebuilds the exact required production
closure. `all` builds and runs only this repository's registered tests and their
support libraries; it does not build production daemons or benchmarks. With
`--force all`, production is rebuilt first, followed by the local tests. If no
local tests are registered, the command reports that and succeeds; an empty
test registry is not passing test proof.

Libraries are static `.a` archives. Release uses `-O3`, host-native CPU targeting
and full LTO; `portable` selects portable CPU targeting. Use `p` or `-j N` for
parallelism and `--help` for the supported options.


## CI

The `cpp` workflow overlays the triggering Backtest revision into the private
CXETCPP root checkout for direct-source family integration. The retired offline
CTest suite is not current acceptance evidence; new meaningful checks are
deferred. The badge is a workflow link, not evidence of current passing CI.
The root checkout requires
the repository secret `CI_CXETCPP_SSH_KEY`; fork pull requests without that
secret fail explicitly before checkout and do not provide a passing CI result.
