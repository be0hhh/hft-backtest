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
switch it. Project-owned libraries remain static `.a`; `p` selects automatic
memory-aware parallelism, using available processors as an upper bound and
reserving 2 GiB per compilation job below `MemoryHigh`, with at least one job.
An explicit positive `-j N` overrides this selection. See `--help` for supported
options.


## CI

The `cpp` workflow overlays the triggering Backtest revision into the private
CXETCPP root checkout for direct-source family integration. The retired offline
CTest suite is not current acceptance evidence. The current registered owner
offline suites check quota, captured replay and canonical order evidence; their
local results are separate from CI. The badge is a workflow link, not evidence
of current passing CI.
The root checkout requires
the repository secret `CI_CXETCPP_SSH_KEY`; fork pull requests without that
secret fail explicitly before checkout and do not provide a passing CI result.

Run and sweep artifacts contain financial/simulation results without stage,
wall/CPU or depth-read profiling. Simulated execution latency, replay timestamps,
fills, equity, drawdown, progress and bounded sweep worker scheduling remain.
The public Backtest ABI is version 7; Recorder and the CLI check that numeric guard before
invoking the runtime. New `run.result` artifacts use schema 5; `fills.csv` adds
`context_index` after the existing columns. Recorder can display older schema 4
financial summaries read-only.

Binary corpus runs stream checked compressed blocks directly into bounded RAM.
An empty corpus source selection discovers eligible contexts over the recording
folder: single-source strategies run each source, canonical cross-venue
strategies group the same instrument, and explicit groups come from the strategy
configuration. Account balances and quotas are shared across its instruments.
The whole run requires one common exact captured quote asset; unknown or mixed
currencies are refused before strategy admission. Account totals use this same
unit without conversion. Missing canonical execution/reference inputs are refused.

Recorded monotonic arrival determines when data becomes visible to the strategy;
exchange timestamps remain market evidence. Source lifecycle boundaries suspend
affected contexts and preserve their orders and positions. Stream decode creates
no unpacked file. The preparation memory guard is not an RSS or throughput proof.

This adapter requires the recorded canonical executable quantity grid and
exact recorded market-quantity conversion authority for Crypto Spot or linear
Futures/Swaps. Unknown, inverse and traditional-finance market units,
nonunit price-basis accounting, transaction
actors and instrument-rule rebinding remain unsupported and fail before admission
or at the captured rule boundary. A source pause retains venue orders; real data
from a healthy sibling source can still fill its existing venue-active orders.

Whole-universe replay uses the binary corpus's exact quote identity. Configured
sources without market rows keep paused contexts while healthy sources proceed.
Legacy JSON archives remain structurally readable, but executable replay
requires the current binary quantity authority. Multiple legacy sessions also
lack authoritative common quote identity and refuse before balance aggregation.

Spot simulation owns shared quote cash and working-buy reservations per venue
account, and base inventory and working-sell reservations per captured source.
It refuses unfunded buys and naked sells. Fills debit or credit principal and
exact Core fees; linear Futures/Swaps retain the PnL wallet model. Local pending
reservations are immediately known, while venue ACK, fill, rejection and cancel
facts advance observed cash, inventory and terminal releases only at modeled
private delivery. Shared-account private delivery preserves venue event order
across sources even when latency samples tie; payload timestamps are unchanged.

Spot equity includes marked positive base inventory. During a source pause it
uses the last genuine bid received on each actual/observed plane as a historical
valuation; this cache supplies no quote readiness or fill liquidity. Service
unwind on Spot and linear Futures/Swaps requires a genuine opposite venue price and sufficient visible quantity;
missing liquidity retains exposure and refuses continuation. Aggregate wallet
results sum actual source cash contributions with each account seeded once.

Warmup command timestamps describe local sends. ACK timestamps and warmup
settlement become visible only on correlated private delivery. Service exposure
tracks only delivered tagged warmup/unwind fills, separately from ordinary
strategy inventory. PnL curves include actual per-source unrealized marks; final
aggregate snapshots sum quote-denominated PnL without a cross-symbol price.

The bounded canonical simulation evidence owner retains at most 1024 orders per
source, including terminals, and refuses exhaustion without eviction. These
source changes and fixtures are static-only until the authorized owner check
can compile past the current Core dependency blockers. They provide no CI,
performance, runtime or live-trading proof.
