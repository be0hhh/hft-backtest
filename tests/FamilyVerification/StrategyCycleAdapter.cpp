#include "FamilyDifferentialContract.hpp"

#include "../../src/Runtime/src/Strategy/BacktestStrategyIntentLowering.hpp"
#include "../../src/Runtime/src/Strategy/BacktestStrategyRuntime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>

namespace {

struct alignas(trading_core::kStrategyRuntimeStateAlign) StrategyStateStorage {
  std::array<std::byte, trading_core::kStrategyRuntimeStateBytes> bytes{};
};

thread_local const trading_core::StrategyDescriptor* capturedDescriptor =
    nullptr;
thread_local const cxet::family_verification::StrategyCycleFixture*
    capturedFixture = nullptr;
thread_local cxet::family_verification::StrategyCycleObservation*
    capturedObservation = nullptr;

bool captureStrategyRun(void* state,
                        trading_core::StrategyContext& context) noexcept {
  if (!capturedDescriptor || !capturedDescriptor->runCycle ||
      !capturedFixture ||
      !capturedObservation) {
    return false;
  }
  cxet::family_verification::applyCanonicalFixtureInput(*capturedFixture,
                                                        context);
  const bool progressed = capturedDescriptor->runCycle(state, context);
  cxet::family_verification::captureCanonicalStrategyRun(
      *capturedDescriptor, state, context, progressed, *capturedObservation);
  return progressed;
}

}  // namespace

namespace cxet::family_verification {

bool runBacktestStrategyCycle(const StrategyCycleFixture& fixture,
                              StrategyCycleObservation& out) noexcept {
  out = StrategyCycleObservation{};
  if (!fixture.descriptor || !fixture.descriptor->initState ||
      !fixture.descriptor->runCycle || !fixture.descriptor->captureState ||
      !hft_backtest::strategy_intent_lowering::footprintSupported(
          *fixture.descriptor)) {
    return false;
  }
  out.post.footprintValid = true;

  auto state = std::make_unique<StrategyStateStorage>();
  auto engine = std::make_unique<hft_backtest::detail::EngineState>();
  auto scratch = std::make_unique<hft_backtest::SingleLegCycleScratch>();
  if (!state || !engine || !scratch) return false;
  if (!fixture.descriptor->initState(state->bytes.data(), fixture.parameters,
                                     fixture.instrument)) {
    return false;
  }
  out.initialized = true;

  engine->snapshot = fixture.snapshot;
  engine->market = canon::kMarketTypeFutures;
  engine->symbol = fixture.shard.symbol;
  const hft_backtest::BacktestStrategyInputPolicy inputPolicy =
      hft_backtest::resolveBacktestStrategyInputPolicy(
          *fixture.descriptor, fixture.parameters);
  for (std::uint8_t raw = 1u;
       raw <= cxet::api::market::kPublicMarketDataStreamCount;
       ++raw) {
    const auto stream = static_cast<
        cxet::api::market::PublicMarketDataStream>(raw);
    if ((inputPolicy.requiredStreamReadyMask &
         cxet::api::market::publicMarketDataStreamReadyBit(stream)) != 0u) {
      hft_backtest::detail::setStrategySourceDeliveryTimestamp(
          *engine, stream, fixture.cycleTimestampNs);
    }
  }
  engine->lastReplayMonotonicNs = fixture.cycleTimestampNs;
  engine->lastOrderPathHotMono.raw =
      static_cast<std::uint64_t>(fixture.cycleTimestampNs);

  trading_core::StrategyDescriptor instrumented = *fixture.descriptor;
  instrumented.runCycle = &captureStrategyRun;
  hft_backtest::BacktestRunRequest request{};
  capturedDescriptor = fixture.descriptor;
  capturedFixture = &fixture;
  capturedObservation = &out;
  out.seamReturned = hft_backtest::runCycle(
      instrumented, inputPolicy, state->bytes.data(), *engine,
      fixture.instrument, *scratch, fixture.shard, fixture.control,
      fixture.cycleTimestampNs, request, nullptr);
  capturedDescriptor = nullptr;
  capturedFixture = nullptr;
  capturedObservation = nullptr;

  out.adapterStatusRaw = out.seamReturned ? 1u : 0u;
  out.post.orderCapacity = fixture.descriptor->footprint.maxOrders;
  out.post.cancelCapacity = fixture.descriptor->footprint.maxCancels;
  out.post.desiredOrderCapacity =
      fixture.descriptor->footprint.maxDesiredOrders;
  out.post.commandCapacity = trading_core::strategyRuntimeCommandCapacity(
      fixture.descriptor->footprint);
  out.post.transactionCapacity =
      fixture.descriptor->footprint.maxTransactions;
  out.post.recordCount = engine->orders.size();
  out.post.recordsTruncated =
      engine->orders.size() > out.post.records.size();
  const std::size_t recordCount = std::min(
      engine->orders.size(), out.post.records.size());
  for (std::size_t index = 0u; index < recordCount; ++index) {
    const hft_backtest::detail::JsonOrder& record = engine->orders[index];
    cxet::family_verification::ConsumerOutputRecord& observed =
        out.post.records[index];
    observed.clientIntentIdRaw = record.id;
    observed.targetClientIntentIdRaw = record.targetId;
    observed.submitTimestampNs = record.submitTsNs;
    observed.activeTimestampNs = record.activeTsNs;
    observed.priceRaw = record.priceE8;
    observed.qtyRaw = record.qtyE8;
    observed.kindRaw = record.action;
    observed.statusRaw = record.status;
    observed.sideRaw = record.side;
    observed.typeRaw = record.type;
    observed.reduceOnly = record.reduceOnly;
    observed.postOnly = record.postOnly;
    if (record.action == hft_backtest::detail::kRecordActionOrder) {
      ++out.post.orderCount;
    } else if (record.action ==
               hft_backtest::detail::kRecordActionCancel) {
      ++out.post.cancelCount;
    }
  }
  out.post.resultStatusRaw = out.seamReturned ? 1u : 0u;
  out.post.finalStatusRaw = out.post.resultStatusRaw;
  out.post.resultAvailable = true;
  fixture.descriptor->captureState(state->bytes.data(), out.post.state);
  out.post.stateAvailable = true;
  return out.strategyProgressedKnown && out.stateAvailable;
}

}  // namespace cxet::family_verification
