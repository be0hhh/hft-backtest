# HFT Backtest

[![cpp](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml/badge.svg?branch=main)](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml?query=branch%3Amain)

HFT Backtest is the deterministic replay/simulation consumer of canonical
CXET events, recorder corpus contracts and trading-core semantics. It builds
only as part of the CXET direct-source root graph.

## Build

From this directory in the canonical CXET checkout:

```bash
./compile.sh p
./compile.sh all p
./compile.sh all portable p
```

The default incrementally builds this repository's production targets and their
full required dependency closure, including missing or stale foreign providers.
Ready targets are reused. `all` builds and runs only this repository's registered
tests and their required production dependencies; it does not select unrelated
production daemons, benchmarks or another repository's tests. If no
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
