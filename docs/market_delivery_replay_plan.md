# Market delivery and replay-clock plan

Status: proposed, static-only. This plan depends on recorder corpus v4 as
defined by `apps/hft-recorder/doc/DELIVERY_TIMESTAMPS_PLAN.md`.

## Goal

Make backtest delivery order explicit. Exchange event time remains market
evidence; captured arrival time or a declared synthetic delivery model decides
when the strategy observes an event.

The model must reproduce independent BBO and trade delivery axes and must never
create lookahead by sorting all streams on exchange timestamps.

## Current truth

`src/input/session_loader.hpp` currently represents trade and BBO rows with
`tsNs` and `ingestSeq`. Timeline keys are built from `(tsNs, ingestSeq)`.

The engine already supports latency configuration, but current session rows do
not contain a durable captured-arrival clock. Generic latency must not be
presented as an empirical per-stream delivery distribution.

Backtest owns replay execution, fill timing, accounting and latency simulation.
Recorder remains the corpus owner. Strategy code is not changed to accommodate
the loader.

## Replay clock modes

The future request has one explicit mode:

```cpp
enum class ReplayClockMode : std::uint8_t {
  ExchangeTime = 1u,
  CapturedArrival = 2u,
  SyntheticDelivery = 3u,
};
```

### `ExchangeTime`

Orders rows by `(exchangeTsNs, ingestSeq)`.

This is the legacy/research view. It is allowed for v3 and v4 sessions but is
not claimed to reproduce live observation order. It must be visible in result
JSON and reports.

### `CapturedArrival`

Orders rows by `(captureMonoNs, ingestSeq)`.

Requirements:

- corpus v4 captured-arrival capability is present;
- every required live stream row has a valid `captureMonoNs`;
- no monotonic regression exists relative to `ingestSeq`;
- missing capability fails the run before strategy initialization.

There is no silent fallback to `ExchangeTime`.

### `SyntheticDelivery`

Computes a delivery timestamp from an explicitly selected distribution:

```text
deliveryNs = exchangeTsNs + sampledDelayNs
```

The synthetic ordering key is `(deliveryNs, ingestSeq)`. The sampled delay is
non-negative, bounded, deterministic from the run seed, and selected per venue,
market, logical stream and latency-profile version.

Synthetic delivery is unavailable when `exchangeTsNs` is missing. It does not
substitute capture monotonic time for exchange time.

## Proposed row and timeline types

The in-memory name makes the axes explicit even though the durable recorder
field remains `tsNs`:

```cpp
struct DeliveryTimeV1 {
  std::int64_t exchangeTsNs{0};
  std::int64_t captureWallNs{0};
  std::int64_t captureMonoNs{0};
  std::int64_t ingestSeq{0};
  bool capturedArrivalAvailable{false};
};

struct TradeRow {
  std::uint64_t eventId{0u};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t side{0};
  DeliveryTimeV1 time{};
};

struct BookTickerRow {
  std::uint64_t eventId{0u};
  std::int64_t bidPriceE8{0};
  std::int64_t bidQtyE8{0};
  std::int64_t askPriceE8{0};
  std::int64_t askQtyE8{0};
  DeliveryTimeV1 time{};
};
```

The later implementation may preserve flat physical fields to reduce migration
noise. The semantic API must still require an explicit axis selector.

```cpp
struct TimelineRowKey {
  std::int64_t deliveryTsNs{0};
  std::int64_t ingestSeq{0};
};

[[nodiscard]] Status timelineKey(
    const DeliveryTimeV1& time,
    ReplayClockMode mode,
    const SyntheticDelaySample* synthetic,
    TimelineRowKey& out) noexcept;
```

`timelineKey()` returns an error for unavailable/invalid time. It never chooses
another mode internally.

## Session capability and defaults

Proposed request fields:

```cpp
struct MarketDeliveryReplayConfig {
  ReplayClockMode clockMode{ReplayClockMode::ExchangeTime};
  std::uint64_t deterministicSeed{0u};
  SyntheticDelayProfileId syntheticProfile{};
};
```

Defaults are compatibility-sensitive:

- existing CLI/config that does not mention delivery keeps
  `ExchangeTime` and records that fact;
- a new live-realism preset explicitly selects `CapturedArrival`;
- corpus v4 does not silently change the behavior of an old run request;
- `SyntheticDelivery` requires a profile ID and deterministic seed.

This prevents result drift merely because the same session was upgraded or
reindexed.

## Arrival-causal market publication

In `CapturedArrival` mode:

