# HFT Backtest

[![cpp](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml/badge.svg?branch=main)](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml?query=branch%3Amain)

HFT Backtest is the deterministic replay/simulation consumer of canonical
CXET events, recorder corpus contracts and trading-core semantics. It builds
only as part of the CXET direct-source root graph.

## Build

From this directory in the canonical CXET checkout:

```bash
./compile.sh p
./compile.sh --force portable p
./compile.sh all p
./compile.sh all portable p
```

The default builds this owner's product incrementally with pinned Clang, GNU
Make, Release `-O3`, native CPU targeting and LTO OFF. `portable` disables CPU
targeting. Fresh foreign providers are reused silently; missing or stale foreign
modules are listed in one combined prompt before rebuilding them. Decline or EOF
cancels; a noninteractive invocation with stale providers exits with an explanatory
error. `--force` builds the selected product and necessary closure incrementally
with FULL LTO, without cleaning or prompting; ThinLTO is never selected.

`all` builds and runs only this owner's registered tests and needed dependencies.
`--force all` first builds the optimized product, then local tests. Unrelated
products, benchmarks and other owners' tests are not selected. An empty test
registry is reported explicitly and does not establish passing test proof.

Optimized trees use `build` (native) or `build/modes/portable`; development trees
use `build/modes/dev-native` or `build/modes/dev-portable`. `CXET_BUILD_DIR` is the
exact caller-supplied path; an incompatible existing profile is rejected. Only a
successful product build updates `build/.compile-active/<owner>.json`; default
launchers resolve that selected tree. Failed, UI-only and test-only runs do not
switch it. Project-owned libraries remain static `.a`; `p` selects available
processors and `-j N` overrides it. See `--help` for supported options.


## CI

The `cpp` workflow overlays the triggering Backtest revision into the private
CXETCPP root checkout for direct-source family integration. The retired offline
CTest suite is not current acceptance evidence; new meaningful checks are
deferred. The badge is a workflow link, not evidence of current passing CI.
The root checkout requires
the repository secret `CI_CXETCPP_SSH_KEY`; fork pull requests without that
secret fail explicitly before checkout and do not provide a passing CI result.

Run and sweep artifacts contain financial/simulation results without stage,
wall/CPU or depth-read profiling. Simulated execution latency, replay timestamps,
fills, equity, drawdown, progress and bounded sweep worker scheduling remain.
The public Backtest ABI is version 4; Recorder checks that same numeric guard
before invoking the runtime. Financial JSON schemas remain `run.result` schema 4
and `sweep.result.v1` schema 1: removing optional profiling metadata leaves their
financial payload unchanged, and existing financial artifacts remain readable.
The cleanup has static-only acceptance; build, tests and GUI execution are unrun.
