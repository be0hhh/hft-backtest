# Market delivery replay: current contract

Status: implemented source contract, static-only evidence.

The detailed cross-product contract lives in
[`BacktestEngineContract.md`](../../hft-recorder/docs/BacktestEngineContract.md).
This document records the backtest-owned rules.

## One market-delivery mode

There is no exchange-time compatibility mode and no synthetic market-data
delivery mode. A backtest accepts only current captured-arrival corpus data.
Old recorder sessions fail preflight instead of selecting a weaker clock.

For every live row the loader retains:

- exchange/source event time;
- parser application-boundary `CLOCK_REALTIME`;
- parser application-boundary `CLOCK_MONOTONIC`;
- producer, source generation, session, frame, shard and ordinal identity.

Historical REST/archive rows are warmup seeds only. They never enter the live
delivery timeline.

## Binary folder replay

The strategy-delivery coordinate is the captured monotonic delta projected from
the corpus anchor pair. Strategy-visible state changes and market-driven
`runCycle` happen only at this coordinate.

The binary folder consumer uses captured arrival for both venue updates and
strategy visibility. Exchange timestamps remain exact payload evidence; a later
frame cannot be applied early based on its exchange timestamp. One Recorder
cursor retains one catalog and bounded shard blocks, and Backtest stages only
the current complete native transaction. Legacy JSON-session replay retains its
existing exchange projection and is a separate input consumer.

Before a captured transaction, the universe scheduler drains the globally
earliest pending action, user event or canonical timer strictly before arrival.
It applies every depth part before invoking each subscribed context once, then
drains internal actions at the same coordinate. Equal arrival records retain
their durable shard/frame/ordinal order.

Order-submit, cancel and user-data latency remains a separate deterministic
execution model. It is never added to market-data delivery.

## Atomic application delivery

All logical parts of one committed depth transaction share one captured arrival
and one transaction frame identity. Backtest applies the snapshot and every
replayed delta to the strategy-visible book before one `runCycle`. Venue-state
updates retain their individual exchange timestamps.

Records with equal captured timestamps are ordered by durable producer/shard
identity. No stream-priority tie-break is invented.

A caught-up equal-sequence BBO freshness record is application-plane liveness
evidence, not a new venue state: it refreshes captured receive clocks without
creating a fill or an immediate strategy cycle. If the selected interval starts
with a coalesced freshness record, its complete payload installs the initial
BBO state. A freshness flag whose payload changes retained state is corrupt.
Application-only freshness contributes to delivery ranges, but not to venue
schedule ranges or venue-adjustment counters.

## Fail-closed input policy

Before strategy initialization, loading fails when any required condition is
missing:

- current corpus/ABI/schema contract;
- exact application-frame arrival evidence;
- required strategy channel and at least one row in the selected interval;
- exact trader-replay compatibility;
- complete depth transaction chunks;
- source-generation continuity;
- absence of an intersecting capture or source-sequence gap.

Clock anomalies that still retain exact rows are preserved as flags and result
warnings. An unavailable arrival clock is a capture gap and makes the selected
interval ineligible.

## Result evidence

The current `run.result` labels the model as:

- `arrival_boundary = hft-parser.application-frame-ready`;
- `market_data_delivery = captured_application_frame_arrival`;
- `venue_schedule = captured_arrival_no_future_lookahead` for binary folders.

It also carries captured clock ranges, replay delivery ranges, venue ranges,
anomaly counters and adjusted-venue-event counts. The final watermark is the
greatest replay/execution coordinate actually processed, not the last exchange
timestamp found in input.

Build, tests, runtime capture and live exchange behavior require separate proof.


## Whole universe and virtual accounts

An empty binary source selection discovers config-eligible sources in the
folder's selected common capture interval. `SingleSource` creates one context
per source; `SameCanonicalCrossVenue` groups equal captured canonical IDs with
distinct venues; `ExplicitGroup` consumes complete configured portfolios only.
The descriptor's leg bound applies to each context. A context borrows permanent
source owners, including when configured groups overlap.

Common quote currency is validated again from the cursor's effective
`sourcesAtBegin` metadata after interval hydration and before account creation.
An initially common catalog cannot authorize aggregation when a preceding
lifecycle transition changed one selected source's actual quote currency.

One configured exchange/market venue is one virtual account. Its initial wallet
is seeded once, every source fill updates that wallet once, and all sources on
that account debit one permit ledger. Conflicting explicit source balances on
one account fail preflight. Context-local IDs retain an exact simulated
producer correlation; native client-order text supplies no strategy ID.
Result fill rows append `context_index`, and lifetime joins use source/context/ID.
The result and `rate_limits.venues` manifest entries report the schedules
actually installed on those accounts. Aggregate `enabled` means at least one
account enforces quotas; `strict_rejects` is false when every installed policy
is disabled. Each venue records its own effective enablement and strict mode,
including explicit request overlays and the global disable switch.