- BBO and trade rows are merged only by `(captureMonoNs, ingestSeq)`;
- exchange timestamps remain attached to their source events;
- a trade may arrive after a BBO with a numerically lower trade exchange time;
- the trader reconstruction contract is replayed in observed arrival order;
- equal capture timestamps are resolved by `ingestSeq`, never stream priority;
- no BBO-before-trade or trade-before-BBO precedence is invented.

Backtest reconstruction must use the same state transitions described by
`apps/hft-trader/docs/MARKET_DATA_CAUSALITY_PLAN.md`. The app boundary may adapt
rows into the prebuilt trader runtime types; it must not fork the semantics in a
second independent algorithm.

## Empirical delay profiles

An empirical sample is eligible only when:

- `captureWallNs` and `exchangeTsNs` are both present;
- recorder manifest `wall_sync_status` is `synchronized`;
- `wall_sync_max_error_ns` is present and inside the research acceptance bound;
- the sample is not within a wall-step/reconnect/degraded interval;
- the stream kind and venue/product identity are known.

Raw delay candidate:

```text
candidateDelayNs = captureWallNs - exchangeTsNs
```

Negative candidates are rejected from the latency distribution and counted as
clock-invalid evidence. They are neither clamped nor converted to absolute
values.

Profiles are separated by:

```text
exchange + market/product + logical stream + capture boundary + profile version
```

BBO and trades never share one distribution merely because they use the same
WebSocket endpoint.

The profile artifact stores integer nanoseconds only:

```cpp
struct BoundedDelayDistributionV1 {
  std::array<std::uint64_t, kDelayQuantileCount> quantileNs{};
  std::uint64_t minNs{0u};
  std::uint64_t maxNs{0u};
  std::uint64_t sampleCount{0u};
  std::uint64_t rejectedClockSampleCount{0u};
  std::uint64_t sourceDigest{0u};
};
```

The first implementation should consume a precomputed bounded artifact, not
retain unbounded raw samples in the replay hot loop.

## Synthetic sampling

Sampling requirements:

- integer-only and deterministic;
- one independent PRNG stream per venue/stream/leg;
- stable seed derivation from run seed plus bounded identifiers;
- bounded by the artifact's declared maximum;
- no allocation or file I/O inside replay dispatch;
- delay sampled exactly once per input event;
- result JSON records profile version, digest and seed.

Cross-stream correlation is not invented in v1. If future data proves a joint
delay distribution is required, it needs a separate artifact version and
product decision.

## Fill and decision timing

The engine distinguishes:

- `exchangeTsNs`: evidence carried by the market event;
- `deliveryTsNs`: time the strategy observes the event;
- strategy decision time on the replay monotonic axis;
- configured outbound order latency;
- venue acknowledgment/fill time.

Fill/accounting code must not use exchange event time as the strategy decision
time in captured or synthetic delivery modes. Strategy traces and result JSON
must report both source and delivery time where relevant.

Historical trade warmup remains exchange-time history. Warmup rows initialize
declared strategy history before replay start and must not be interleaved into
captured live delivery as if their arrival had been recorded.

## Failure policy

| Condition | Result |
|---|---|
| Captured mode on v3 corpus | fail preflight |
| Missing capture clock on required v4 row | corrupt/incompatible input |
| Regressing capture monotonic time | corrupt input |
| Missing synthetic profile | invalid request |
| Missing exchange time in synthetic mode | invalid input |
| Negative empirical delay candidate | exclude and count clock-invalid |
| Stale/wrong profile digest | fail preflight |
| Unknown replay clock enum | unsupported request version |

No row is dropped silently to make a run continue.

## Deterministic test matrix

1. BBO exchange ts 2,000 captured after trade exchange ts 9,000: captured mode
   follows capture order, exchange mode follows source-time order.
2. Equal capture timestamps preserve `ingestSeq` order across stream kinds.
3. Same request, seed and profile produce byte-identical timeline/result order.
4. Different per-stream distributions do not share PRNG state.
5. Captured mode refuses v3 without fallback.
6. Synthetic mode refuses a missing source timestamp.
7. Negative wall-minus-exchange sample remains visible in rejected counts.
8. A clock-degraded session cannot generate an empirical profile.
9. Trader reconstructed-BBO behavior matches the arrival-causal contract for
   alternating BBO/trade axes.
10. Result JSON records mode, profile identity/digest and seed.
11. Warmup does not leak future captured-arrival information.
12. Replay seek restores all rows up to the selected delivery key without
    inventing stream precedence.

## Evidence boundary

No code, build, test, replay, benchmark or runtime run is part of this document.
Future implementation must preserve deterministic integer state and obtain
backtest/strategy review before claiming live parity.
