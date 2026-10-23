# HFT Backtest

[![cpp](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml/badge.svg?branch=main)](https://github.com/be0hhh/hft-backtest/actions/workflows/cpp.yml?query=branch%3Amain)

HFT Backtest is the deterministic replay/simulation consumer of canonical
CXET events, recorder corpus contracts and trading-core semantics. It builds
only as part of the CXET direct-source root graph.

## Autonomous research

The product includes `hft-backtestd` and `hft-backtest-tui`. The daemon owns
experiments, independent strategy-variant portfolios and persistent analytics;
neither a terminal nor Vetron is needed while experiments run. The terminal is
a client: closing it never stops recording or experiments.

Recorder publishes compressed blocks only after a durable, producer-complete
capture cut. Backtest normally obtains those bytes through the Recorder local
client and decodes into bounded RAM. An evicted block is read from the same
compressed archive. Protocol or CRC failures are errors, not fallback signals.
Original receive monotonic/UTC stamps and event identity determine simulation
ordering; disk speed and UI polling never supply the simulation clock.

Start already-built programs explicitly (these are examples, not verification
results):

```bash
build/hft-backtestd --root /absolute/private/research --socket /absolute/private/research/control.sock --build-id BUILD --module-id MODULE
build/hft-backtest-tui --research-socket /absolute/private/research/control.sock --recorder-runtime /absolute/private/recorder
```

The roots must be real owner-private directories. Build/module identity must
describe the deployed implementation; clients obtain the admitted values from
the service catalog. Saved experiments retain their original provenance.
Profiles and exact decimal parameter ranges expand to a complete bounded grid;
every variant has its own portfolio and the same explicit scenario seed.
Account currencies, fees, host budget and valuation requirements are explicit.
Unknown instrument or collateral facts refuse new risk. Unsupported financial
representations, including missing currency valuation, are never synthesized.

Enabled experiments restore automatically. Recovery replays the saved input
from the original start into a new immutable attempt generation, then switches
the published result manifest after catching the prior visible cut. Existing
generations are not appended with duplicate replay rows. Analytics include
qualified financial rows, executions, interval returns and distributions, with
input position and account/currency provenance. Data-only C++ modules use the
same scheduling and storage, beginning with `midprice_return_distribution`.

Capture loss stops affected experiments as incomplete. User pause retains state
and catches up from the archive on resume; a native risk stop is terminal for
that variant. Stop does not invent liquidation fills or discard open inventory.
RAM, source/context, variant and disk admission remain bounded. Data is not
automatically deleted to make a quota fit.

Saved specifications consume bounded catalog metadata. Running and retained
paused experiments reserve simulation resources; completed experiments release
those resources while their analytics remain readable. Saved jobs from another
build/module remain available through `Describe` and result queries, with
execution disabled until their exact implementation is deployed.

The daemon's cooperative work quantum is selected with
`--work-items-per-step` (1–64). `--max-state-bytes`,
`--max-resident-variants`, `--max-output-bytes` and `--max-experiments` bound
the service. One cooperative owner advances admitted experiments. Saved profiles
and expanded variant labels can be inspected through `Describe` (`:describe`
in the TUI) after reconnecting. An explicit clone rebuilds the complete grid
in a separate draft.

New C++ studies register a `DataStudyDescriptor` with declared input channels,
parameters, outputs, a state-size bound and an owned prepare/observe/destroy
binding. They do not receive an order or execution API.

Vetron's Research section uses the same producer-owned local clients. Its
optional `--recorder-service-runtime-dir` and `--backtest-research-socket`
arguments attach to existing services without launching them. Live trading
authority remains separate.

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
The public Backtest ABI is version 8; Recorder and the CLI use the same numeric
guard. Finite `run.result` artifacts retain schema 5 with JSONL streams including
`fills.jsonl`, `equity.jsonl` and `order_lifetimes.jsonl`. Continuous research
uses its separately guarded, immutable attempt/segment schema. Recorder can
display older financial summaries read-only.

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

This adapter requires a recorded canonical executable quantity grid, exact
quantity-conversion authority, typed economic identity and the descriptor's
reference/financial inputs. Admission follows those properties rather than a
venue-name allowlist. Unsupported inverse units, nonunit price-basis accounting,
descriptor capabilities or rule changes fail before admission or at their
captured boundary. A source pause retains venue orders; real data
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

Depth execution consumes finite recorded liquidity across orders until the
affected price level is refreshed; unrelated updates do not replenish consumed
levels. Recorded trades consume existing depth; trade-through makes better
levels unavailable. Finite execution uses authoritative depth without cached
BBO overlays. An accepted empty depth book remains finite. Resting maker
orders fill only from eligible recorded trades, sharing
each trade's finite quantity across orders. Partial-fill private reports carry
the cumulative executed quantity and weighted average execution price.

BBO execution is explicitly optimistic: without execution depth, including
`BboNoDepth` and depth-failure fallback, eligible ordinary Market, marketable
Limit and triggered Stop orders fill their full remaining quantity at the best
price regardless of displayed quantity. It does not model
market impact or finite top liquidity. Actual BBO fills are counted and produce
a result warning; a declared-depth failure retains its separate diagnostic even
if no fallback fill occurs. Depth execution remains quantity-limited.

Spot equity includes marked positive base inventory. During a source pause it
uses the last genuine bid received on each actual/observed plane as a historical
valuation; this cache supplies no quote readiness or fill liquidity. Service
unwind on Spot and linear Futures/Swaps retains its stricter requirement for a
genuine opposite venue price and sufficient visible quantity; missing liquidity
retains exposure and refuses continuation. Aggregate wallet
results sum actual source cash contributions with each account seeded once.

Warmup command timestamps describe local sends. ACK timestamps and warmup
settlement become visible only on correlated private delivery. Service exposure
tracks only delivered tagged warmup/unwind fills, separately from ordinary
strategy inventory. PnL curves include actual per-source unrealized marks; final
aggregate snapshots sum quote-denominated PnL without a cross-symbol price.
Drawdown starts at the initial zero-PnL baseline, including a loss in the first
observation. Portfolio metrics synchronize legs by timestamp using each leg's
latest observation at that timestamp, with zero before its first observation.

The bounded canonical simulation evidence owner retains at most 1024 orders per
source. Continuous sessions reclaim terminal records only after final private
delivery, actor consumption and release of every borrowed reference; exhaustion
refuses further admission. Implementation and fixture evidence is static-only.
Builds and tests have not been run for these changes; no performance, runtime
or live-trading result is established.
