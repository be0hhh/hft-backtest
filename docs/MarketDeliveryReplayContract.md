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

## Two replay planes

The strategy-delivery coordinate is the captured monotonic delta projected from
the corpus anchor pair. Strategy-visible state changes and market-driven
`runCycle` happen only at this coordinate.

The venue coordinate projects the exchange timestamp onto the same replay axis.
It is never allowed after captured delivery. Missing or unusable exchange time
falls back visibly to delivery and increments venue-adjustment evidence.

The venue plane may update fills, stops and execution state before a message is
visible locally. It cannot make that message visible to the strategy and it
cannot retroactively fill an order that had not reached the venue.

The scheduler never advances internal actions past an undispatched captured
arrival. Before one application delivery it drains only actions strictly
earlier than that delivery; after the market-driven strategy cycle it drains
actions at the same coordinate. On an exact venue/application tie the venue
state is applied first, then the application delivery, then internal actions.

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

The current `run.result` with mandatory `schema_version = 4` labels the model as:

- `arrival_boundary = hft-parser.application-frame-ready`;
- `market_data_delivery = captured_application_frame_arrival`;
- `venue_schedule = exchange_projected_not_after_delivery`.

It also carries captured clock ranges, replay delivery ranges, venue ranges,
anomaly counters and adjusted-venue-event counts. The final watermark is the
greatest replay/execution coordinate actually processed, not the last exchange
timestamp found in input.

Build, tests, runtime capture and live exchange behavior require separate proof.