Cold source, cursor, book and context storage is checked against a 3 GiB
preparation envelope. This static preparation calculation is separate from RSS,
throughput and whole-run output-growth evidence. A canonical actor requiring
unavailable typed reference inputs or execution evidence fails preflight.

Capture exhaustion preserves managed orders and positions without a fabricated
teardown fill, cancellation or flatten. Actual producer `SourceLifecycle`
administrative records use their captured clocks, masks, directory revisions,
source generation and native session epoch. They suspend only contexts whose
required lanes disappeared. Unaffected native lanes retain their state. A real
add/readd clears the affected market view and waits for fresh captured data;
strategy state, orders, pending actions and positions survive. Interval preflight
returns exact effective source metadata and membership strictly before the
selected beginning, so replay uses that initial state without prefix events. Old simulated
execution actions remain fenced across the observed membership boundary even
when the producer's logical source generation is unchanged. No lifecycle row is
represented as a market frame or an extra strategy market ping.

Captured `Stale`, `Degraded` and source `SequenceGap` flags, and an exact
native Depth `State/Gap` payload, are observed health evidence. They produce a
typed pause on the affected lane without installing its unhealthy prices or
calling an extra market cycle. Other contexts continue. The next healthy
compatible record clears the affected view and installs only its actual native
payload; old simulated execution remains fenced by an application-owned
monotonic boundary epoch. Producer directory revisions and source generations
are retained unchanged. An interval containing only exact health observations
can remain paused throughout and emit no strategy actions.

Unexplained capture-loss ledger entries, `GapBoundary` records and corrupt
market rows still refuse preflight. Intentional source membership and observed
health records are distinct from unknown missing capture data. Source freshness
continues to gate new actions through the canonical strategy policy.


## Simulation capability and quantity authority

Strategy preparation consumes the producer's separately flagged
`canonicalStepSizeRaw` and `executionQuantityStepRaw`, both already in canonical
base E8. Native lot size and economic contract base quantity remain evidence;
they never manufacture an execution grid. The canonical derived-grid check is
mandatory on initial and lifecycle metadata. Legacy JSON requires explicit
`canonical_step_size_e8` and `execution_quantity_step_e8` metadata. Missing
quantity authority refuses admission.

The current accounting adapter accepts unit native price basis only. Captured
market quantities admit Crypto Spot and linear Futures/Swaps with an exact
producer-owned quantity conversion for every selected replay lane. Base,
contract and lot lanes first scale native quantities to E8 and use the public
Core exact rational projection with recorded package multipliers and native
contract/lot base factors. Already-canonical Spot quantities use Base 1/1 and
are never packaged twice. Native corpus quantities remain unchanged. Unknown
selected units, missing factors, inverse/quote quantities, nonexact division
and overflow refuse replay. Traditional-finance units remain unsupported.
Effective interval-start metadata supplies the same conversion facts as real
in-interval lifecycle rows; a changed conversion refuses rebind instead of
reinterpreting retained state.
A changed instrument grid or tick requires a
canonical strategy-state rebind; without that boundary the current replay
refuses the transition rather than resetting strategy state.

The simulation owns a fixed 1024-record canonical order store and context-scoped
identity table per captured source. Each has at most 32 lookup/admission probes;
terminal records remain retained and capacity/correlation exhaustion refuses
further replay actions. Unresolved records also respect the 256-order snapshot
capacity. Command IDs have disjoint source ranges and remain
separate from strategy-local IDs. The fresh empty virtual venue starts from its
actual empty model cut. Local submission and send precede delayed venue ACK,
fill or cancel evidence; canonical observation occurs only when the exact
private event reaches its source delivery head. Service feedback updates that
store while remaining outside strategy user-event cycles.

Venue position, wallet and financial results retain the actual fill time.
Strategy order, position and shared-wallet views expose only local intent/send
facts and consumed private evidence. Pending quotes stay pending before private
ACK and remain visible after a venue terminal until its private event arrives.
Exact cumulative fill deltas, prices, fees and realized amounts advance the
observed position and account only at delivery; another symbol on the shared
account sees the same observed wallet. Private source heads drain while their
strategy contexts are paused or waiting for market readiness, without invoking
those strategies. Terminal evidence remains available when market data recovers.

Transaction-emitting descriptors still require transaction execution and
reconciliation inputs beyond these order views. They explicitly refuse
preflight by capability. Simulated user-data
latency preserves source emission order: independently sampled jitter is
bounded below by the previous scheduled delivery, with ties retaining append
order and exact event identities/payload timestamps.
