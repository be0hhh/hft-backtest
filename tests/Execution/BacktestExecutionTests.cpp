#include "../../src/Runtime/src/BacktestEngine.hpp"

#include "../../src/Runtime/src/Accounting/BacktestResultAggregation.hpp"
#include "../../src/Runtime/src/Execution/BacktestExecutionDepth.hpp"
#include "../../src/Runtime/src/Execution/BacktestLatency.hpp"
#include "../../src/Runtime/src/Config/BacktestRunConfig.hpp"
#include "../../src/Runtime/src/Strategy/BacktestStrategyRuntime.hpp"
#include "../Support/CapturedSessionFixture.hpp"

#include "trading_core/Execution/User/UserDataTools.hpp"
#include "trading_core/Algorithm/RateLimit/RateLimit.hpp"
#include "trading_core/Algorithm/RateLimit/RateLimitPresets.hpp"
#include "trading_core/Strategy/GeneratedStrategyRegistry.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace hft_backtest;
using namespace hft_backtest::detail;

constexpr std::int64_t kPx100 = 10000000000ll;
constexpr std::int64_t kQty1 = 100000000ll;

Id id(std::uint64_t raw) noexcept {
  Id out{};
  out.raw = raw;
  return out;
}

Price price(std::int64_t raw) noexcept {
  Price out{};
  out.raw = raw;
  return out;
}

Amount amount(std::int64_t raw) noexcept {
  Amount out{};
  out.raw = raw;
  return out;
}

trading_core::OrderIntent orderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  trading_core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Limit;
  out.price = price(px);
  out.qty = amount(kQty1);
  return out;
}

trading_core::OrderIntent postOnlyOrderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  trading_core::OrderIntent out = orderIntent(orderId, side, px);
  out.limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::PostOnlyPrefer;
  return out;
}

trading_core::OrderIntent strictPostOnlyOrderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  trading_core::OrderIntent out = orderIntent(orderId, side, px);
  out.limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
  return out;
}

trading_core::OrderIntent reduceMarketIntent(std::uint64_t orderId, Side side) noexcept {
  trading_core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Market;
  out.qty = amount(kQty1);
  out.positionReduction = trading_core::PositionReductionIntent::Reduce;
  return out;
}

trading_core::OrderIntent reduceMarketIntentQty(std::uint64_t orderId, Side side, std::int64_t qtyRaw) noexcept {
  trading_core::OrderIntent out = reduceMarketIntent(orderId, side);
  out.qty = amount(qtyRaw);
  return out;
}

trading_core::OrderIntent marketIntent(std::uint64_t orderId, Side side) noexcept {
  trading_core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Market;
  out.qty = amount(kQty1);
  return out;
}

trading_core::OrderIntent stopMarketIntent(std::uint64_t orderId,
                                               Side side,
                                               std::int64_t triggerPx,
                                               canon::OrderTriggerSource triggerSource = canon::OrderTriggerSource::Last) noexcept {
  trading_core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Stop;
  out.triggerPrice = price(triggerPx);
  out.triggerSource = triggerSource;
  out.qty = amount(kQty1);
  return out;
}

trading_core::CancelIntent cancelIntent(std::uint64_t commandId, std::uint64_t targetId) noexcept {
  trading_core::CancelIntent out{};
  out.clientIntentId = id(commandId);
  out.targetClientIntentId = id(targetId);
  out.targetOrderType = canon::OrderType::Limit;
  return out;
}

void setTop(EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  cxet::composite::BookTickerRuntimeV1 top{};
  top.bid.px.raw = bid;
  top.ask.px.raw = ask;
  trading_core::copyBookTickerSnapshot(engine.snapshot, top);
  trading_core::copyBookTickerSnapshot(engine.venueSnapshot, top);
  setStrategySourceDeliveryTimestamp(
      engine, cxet::api::market::PublicMarketDataStream::BookTicker,
      1000ll);
  engine.snapshot.lastBookTicker.ts.raw = 1000u;
  engine.venueSnapshot.lastBookTicker.ts.raw = 1000u;
}

void markReconstructedBookTickerReady(EngineState& engine) noexcept {
  cxet::composite::BboReconstructionView reconstructed{};
  reconstructed.bookTicker = engine.snapshot.lastBookTicker;
  reconstructed.source =
      cxet::composite::kBboReconstructionSourceBookTicker;
  reconstructed.valid = cxet::composite::kBboReconstructionFlagTrue;
  reconstructed.bidPresent = cxet::composite::kBboReconstructionFlagTrue;
  reconstructed.askPresent = cxet::composite::kBboReconstructionFlagTrue;
  trading_core::copyReconstructedBboSnapshot(
      engine.snapshot, reconstructed, TimeNs{1000u});
  trading_core::copyReconstructedBboSnapshot(
      engine.venueSnapshot, reconstructed, TimeNs{1000u});
}

BacktestStrategyInputPolicy bookTickerInputPolicy() noexcept {
  BacktestStrategyInputPolicy out{};
  out.requiredStreamReadyMask =
      cxet::api::market::publicMarketDataStreamReadyBit(
          cxet::api::market::PublicMarketDataStream::BookTicker);
  out.maxAgeNs = trading_core::strategy::strategyInputFreshnessMaxAgeNs(
      trading_core::strategy::kDefaultStrategyInputFreshnessMaxAgeMs);
  return out;
}

void setTopWithQty(EngineState& engine,
                   std::int64_t bid,
                   std::int64_t bidQty,
                   std::int64_t ask,
                   std::int64_t askQty) noexcept {
  setTop(engine, bid, ask);
  engine.snapshot.lastBookTicker.bid.qty.raw = bidQty;
  engine.snapshot.lastBookTicker.ask.qty.raw = askQty;
  engine.venueSnapshot.lastBookTicker.bid.qty.raw = bidQty;
  engine.venueSnapshot.lastBookTicker.ask.qty.raw = askQty;
}

void setTopAt(EngineState& engine, std::int64_t bid, std::int64_t ask, std::uint64_t tsNs) noexcept {
  setTop(engine, bid, ask);
  engine.snapshot.lastBookTicker.ts.raw = tsNs;
  engine.venueSnapshot.lastBookTicker.ts.raw = tsNs;
}

void seedVenueBook(EngineState& engine,
                   std::initializer_list<std::pair<std::int64_t, std::int64_t>> bids,
                   std::initializer_list<std::pair<std::int64_t, std::int64_t>> asks) noexcept {
  cxet::composite::OrderBookSnapshot snapshot{};
  snapshot.ts.raw = 1000u;
  for (const auto& level : bids) {
    (void)cxet::composite::appendOrderBookLevel(snapshot, price(level.first), amount(level.second), Side::Buy());
  }
  for (const auto& level : asks) {
    (void)cxet::composite::appendOrderBookLevel(snapshot, price(level.first), amount(level.second), Side::Sell());
  }
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = static_cast<std::uint32_t>(
      std::max(bids.size(), asks.size()));
  if (!engine.venueOrderBook.reserveLevelsPerSide(
          config.maxStoredLevelsPerSide) ||
      !trading_core::applySnapshot(
          engine.venueOrderBook, snapshot, engine.symbol, config)) {
    return;
  }
  trading_core::bindOrderBookSnapshot(engine.venueSnapshot, engine.venueOrderBook);
  engine.venueOrderBookExecutionEnabled = true;
  if (!bids.size() || !asks.size()) return;
  setTop(engine, bids.begin()->first, asks.begin()->first);
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

bool testBacktestCanonicalizesAsterSessionExchange() {
  return expect(exchangeFromText("aster").raw == canon::kExchangeIdAster.raw,
                "Aster recorder session exchange is recognized by backtest");
}

const trading_core::RateLimitBucketConfig* findRateLimitBucket(
    const BacktestRateLimitSchedule& schedule,
    trading_core::RateLimitBucketKind kind,
    std::uint64_t intervalNs) noexcept {
  for (const trading_core::RateLimitBucketConfig& bucket : schedule.buckets) {
    if (bucket.enabled && bucket.kind == kind && bucket.intervalNs == intervalNs) return &bucket;
  }
  return nullptr;
}

bool testBacktestRateLimitScheduleUsesExchangePreset() {
  BacktestRunRequest request{};
  const BacktestRateLimitSchedule schedule = resolveRateLimitSchedule(request, "binance", "futures");
  const trading_core::RateLimitBucketConfig* tenSecond = findRateLimitBucket(
      schedule, trading_core::RateLimitBucketKind::Orders, 10000000000ull);
  const trading_core::RateLimitBucketConfig* minute = findRateLimitBucket(
      schedule, trading_core::RateLimitBucketKind::Orders, 60000000000ull);
  return expect(tenSecond && tenSecond->limit == 300,
                "binance futures preset has 300 orders / 10s") &&
         expect(minute && minute->limit == 1200,
                "binance futures preset keeps the 1m order window");
}

bool testBacktestRateLimitScheduleCanDisablePresets() {
  BacktestRunRequest request{};
  request.rateLimitsEnabled = false;
  const BacktestRateLimitSchedule schedule = resolveRateLimitSchedule(request, "binance", "futures");
  return expect(schedule.buckets.empty(), "disabled backtest rate limits remove preset buckets") &&
         expect(schedule.actions.empty(), "disabled backtest rate limits remove preset actions");
}

bool testWarmupOrderIsServiceHiddenAndMarksHot() {
  EngineState engine{};
  setTop(engine, kPx100, kPx100 + 100000000ll);

  trading_core::WarmupOrderIntent warmup{};
  warmup.order = strictPostOnlyOrderIntent(500u, Side::Buy(), kPx100 - 200000000ll);
  warmup.order.qty = amount(kQty1 / 10);
  warmup.order.service = true;
  warmup.cancelTimeoutMs = 250u;
  warmup.cancelAfterAck = true;

  trading_core::StrategyCommandIntent command{};
  command.kind = trading_core::StrategyCommandIntentKind::WarmupOrder;
  command.warmup = warmup;
  trading_core::StrategyIo io{};
  io.commandOut = Span<trading_core::StrategyCommandIntent>(&command, 1u);
  io.commandCount = 1u;

  applyIntents(engine, io, 1000ll, 0u, 0u);

  trading_core::StrategyOrderState visible[4]{};
  const std::size_t visibleCount = visibleOrders(engine, visible, 4u);
  return expect(visibleCount == 0u, "service warmup order is hidden from strategy order state") &&
         expect(engine.userEvents.empty(), "service warmup ack/cancel does not publish strategy user events") &&
         expect(engine.lastOrderPathHotMono.raw == 1000u, "service warmup ack marks order path hot") &&
         expect(!engine.warmupInFlight, "service warmup is not left in-flight after cancel ack");
}

struct WarmupGateProbeState {
  std::uint64_t cycles{0u};
  std::uint64_t nextClientId{1u};
};

bool warmupGateProbeRunCycle(void* rawState, trading_core::StrategyContext& ctx) noexcept {
  auto* state = static_cast<WarmupGateProbeState*>(rawState);
  if (!state || ctx.io.commandOut.size() == 0u) return false;
  trading_core::StrategyCommandIntent command{};
  command.kind = trading_core::StrategyCommandIntentKind::Order;
  command.order.clientIntentId.raw = state->nextClientId++;
  command.order.side = Side::Buy();
  command.order.orderType = canon::OrderType::Market;
  command.order.qty = amount(kQty1);
  command.order.routeHandle = trading_core::ExecutionRouteHandle{1u};
  ctx.io.commandOut[0] = command;
  ctx.io.commandCount = 1u;
  ++state->cycles;
  return true;
}

void warmupGateProbeCaptureState(const void* rawState, trading_core::StrategyStateSnapshot& out) noexcept {
  const auto* state = static_cast<const WarmupGateProbeState*>(rawState);
  out = trading_core::StrategyStateSnapshot{};
  if (!state) return;
  out.cycles = state->cycles;
  out.nextClientIntentId.raw = state->nextClientId;
}

void warmupGateProbeRestoreState(void* rawState, const trading_core::StrategyStateSnapshot& before) noexcept {
  auto* state = static_cast<WarmupGateProbeState*>(rawState);
  if (!state) return;
  state->cycles = before.cycles;
  state->nextClientId = before.nextClientIntentId.raw;
}

bool testBacktestWarmupGateAllowsRealOrderAfterWarmupAckMarksHot() {
  trading_core::StrategyDescriptor descriptor{};
  descriptor.name = "warmup_gate_probe";
  descriptor.universeTopology =
      trading_core::StrategyUniverseTopology::SingleSource;
  descriptor.domainMask = trading_core::StrategyDomainMask::All;
  descriptor.requiredOrderCapabilities =
      trading_core::StrategyOrderCapability::Market;
  descriptor.minLegs = 1u;
  descriptor.maxLegs = 1u;
  descriptor.streamCount = 1u;
  descriptor.streams[0] =
      cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.footprint =
      trading_core::strategyRuntimeFootprint<WarmupGateProbeState>(
          0u, 0u, 0u, 1u, 0u);
  descriptor.runCycle = warmupGateProbeRunCycle;
  descriptor.captureState = warmupGateProbeCaptureState;
  descriptor.rollbackCapacityCycle = warmupGateProbeRestoreState;
  descriptor.restoreAfterOrderFailure = warmupGateProbeRestoreState;

  std::vector<EngineState> engines(1u);
  EngineState& engine = engines[0];
  engine.exchange = canon::kExchangeIdBinance;
  engine.market = canon::kMarketTypeFutures;
  setTop(engine, kPx100, kPx100 + 100000000ll);
  markReconstructedBookTickerReady(engine);
  BacktestLatencyConfig latency{};
  latency.marketOrder = BacktestLatencyProfile{};
  latency.limitOrder = BacktestLatencyProfile{};
  latency.cancelOrder = BacktestLatencyProfile{};
  initLatencyRuntime(engine.latency, latency);

  std::vector<StrategyInstrumentSpec> instruments(1u);
  instruments[0].tickSizeRaw = 100000000ll;
  instruments[0].stepSizeRaw = 100000000ll;
  instruments[0].tickGrid = trading_core::prepareFastStepGrid(instruments[0].tickSizeRaw);
  instruments[0].qtyStepGrid = trading_core::prepareFastStepGrid(instruments[0].stepSizeRaw);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  scratch->reset(1u);
  WarmupGateProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool warmupOk = hft_backtest::runCycleMulti(
      descriptor, bookTickerInputPolicy(), &state, engines, instruments,
      *scratch, shard, control, 0u, 1000ll, request, nullptr);
  const std::size_t ordersAfterWarmup = engine.orders.size();
  const std::uint64_t hotMonoAfterWarmup = engine.lastOrderPathHotMono.raw;

  const bool realOrderOk = hft_backtest::runCycleMulti(
      descriptor, bookTickerInputPolicy(), &state, engines, instruments,
      *scratch, shard, control, 0u, 1001ll, request, nullptr);

  return expect(warmupOk, "first cycle emits service warmup order") &&
         expect(ordersAfterWarmup == 2u, "warmup order and cancel are recorded") &&
         expect(engine.orders[0].warmup && engine.orders[0].service, "first order is service warmup") &&
         expect(hotMonoAfterWarmup == 1000u, "warmup ack marks route hot") &&
         expect(realOrderOk, "second cycle runs after warmup") &&
         expect(engine.fills.size() == 1u, "route-hot second cycle sends real market order and fills it") &&
         expect(engine.userEvents.size() == 1u, "real fill publishes strategy user event") &&
         expect(cxet::composite::userDataEventContractValid(engine.userEvents[0]),
                "backtest fill satisfies the normalized USER contract") &&
         expect(engine.userEvents[0].eventId.raw == 1u,
                "backtest USER sequence starts at one") &&
         expect(engine.orders.size() == 3u, "no second warmup is inserted once route is hot") &&
         expect(!engine.orders.back().service && !engine.orders.back().warmup, "final order is real strategy order");
}

enum class ReservedSequenceProbeMode : std::uint8_t {
  HasSequenceFlag = 0,
  FilteredFlag = 1,
  RawMetadata = 2,
  Ordinary = 3,
};

struct ReservedSequenceProbeState {
  ReservedSequenceProbeMode mode{ReservedSequenceProbeMode::HasSequenceFlag};
  std::uint64_t cycles{0u};
  std::uint64_t restoreCalls{0u};
};

bool reservedSequenceProbeRunCycle(
    void* rawState,
    trading_core::StrategyContext& ctx) noexcept {
  auto* state = static_cast<ReservedSequenceProbeState*>(rawState);
  if (!state || ctx.io.commandOut.size() == 0u) return false;
  trading_core::StrategyCommandIntent command{};
  command.kind = trading_core::StrategyCommandIntentKind::Order;
  command.order = postOnlyOrderIntent(
      700u + state->cycles, Side::Buy(), kPx100 - 200000000ll);
  command.order.routeHandle = trading_core::ExecutionRouteHandle{1u};
  if (state->mode == ReservedSequenceProbeMode::RawMetadata) {
    command.sequenceKind =
        static_cast<trading_core::StrategyCommandSequenceKind>(1u);
    command.sequenceStep =
        static_cast<trading_core::StrategyCommandSequenceStep>(1u);
    command.sequenceId.raw = 1u;
  }
  ctx.io.commandOut[0] = command;
  ctx.io.commandCount = 1u;
  ctx.io.commandSequenceFiltered =
      state->mode == ReservedSequenceProbeMode::FilteredFlag;
  ctx.io.hasCommandSequence =
      state->mode == ReservedSequenceProbeMode::HasSequenceFlag;
  ++state->cycles;
  return true;
}

void reservedSequenceProbeCaptureState(
    const void* rawState,
    trading_core::StrategyStateSnapshot& out) noexcept {
  const auto* state = static_cast<const ReservedSequenceProbeState*>(rawState);
  out = trading_core::StrategyStateSnapshot{};
  if (state) out.cycles = state->cycles;
}

void reservedSequenceProbeRestoreState(
    void* rawState,
    const trading_core::StrategyStateSnapshot& before) noexcept {
  auto* state = static_cast<ReservedSequenceProbeState*>(rawState);
  if (!state) return;
  state->cycles = before.cycles;
  ++state->restoreCalls;
}

bool testRunCycleRejectsReservedSequenceMetadataBeforeApply() {
  trading_core::StrategyDescriptor descriptor{};
  descriptor.name = "reserved_sequence_probe";
  descriptor.universeTopology =
      trading_core::StrategyUniverseTopology::SingleSource;
  descriptor.domainMask = trading_core::StrategyDomainMask::All;
  descriptor.requiredOrderCapabilities =
      trading_core::StrategyOrderCapability::Limit;
  descriptor.minLegs = 1u;
  descriptor.maxLegs = 1u;
  descriptor.streamCount = 1u;
  descriptor.streams[0] =
      cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.footprint =
      trading_core::strategyRuntimeFootprint<ReservedSequenceProbeState>(
          0u, 0u, 0u, 1u, 0u);
  descriptor.runCycle = reservedSequenceProbeRunCycle;
  descriptor.captureState = reservedSequenceProbeCaptureState;
  descriptor.rollbackCapacityCycle = reservedSequenceProbeRestoreState;
  descriptor.restoreAfterOrderFailure = reservedSequenceProbeRestoreState;

  EngineState engine{};
  engine.exchange = canon::kExchangeIdBinance;
  engine.market = canon::kMarketTypeFutures;
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  markReconstructedBookTickerReady(engine);
  engine.lastOrderPathHotMono.raw = 1000u;

  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = 100000000ll;
  instrument.stepSizeRaw = 100000000ll;
  instrument.tickGrid = trading_core::prepareFastStepGrid(
      instrument.tickSizeRaw);
  instrument.qtyStepGrid = trading_core::prepareFastStepGrid(
      instrument.stepSizeRaw);
  SingleLegCycleScratch scratch{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  ReservedSequenceProbeState state{};

  const bool flaggedOk = hft_backtest::runCycle(
      descriptor, bookTickerInputPolicy(), &state, engine, instrument,
      scratch, shard, control,
      1000ll, request, nullptr);
  const bool flaggedRestored =
      !flaggedOk && state.cycles == 0u && state.restoreCalls == 1u &&
      engine.orders.empty();

  state.mode = ReservedSequenceProbeMode::FilteredFlag;
  const bool filteredOk = hft_backtest::runCycle(
      descriptor, bookTickerInputPolicy(), &state, engine, instrument,
      scratch, shard, control,
      1001ll, request, nullptr);
  const bool filteredRestored =
      !filteredOk && state.cycles == 0u && state.restoreCalls == 2u &&
      engine.orders.empty();

  state.mode = ReservedSequenceProbeMode::RawMetadata;
  const bool rawOk = hft_backtest::runCycle(
      descriptor, bookTickerInputPolicy(), &state, engine, instrument,
      scratch, shard, control,
      1002ll, request, nullptr);
  const bool rawRestored =
      !rawOk && state.cycles == 0u && state.restoreCalls == 3u &&
      engine.orders.empty();

  state.mode = ReservedSequenceProbeMode::Ordinary;
  const bool ordinaryOk = hft_backtest::runCycle(
      descriptor, bookTickerInputPolicy(), &state, engine, instrument,
      scratch, shard, control,
      1003ll, request, nullptr);

  return expect(flaggedRestored,
                "hasCommandSequence output restores strategy state before apply") &&
         expect(filteredRestored,
                "filtered reserved sequence output restores strategy state before apply") &&
         expect(rawRestored,
                "raw reserved metadata restores strategy state before apply") &&
         expect(ordinaryOk && state.cycles == 1u &&
                    state.restoreCalls == 3u && engine.orders.size() == 1u,
                "ordinary single command remains valid after rejected cycles");
}

struct UserSemanticProbeState {
  cxet::composite::UserDataSemanticMask mask{0u};
  std::uint64_t cycles{0u};
  bool hasUserEvent{false};
};

void userSemanticProbeCaptureState(
    const void* rawState,
    trading_core::StrategyStateSnapshot& out) noexcept {
  const auto* state = static_cast<const UserSemanticProbeState*>(rawState);
  out = trading_core::StrategyStateSnapshot{};
  if (state) out.cycles = state->cycles;
}

bool userSemanticProbeRunCycle(
    void* rawState,
    trading_core::StrategyContext& ctx) noexcept {
  auto* state = static_cast<UserSemanticProbeState*>(rawState);
  if (!state) return false;
  state->mask = ctx.userEventAdmittedSemanticMask;
  state->hasUserEvent = ctx.hasUserEvent;
  ++state->cycles;
  return true;
}

bool testRunCycleAdmitsSimulatedUserEventSemantics() {
  trading_core::StrategyDescriptor descriptor{};
  descriptor.name = "user_semantic_probe";
  descriptor.universeTopology =
      trading_core::StrategyUniverseTopology::SingleSource;
  descriptor.domainMask = trading_core::StrategyDomainMask::All;
  descriptor.minLegs = 1u;
  descriptor.maxLegs = 1u;
  descriptor.streamCount = 1u;
  descriptor.streams[0] =
      cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.footprint =
      trading_core::strategyRuntimeFootprint<UserSemanticProbeState>(
          0u, 0u, 0u, 0u, 0u);
  descriptor.runCycle = userSemanticProbeRunCycle;
  descriptor.captureState = userSemanticProbeCaptureState;

  EngineState engine{};
  engine.exchange = canon::kExchangeIdBinance;
  engine.market = canon::kMarketTypeFutures;
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  markReconstructedBookTickerReady(engine);

  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = 100000000ll;
  instrument.stepSizeRaw = 100000000ll;
  instrument.tickGrid = trading_core::prepareFastStepGrid(
      instrument.tickSizeRaw);
  instrument.qtyStepGrid = trading_core::prepareFastStepGrid(
      instrument.stepSizeRaw);
  SingleLegCycleScratch scratch{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  UserSemanticProbeState state{};

  using Field = cxet::composite::UserDataField;
  using Kind = cxet::composite::UserDataEventKind;
  using Semantic = cxet::composite::UserDataSemantic;
  const auto orderMask =
      cxet::composite::userDataSemanticBit(Semantic::OrderTransition);
  const auto fillMask =
      cxet::composite::userDataSemanticBit(Semantic::FillExecution);

  cxet::composite::UserDataEvent canceled{};
  canceled.kind = Kind::Order;
  canceled.orderStatus = canon::OrderStatus::Canceled;
  cxet::composite::markUserDataFieldPresent(canceled, Field::OrderStatus);
  cxet::composite::markUserDataFieldPresent(canceled, Field::LastFilledQty);
  const bool canceledOk = hft_backtest::runCycle(
      descriptor, bookTickerInputPolicy(), &state, engine, instrument,
      scratch, shard, control,
      2000ll, request, &canceled);
  const bool canceledIsOrderOnly =
      canceledOk && state.hasUserEvent && state.mask == orderMask;

  cxet::composite::UserDataEvent rejected{};
  rejected.kind = Kind::Order;
  rejected.orderStatus = canon::OrderStatus::Rejected;
  cxet::composite::markUserDataFieldPresent(rejected, Field::OrderStatus);
  const bool rejectedOk = hft_backtest::runCycle(
      descriptor, bookTickerInputPolicy(), &state, engine, instrument,
      scratch, shard, control,
      2001ll, request, &rejected);
  const bool rejectedIsOrderOnly =
      rejectedOk && state.hasUserEvent && state.mask == orderMask;

  cxet::composite::UserDataEvent fill{};
  fill.kind = Kind::Order;
  fill.orderStatus = canon::OrderStatus::PartiallyFilled;
  fill.lastFilledQty = amount(kQty1 / 4ll);
  cxet::composite::markUserDataFieldPresent(fill, Field::OrderStatus);
  cxet::composite::markUserDataFieldPresent(fill, Field::LastFilledQty);
  const bool fillOk = hft_backtest::runCycle(
      descriptor, bookTickerInputPolicy(), &state, engine, instrument,
      scratch, shard, control,
      2002ll, request, &fill);

  return expect(canceledIsOrderOnly,
                "canceled simulated order event admits order transition only") &&
         expect(rejectedIsOrderOnly,
                "rejected simulated order event admits order transition only") &&
         expect(fillOk && state.hasUserEvent &&
                    state.mask == static_cast<cxet::composite::UserDataSemanticMask>(
                        orderMask | fillMask),
                "positive present LastFilledQty admits order and fill semantics") &&
         expect(state.cycles == 3u,
                "all admitted simulated user events reach the strategy cycle");
}

bool testBacktestFeeScheduleOverridesExchangePreset() {
  BacktestRunRequest request{};
  BacktestFeeSchedule overrideSchedule{};
  overrideSchedule.exchange = "binance";
  overrideSchedule.market = "futures";
  overrideSchedule.makerFeeBpsE8 = -123000000;
  overrideSchedule.takerFeeBpsE8 = 456000000;
  request.feeSchedules.push_back(std::move(overrideSchedule));

  const BacktestFeeSchedule schedule = resolveFeeSchedule(request, "binance", "futures");
  return expect(schedule.makerFeeBpsE8 == -123000000,
                "explicit backtest maker fee overrides exchange preset") &&
         expect(schedule.takerFeeBpsE8 == 456000000,
                "explicit backtest taker fee overrides exchange preset");
}

bool testBacktestGlobalFeeOverrideFillsPresetSides() {
  BacktestRunRequest request{};
  request.makerFeeBpsE8 = 0;
  request.takerFeeBpsE8 = 123000000;
  request.hasMakerFeeBps = true;
  request.hasTakerFeeBps = true;

  const BacktestFeeSchedule schedule = resolveFeeSchedule(request, "binance", "futures");
  return expect(schedule.makerFeeBpsE8 == 0,
                "explicit global maker fee can override preset to zero") &&
         expect(schedule.takerFeeBpsE8 == 123000000,
                "explicit global taker fee overrides preset side");
}

bool testRateLimitDecisionAllowsAfterReset() {
  trading_core::RateLimitState limits{};
  trading_core::RateLimitBucketConfig bucket{};
  bucket.kind = trading_core::RateLimitBucketKind::Orders;
  bucket.limit = 1;
  bucket.intervalNs = 100u;
  bucket.enabled = true;
  trading_core::RateLimitActionConfig action{};
  action.action = trading_core::RateLimitActionKind::LimitOrder;
  action.enabled = true;
  action.costCount = 1u;
  action.costs[0].bucket = trading_core::RateLimitBucketKind::Orders;
  action.costs[0].cost = 1;
  TimeNs first{};
  first.raw = 1u;
  TimeNs blockedTs{};
  blockedTs.raw = 2u;

  const bool configured = limits.configureBucket(bucket) && limits.configureAction(action);
  const trading_core::RateLimitDecision firstDecision =
      limits.consumeDecision(trading_core::RateLimitActionKind::LimitOrder, first);
  const trading_core::RateLimitDecision blockedDecision =
      limits.consumeDecision(trading_core::RateLimitActionKind::LimitOrder, blockedTs);
  const trading_core::RateLimitDecision resetDecision =
      limits.consumeDecision(trading_core::RateLimitActionKind::LimitOrder, blockedDecision.resetMono);

  return expect(configured, "rate-limit test state is configured") &&
         expect(firstDecision.allowed, "first action consumes available quota") &&
         expect(!blockedDecision.allowed, "second action is blocked before reset") &&
         expect(blockedDecision.blockedBucket == trading_core::RateLimitBucketKind::Orders,
                "blocked decision names exhausted bucket") &&
         expect(blockedDecision.resetMono.raw == 100u,
                "blocked decision exposes next reset timestamp") &&
         expect(resetDecision.allowed, "action is allowed again at reset timestamp");
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void writeText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  out << text;
}

struct CapturedDepthFixtureRow {
  std::int64_t exchangeTsNs{0};
  std::uint64_t sequence{0u};
  std::vector<hft_backtest::test_support::DepthLevel> levels{};
};

void writeCapturedDepthManifest(const std::filesystem::path& dir,
                                std::uint64_t depthRows) {
  writeText(
      dir / "manifest.json",
      hft_backtest::test_support::capturedManifestJson(
          "binance",
          "futures",
          "BTC_USDT",
          hft_backtest::test_support::CapturedChannelCounts{
              .depth = depthRows}));
}

void writeCapturedDepthSession(
    const std::filesystem::path& dir,
    std::initializer_list<CapturedDepthFixtureRow> rows) {
  writeCapturedDepthManifest(dir, rows.size());
  std::string tape{};
  std::string sidecar{};
  for (const CapturedDepthFixtureRow& row : rows) {
    std::string tapeRow = "[" + std::to_string(row.sequence) + "," +
        std::to_string(hft_backtest::test_support::taggedDepthTimestamp(
            row.exchangeTsNs)) + "," + std::to_string(row.sequence) + "," +
        std::to_string(row.sequence) + "," +
        hft_backtest::test_support::arrivalTail(
            row.sequence, row.exchangeTsNs);
    std::string sidecarRow = "[" + std::to_string(row.sequence) + "," +
        std::to_string(hft_backtest::test_support::taggedDepthTimestamp(
            row.exchangeTsNs));
    std::size_t levelIndex = 0u;
    while (levelIndex < row.levels.size()) {
      const std::int64_t side = row.levels[levelIndex].side;
      std::uint64_t count = 0u;
      while (levelIndex < row.levels.size() &&
             row.levels[levelIndex].side == side) {
        tapeRow += "," + std::to_string(row.levels[levelIndex].price) +
            "," + std::to_string(row.levels[levelIndex].qty);
        ++levelIndex;
        ++count;
      }
      sidecarRow += "," + std::to_string(side) + "," +
          std::to_string(count);
    }
    tape += tapeRow + "]\n";
    sidecar += sidecarRow + "]\n";
  }
  writeText(dir / "jsonl" / "depth_tape.jsonl", tape);
  writeText(dir / "jsonl" / "depth_sidecar.jsonl", sidecar);
}

Symbol btcUsdtSymbol() {
  Symbol symbol{};
  symbol.copyFrom("BTC_USDT");
  return symbol;
}

bool testStaleCancelDoesNotCancelReusedSlot() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 2> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll), orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  std::array<trading_core::CancelIntent, 1> cancels{cancelIntent(10, 1)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), 1);
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0);
  io.clear();
  io.cancelOut = Span<trading_core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  applyIntents(engine, io, 10, 0, 100);
  (void)fillOrder(engine, engine.open[0], kPx100, 20, false);
  io.clear();
  io.orderOut = Span<trading_core::OrderIntent>(&orders[1], 1);
  io.orderCount = 1;
  applyIntents(engine, io, 30, 0, 0);
  const std::size_t userEventsBeforeStaleCancel = engine.userEvents.size();
  applyDueActions(engine, 200000);
  return expect(engine.open[0].state.valid, "reused slot remains valid after stale cancel") &&
         expect(engine.open[0].state.clientIntentId.raw == 2, "stale cancel does not cancel replacement order") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "original order remains filled") &&
         expect(engine.userEvents.size() == userEventsBeforeStaleCancel, "stale cancel does not publish user event");
}

bool testRateLimitRejectsOrderButAllowsSeparateCancelBucket() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);

  trading_core::RateLimitBucketConfig orders{};
  orders.kind = trading_core::RateLimitBucketKind::Orders;
  orders.limit = 1;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);

  trading_core::RateLimitBucketConfig cancels{};
  cancels.kind = trading_core::RateLimitBucketKind::CancelOrders;
  cancels.limit = 1;
  cancels.intervalNs = 1000000000ll;
  cancels.enabled = true;
  engine.rateLimits.configureBucket(cancels);

  trading_core::RateLimitActionConfig orderAction{};
  orderAction.action = trading_core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1;
  orderAction.costs[0].bucket = trading_core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  trading_core::RateLimitActionConfig cancelAction{};
  cancelAction.action = trading_core::RateLimitActionKind::CancelOrder;
  cancelAction.costCount = 1;
  cancelAction.costs[0].bucket = trading_core::RateLimitBucketKind::CancelOrders;
  cancelAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(cancelAction);

  std::array<trading_core::OrderIntent, 1> first{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(first.data(), first.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<trading_core::CancelIntent, 1> cancel{cancelIntent(10, 1)};
  std::array<trading_core::OrderIntent, 1> second{orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  io.clear();
  io.cancelOut = Span<trading_core::CancelIntent>(cancel.data(), cancel.size());
  io.cancelCount = 1;
  io.orderOut = Span<trading_core::OrderIntent>(second.data(), second.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1100, 0, 0);

  bool hasRateLimitedReject = false;
  for (const cxet::composite::UserDataEvent& event : engine.userEvents) {
    hasRateLimitedReject = hasRateLimitedReject ||
        (event.orderStatus == canon::OrderStatus::Rejected &&
         event.orderProblem == canon::OrderProblem::RateLimited);
  }
  return expect(engine.orders.size() == 3u, "rate-limit scenario records first order, cancel, and rejected order") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "cancel uses separate bucket and cancels original order") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "cancel command reaches exchange") &&
         expect(engine.orders[2].status == kRecordStatusRejected, "second order is rejected by exhausted order bucket") &&
         expect(hasRateLimitedReject, "rate-limited order publishes rejected user event with RateLimited problem");
}

bool testRepeatedRateLimitRejectsShareCooldownWindow() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);

  trading_core::RateLimitBucketConfig orders{};
  orders.kind = trading_core::RateLimitBucketKind::Orders;
  orders.limit = 1;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);

  trading_core::RateLimitActionConfig orderAction{};
  orderAction.action = trading_core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1;
  orderAction.costs[0].bucket = trading_core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  std::array<trading_core::OrderIntent, 1> first{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(first.data(), first.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<trading_core::OrderIntent, 2> blocked{
      orderIntent(2, Side::Sell(), kPx100 + 200000000ll),
      orderIntent(3, Side::Sell(), kPx100 + 300000000ll)};
  io.clear();
  io.orderOut = Span<trading_core::OrderIntent>(blocked.data(), blocked.size());
  io.orderCount = static_cast<std::uint32_t>(blocked.size());
  applyIntents(engine, io, 1100, 0, 0);

  std::size_t rateLimitedEvents = 0u;
  for (const cxet::composite::UserDataEvent& event : engine.userEvents) {
    if (event.orderStatus == canon::OrderStatus::Rejected &&
        event.orderProblem == canon::OrderProblem::RateLimited) {
      ++rateLimitedEvents;
    }
  }
  return expect(engine.orders.size() == 3u, "repeated rate-limit scenario records all attempted orders") &&
         expect(engine.orders[1].status == kRecordStatusRejected, "first blocked order is rejected") &&
         expect(engine.orders[2].status == kRecordStatusRejected, "second blocked order is rejected") &&
         expect(rateLimitedEvents == 2u, "each rate-limited order publishes terminal rejected user event") &&
         expect(engine.rateLimitCooldownUntilTsNs == 1000000000ll, "rate-limit cooldown resumes at bucket reset");
}

bool testRateLimitRejectAddsRunWarningWithoutFailingRun() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);

  trading_core::RateLimitBucketConfig orders{};
  orders.kind = trading_core::RateLimitBucketKind::Orders;
  orders.limit = 1;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);

  trading_core::RateLimitActionConfig orderAction{};
  orderAction.action = trading_core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1;
  orderAction.costs[0].bucket = trading_core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  std::array<trading_core::OrderIntent, 1> first{
      orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(first.data(), first.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<trading_core::OrderIntent, 1> blocked{
      orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  io.clear();
  io.orderOut =
      Span<trading_core::OrderIntent>(blocked.data(), blocked.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1100, 0, 0);

  hft_backtest::BacktestRunResult result{};
  result.status = hft_backtest::Status::Ok;
  hft_backtest::appendRateLimitWarnings(result, engine);

  return expect(result.status == hft_backtest::Status::Ok, "rate-limit warning does not fail backtest result") &&
         expect(result.warnings.size() == 1u, "rate-limit reject emits one run warning") &&
         expect(result.warnings[0].find("rate limit reached during backtest") != std::string::npos,
                "rate-limit warning names the condition") &&
         expect(result.warnings[0].find("orders=1") != std::string::npos,
                "rate-limit warning includes rejected order count") &&
         expect(result.warnings[0].find("cancels=0") != std::string::npos,
                "rate-limit warning includes rejected cancel count");
}

bool testPendingDrainActivatesLateOrder() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{orderIntent(3, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 100, 0);
  return expect(engine.orders[0].status == kRecordStatusPending, "late order initially pending") &&
         expect(engine.open[0].state.state == trading_core::StrategyOrderLocalState::PendingNew,
                "late order is strategy-visible but not exchange-live before latency drain") &&
         (applyDueActions(engine, 101000), true) &&
         expect(engine.orders[0].status == kRecordStatusActive, "final drain activates due order") &&
         expect(engine.open[0].state.state == trading_core::StrategyOrderLocalState::LiveLocal,
                "late order becomes exchange-live after latency drain");
}

bool testReservedCommandSequenceMetadataFailsClosed() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);

  std::array<trading_core::OrderIntent, 1> initial{
      postOnlyOrderIntent(1u, Side::Buy(), kPx100 - 300000000ll)};
  trading_core::StrategyIo seed{};
  seed.orderOut = Span<trading_core::OrderIntent>(
      initial.data(), initial.size());
  seed.orderCount = initial.size();
  applyIntents(engine, seed, 1000ll, 0u, 0u);

  std::array<trading_core::CancelIntent, 1> cancels{cancelIntent(2u, 1u)};
  std::array<trading_core::OrderIntent, 1> directOrders{
      postOnlyOrderIntent(3u, Side::Buy(), kPx100 - 200000000ll)};
  std::array<trading_core::StrategyCommandIntent, 1> commands{};
  commands[0].kind = trading_core::StrategyCommandIntentKind::Order;
  commands[0].order =
      postOnlyOrderIntent(4u, Side::Buy(), kPx100 - 100000000ll);

  trading_core::StrategyIo malformed{};
  malformed.cancelOut = Span<trading_core::CancelIntent>(
      cancels.data(), cancels.size());
  malformed.cancelCount = cancels.size();
  malformed.orderOut = Span<trading_core::OrderIntent>(
      directOrders.data(), directOrders.size());
  malformed.orderCount = directOrders.size();
  malformed.commandOut = Span<trading_core::StrategyCommandIntent>(
      commands.data(), commands.size());
  malformed.commandCount = commands.size();
  malformed.hasCommandSequence = true;
  applyIntents(engine, malformed, 2000ll, 0u, 0u);

  const bool flaggedMetadataRejected =
      engine.orders.size() == 1u && engine.open[0].state.valid &&
      engine.open[0].state.clientIntentId.raw == 1u &&
      engine.open[0].state.state ==
          trading_core::StrategyOrderLocalState::LiveLocal;

  malformed.hasCommandSequence = false;
  malformed.commandSequenceFiltered = true;
  applyIntents(engine, malformed, 3000ll, 0u, 0u);

  const bool filteredMetadataRejected =
      engine.orders.size() == 1u && engine.open[0].state.valid &&
      engine.open[0].state.clientIntentId.raw == 1u &&
      engine.open[0].state.state ==
          trading_core::StrategyOrderLocalState::LiveLocal;

  malformed.commandSequenceFiltered = false;
  commands[0].sequenceKind =
      static_cast<trading_core::StrategyCommandSequenceKind>(1u);
  commands[0].sequenceStep =
      static_cast<trading_core::StrategyCommandSequenceStep>(1u);
  commands[0].sequenceId.raw = 99u;
  applyIntents(engine, malformed, 4000ll, 0u, 0u);

  const bool rawMetadataRejected =
      engine.orders.size() == 1u && engine.open[0].state.valid &&
      engine.open[0].state.clientIntentId.raw == 1u &&
      engine.open[0].state.state ==
          trading_core::StrategyOrderLocalState::LiveLocal;

  commands[0] = trading_core::StrategyCommandIntent{};
  commands[0].kind = trading_core::StrategyCommandIntentKind::Order;
  commands[0].order =
      postOnlyOrderIntent(5u, Side::Buy(), kPx100 - 200000000ll);
  trading_core::StrategyIo ordinary{};
  ordinary.commandOut = Span<trading_core::StrategyCommandIntent>(
      commands.data(), commands.size());
  ordinary.commandCount = commands.size();
  applyIntents(engine, ordinary, 5000ll, 0u, 0u);

  return expect(flaggedMetadataRejected,
                "hasCommandSequence must reject the entire intent batch") &&
         expect(filteredMetadataRejected,
                "filtered reserved sequence metadata must reject the entire intent batch") &&
         expect(rawMetadataRejected,
                "non-default reserved sequence metadata must reject the entire intent batch") &&
         expect(engine.orders.size() == 2u,
                "ordinary single command remains executable") &&
         expect(engine.orders[1].action == kRecordActionOrder &&
                    engine.orders[1].id == 5u &&
                    engine.orders[1].status == kRecordStatusActive,
                "ordinary single command materializes its order");
}

bool testConservativeReplacementWaitsForCancelTerminal() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(1, Side::Buy(), kPx100 - 300000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<trading_core::CancelIntent, 1> cancels{cancelIntent(2, 1)};
  std::array<trading_core::OrderIntent, 1> replacements{postOnlyOrderIntent(2, Side::Buy(), kPx100 - 200000000ll)};
  io.clear();
  io.cancelOut = Span<trading_core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  applyIntents(engine, io, 2000, 0, 100);

  const bool cancelPendingWithoutNew =
      engine.orders.size() == 2u &&
      engine.orders[1].action == kRecordActionCancel &&
      engine.orders[1].status == kRecordStatusPending &&
      engine.open[0].state.valid &&
      engine.open[0].state.state ==
          trading_core::StrategyOrderLocalState::CancelPending;

  applyDueActions(engine, 102000);
  const bool terminalBeforeNew =
      engine.orders.size() == 2u &&
      engine.orders[0].status == kRecordStatusCancelled &&
      engine.orders[1].status == kRecordStatusAcked &&
      !engine.open[0].state.valid;

  io.clear();
  io.orderOut = Span<trading_core::OrderIntent>(
      replacements.data(), replacements.size());
  io.orderCount = replacements.size();
  applyIntents(engine, io, 102001, 0, 0);
  std::array<trading_core::StrategyOrderState, 4> visible{};
  const std::size_t visibleCount = visibleOrders(engine, visible.data(), visible.size());

  return expect(cancelPendingWithoutNew,
                "live mismatch emits only a pending cancel") &&
         expect(terminalBeforeNew,
                "cancel must become terminal before replacement placement") &&
         expect(engine.orders.size() == 3u,
                "next ordinary cycle writes the replacement order") &&
         expect(engine.orders[1].targetId == 1u,
                "cancel targets the original order") &&
         expect(engine.orders[2].action == kRecordActionOrder &&
                    engine.orders[2].id == 2u,
                "replacement is a later independent order") &&
         expect(engine.orders[2].type == kRecordTypeLimit &&
                    engine.orders[2].priceE8 == kPx100 - 200000000ll &&
                    engine.orders[2].postOnly,
                "replacement preserves limit price and post-only policy") &&
         expect(visibleCount == 1u &&
                    visible[0].clientIntentId.raw == 2u &&
                    visible[0].desiredPrice.raw == kPx100 - 200000000ll,
                "replacement becomes visible only after terminal cancel");
}

bool testCancelAckPublishesCanceledUserEventWithCumulativeFill() {
  EngineState engine{};
  engine.exchange = canon::kExchangeIdBinance;
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 77u;
  config.cancelOrder.baseUs = 120u;
  config.userData.baseUs = 500u;
  initLatencyRuntime(engine.latency, config);

  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(61, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  FillExecutionMetadata metadata{};
  metadata.fillReason = kFillReasonPassiveQueue;
  if (!fillOrderQty(engine, engine.open[0], kPx100, 25000000, 2000, false, metadata)) {
    return expect(false, "partial fill before cancel succeeds");
  }

  std::array<trading_core::CancelIntent, 1> cancels{cancelIntent(62, 61)};
  io.clear();
  io.cancelOut = Span<trading_core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = cancels.size();
  applyIntents(engine, io, 3000, engine.latency);
  applyDueActions(engine, 123000);

  trading_core::strategy::RuntimeClientOrderId parsed{};
  return expect(engine.userEvents.size() == 2u, "partial fill plus cancel publish two user events") &&
         expect(cxet::composite::userDataEventContractValid(engine.userEvents[0]) &&
                    cxet::composite::userDataEventContractValid(engine.userEvents[1]),
                "fill and cancel both satisfy the normalized USER contract") &&
         expect(engine.userEvents[0].eventId.raw == 1u &&
                    engine.userEvents[1].eventId.raw == 2u,
                "backtest USER sequence is monotonic across event kinds") &&
         expect(engine.userEvents[1].kind == cxet::composite::UserDataEventKind::Order, "cancel user event is order kind") &&
         expect(engine.userEvents[1].orderStatus == canon::OrderStatus::Canceled, "cancel ack publishes canceled status") &&
         expect(engine.userEvents[1].lastFilledQty.raw == 0, "cancel user event has zero last fill") &&
         expect(cxet::composite::userDataFieldPresent(
                    engine.userEvents[1],
                    cxet::composite::UserDataField::LastFilledQty),
                "cancel preserves explicit zero last-fill presence") &&
         expect(engine.userEvents[1].filledQty.raw == 25000000, "cancel user event carries cumulative filled qty") &&
         expect(trading_core::strategy::parseRuntimeClientOrderId(engine.userEvents[1].clientOrderId, parsed),
                "cancel client order id parses") &&
         expect(parsed.clientIntentId.raw == 61u, "cancel user event targets original order id") &&
         expect(engine.userEventDeliveryTsNs.size() == 2u, "cancel user event stores delivery timestamp") &&
         expect(engine.userEventDeliveryTsNs[1] == 623000, "cancel user event delivery applies user-data latency") &&
         expect(!engine.open[0].state.valid, "cancel ack clears open order");
}

bool testLatencySamplerIsReproducibleAndDeterministicWithoutJitter() {
  BacktestLatencyConfig config{};
  config.seed = 77u;
  config.userData.baseUs = 2500u;
  config.userData.jitterUs = 1000u;
  config.marketOrder.baseUs = 3100u;
  config.marketOrder.jitterUs = 0u;

  BacktestLatencyRuntime first{};
  BacktestLatencyRuntime second{};
  initLatencyRuntime(first, config);
  initLatencyRuntime(second, config);

  bool same = true;
  bool varied = false;
  std::uint64_t previous = 0u;
  for (std::uint32_t i = 0u; i < 64u; ++i) {
    const std::uint64_t a = sampleLatencyUs(first, BacktestLatencyChannel::UserData);
    const std::uint64_t b = sampleLatencyUs(second, BacktestLatencyChannel::UserData);
    same = same && a == b;
    if (i != 0u && a != previous) varied = true;
    previous = a;
  }

  return expect(same, "same latency seed produces same sequence") &&
         expect(varied, "jittered latency sequence varies") &&
         expect(sampleLatencyUs(first, BacktestLatencyChannel::MarketOrder) == 3100u,
                "zero jitter market order latency returns base") &&
         expect(latencyProfileMaxUs(config.userData) == 10000u, "user data latency cap uses built-in spike bound");
}

bool testLatencyRuntimeSplitsMarketAndLimitOrderActivation() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 5u;
  config.marketOrder.baseUs = 100u;
  config.limitOrder.baseUs = 700u;
  initLatencyRuntime(engine.latency, config);

  std::array<trading_core::OrderIntent, 2> orders{reduceMarketIntent(30, Side::Buy()), orderIntent(31, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, engine.latency);

  return expect(engine.orders.size() == 2u, "market and limit orders are recorded") &&
         expect(engine.orders[0].type == kRecordTypeMarket, "first order is market") &&
         expect(engine.orders[0].activeTsNs == 101000, "market order uses market order latency") &&
         expect(engine.orders[1].type == kRecordTypeLimit, "second order is limit") &&
         expect(engine.orders[1].activeTsNs == 701000, "limit order uses limit order latency");
}

bool testLatencyRuntimeTreatsStopMarketAsMarketOrderActivation() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 5u;
  config.marketOrder.baseUs = 100u;
  config.limitOrder.baseUs = 700u;
  initLatencyRuntime(engine.latency, config);

  std::array<trading_core::OrderIntent, 2> orders{
      stopMarketIntent(32, Side::Buy(), kPx100 + 300000000ll),
      orderIntent(33, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, engine.latency);

  return expect(engine.orders.size() == 2u, "stop-market and limit orders are recorded") &&
         expect(engine.orders[0].type == kRecordTypeStopMarket, "first order is stop-market") &&
         expect(engine.orders[0].triggerPriceE8 == kPx100 + 300000000ll, "stop-market record keeps trigger price") &&
         expect(engine.orders[0].triggerSource == static_cast<std::uint8_t>(canon::OrderTriggerSource::Last), "stop-market record defaults trigger source to last") &&
         expect(engine.orders[0].activeTsNs == 101000, "stop-market order uses market order latency") &&
         expect(engine.orders[1].activeTsNs == 701000, "limit order still uses limit order latency");
}

bool testMarketOrderAtSameTimestampWaitsForVenueTopUpdate() {
  EngineState engine{};
  setTop(engine, 120000000ll, 121000000ll);

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(70, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 0, 1, 0);

  applyDueActionsBefore(engine, 1000);
  const bool notFilledBeforeMarketEvent = engine.fills.empty() && !engine.pending.empty();

  setTop(engine, 114000000ll, 115000000ll);
  applyDueActions(engine, 1000);

  return expect(notFilledBeforeMarketEvent, "market order active at market timestamp waits for that market event") &&
         expect(engine.fills.size() == 1u, "market order fills after same-timestamp venue update") &&
         expect(engine.fills[0].priceE8 == 115000000ll, "market buy fills at updated same-timestamp ask");
}

bool testSellMarketOrderAtSameTimestampWaitsForVenueTopUpdate() {
  EngineState engine{};
  setTop(engine, 120000000ll, 121000000ll);

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(71, Side::Sell())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 0, 1, 0);

  applyDueActionsBefore(engine, 1000);
  const bool notFilledBeforeMarketEvent = engine.fills.empty() && !engine.pending.empty();

  setTop(engine, 114000000ll, 115000000ll);
  applyDueActions(engine, 1000);

  return expect(notFilledBeforeMarketEvent, "sell market order active at market timestamp waits for that market event") &&
         expect(engine.fills.size() == 1u, "sell market order fills after same-timestamp venue update") &&
         expect(engine.fills[0].priceE8 == 114000000ll, "market sell fills at updated same-timestamp bid");
}

bool testStopMarketBuyTriggersOnReconstructedAskForLastSource() {
  EngineState engine{};
  setTop(engine, 104000000ll, 105000000ll);

  std::array<trading_core::OrderIntent, 1> orders{stopMarketIntent(74, Side::Buy(), 110000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  const bool resting = engine.fills.empty() && engine.open[0].state.valid && engine.open[0].active;
  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 2000;
  trade.price.raw = 115000000ll;
  trade.qty.raw = kQty1;
  tryFillOnTrade(engine, trade, static_cast<std::int64_t>(trade.ts.raw));
  const bool tradeDidNotTrigger = engine.fills.empty() && engine.open[0].state.valid;
  setTop(engine, 109000000ll, 110000000ll);
  tryFillOnTop(engine, 3000, true);

  return expect(resting, "buy stop-market rests while ask is below trigger") &&
         expect(tradeDidNotTrigger, "buy stop-market last source ignores trade-last") &&
         expect(engine.fills.size() == 1u, "buy stop-market fills after reconstructed ask reaches trigger") &&
         expect(engine.fills[0].priceE8 == 110000000ll, "buy stop-market fills at ask like market") &&
         expect(engine.orders[0].triggerSource == static_cast<std::uint8_t>(canon::OrderTriggerSource::Last), "buy stop-market records last trigger source") &&
         expect(engine.orders[0].triggerSourcePriceE8 == 110000000ll, "buy stop-market records trigger source price") &&
         expect(engine.orders[0].triggerTsNs == 3000, "buy stop-market records trigger timestamp") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "triggered buy stop-market record is filled") &&
         expect(!engine.open[0].state.valid, "triggered buy stop-market clears open order");
}

bool testStopMarketBuyRejectsIfAlreadyTriggeredAtActivation() {
  EngineState engine{};
  setTop(engine, 114000000ll, 115000000ll);

  std::array<trading_core::OrderIntent, 1> orders{stopMarketIntent(75, Side::Buy(), 110000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.fills.empty(), "already-triggered buy stop-market does not fill") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "already-triggered buy stop-market is rejected") &&
         expect(!engine.open[0].state.valid, "rejected buy stop-market leaves no open order") &&
         expect(engine.userEvents.size() == 1u, "rejected buy stop-market publishes user event") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "stop-market reject event is rejected");
}

bool testStopMarketRejectsInvalidTriggerConfig() {
  EngineState badPrice{};
  setTop(badPrice, 114000000ll, 115000000ll);
  std::array<trading_core::OrderIntent, 1> zeroPrice{stopMarketIntent(790, Side::Buy(), 0)};
  trading_core::StrategyIo zeroIo{};
  zeroIo.orderOut = Span<trading_core::OrderIntent>(zeroPrice.data(), zeroPrice.size());
  zeroIo.orderCount = zeroPrice.size();
  applyIntents(badPrice, zeroIo, 1000, 0, 0);

  EngineState badSource{};
  setTop(badSource, 114000000ll, 115000000ll);
  std::array<trading_core::OrderIntent, 1> unknownSource{
      stopMarketIntent(791, Side::Buy(), 110000000ll, canon::OrderTriggerSource::Unknown)};
  trading_core::StrategyIo sourceIo{};
  sourceIo.orderOut = Span<trading_core::OrderIntent>(unknownSource.data(), unknownSource.size());
  sourceIo.orderCount = unknownSource.size();
  applyIntents(badSource, sourceIo, 1000, 0, 0);

  return expect(badPrice.orders[0].status == kRecordStatusRejected, "zero trigger stop-market is rejected") &&
         expect(!badPrice.open[0].state.valid, "zero trigger stop-market leaves no open order") &&
         expect(badSource.orders[0].status == kRecordStatusRejected, "unknown trigger source stop-market is rejected") &&
         expect(!badSource.open[0].state.valid, "unknown trigger source stop-market leaves no open order");
}

bool testStopMarketSellTriggersOnReconstructedBidForLastSource() {
  EngineState engine{};
  setTop(engine, 115000000ll, 116000000ll);

  std::array<trading_core::OrderIntent, 1> orders{stopMarketIntent(76, Side::Sell(), 110000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  const bool resting = engine.fills.empty() && engine.open[0].state.valid && engine.open[0].active;
  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 2000;
  trade.price.raw = 109000000ll;
  trade.qty.raw = kQty1;
  tryFillOnTrade(engine, trade, static_cast<std::int64_t>(trade.ts.raw));
  const bool tradeDidNotTrigger = engine.fills.empty() && engine.open[0].state.valid;
  setTop(engine, 110000000ll, 111000000ll);
  tryFillOnTop(engine, 3000, true);

  return expect(resting, "sell stop-market rests while bid is above trigger") &&
         expect(tradeDidNotTrigger, "sell stop-market last source ignores trade-last") &&
         expect(engine.fills.size() == 1u, "sell stop-market fills after reconstructed bid reaches trigger") &&
         expect(engine.fills[0].priceE8 == 110000000ll, "sell stop-market fills at bid like market") &&
         expect(engine.orders[0].triggerSourcePriceE8 == 110000000ll, "sell stop-market records bid trigger price") &&
         expect(engine.orders[0].triggerTsNs == 3000, "sell stop-market records trigger timestamp") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "triggered sell stop-market record is filled");
}

bool testStopMarketBuyTriggersOnMarkPrice() {
  EngineState engine{};
  setTop(engine, 104000000ll, 105000000ll);

  std::array<trading_core::OrderIntent, 1> orders{
      stopMarketIntent(77, Side::Buy(), 110000000ll, canon::OrderTriggerSource::Mark)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  setTop(engine, 114000000ll, 115000000ll);
  cxet::composite::MarkPriceRuntimeV1 mark{};
  mark.ts.raw = 2500;
  mark.markPrice.raw = 110000000ll;
  tryTriggerStopMarketsOnMark(engine, mark, 7500);

  return expect(engine.fills.size() == 1u, "mark stop-market fills after mark reaches trigger") &&
         expect(engine.fills[0].priceE8 == 115000000ll, "mark stop-market buy still executes at ask") &&
         expect(engine.orders[0].triggerSource == static_cast<std::uint8_t>(canon::OrderTriggerSource::Mark), "mark stop-market records trigger source") &&
         expect(engine.orders[0].triggerSourcePriceE8 == 110000000ll, "mark stop-market records mark trigger price") &&
         expect(engine.orders[0].triggerTsNs == 7500,
                "mark stop-market records projected venue timestamp") &&
         expect(engine.fills[0].exitTsNs == 7500,
                "mark stop-market fill uses projected venue timestamp");
}

bool testStopMarketSellTriggersOnIndexPrice() {
  EngineState engine{};
  setTop(engine, 109000000ll, 110000000ll);

  std::array<trading_core::OrderIntent, 1> orders{
      stopMarketIntent(78, Side::Sell(), 110000000ll, canon::OrderTriggerSource::Index)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  cxet::composite::IndexPriceRuntimeV1 index{};
  index.ts.raw = 2600;
  index.indexPrice.raw = 110000000ll;
  tryTriggerStopMarketsOnIndex(engine, index, 7600);

  return expect(engine.fills.size() == 1u, "index stop-market fills after index reaches trigger") &&
         expect(engine.fills[0].priceE8 == 109000000ll, "index stop-market sell still executes at bid") &&
         expect(engine.orders[0].triggerSource == static_cast<std::uint8_t>(canon::OrderTriggerSource::Index), "index stop-market records trigger source") &&
         expect(engine.orders[0].triggerSourcePriceE8 == 110000000ll, "index stop-market records index trigger price") &&
         expect(engine.orders[0].triggerTsNs == 7600,
                "index stop-market records projected venue timestamp") &&
         expect(engine.fills[0].exitTsNs == 7600,
                "index stop-market fill uses projected venue timestamp");
}

bool testMarketBuyConsumesVisibleAskLevelsWithoutMutatingBook() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 500000000ll}},
                {{10100000000ll, 40000000ll}, {10200000000ll, 60000000ll}});

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(72, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  tryFillOnTop(engine, 2000, true);

  const trading_core::LocalOrderBookView book = trading_core::orderBook(engine.venueOrderBook);
  const trading_core::LocalOrderBookSideState* asks = book.side(trading_core::BookSide::Ask);

  return expect(engine.fills.size() == 2u, "market buy writes one fill per consumed ask level") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "market buy first chunk fills at best ask") &&
         expect(engine.fills[0].qtyE8 == 40000000ll, "market buy first chunk uses best ask quantity") &&
         expect(engine.fills[1].priceE8 == 10200000000ll, "market buy second chunk fills at next ask") &&
         expect(engine.fills[1].qtyE8 == 60000000ll, "market buy second chunk uses remaining order quantity") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "market buy tags depth-aware taker reason") &&
         expect(engine.fills[0].avgPriceE8 == 10100000000ll, "first chunk avg price is first level") &&
         expect(engine.fills[1].avgPriceE8 == 10160000000ll, "second chunk records cumulative average price") &&
         expect(engine.fills[0].chunkIndex == 1u && engine.fills[0].chunkCount == 2u,
                "first market chunk records execution chunk index") &&
         expect(engine.fills[1].chunkIndex == 2u && engine.fills[1].chunkCount == 2u,
                "second market chunk records execution chunk index") &&
         expect(engine.fills[0].executionQtyE8 == kQty1, "chunk hover carries total execution qty") &&
         expect(engine.fills[0].executionAvgPriceE8 == 10160000000ll, "chunk hover carries total execution avg") &&
         expect(engine.fills[0].referencePriceE8 == 10100000000ll, "chunk hover carries reference ask") &&
         expect(engine.fills[0].slippageE8 == 60000000ll, "chunk hover carries absolute slippage") &&
         expect(engine.fills[0].slippageBpsE8 == 5940594059ll, "chunk hover carries bps slippage") &&
         expect(engine.fills[0].executionBookConsumedPctE8 == 10000000000ll,
                "chunk hover carries total consumed visible book percent") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "fully visible market buy is filled") &&
         expect(!engine.open[0].state.valid, "fully visible market buy clears open order") &&
         expect(asks != nullptr && asks->levels[0].qty.raw == 40000000ll,
                "simulated market buy does not mutate recorded ask liquidity");
}

bool testMarketBuyFallsBackToBboWhenVisibleDepthCannotFillMarket() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 500000000ll}},
                {{10100000000ll, 30000000ll}});
  setTop(engine, 9900000000ll, 10500000000ll);

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(73, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  tryFillOnTop(engine, 2000, true);

  return expect(engine.fills.size() == 1u, "market buy falls back to one BBO fill when visible depth is insufficient") &&
         expect(engine.fills[0].priceE8 == 10500000000ll, "insufficient depth market buy fills at BBO ask") &&
         expect(engine.fills[0].qtyE8 == kQty1, "insufficient depth market buy fills full order quantity") &&
         expect(engine.fills[0].remainingQtyE8 == 0, "insufficient depth BBO fallback leaves no remainder") &&
         expect(engine.fills[0].fillReason == kFillReasonTop, "insufficient depth BBO fallback keeps top fill reason") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "insufficient depth BBO fallback fills market order") &&
         expect(!engine.open[0].state.valid, "insufficient depth BBO fallback clears open order") &&
         expect(engine.netQtyRaw == kQty1, "insufficient depth BBO fallback opens full filled quantity");
}

bool testMarketBuyOverlaysBboBeforeWalkingVisibleDepth() {
  EngineState engine{};
  seedVenueBook(engine,
                {{15096000ll, 10400000000ll}},
                {{13763000ll, 18900000000ll},
                 {13769000ll, 4600000000ll},
                 {15120000ll, 198400000000ll}});
  setTopWithQty(engine, 15096000ll, 10400000000ll, 15120000ll, 198400000000ll);

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(178, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  const trading_core::LocalOrderBookView book = trading_core::orderBook(engine.venueOrderBook);
  const trading_core::LocalOrderBookSideState* asks = book.side(trading_core::BookSide::Ask);

  return expect(engine.fills.size() == 1u, "market buy overlays BBO before consuming stale ask depth") &&
         expect(engine.fills[0].priceE8 == 15120000ll, "market buy fills at reconstructed BBO ask") &&
         expect(engine.fills[0].qtyE8 == kQty1, "market buy overlay fill keeps full requested quantity") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "market buy overlay still uses depth-aware reason") &&
         expect(asks != nullptr && asks->levels[0].px.raw == 13763000ll,
                "temporary BBO overlay does not mutate stored execution depth book");
}

bool testMarketSellOverlaysBboBeforeWalkingVisibleDepth() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 12000000000ll},
                 {9700000000ll, 50000000000ll}},
                {{10400000000ll, 10000000000ll}});
  setTopWithQty(engine, 9700000000ll, 50000000000ll, 10400000000ll, 10000000000ll);

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(179, Side::Sell())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.fills.size() == 1u, "market sell overlays BBO before consuming stale bid depth") &&
         expect(engine.fills[0].priceE8 == 9700000000ll, "market sell fills at reconstructed BBO bid") &&
         expect(engine.fills[0].qtyE8 == kQty1, "market sell overlay fill keeps full requested quantity") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "market sell overlay still uses depth-aware reason");
}

bool testLimitBuyBelowBboAskIgnoresStaleDepthAsk() {
  EngineState engine{};
  seedVenueBook(engine,
                {{15096000ll, 10400000000ll}},
                {{13763000ll, 18900000000ll},
                 {15120000ll, 198400000000ll}});
  setTopWithQty(engine, 15096000ll, 10400000000ll, 15120000ll, 198400000000ll);

  std::array<trading_core::OrderIntent, 1> orders{orderIntent(180, Side::Buy(), 14000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.fills.empty(), "non-crossing limit buy ignores stale ask below reconstructed BBO") &&
         expect(engine.orders[0].status == kRecordStatusActive, "non-crossing limit buy remains active") &&
         expect(engine.open[0].state.valid, "non-crossing limit buy stays open") &&
         expect(engine.open[0].filledQtyRaw == 0, "non-crossing limit buy has no stale-depth partial fill");
}

bool testCrossingLimitBuyOverlaysBboBeforeWalkingVisibleDepth() {
  EngineState engine{};
  seedVenueBook(engine,
                {{15096000ll, 10400000000ll}},
                {{13763000ll, 18900000000ll},
                 {15120000ll, 198400000000ll}});
  setTopWithQty(engine, 15096000ll, 10400000000ll, 15120000ll, 198400000000ll);

  std::array<trading_core::OrderIntent, 1> orders{orderIntent(181, Side::Buy(), 15120000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.fills.size() == 1u, "crossing limit buy overlays BBO before consuming ask depth") &&
         expect(engine.fills[0].priceE8 == 15120000ll, "crossing limit buy fills at reconstructed BBO ask") &&
         expect(engine.fills[0].qtyE8 == kQty1, "crossing limit buy fills requested quantity at valid top") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "crossing limit buy remains depth-aware taker fill") &&
         expect(!engine.open[0].state.valid, "crossing limit buy clears when overlayed depth covers full quantity");
}

bool testLazyExecutionDepthProviderRejectsLegacyDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_legacy_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,100000000,0],[10100000000,40000000,1],[10200000000,60000000,1],1000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9900000000ll, 10100000000ll);
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const ExecutionDepthOpenStatus openStatus =
      provider.open(dir, engine.symbol, config, engine.venueOrderBook);
  engine.executionDepthProvider = &provider;

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(172, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(openStatus == ExecutionDepthOpenStatus::Failed,
                "lazy execution depth rejects the removed legacy corpus") &&
         expect(provider.failed(), "legacy depth rejection is fail-closed") &&
         expect(std::string_view{provider.failureReason()} ==
                    "manifest_contract_mismatch",
                "legacy depth rejection names the contract mismatch") &&
         expect(engine.fills.size() == 1u,
                "failed legacy provider falls back to one labeled BBO fill") &&
         expect(engine.fills[0].priceE8 == 10100000000ll,
                "legacy rejection does not leak its hidden second level");
}

bool testDeclaredMissingExecutionDepthReportsFallbackStatus() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_missing_" + std::to_string(std::rand()));
  writeCapturedDepthManifest(dir, 1u);

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const ExecutionDepthOpenStatus status = provider.open(dir, engine.symbol, config, engine.venueOrderBook);

  return expect(status == ExecutionDepthOpenStatus::Failed,
                "declared missing execution depth requests BBO fallback") &&
         expect(provider.failed(), "declared missing execution depth records failure") &&
         expect(std::string_view{provider.failureReason()} == "declared_files_missing",
                "declared missing execution depth exposes structured reason");
}

bool testUndeclaredExecutionDepthUsesPlainBboMode() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_no_depth_" + std::to_string(std::rand()));
  writeText(
      dir / "manifest.json",
      hft_backtest::test_support::capturedManifestJson(
          "binance",
          "futures",
          "BTC_USDT",
          hft_backtest::test_support::CapturedChannelCounts{
              .bookTickers = 1u}));

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const ExecutionDepthOpenStatus status = provider.open(dir, engine.symbol, config, engine.venueOrderBook);

  return expect(status == ExecutionDepthOpenStatus::Absent,
                "undeclared execution depth keeps normal BBO execution") &&
         expect(!provider.failed(), "undeclared execution depth is not a warning-worthy failure");
}

bool testLazyExecutionDepthProviderMarketBuyConsumesTapeDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_tape_" + std::to_string(std::rand()));
  writeCapturedDepthSession(
      dir,
      {{1000, 1u, {{9900000000ll, 100000000ll, 0},
                    {10100000000ll, 40000000ll, 1},
                    {10200000000ll, 60000000ll, 1}}},
       {1001, 2u, {{9900000000ll, 100000000ll, 0},
                    {10300000000ll, 100000000ll, 1}}}});

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9800000000ll, 10100000000ll);
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(174, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(opened, "lazy execution depth provider opens tape sidecar channel") &&
         expect(provider.rowsRead() == 1u, "lazy tape execution reads only first needed depth row") &&
         expect(engine.fills.size() == 2u, "lazy tape market buy writes one fill per ask level without standalone snapshot") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "lazy tape uses first ask level") &&
         expect(engine.fills[1].priceE8 == 10200000000ll, "lazy tape walks second ask level") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "lazy tape uses depth execution instead of BBO fallback") &&
         expect(engine.fills[0].fillReason != kFillReasonTop, "lazy tape must not fall back to top fill reason") &&
         expect(engine.fills[0].chunkCount == 2u, "lazy tape carries chunk count") &&
         expect(engine.fills[0].executionAvgPriceE8 == 10160000000ll, "lazy tape carries execution avg") &&
         expect(engine.fills[0].referencePriceE8 == 10100000000ll, "lazy tape carries reference ask") &&
         expect(engine.fills[0].slippageE8 == 60000000ll, "lazy tape carries absolute slippage") &&
         expect(engine.fills[0].slippageBpsE8 == 5940594059ll, "lazy tape carries bps slippage");
}

bool testStaleLazyExecutionDepthMarketBuyFallsBackToBbo() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_stale_buy_" + std::to_string(std::rand()));
  writeCapturedDepthSession(
      dir,
      {{1000, 1u, {{9900000000ll, 100000000ll, 0},
                    {10100000000ll, 100000000ll, 1}}}});

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTopAt(engine, 9900000000ll, 10400000000ll, 501002000u);
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(175, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 501002000, 0, 0);

  return expect(opened, "stale lazy depth provider opens captured fixture") &&
         expect(provider.rowsRead() == 1u, "stale lazy depth provider advances to active timestamp") &&
         expect(engine.fills.size() == 1u, "stale lazy depth market buy falls back to one BBO fill") &&
         expect(engine.fills[0].priceE8 == 10400000000ll, "stale lazy depth market buy fills at BBO ask") &&
         expect(engine.fills[0].qtyE8 == kQty1, "stale lazy depth market buy fills full order quantity") &&
         expect(engine.fills[0].fillReason == kFillReasonTop, "stale lazy depth market buy tags top fill reason");
}

bool testFreshLazyExecutionDepthMarketBuyMayUseSlightlyOlderDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_fresh_buy_" + std::to_string(std::rand()));
  writeCapturedDepthSession(
      dir,
      {{1000000, 1u, {{9900000000ll, 100000000ll, 0},
                       {10100000000ll, 40000000ll, 1},
                       {10200000000ll, 60000000ll, 1}}}});

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTopAt(engine, 9900000000ll, 10100000000ll, 26000000u);
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(176, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 26000000, 0, 0);

  return expect(opened, "fresh lazy depth provider opens captured fixture") &&
         expect(provider.rowsRead() == 1u, "fresh lazy depth provider advances to active timestamp") &&
         expect(engine.fills.size() == 2u, "fresh lazy depth market buy still consumes depth chunks") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "fresh lazy depth market buy uses best depth ask") &&
         expect(engine.fills[1].priceE8 == 10200000000ll, "fresh lazy depth market buy walks second depth ask") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "fresh lazy depth market buy stays depth-aware");
}

bool testStaleLazyExecutionDepthMarketSellFallsBackToBboBid() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_stale_sell_" + std::to_string(std::rand()));
  writeCapturedDepthSession(
      dir,
      {{1000, 1u, {{9900000000ll, 100000000ll, 0},
                    {10100000000ll, 100000000ll, 1}}}});

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTopAt(engine, 9700000000ll, 10400000000ll, 501002000u);
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(177, Side::Sell())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 501002000, 0, 0);

  return expect(opened, "stale lazy sell depth provider opens captured fixture") &&
         expect(provider.rowsRead() == 1u, "stale lazy sell depth provider advances to active timestamp") &&
         expect(engine.fills.size() == 1u, "stale lazy depth market sell falls back to one BBO fill") &&
         expect(engine.fills[0].priceE8 == 9700000000ll, "stale lazy depth market sell fills at BBO bid") &&
         expect(engine.fills[0].qtyE8 == kQty1, "stale lazy depth market sell fills full order quantity") &&
         expect(engine.fills[0].fillReason == kFillReasonTop, "stale lazy depth market sell tags top fill reason");
}

bool testLazyExecutionDepthProviderDoesNotAdvanceWithoutExecutionCandidate() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_idle_" + std::to_string(std::rand()));
  writeCapturedDepthSession(
      dir,
      {{1000, 1u, {{9900000000ll, 100000000ll, 0},
                    {10100000000ll, 100000000ll, 1}}},
       {2000, 2u, {{9900000000ll, 100000000ll, 0},
                    {10200000000ll, 100000000ll, 1}}}});

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9900000000ll, 10100000000ll);
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  tryFillOnTop(engine, 2000, true);

  return expect(opened, "lazy execution depth provider opens idle fixture") &&
         expect(provider.rowsRead() == 0u, "lazy execution depth does not advance without executable orders") &&
         expect(engine.fills.empty(), "idle lazy execution does not create fills");
}

bool testLazyExecutionDepthProviderInitializesPassiveQueueAtActivation() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_queue_" + std::to_string(std::rand()));
  writeCapturedDepthSession(
      dir,
      {{1000, 1u, {{9900000000ll, 55000000ll, 0},
                    {10100000000ll, 100000000ll, 1}}}});

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9900000000ll, 10100000000ll);
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(146, Side::Buy(), 9900000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 55000000ll;

  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 2000;
  trade.side = Side::Sell();
  trade.price.raw = 9900000000ll;
  trade.qty.raw = 70000000ll;
  tryFillOnTrade(engine, trade, static_cast<std::int64_t>(trade.ts.raw));

  return expect(opened, "lazy execution depth provider opens passive queue fixture") &&
         expect(capturedQueueAhead, "lazy execution depth captures queue ahead at activation") &&
         expect(engine.fills.size() == 1u, "lazy execution depth queue fills after trade volume consumes queue") &&
         expect(engine.fills[0].qtyE8 == 15000000ll, "lazy execution depth queue subtracts visible queue ahead") &&
         expect(engine.fills[0].queueAheadBeforeE8 == 55000000ll, "lazy execution depth queue records queue before trade") &&
         expect(engine.fills[0].queueAheadAfterE8 == 0, "lazy execution depth queue records queue after trade");
}

bool testPassiveQueueDoesNotFillFromTopQtyShrinkWithoutTrade() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 55000000ll}},
                {{10100000000ll, 100000000ll}});

  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(190, Side::Buy(), 9900000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 55000000ll;

  setTopWithQty(engine, 9900000000ll, 5000000ll, 10100000000ll, 100000000ll);
  tryFillOnTop(engine, 2000, true);

  return expect(capturedQueueAhead, "passive queue shrink test captures queue ahead at activation") &&
         expect(engine.fills.empty(), "top quantity shrink without trade does not create maker fill") &&
         expect(engine.open[0].state.valid, "resting maker order remains open after top shrink") &&
         expect(engine.open[0].queueAheadQtyRaw == 55000000ll,
                "top quantity shrink alone does not consume queue ahead");
}

bool testInitialSnapshotBookDoesNotReplaceBboFallbackWithoutExecutionDepth() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 100000000ll}},
                {{10100000000ll, 100000000ll}});
  engine.venueOrderBookExecutionEnabled = false;
  setTop(engine, 9900000000ll, 10500000000ll);

  std::array<trading_core::OrderIntent, 1> orders{marketIntent(173, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.fills.size() == 1u, "missing execution depth falls back to BBO") &&
         expect(engine.fills[0].priceE8 == 10500000000ll, "initial snapshot book does not replace BBO fallback") &&
         expect(engine.fills[0].fillReason == kFillReasonTop, "BBO fallback keeps top fill reason");
}

bool testCrossingLimitConsumesVisibleDepthThenRestsRemainder() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 500000000ll}},
                {{10100000000ll, 30000000ll}});

  std::array<trading_core::OrderIntent, 1> orders{orderIntent(74, Side::Buy(), 10100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  tryFillOnTop(engine, 2000, true);

  return expect(engine.fills.size() == 1u, "crossing limit consumes visible executable ask depth") &&
         expect(engine.fills[0].qtyE8 == 30000000ll, "crossing limit fill is capped by visible ask depth") &&
         expect(engine.fills[0].remainingQtyE8 == 70000000ll, "crossing limit records remaining passive quantity") &&
         expect(engine.orders[0].status == kRecordStatusActive, "crossing limit remainder stays active") &&
         expect(engine.open[0].state.valid, "crossing limit remainder remains open") &&
         expect(engine.open[0].filledQtyRaw == 30000000ll, "crossing limit stores cumulative filled qty");
}

bool testLatencyRuntimeUsesDedicatedCancelChannel() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 6u;
  config.limitOrder.baseUs = 700u;
  config.cancelOrder.baseUs = 120u;
  initLatencyRuntime(engine.latency, config);

  std::array<trading_core::OrderIntent, 1> orders{orderIntent(61, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  std::array<trading_core::CancelIntent, 1> cancels{cancelIntent(62, 61)};
  io.clear();
  io.cancelOut = Span<trading_core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = cancels.size();
  applyIntents(engine, io, 2000, engine.latency);

  return expect(engine.orders.size() == 2u, "cancel channel test records order and cancel") &&
         expect(engine.orders[1].action == kRecordActionCancel, "second record is cancel") &&
         expect(engine.orders[1].activeTsNs == 122000, "cancel uses dedicated cancel latency, not limit latency");
}

bool testFillPublishesRuntimeUserEvent() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{orderIntent(4, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0);
  const bool filled = fillOrder(engine, engine.open[0], kPx100, 20, false);
  trading_core::strategy::RuntimeClientOrderId parsed{};
  return expect(filled, "manual fill succeeds") &&
         expect(engine.userEvents.size() == 1u, "fill creates one user event") &&
         expect(engine.userEvents[0].kind == cxet::composite::UserDataEventKind::Order, "user event is order kind") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Closed, "user event is final fill") &&
         expect(engine.userEvents[0].lastFilledQty.raw == kQty1, "user event has last fill qty") &&
         expect(trading_core::strategy::parseRuntimeClientOrderId(engine.userEvents[0].clientOrderId, parsed), "client order id parses") &&
         expect(parsed.clientIntentId.raw == 4, "client order id carries intent id");
}

bool testFillUserEventHasSeparateDeliveryLatency() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 7u;
  config.userData.baseUs = 500u;
  initLatencyRuntime(engine.latency, config);

  std::array<trading_core::OrderIntent, 1> orders{orderIntent(64, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0);
  const bool filled = fillOrder(engine, engine.open[0], kPx100, 20, false);

  return expect(filled, "manual fill succeeds with user-data latency config") &&
         expect(engine.userEvents.size() == 1u, "fill creates one user event") &&
         expect(engine.userEventDeliveryTsNs.size() == 1u, "fill stores one user delivery timestamp") &&
         expect(engine.userEvents[0].eventTime.raw == 20u, "exchange event time remains fill timestamp") &&
         expect(engine.userEvents[0].updateTime.raw == 20u, "exchange update time remains fill timestamp") &&
         expect(engine.userEventDeliveryTsNs[0] == 500020, "user event delivery applies user-data latency");
}

bool testPostOnlyCrossingBuyRejectsWithoutTakerFill() {
  EngineState engine{};
  engine.exchange = canon::kExchangeIdBinance;
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(40, Side::Buy(), kPx100 + 100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders.size() == 1u, "post-only crossing buy is recorded") &&
         expect(engine.orders[0].postOnly, "post-only crossing buy record keeps post-only flag") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "post-only crossing buy is rejected") &&
         expect(engine.fills.empty(), "post-only crossing buy does not taker-fill") &&
         expect(!engine.open[0].state.valid, "post-only crossing buy leaves no open order") &&
         expect(engine.userEvents.size() == 1u, "post-only crossing buy publishes rejected user event") &&
         expect(cxet::composite::userDataEventContractValid(engine.userEvents[0]),
                "backtest reject satisfies the normalized USER contract") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "post-only crossing buy event is rejected");
}

bool testPostOnlyCrossingSellRejectsWithoutTakerFill() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(41, Side::Sell(), kPx100 - 100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders.size() == 1u, "post-only crossing sell is recorded") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "post-only crossing sell is rejected") &&
         expect(engine.fills.empty(), "post-only crossing sell does not taker-fill") &&
         expect(!engine.open[0].state.valid, "post-only crossing sell leaves no open order");
}

bool testStrictPostOnlyCrossingBuyRejectsWithoutTakerFill() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{strictPostOnlyOrderIntent(140, Side::Buy(), kPx100 + 100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders.size() == 1u, "strict post-only crossing buy is recorded") &&
         expect(engine.orders[0].postOnly, "strict post-only crossing buy record keeps post-only flag") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "strict post-only crossing buy is rejected") &&
         expect(engine.fills.empty(), "strict post-only crossing buy does not taker-fill") &&
         expect(!engine.open[0].state.valid, "strict post-only crossing buy leaves no open order") &&
         expect(engine.userEvents.size() == 1u, "strict post-only crossing buy publishes rejected user event") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "strict post-only crossing buy event is rejected");
}

bool testStrictPostOnlyCrossingSellRejectsWithoutTakerFill() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{strictPostOnlyOrderIntent(141, Side::Sell(), kPx100 - 100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders.size() == 1u, "strict post-only crossing sell is recorded") &&
         expect(engine.orders[0].postOnly, "strict post-only crossing sell record keeps post-only flag") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "strict post-only crossing sell is rejected") &&
         expect(engine.fills.empty(), "strict post-only crossing sell does not taker-fill") &&
         expect(!engine.open[0].state.valid, "strict post-only crossing sell leaves no open order") &&
         expect(engine.userEvents.size() == 1u, "strict post-only crossing sell publishes rejected user event") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "strict post-only crossing sell event is rejected");
}

bool testNoFreeOrderSlotPublishesRejectedUserEvent() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, kMaxOrderStates + 1u> orders{};
  for (std::size_t i = 0u; i < orders.size(); ++i) {
    orders[i] = orderIntent(1000u + i, Side::Buy(), kPx100 - 200000000ll);
  }
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();

  applyIntents(engine, io, 1000, 100, 0);

  trading_core::strategy::RuntimeClientOrderId parsed{};
  return expect(engine.orders.size() == kMaxOrderStates + 1u, "overflow order is recorded") &&
         expect(engine.orders[kMaxOrderStates].status == kRecordStatusRejected, "overflow order is rejected") &&
         expect(engine.userEvents.size() == 1u, "overflow order publishes rejected user event") &&
         expect(engine.userEventDeliveryTsNs.size() == 1u, "overflow rejection stores delivery timestamp") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "overflow user event is rejected") &&
         expect(trading_core::strategy::parseRuntimeClientOrderId(engine.userEvents[0].clientOrderId, parsed),
                "overflow rejected client order id parses") &&
         expect(parsed.clientIntentId.raw == 1000u + kMaxOrderStates, "overflow rejection carries intent id");
}

bool testPostOnlyRestingOrderDoesNotFillOnTopButFillsOnTrade() {
  EngineState engine{};
  setTop(engine, kPx100 - 200000000ll, kPx100 + 200000000ll);
  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(42, Side::Buy(), kPx100 - 100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);
  const bool policyVisible = engine.open[0].state.limitTimeInForcePolicy == trading_core::LimitTimeInForcePolicy::PostOnlyPrefer;

  setTop(engine, kPx100 - 200000000ll, kPx100);
  tryFillOnTop(engine, 2000, true);

  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 3000;
  trade.side = Side::Sell();
  trade.price.raw = kPx100 - 100000000ll;
  trade.qty.raw = kQty1;
  tryFillOnTrade(engine, trade, 7000);

  return expect(policyVisible, "post-only resting order state keeps post-only policy") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "post-only resting order fills from trade") &&
         expect(engine.fills.size() == 1u, "post-only resting order has one maker fill") &&
         expect(engine.fills[0].exitTsNs == 7000,
                "passive trade fill uses projected venue timestamp") &&
         expect(engine.fills[0].orderId == 42u, "post-only maker fill uses order id") &&
         expect(engine.userEvents.size() == 1u, "post-only maker fill publishes one user event") &&
         expect(engine.userEvents[0].maker == 1u, "post-only trade fill is maker");
}

bool testMakerFillMinCrossTicksRequiresTradeThrough() {
  EngineState engine{};
  engine.tickSizeE8 = 100000000ll;
  engine.makerFillMinCrossTicks = 1u;
  setTop(engine, kPx100 - 200000000ll, kPx100 + 200000000ll);
  const std::int64_t orderPrice = kPx100 - 100000000ll;
  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(44, Side::Buy(), orderPrice)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  cxet::composite::TradeRuntimeV1 touch{};
  touch.ts.raw = 2000;
  touch.side = Side::Sell();
  touch.price.raw = orderPrice;
  touch.qty.raw = kQty1;
  tryFillOnTrade(engine, touch, static_cast<std::int64_t>(touch.ts.raw));

  const bool noTouchFill = engine.fills.empty() && engine.open[0].state.valid;

  cxet::composite::TradeRuntimeV1 through{};
  through.ts.raw = 3000;
  through.side = Side::Sell();
  through.price.raw = orderPrice - engine.tickSizeE8;
  through.qty.raw = kQty1;
  tryFillOnTrade(engine, through,
                 static_cast<std::int64_t>(through.ts.raw));

  return expect(noTouchFill, "maker min-cross ignores touch-only trade") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "maker min-cross fills after trade-through") &&
         expect(engine.fills.size() == 1u, "maker min-cross writes one fill") &&
         expect(engine.fills[0].priceE8 == orderPrice, "maker min-cross fills at order price");
}

bool testTradeQtyPartialFillKeepsMakerOrderLiveUntilCumulativeFilled() {
  EngineState engine{};
  setTop(engine, kPx100 - 200000000ll, kPx100 + 200000000ll);
  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(43, Side::Buy(), kPx100 - 100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  cxet::composite::TradeRuntimeV1 first{};
  first.ts.raw = 2000;
  first.side = Side::Sell();
  first.price.raw = kPx100 - 100000000ll;
  first.qty.raw = 25000000;
  tryFillOnTrade(engine, first, static_cast<std::int64_t>(first.ts.raw));

  const bool partialVisible = engine.open[0].state.valid && engine.open[0].active;
  const bool partialOrderActive = engine.orders[0].status == kRecordStatusActive;
  const bool partialUserEvent = engine.userEvents.size() == 1u &&
                                engine.userEvents[0].orderStatus == canon::OrderStatus::PartiallyFilled &&
                                engine.userEvents[0].lastFilledQty.raw == 25000000 &&
                                engine.userEvents[0].filledQty.raw == 25000000;

  cxet::composite::TradeRuntimeV1 second{};
  second.ts.raw = 3000;
  second.side = Side::Sell();
  second.price.raw = kPx100 - 100000000ll;
  second.qty.raw = 75000000;
  tryFillOnTrade(engine, second, static_cast<std::int64_t>(second.ts.raw));

  return expect(partialVisible, "partial maker trade leaves order live") &&
         expect(partialOrderActive, "partial maker trade keeps order record active") &&
         expect(partialUserEvent, "partial maker trade publishes partial cumulative user event") &&
         expect(engine.fills.size() == 2u, "partial maker order writes two fills") &&
         expect(engine.fills[0].qtyE8 == 25000000, "first maker fill uses trade qty") &&
         expect(engine.fills[1].qtyE8 == 75000000, "second maker fill uses remaining trade qty") &&
         expect(engine.userEvents.size() == 2u, "final maker fill publishes second user event") &&
         expect(engine.userEvents[1].orderStatus == canon::OrderStatus::Closed, "final maker fill closes user order") &&
         expect(engine.userEvents[1].lastFilledQty.raw == 75000000, "final maker fill carries last delta") &&
         expect(engine.userEvents[1].filledQty.raw == 100000000, "final maker fill carries cumulative filled qty") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "final maker fill marks order filled") &&
         expect(!engine.open[0].state.valid, "final maker fill clears open order") &&
         expect(engine.netQtyRaw == 100000000, "partial maker fills accumulate position");
}

bool testPassiveQueueAdvancesOnlyFromOppositeTradesAtOrderPrice() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 55000000ll}},
                {{10100000000ll, 500000000ll}});

  std::array<trading_core::OrderIntent, 1> orders{postOnlyOrderIntent(46, Side::Buy(), 9900000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 55000000ll;

  cxet::composite::TradeRuntimeV1 first{};
  first.ts.raw = 2000;
  first.side = Side::Sell();
  first.price.raw = 9900000000ll;
  first.qty.raw = 50000000ll;
  tryFillOnTrade(engine, first, static_cast<std::int64_t>(first.ts.raw));
  const bool stillWaiting = engine.fills.empty() && engine.open[0].queueAheadQtyRaw == 5000000ll;

  cxet::composite::TradeRuntimeV1 second{};
  second.ts.raw = 3000;
  second.side = Side::Sell();
  second.price.raw = 9900000000ll;
  second.qty.raw = 70000000ll;
  tryFillOnTrade(engine, second, static_cast<std::int64_t>(second.ts.raw));

  return expect(capturedQueueAhead, "passive buy captures visible bid queue ahead at activation") &&
         expect(stillWaiting, "trade volume first consumes queue ahead without filling us") &&
         expect(engine.fills.size() == 1u, "second trade fills only after queue ahead is consumed") &&
         expect(engine.fills[0].qtyE8 == 65000000ll, "maker fill uses trade quantity remaining after queue ahead") &&
         expect(engine.fills[0].queueAheadBeforeE8 == 5000000ll, "maker fill records queue ahead before trade") &&
         expect(engine.fills[0].queueAheadAfterE8 == 0, "maker fill records consumed queue ahead");
}

bool testGtcCrossingLimitStillTakerFills() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{orderIntent(45, Side::Buy(), kPx100 + 100000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders[0].status == kRecordStatusFilled, "gtc crossing buy still fills") &&
         expect(engine.fills.size() == 1u, "gtc crossing buy writes fill") &&
         expect(engine.userEvents.size() == 1u, "gtc crossing buy publishes fill event") &&
         expect(engine.userEvents[0].maker == 0u, "gtc crossing buy is taker");
}

bool testReduceOnlyMarketBuyDoesNotOpenWhenFlat() {
  EngineState engine{};
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  std::array<trading_core::OrderIntent, 1> orders{reduceMarketIntent(7, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0);
  return expect(engine.netQtyRaw == 0, "reduce-only buy cannot open long when flat") &&
         expect(engine.fills.empty(), "invalid reduce-only order does not write fill") &&
         expect(engine.orders.size() == 1u, "invalid reduce-only order is recorded") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "invalid reduce-only order is rejected") &&
         expect(engine.userEvents.size() == 1u, "invalid reduce-only order publishes final user event") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "invalid reduce-only event is rejected");
}

bool testReduceOnlyMarketSellClosesLongAtBid() {
  EngineState engine{};
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0);
  std::array<trading_core::OrderIntent, 1> orders{reduceMarketIntent(5, Side::Sell())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0);
  return expect(engine.netQtyRaw == 0, "reduce-only sell flattens long") &&
         expect(engine.fills.size() == 1u, "reduce-only close writes fill") &&
         expect(engine.fills[0].priceE8 == kPx100 + 200000000ll, "long close uses bid") &&
         expect(engine.fills[0].reduceOnly, "fill is marked reduce-only") &&
         expect(engine.reduceOnlyOrders == 1u, "reduce-only order is counted") &&
         expect(engine.reduceOnlyFills == 1u, "reduce-only fill is counted") &&
         expect(engine.netRealizedPnlE8 == 200000000ll, "reduce-only close realizes bid-entry pnl");
}

bool testReduceOnlyMarketSellCapsToActualLongPosition() {
  EngineState engine{};
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  (void)updatePosition(engine, Side::Buy(), kPx100, 40000000, 100, 0);
  std::array<trading_core::OrderIntent, 1> orders{reduceMarketIntentQty(50, Side::Sell(), kQty1)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0);
  return expect(engine.netQtyRaw == 0, "oversized reduce-only sell does not open short") &&
         expect(engine.fills.size() == 1u, "oversized reduce-only close writes one capped fill") &&
         expect(engine.fills[0].qtyE8 == 40000000, "reduce-only fill qty is capped to actual long") &&
         expect(engine.fills[0].reduceOnly, "capped fill is marked reduce-only") &&
         expect(engine.orders[0].qtyE8 == kQty1, "order record keeps requested qty") &&
         expect(engine.userEvents.size() == 1u, "capped reduce-only fill publishes one user event") &&
         expect(engine.userEvents[0].lastFilledQty.raw == 40000000, "user event reports capped fill qty");
}

bool testReduceOnlyMarketBuyClosesShortAtAsk() {
  EngineState engine{};
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  (void)updatePosition(engine, Side::Sell(), kPx100, kQty1, 100, 0);
  std::array<trading_core::OrderIntent, 1> orders{reduceMarketIntent(6, Side::Buy())};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0);
  return expect(engine.netQtyRaw == 0, "reduce-only buy flattens short") &&
         expect(engine.fills.size() == 1u, "short close writes fill") &&
         expect(engine.fills[0].priceE8 == kPx100 - 200000000ll, "short close uses ask") &&
         expect(engine.netRealizedPnlE8 == 200000000ll, "short close realizes entry-ask pnl");
}

bool testReduceOnlyInt64MinShortLeavesBoundedResidual() {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  EngineState engine{};
  engine.netQtyRaw = kMin;
  engine.entryPriceE8 = kPx100;
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  std::array<trading_core::OrderIntent, 1> orders{
      reduceMarketIntentQty(67, Side::Buy(), kMax)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 200, 0, 0);

  return expect(engine.netQtyRaw == -1,
                "INT64_MIN reduce-only buy leaves the explicit one-unit short residual") &&
         expect(engine.position.netQty.raw == -1 &&
                    engine.position.entryPrice.raw == kPx100,
                "bounded reduce-only fill preserves residual position basis") &&
         expect(engine.fills.size() == 1u &&
                    engine.fills[0].qtyE8 == kMax &&
                    engine.fills[0].reduceOnly,
                "INT64_MIN reduce-only fill is capped at representable INT64_MAX") &&
         expect(engine.orders.size() == 1u &&
                    engine.orders[0].status == kRecordStatusFilled,
                "bounded reduce-only order terminates without crossing through flat");
}

bool testFeesReduceWalletAndNetPnl() {
  EngineState engine{};
  engine.initialBalanceE8 = 100000000000ll;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.takerFeeBpsE8 = 100000000ll;
  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, engine.takerFeeBpsE8);
  (void)updatePosition(engine, Side::Sell(), kPx100 + 100000000ll, kQty1, 200, engine.takerFeeBpsE8);
  return expect(engine.grossRealizedPnlE8 == 100000000ll, "gross pnl ignores fees") &&
         expect(engine.feesPaidE8 > 0, "fees are charged") &&
         expect(engine.netRealizedPnlE8 == engine.grossRealizedPnlE8 - engine.feesPaidE8, "net pnl subtracts fees") &&
         expect(engine.walletBalanceE8 == engine.initialBalanceE8 + engine.netRealizedPnlE8, "wallet tracks net pnl");
}

bool testEquityPointCapturesRealizedOnlyPnl() {
  EngineState engine{};
  engine.initialBalanceE8 = 100000000000ll;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.feesPaidE8 = 30000000ll;
  engine.grossRealizedPnlE8 = 120000000ll;
  engine.netRealizedPnlE8 = 90000000ll;
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0);

  appendEquityPoint(engine, 200);

  return expect(engine.equityPoints.size() == 1u, "equity point is appended") &&
         expect(engine.equityPoints[0].grossRealizedPnlE8 == 120000000ll, "gross realized pnl is captured") &&
         expect(engine.equityPoints[0].realizedPnlE8 == 90000000ll, "realized pnl is net pnl") &&
         expect(engine.equityPoints[0].unrealizedPnlE8 == 0, "equity point does not carry mark-to-market pnl") &&
         expect(engine.equityPoints[0].grossTotalPnlE8 == 120000000ll, "gross total stays realized-only") &&
         expect(engine.equityPoints[0].netTotalPnlE8 == 90000000ll, "net total stays realized-only") &&
         expect(engine.equityPoints[0].totalPnlE8 == engine.equityPoints[0].netTotalPnlE8, "total pnl aliases net total") &&
         expect(engine.equityPoints[0].feesPaidE8 == 30000000ll, "fees are captured as positive cost") &&
         expect(engine.equityPoints[0].walletBalanceE8 == engine.initialBalanceE8, "wallet is preserved before close") &&
         expect(engine.equityPoints[0].positionQtyE8 == kQty1, "position qty is captured");
}

bool testRiskSnapshotCapturesCompactMarkToMarketPnl() {
  EngineState engine{};
  engine.initialBalanceE8 = 100000000000ll;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.feesPaidE8 = 30000000ll;
  engine.grossRealizedPnlE8 = 120000000ll;
  engine.netRealizedPnlE8 = 90000000ll;
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0);

  appendRiskSnapshot(engine, 200, kRiskSnapshotReasonFill);

  return expect(engine.riskSnapshots.size() == 1u, "risk snapshot is appended") &&
         expect(engine.riskSnapshots[0].unrealizedPnlE8 == 200000000ll, "risk snapshot marks long position to bid") &&
         expect(engine.riskSnapshots[0].grossTotalPnlE8 == 320000000ll, "risk gross total includes unrealized pnl") &&
         expect(engine.riskSnapshots[0].netTotalPnlE8 == 290000000ll, "risk net total includes unrealized pnl") &&
         expect(engine.riskSnapshots[0].entryPriceE8 == kPx100, "risk snapshot stores entry price") &&
         expect(engine.riskSnapshots[0].bidPriceE8 == kPx100 + 200000000ll, "risk snapshot stores bid") &&
         expect(engine.riskSnapshots[0].askPriceE8 == kPx100 + 300000000ll, "risk snapshot stores ask") &&
         expect(engine.riskSnapshots[0].reason == kRiskSnapshotReasonFill, "risk snapshot stores reason");
}

bool testRiskSnapshotCadenceLimitsOpenPositionMtmRows() {
  EngineState engine{};
  setTop(engine, kPx100, kPx100 + 100000000ll);
  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0);
  appendRiskSnapshot(engine, 100, kRiskSnapshotReasonFill);

  setTop(engine, kPx100 + 100000000ll, kPx100 + 200000000ll);
  const bool early = appendRiskSnapshotIfDue(engine, 100 + 500000000ll);
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  const bool due = appendRiskSnapshotIfDue(engine, 100 + 1000000000ll);
  const bool unchanged = appendRiskSnapshotIfDue(engine, 100 + 2000000000ll);

  EngineState overflowBoundary{};
  overflowBoundary.netQtyRaw = 1;
  overflowBoundary.entryPriceE8 = kPx100;
  overflowBoundary.hasRiskSnapshot = true;
  overflowBoundary.lastRiskSnapshotTsNs =
      std::numeric_limits<std::int64_t>::max() - 500000000ll;
  overflowBoundary.lastRiskSnapshotUnrealizedPnlE8 = 0;
  setTop(overflowBoundary,
         kPx100 + 100000000ll,
         kPx100 + 200000000ll);
  const bool overflowDue = appendRiskSnapshotIfDue(
      overflowBoundary, std::numeric_limits<std::int64_t>::max());

  return expect(!early, "risk cadence skips sub-second MTM changes") &&
         expect(due, "risk cadence writes after one second") &&
         expect(!unchanged, "risk cadence skips unchanged MTM value") &&
         expect(engine.riskSnapshots.size() == 2u, "risk snapshots stay compact") &&
         expect(engine.riskSnapshots[1].reason == kRiskSnapshotReasonInterval, "cadence row is marked interval") &&
         expect(!overflowDue && overflowBoundary.riskSnapshots.empty(),
                "risk cadence fails closed when deadline exceeds INT64_MAX");
}

bool testHardLiquidationClearsStateAndBlocksNewOrders() {
  hft_backtest::BacktestRunRequest request{};
  request.initialBalanceE8 = 5000000000ll;

  EngineState engine{};
  initializeAccounting(engine, request);
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{orderIntent(40, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 100, 0);

  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0);
  setTop(engine, kPx100 - 6000000000ll, kPx100 - 5900000000ll);
  const std::int64_t breachedEquity = equityE8(engine);

  hardLiquidate(engine, 2000, breachedEquity);
  std::array<trading_core::StrategyOrderState, 4> visible{};
  io.clear();
  orders[0] = orderIntent(41, Side::Sell(), kPx100 + 200000000ll);
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 3000, 0, 0);

  return expect(breachedEquity <= 0, "test setup breaches 100 percent equity loss") &&
         expect(engine.liquidated, "engine records hard liquidation") &&
         expect(engine.liquidationTsNs == 2000, "liquidation stores timestamp") &&
         expect(engine.liquidationEquityE8 == breachedEquity, "liquidation stores breached equity") &&
         expect(engine.walletBalanceE8 == 0, "liquidation zeroes wallet") &&
         expect(engine.position.valid && engine.position.netQty.raw == 0, "liquidation flattens strategy position view") &&
         expect(engine.netQtyRaw == 0, "liquidation clears accounting position") &&
         expect(engine.pending.empty(), "liquidation clears pending actions") &&
         expect(visibleOrders(engine, visible.data(), visible.size()) == 0u, "liquidation exposes no open orders") &&
         expect(engine.riskSnapshots.back().reason == kRiskSnapshotReasonLiquidation, "liquidation writes risk snapshot reason") &&
         expect(engine.orders.size() == 1u, "liquidated engine ignores later order intents");
}

bool testTraderRiskStopCancelsOrdersKeepsPositionAndBlocksNewOrders() {
  hft_backtest::BacktestRunRequest request{};
  request.initialBalanceE8 = 5000000000ll;

  EngineState engine{};
  initializeAccounting(engine, request);
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{orderIntent(50, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0);
  setTop(engine, kPx100 - 3000000000ll, kPx100 - 2900000000ll);
  const std::int64_t breachedEquity = equityE8(engine);
  const std::int64_t thresholdEquity = 3000000000ll;

  stopForTraderRisk(engine, 2000, breachedEquity, thresholdEquity);
  std::array<trading_core::StrategyOrderState, 4> visible{};
  io.clear();
  orders[0] = orderIntent(51, Side::Sell(), kPx100 + 200000000ll);
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 3000, 0, 0);

  return expect(breachedEquity < thresholdEquity, "test setup breaches trader risk threshold") &&
         expect(!engine.liquidated, "trader risk stop is not hard liquidation") &&
         expect(engine.riskStopped, "engine records trader risk stop") &&
         expect(engine.riskStopTsNs == 2000, "risk stop stores timestamp") &&
         expect(engine.riskStopEquityE8 == breachedEquity, "risk stop stores breached equity") &&
         expect(engine.riskStopThresholdEquityE8 == thresholdEquity, "risk stop stores threshold equity") &&
         expect(engine.walletBalanceE8 == request.initialBalanceE8, "risk stop keeps wallet balance") &&
         expect(engine.position.valid && engine.position.netQty.raw == kQty1, "risk stop keeps strategy position view") &&
         expect(engine.netQtyRaw == kQty1, "risk stop keeps accounting position") &&
         expect(engine.pending.empty(), "risk stop clears pending actions") &&
         expect(visibleOrders(engine, visible.data(), visible.size()) == 0u, "risk stop exposes no open orders") &&
         expect(engine.riskSnapshots.back().reason == kRiskSnapshotReasonTraderRiskStop, "risk stop writes risk snapshot reason") &&
         expect(engine.orders.size() == 1u, "risk-stopped engine ignores later order intents");
}

bool testTraderRiskStopAfterForceCloseKeepsRiskFieldsAndFlatPosition() {
  hft_backtest::BacktestRunRequest request{};
  request.initialBalanceE8 = 5000000000ll;

  EngineState engine{};
  initializeAccounting(engine, request);
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 - 3000000000ll, kPx100 - 2900000000ll);
  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0);
  const std::int64_t breachedEquity = equityE8(engine);
  const std::int64_t thresholdEquity = 3000000000ll;

  const bool closed = forceClosePosition(engine, 2000, 0);
  stopForTraderRisk(engine, 2000, breachedEquity, thresholdEquity);

  return expect(breachedEquity < thresholdEquity, "test setup breaches trader risk threshold") &&
         expect(closed, "risk stop force close reports success") &&
         expect(engine.riskStopped, "engine records trader risk stop after close") &&
         expect(engine.riskStopEquityE8 == breachedEquity, "risk stop preserves breach equity before close") &&
         expect(engine.riskStopThresholdEquityE8 == thresholdEquity, "risk stop preserves threshold equity") &&
         expect(engine.position.valid && engine.position.netQty.raw == 0, "risk stop after force close stores flat position view") &&
         expect(engine.netQtyRaw == 0, "risk stop after force close flattens accounting position") &&
         expect(engine.orders.size() == 1u, "risk stop force close writes one close order") &&
         expect(engine.orders[0].type == kRecordTypeMarket, "risk stop force close uses market order") &&
         expect(engine.orders[0].side == kRecordSideSell, "long risk stop force close submits sell") &&
         expect(engine.orders[0].reduceOnly, "risk stop force close order is reduce-only") &&
         expect(engine.fills.size() == 1u, "risk stop force close writes a fill") &&
         expect(engine.fills[0].reduceOnly, "risk stop force close fill is reduce-only") &&
         expect(engine.riskSnapshots.back().reason == kRiskSnapshotReasonTraderRiskStop, "risk stop writes final snapshot reason");
}

bool testForceCloseOpenLongPositionUsesLazyExecutionDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_force_close_lazy_depth_" + std::to_string(std::rand()));
  writeCapturedDepthSession(
      dir,
      {{1000, 1u, {{9900000000ll, 40000000ll, 0},
                    {9800000000ll, 60000000ll, 0},
                    {10100000000ll, 100000000ll, 1}}}});

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  engine.takerFeeBpsE8 = 0;
  setTopWithQty(engine, 9900000000ll, 40000000ll, 10100000000ll, 100000000ll);
  (void)updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0);

  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  const bool closed = forceClosePosition(engine, 2000, 0);

  return expect(opened, "force-close lazy depth provider opens captured fixture") &&
         expect(closed, "lazy depth force close reports success") &&
         expect(provider.rowsRead() == 1u, "lazy depth force close advances only needed depth row") &&
         expect(engine.netQtyRaw == 0, "lazy depth force close flattens long") &&
         expect(engine.fills.size() == 2u, "lazy depth force close writes one fill per bid level") &&
         expect(engine.fills[0].priceE8 == 9900000000ll, "lazy depth force close uses best depth bid") &&
         expect(engine.fills[1].priceE8 == 9800000000ll, "lazy depth force close walks second bid") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "lazy depth force close is depth-aware") &&
         expect(engine.fills[0].chunkCount == 2u, "lazy depth force close records depth chunk count") &&
         expect(engine.fills[0].reduceOnly && engine.fills[1].reduceOnly, "lazy depth force close fills are reduce-only");
}

bool testForceCloseOpenShortPositionSubmitsReduceOnlyMarketBuy() {
  EngineState engine{};
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  (void)updatePosition(engine, Side::Sell(), kPx100, kQty1, 100, 0);

  const bool closed = forceClosePosition(engine, 200, 100);

  return expect(closed, "force close reports success") &&
         expect(engine.netQtyRaw == 0, "force close flattens short") &&
         expect(engine.orders.size() == 1u, "force close writes one order") &&
         expect(engine.orders[0].side == kRecordSideBuy, "short force close submits buy") &&
         expect(engine.orders[0].type == kRecordTypeMarket, "force close uses market order") &&
         expect(engine.orders[0].reduceOnly, "force close order is reduce-only") &&
         expect(engine.orders[0].activeTsNs == 100200, "force close honors order latency") &&
         expect(engine.fills.size() == 1u, "force close writes fill") &&
         expect(engine.fills[0].reduceOnly, "force close fill is reduce-only");
}

bool testForceCloseInt64MinPositionFailsClosed() {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  EngineState direct{};
  direct.netQtyRaw = kMin;
  direct.entryPriceE8 = kPx100;
  setTop(direct, kPx100 - 100000000ll, kPx100 + 100000000ll);
  const bool directClosed = forceClosePosition(direct, 200, 0);

  EngineState scheduled{};
  scheduled.netQtyRaw = kMin;
  scheduled.entryPriceE8 = kPx100;
  setTop(scheduled, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyRuntime latency{};
  const bool scheduledClose = forceClosePosition(scheduled, 200, latency);

  return expect(!directClosed && direct.netQtyRaw == kMin,
                "direct force close rejects unrepresentable INT64_MIN magnitude") &&
         expect(direct.orders.empty() && direct.fills.empty(),
                "direct force close leaves no partial close side effects") &&
         expect(!scheduledClose && scheduled.netQtyRaw == kMin,
                "scheduled force close rejects unrepresentable INT64_MIN magnitude") &&
         expect(scheduled.orders.empty() && scheduled.pending.empty(),
                "scheduled force close leaves no partial close side effects");
}


bool testCancelOpenOrdersAtFinishWritesCancelAcks() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<trading_core::OrderIntent, 1> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  cancelOpenOrders(engine, 2000, 100);

  return expect(engine.orders.size() == 2u, "final cleanup writes cancel command") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "final cleanup cancels active quote row") &&
         expect(engine.orders[1].action == kRecordActionCancel, "final cleanup uses cancel action") &&
         expect(engine.orders[1].targetId == 1u, "final cleanup targets live client intent id") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "final cleanup cancel is acked") &&
         expect(engine.orders[1].activeTsNs == 102000, "final cleanup honors cancel latency") &&
         expect(!engine.open[0].state.valid, "final cleanup clears open order state");
}
bool testDepthRowBuildsInitialOrderBookSnapshot() {
  DepthRow row{};
  row.tsNs = 1002;
  row.levels.push_back(PriceLevelRow{9900000000ll, 100000000ll, 0});
  row.levels.push_back(PriceLevelRow{10100000000ll, 200000000ll, 1});
  cxet::composite::OrderBookTapeRuntimeV1 tape{};
  cxet::composite::OrderBookTapeSidesRuntimeV1 sides{};
  toRuntime(row, tape, sides);
  cxet::composite::OrderBookSnapshot snapshot = toRuntimeSnapshot(row);

  trading_core::LocalOrderBookState book{};
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = 1u;
  config.tickSizeRaw = 1000000;
  Symbol symbol{};
  symbol.copyFrom("BTC_USDT");
  const bool bookReady =
      book.reserveLevelsPerSide(config.maxStoredLevelsPerSide) &&
      trading_core::applySnapshot(book, snapshot, symbol, config);

  const trading_core::LocalOrderBookView view = trading_core::orderBook(book);
  return expect(bookReady, "depth row allocates bounded local orderbook") &&
         expect(snapshot.levelCount.raw == 2u, "depth row snapshot carries levels") &&
         expect(sides.sideCount.raw == 2u, "depth row tape carries side count") &&
         expect(sides.sides[0] == Side::Buy().raw, "depth row tape maps side zero to bid") &&
         expect(sides.sides[1] == Side::Sell().raw, "depth row tape maps side one to ask") &&
         expect(book.initialized.raw == trading_core::kAlgoFlagTrue.raw, "depth row snapshot initializes local orderbook") &&
         expect(trading_core::bestBid(view).raw == 9900000000ll, "depth row snapshot sets best bid") &&
         expect(trading_core::bestAsk(view).raw == 10100000000ll, "depth row snapshot sets best ask");
}

bool testCapturedBboFreshnessUsesReplayClockCoordinates() {
  SessionData session{};
  BookTickerRow initial{};
  initial.bidPriceE8 = 9900000000ll;
  initial.bidQtyE8 = 100000000ll;
  initial.askPriceE8 = 10100000000ll;
  initial.askQtyE8 = 200000000ll;
  initial.tsNs = 1'000;
  initial.ingestSeq = 1;
  session.bookTickers.push_back(initial);

  EngineState strategyEngine{};
  EngineState venueEngine{};
  const Symbol symbol = btcUsdtSymbol();
  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = 1000000;
  Price reconstructionTick{};
  reconstructionTick.raw = instrument.tickSizeRaw;
  if (!expect(cxet::composite::configureBboReconstruction(
                  strategyEngine.bboReconstruction, reconstructionTick) &&
                  cxet::composite::configureBboReconstruction(
                      venueEngine.venueBboReconstruction, reconstructionTick),
              "freshness fixture configures strategy and venue BBO state")) {
    return false;
  }

  TimelineEvent initialEvent{};
  initialEvent.kind = kTimelineKindBookTicker;
  initialEvent.index = 0u;
  initialEvent.tsNs = 5'000;
  initialEvent.deliveryTsNs = 9'000;
  initialEvent.deliveryRealtimeNs = 1'700'000'000'000'009'000ll;
  initialEvent.receiveMonotonicNs = 123u;
  applyMarketEventToSnapshots(strategyEngine, session, initialEvent, symbol,
                              instrument, true);
  applyMarketEventToSnapshots(venueEngine, session, initialEvent, symbol,
                              instrument, false);

  BookTickerRow freshness = initial;
  freshness.ingestSeq = 2;
  freshness.freshnessOnly = true;
  session.bookTickers.push_back(freshness);
  TimelineEvent freshnessEvent = initialEvent;
  freshnessEvent.index = 1u;
  freshnessEvent.deliveryTsNs = 11'000;
  freshnessEvent.deliveryRealtimeNs =
      1'700'000'000'000'011'000ll;
  freshnessEvent.receiveMonotonicNs = 456u;
  freshnessEvent.applicationOnly = true;
  applyMarketEventToSnapshots(strategyEngine, session, freshnessEvent, symbol,
                              instrument, true);

  return expect(
             strategyEngine.snapshot.lastBookTickerRecvMono.raw == 11'000u,
             "strategy BBO freshness uses replay delivery monotonic time") &&
         expect(
             strategyEngine.snapshot.reconstructedBookTickerRecvMono.raw ==
                 11'000u,
             "reconstructed strategy BBO freshness uses replay delivery time") &&
         expect(
             strategyEngine.snapshot.lastBookTickerRecvUtc.raw ==
                 1'700'000'000'000'011'000ull,
             "strategy BBO keeps captured realtime evidence") &&
         expect(
             venueEngine.venueSnapshot.lastBookTickerRecvMono.raw == 5'000u,
             "venue BBO freshness uses exchange-projected replay coordinate") &&
         expect(
             strategyEngine.snapshot.lastBookTicker.bid.px.raw ==
                 initial.bidPriceE8 &&
                 strategyEngine.snapshot.lastBookTicker.ask.px.raw ==
                     initial.askPriceE8,
             "freshness-only BBO does not replace retained prices");
}

bool testTieredCandleHistoryBuildsCounts() {
  std::vector<CandleRow> rows{};
  rows.push_back(CandleRow{1, 1000, 10200000000ll, 9800000000ll, 500000000ll});
  rows.push_back(CandleRow{2, 900, 10300000000ll, 9700000000ll, 600000000ll});
  rows.push_back(CandleRow{3, 800, 10400000000ll, 9600000000ll, 700000000ll});
  cxet::composite::TieredCandleHistory history{};
  const bool ok = buildTieredCandleHistory(rows, history);

  return expect(ok, "tiered candle history accepts valid rows") &&
         expect(history.m1Count.raw == 1u, "tiered candle history writes m1 count") &&
         expect(history.m15Count.raw == 1u, "tiered candle history writes m15 count") &&
         expect(history.d1Count.raw == 1u, "tiered candle history writes d1 count") &&
         expect(history.m1Status == cxet::composite::CandleHistoryTierStatus::Ok, "tiered candle history marks m1 ok") &&
         expect(history.m15Status == cxet::composite::CandleHistoryTierStatus::Ok, "tiered candle history marks m15 ok") &&
         expect(history.d1Status == cxet::composite::CandleHistoryTierStatus::Ok, "tiered candle history marks d1 ok") &&
         expect(history.m1[0].quoteAmount.raw == 500000000ll, "tiered candle history preserves quote amount");
}

bool testWriterCreatesRunDirectory() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_writer_" + std::to_string(std::rand()));
  hft_backtest::BacktestRunResult result{};
  result.status = hft_backtest::Status::Ok;
  result.requestId = "run-v2";
  result.runId = "run-v2";
  result.strategy = "spread_maker1and2";
  result.indicatorProfile = "trend_score";
  result.sessionPath = dir.parent_path() / "session";
  result.configPath = dir / "source_config.ini";
  result.resultPath = dir;
  result.orderLatencyUs = 3000;
  result.cancelLatencyUs = 3000;
  result.cancelOrderLatency = hft_backtest::BacktestLatencyProfile{1200u, 400u};
  result.userDataLatency = hft_backtest::BacktestLatencyProfile{800u, 200u};
  result.events = 2;
  result.trades = 1;
  result.bookTickers = 1;
  result.initialBalanceE8 = 100000000000ll;
  result.walletBalanceE8 = 100100000000ll;
  result.totalPnlE8 = 100000000ll;
  result.depthExecution.mode = hft_backtest::BacktestDepthExecutionMode::BboFallback;
  result.depthExecution.bboFallbackFills = 2u;
  result.depthExecution.firstBboFallbackTsNs = 150;
  result.performance.loadNs = 11u;
  result.performance.replayNs = 22u;
  result.warnings.push_back("partial depth replay: dropped 1 unpaired depth_tape rows; exact replay lost");
  result.liveProbe.configured = true;
  result.liveProbe.enabled = true;
  result.liveProbe.ignored = true;
  hft_backtest::BacktestLegSummary leg{};
  leg.exchange = "binance";
  leg.market = "futures";
  leg.symbol = "BTC_USDT";
  leg.marketOrderLatency = hft_backtest::BacktestLatencyProfile{2500u, 1000u};
  leg.limitOrderLatency = hft_backtest::BacktestLatencyProfile{1800u, 700u};
  leg.cancelOrderLatency = hft_backtest::BacktestLatencyProfile{1200u, 400u};
  leg.userDataLatency = hft_backtest::BacktestLatencyProfile{800u, 200u};
  result.legs.push_back(leg);

  std::filesystem::create_directories(dir);
  {
    std::ofstream config(result.configPath, std::ios::out | std::ios::trunc);
    config << "[strategy]\ntype=spread_maker1and2\n";
  }

  EngineState engine{};
  engine.orders.push_back(JsonOrder{0, 1, 0, 100, 110, 100, kRecordActionOrder, kRecordSideBuy, kRecordTypeLimit, kRecordStatusFilled, kPx100, kQty1, false});
  JsonOrder stopOrder{};
  stopOrder.legIndex = 0;
  stopOrder.id = 2;
  stopOrder.submitTsNs = 100;
  stopOrder.activeTsNs = 120;
  stopOrder.tsNs = 100;
  stopOrder.action = kRecordActionOrder;
  stopOrder.side = kRecordSideSell;
  stopOrder.type = kRecordTypeStopMarket;
  stopOrder.status = kRecordStatusCancelled;
  stopOrder.qtyE8 = kQty1;
  stopOrder.triggerPriceE8 = kPx100 - 100000000ll;
  engine.orders.push_back(stopOrder);
  JsonOrder stopCancel{};
  stopCancel.legIndex = 0;
  stopCancel.id = 3;
  stopCancel.targetId = 2;
  stopCancel.submitTsNs = 150;
  stopCancel.activeTsNs = 160;
  stopCancel.tsNs = 150;
  stopCancel.action = kRecordActionCancel;
  stopCancel.status = kRecordStatusCancelled;
  engine.orders.push_back(stopCancel);
  engine.fills.push_back(JsonFill{0, 1, 110, 150, kRecordSideBuy, kPx100, kQty1, 0, false});
  engine.equityPoints.push_back(JsonEquityPoint{150, 0, 0, 0, 0, 0, 0, 0, 100000000000ll, kQty1, 1});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{150, 0, 0, 0, 0, 0, 0, 100000000000ll, kQty1, kPx100, kPx100 - 100000000ll, kPx100 + 100000000ll, 1, kRiskSnapshotReasonFill});
  engine.strategyIndicatorProfile = "trend_score";
  engine.strategyIndicatorTitle = "Trend score";
  engine.strategyIndicatorValueLabel = "Score";
  engine.strategyIndicatorAuxLabel = "Raw";
  engine.strategyIndicatorUnit = "score";
  engine.strategyIndicator.push_back(JsonStrategyIndicatorPoint{150, 42, 44, 1, 7, 3, 1});
  engine.strategySpread.push_back(JsonStrategySpreadPoint{150, 100000000, 90000000, 10000000, 5000000, 5000000, 1, 3, 0});
  engine.strategyRange.push_back(JsonStrategyRangePoint{150, 9900000000ll, 0, 10100000000ll});
  result.orders = engine.orders.size();
  result.fills = engine.fills.size();

  const hft_backtest::Status status = writeResult(result, engine);
  const std::string manifest = readText(dir / "manifest.json");
  const std::string orderLifetimes = readText(dir / "order_lifetimes.jsonl");
  const std::string fills = readText(dir / "fills.jsonl");
  const std::string equity = readText(dir / "equity.jsonl");
  const std::string risk = readText(dir / "risk_snapshots.jsonl");
  const std::string indicator = readText(dir / "strategy_indicator.jsonl");
  const std::string spread = readText(dir / "strategy_spread.jsonl");
  const std::string range = readText(dir / "strategy_range.jsonl");

  return expect(status == hft_backtest::Status::Ok, "v2 writer reports ok") &&
         expect(std::filesystem::is_directory(dir), "result path is a directory") &&
         expect(manifest.find("\"type\": \"run.result.v3\"") != std::string::npos, "manifest declares v3 result") &&
         expect(manifest.find("\"pipeline\": \"inline\"") != std::string::npos, "manifest declares inline execution pipeline") &&
         expect(manifest.find("\"depth_execution\": {") != std::string::npos, "manifest declares depth execution telemetry") &&
         expect(manifest.find("\"mode\": \"bbo_fallback\"") != std::string::npos, "manifest records BBO fallback mode") &&
         expect(manifest.find("\"bbo_fallback_fills\": 2") != std::string::npos, "manifest records BBO fallback fills") &&
         expect(manifest.find("\"performance\": {") != std::string::npos, "manifest declares stage timings") &&
         expect(manifest.find("\"load_ns\": 11") != std::string::npos, "manifest records load timing") &&
         expect(manifest.find("\"orders\": 3") != std::string::npos, "manifest summary includes order count") &&
         expect(manifest.find("\"order_lifetimes\": {") != std::string::npos, "manifest declares order lifetimes stream") &&
         expect(manifest.find("leg_index,order_type") != std::string::npos, "manifest declares order lifetime type") &&
         expect(manifest.find("\"cancel_order\": {\"base_us\": 1200") != std::string::npos, "manifest includes cancel latency profile") &&
         expect(manifest.find("\"user_data\": {\"base_us\": 800") != std::string::npos, "manifest includes user-data latency profile") &&
         expect(manifest.find("\"risk_snapshots\": {") != std::string::npos, "manifest declares risk snapshots stream") &&
         expect(manifest.find("fill_reason,liquidity,order_qty_e8") != std::string::npos, "manifest declares extended fill metadata") &&
         expect(manifest.find("\"5\": \"passive_bbo_touch\"") != std::string::npos, "manifest declares passive BBO-touch fill reason") &&
         expect(manifest.find("\"partial_unfilled\"") != std::string::npos, "manifest declares partial-unfilled order status") &&
         expect(manifest.find("\"strategy_indicator\": {") != std::string::npos, "manifest declares strategy indicator stream") &&
         expect(manifest.find("trend_fast_score") != std::string::npos, "manifest declares trend indicator components") &&
         expect(manifest.find("\"strategy_spread\": {") != std::string::npos, "manifest declares strategy spread stream") &&
         expect(manifest.find("\"strategy_range\": {") != std::string::npos, "manifest declares strategy range stream") &&
         expect(manifest.find("\"warnings\": [{\"message\": \"partial depth replay: dropped 1 unpaired depth_tape rows; exact replay lost\"}]") != std::string::npos,
                "manifest saves run warnings separately from errors") &&
         expect(manifest.find("\"profile\": \"trend_score\"") != std::string::npos, "indicator stream carries profile") &&
         expect(manifest.find("\"aux_label\": \"Raw\"") != std::string::npos, "trend score aux line is raw score") &&
         expect(manifest.find("\"live_probe\": {") != std::string::npos, "manifest carries live probe metadata") &&
         expect(manifest.find("\"mode\": \"ignored_backtest\"") != std::string::npos, "manifest marks live probe ignored in backtest") &&
         expect(manifest.find("\"notional_usdt_e8\"") == std::string::npos, "manifest omits removed live probe notional") &&
         expect(manifest.find("\"timeout_ms\"") == std::string::npos, "manifest omits removed live probe timeout") &&
         expect(manifest.find("\"limit_offset_spread_pct\"") == std::string::npos, "manifest omits removed live probe offset") &&
         expect(manifest.find("\"market_data_delivery\": \"captured_application_frame_arrival\"") != std::string::npos,
                "manifest labels captured market-data delivery") &&
         expect(manifest.find("\"latency_model\": \"captured_market_data_plus_synthetic_execution\"") != std::string::npos,
                "manifest separates captured market arrival from synthetic execution latency") &&
         expect(manifest.find("\"venue_schedule\": \"exchange_projected_not_after_delivery\"") != std::string::npos,
                "manifest labels the clamped exchange-projected venue plane") &&
         expect(orderLifetimes == "[110,150,10000000000,100000000,1,0,0,1]\n"
                                  "[120,160,9900000000,100000000,0,0,0,3]\n",
                "order lifetimes stream keeps limit and stop-market arrays") &&
         expect(!std::filesystem::exists(dir / "orders.jsonl"), "writer does not emit raw orders stream") &&
         expect(fills == "[1,110,150,1,10000000000,100000000,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]\n",
                "fills stream appends execution metadata after legacy fields") &&
         expect(equity == "[150,0,0,0,0,0,0,0,100000000000,100000000,1]\n", "equity stream uses compact arrays") &&
         expect(risk == "[150,0,0,0,0,0,0,100000000000,100000000,10000000000,9900000000,10100000000,1,2]\n", "risk stream uses compact arrays") &&
         expect(indicator == "[150,42,44,1,7,3,1,0,0,0,0,0,0]\n", "strategy indicator stream uses compact arrays") &&
         expect(spread == "[150,100000000,90000000,10000000,5000000,5000000,1,3,0]\n", "strategy spread stream uses compact arrays") &&
         expect(range == "[150,9900000000,0,10100000000]\n", "strategy range stream uses compact arrays") &&
         expect(std::filesystem::exists(dir / "config.ini"), "writer copies run config into result directory") &&
         expect(!std::filesystem::exists(dir.parent_path() / "run-v2.summary.json"), "writer does not create legacy summary json");
}

bool testWriterDiagnosesNoSignalBelowThreshold() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_diagnostics_no_signal_" + std::to_string(std::rand()));
  hft_backtest::BacktestRunResult result{};
  result.status = hft_backtest::Status::Ok;
  result.requestId = "run-no-signal";
  result.runId = "run-no-signal";
  result.strategy = "stat_arb_band_ladder";
  result.sessionPath = dir.parent_path() / "session";
  result.configPath = dir / "source_config.ini";
  result.resultPath = dir;
  result.events = 4;
  result.trades = 2;
  result.bookTickers = 2;
  result.orders = 0;
  result.fills = 0;

  std::filesystem::create_directories(dir);
  {
    std::ofstream config(result.configPath, std::ios::out | std::ios::trunc);
    config << "[strategy]\n"
           << "type=stat_arb_band_ladder\n"
           << "entry_edge_bps=50\n";
  }

  EngineState engine{};
  engine.strategySpread.push_back(JsonStrategySpreadPoint{100, 0, 0, 4000000000ll, 100000000ll, 3900000000ll, 1, 1, 10});
  engine.strategySpread.push_back(JsonStrategySpreadPoint{200, 0, 0, 4900000000ll, 86203413ll, 4813596587ll, 1, 1, 10});
  engine.equityPoints.push_back(JsonEquityPoint{200, 0, 0, 0, 0, 0, 0, 0, 10000000000ll, 0, 0});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{200, 0, 0, 0, 0, 0, 0, 10000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonFinal});

  const hft_backtest::Status status = writeResult(result, engine);
  const std::string manifest = readText(dir / "manifest.json");

  return expect(status == hft_backtest::Status::Ok, "diagnostic no-signal writer reports ok") &&
         expect(manifest.find("\"diagnostics\": {") != std::string::npos, "manifest writes diagnostics object") &&
         expect(manifest.find("\"zero_activity_reason\": \"no_signal_below_threshold\"") != std::string::npos,
                "diagnostics classifies no-signal threshold miss") &&
         expect(manifest.find("\"entry_edge_bps_e8\": 5000000000") != std::string::npos,
                "diagnostics captures configured entry edge threshold") &&
         expect(manifest.find("\"max_edge_after_cost_bps_e8\": 4813596587") != std::string::npos,
                "diagnostics captures max edge after cost") &&
         expect(manifest.find("{\"decision_kind\": 1, \"reason_raw\": 10, \"count\": 2}") != std::string::npos,
                "diagnostics summarizes strategy spread decision reasons");
}

bool testWriterParsesSignedDecimalAccountingBounds() {
  struct DecimalCase {
    const char* suffix;
    const char* value;
    const char* expectedRaw;
  };
  constexpr DecimalCase kCases[] = {
      {"positive-max", "92233720368.54775807", "9223372036854775807"},
      {"negative-min", "-92233720368.54775808", "-9223372036854775808"},
      {"positive-over", "92233720368.54775808", "null"},
      {"negative-over", "-92233720368.54775809", "null"},
  };

  bool ok = true;
  for (const DecimalCase& testCase : kCases) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        (std::string{"hft_backtest_decimal_bound_"} + testCase.suffix + "_" +
         std::to_string(std::rand()));
    hft_backtest::BacktestRunResult result{};
    result.status = hft_backtest::Status::Ok;
    result.requestId = testCase.suffix;
    result.runId = testCase.suffix;
    result.strategy = "stat_arb_edge_maker";
    result.sessionPath = dir.parent_path() / "session";
    result.configPath = dir / "source_config.ini";
    result.resultPath = dir;
    std::filesystem::create_directories(dir);
    {
      std::ofstream config(result.configPath,
                           std::ios::out | std::ios::trunc);
      config << "[strategy]\n"
             << "type=stat_arb_edge_maker\n"
             << "entry_edge_bps=" << testCase.value << '\n';
    }

    EngineState engine{};
    engine.equityPoints.push_back(
        JsonEquityPoint{100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    engine.riskSnapshots.push_back(JsonRiskSnapshot{
        100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        kRiskSnapshotReasonFinal});
    const hft_backtest::Status status = writeResult(result, engine);
    const std::string manifest = readText(dir / "manifest.json");
    const std::string expected =
        std::string{"\"entry_edge_bps_e8\": "} + testCase.expectedRaw;
    ok = expect(status == hft_backtest::Status::Ok,
                "decimal-bound writer reports ok") &&
         ok;
    ok = expect(manifest.find(expected) != std::string::npos,
                "decimal-bound parser preserves valid bounds and rejects overflow") &&
         ok;
  }
  return ok;
}

bool testWriterUsesSpreadDiagnosticsWithoutFullTrace() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_diagnostics_light_spread_" + std::to_string(std::rand()));
  hft_backtest::BacktestRunResult result{};
  result.status = hft_backtest::Status::Ok;
  result.requestId = "run-light-spread";
  result.runId = "run-light-spread";
  result.strategy = "stat_arb_edge_maker";
  result.sessionPath = dir.parent_path() / "session";
  result.configPath = dir / "source_config.ini";
  result.resultPath = dir;
  result.events = 4;
  result.trades = 2;
  result.bookTickers = 2;
  result.orders = 0;
  result.fills = 0;

  std::filesystem::create_directories(dir);
  {
    std::ofstream config(result.configPath, std::ios::out | std::ios::trunc);
    config << "[strategy]\n"
           << "type=stat_arb_edge_maker\n"
           << "entry_edge_bps=50\n";
  }

  EngineState engine{};
  engine.strategySpreadDiagnostics.rows = 2;
  engine.strategySpreadDiagnostics.minEdgeAfterCostBpsE8 = 3900000000ll;
  engine.strategySpreadDiagnostics.maxEdgeAfterCostBpsE8 = 4813596587ll;
  engine.strategySpreadDiagnostics.maxAbsDeviationBpsE8 = 4900000000ll;
  engine.strategySpreadDiagnostics.decisionCounts[1][10] = 2;
  engine.equityPoints.push_back(JsonEquityPoint{200, 0, 0, 0, 0, 0, 0, 0, 10000000000ll, 0, 0});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{200, 0, 0, 0, 0, 0, 0, 10000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonFinal});

  const hft_backtest::Status status = writeResult(result, engine);
  const std::string manifest = readText(dir / "manifest.json");

  return expect(status == hft_backtest::Status::Ok, "light spread diagnostics writer reports ok") &&
         expect(manifest.find("\"diagnostics\": {") != std::string::npos, "manifest writes diagnostics object") &&
         expect(manifest.find("\"zero_activity_reason\": \"no_signal_below_threshold\"") != std::string::npos,
                "light diagnostics classify no-signal threshold miss") &&
         expect(manifest.find("\"strategy_spread_rows\": 2") != std::string::npos,
                "light diagnostics preserve logical spread row count") &&
         expect(manifest.find("\"max_edge_after_cost_bps_e8\": 4813596587") != std::string::npos,
                "light diagnostics preserve max edge after cost") &&
         expect(manifest.find("{\"decision_kind\": 1, \"reason_raw\": 10, \"count\": 2}") != std::string::npos,
                "light diagnostics summarize strategy decision reasons") &&
         expect(manifest.find("\"strategy_spread\": {") == std::string::npos,
                "manifest omits full strategy spread artifact when rows were not captured") &&
         expect(!std::filesystem::exists(dir / "strategy_spread.jsonl"),
                "writer does not create full strategy spread file for light diagnostics");
}

bool testWriterDiagnosesOrdersWithoutFillsAndRateLimits() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_diagnostics_no_fills_" + std::to_string(std::rand()));
  hft_backtest::BacktestRunResult result{};
  result.status = hft_backtest::Status::Ok;
  result.requestId = "run-no-fills";
  result.runId = "run-no-fills";
  result.strategy = "stat_arb_edge_maker";
  result.sessionPath = dir.parent_path() / "session";
  result.configPath = dir / "source_config.ini";
  result.resultPath = dir;
  result.events = 4;
  result.trades = 2;
  result.bookTickers = 2;

  std::filesystem::create_directories(dir);
  {
    std::ofstream config(result.configPath, std::ios::out | std::ios::trunc);
    config << "[strategy]\n"
           << "type=stat_arb_edge_maker\n"
           << "entry_edge_bps=50\n";
  }

  EngineState engine{};
  engine.orders.push_back(JsonOrder{0, 1, 0, 100, 110, 100, kRecordActionOrder, kRecordSideBuy, kRecordTypeLimit, kRecordStatusCancelled, kPx100 - 200000000ll, kQty1, false, true});
  engine.orders.push_back(JsonOrder{0, 2, 0, 120, 130, 120, kRecordActionOrder, kRecordSideSell, kRecordTypeLimit, kRecordStatusRejected, kPx100 + 200000000ll, kQty1, false, true});
  engine.rateLimitRejectedOrders = 1;
  engine.firstRateLimitRejectTsNs = 120;
  engine.lastRateLimitRejectTsNs = 120;
  engine.equityPoints.push_back(JsonEquityPoint{200, 0, 0, 0, 0, 0, 0, 0, 10000000000ll, 0, 0});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{200, 0, 0, 0, 0, 0, 0, 10000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonFinal});
  result.orders = engine.orders.size();
  result.fills = engine.fills.size();

  const hft_backtest::Status status = writeResult(result, engine);
  const std::string manifest = readText(dir / "manifest.json");

  return expect(status == hft_backtest::Status::Ok, "diagnostic no-fill writer reports ok") &&
         expect(manifest.find("\"zero_activity_reason\": \"orders_without_fills_rate_limited\"") != std::string::npos,
                "diagnostics classifies orders without fills plus rate-limit rejects") &&
         expect(manifest.find("\"rate_limit_rejected_orders\": 1") != std::string::npos,
                "diagnostics captures rejected order count") &&
         expect(manifest.find("\"post_only_orders\": 2") != std::string::npos,
                "diagnostics counts post-only orders") &&
         expect(manifest.find("\"rejected\": 1") != std::string::npos,
                "diagnostics counts rejected order status");
}

bool staleProbeStrategyIsRejected(std::string_view strategy) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_stale_probe_" + std::to_string(std::rand()));
  const std::filesystem::path configPath = dir / "stale_probe.ini";
  std::string config =
      "[strategy]\n"
      "type=";
  config += strategy;
  config +=
      "\n"
      "enabled=1\n"
      "[venue.binance_futures]\n"
      "api_slot=1\n"
      "symbols=BTC_USDT\n";
  writeText(configPath, config);

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir / "session";
  request.configPath = configPath;
  request.strategy = std::string{strategy};
  request.runId = std::string{strategy};
  request.outputPath = dir / "backtests" / std::string{strategy};

  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  return expect(result.status == hft_backtest::Status::Unsupported, "stale probe strategy is rejected before replay") &&
         expect(result.error == "strategy descriptor is missing required callbacks",
                "stale probe rejection points at missing descriptor callbacks");
}

bool testStaleProbeStrategiesAreRejectedByRegistry() {
  return staleProbeStrategyIsRejected("trend_probe") &&
         staleProbeStrategyIsRejected("volatility_probe") &&
         staleProbeStrategyIsRejected("toxic_flow_probe");
}

bool metadataHasParam(const hft_backtest::StrategyMetadata& metadata, std::string_view key) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key && key == metadata.params[i].key) return true;
  }
  return false;
}

bool metadataHasParamGroup(const hft_backtest::StrategyMetadata& metadata,
                           std::uint8_t id,
                           std::string_view key) noexcept {
  for (std::size_t i = 0; i < metadata.paramGroupCount; ++i) {
    if (metadata.paramGroups[i].id == id && metadata.paramGroups[i].key && key == metadata.paramGroups[i].key) return true;
  }
  return false;
}

bool metadataParamGroupIs(const hft_backtest::StrategyMetadata& metadata,
                          std::string_view key,
                          std::uint8_t group,
                          bool defaultActive) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    const hft_backtest::StrategyParamMetadata& param = metadata.params[i];
    if (param.key && key == param.key) {
      return param.exclusiveGroup == group && param.defaultActive == defaultActive;
    }
  }
  return false;
}

bool metadataDisplayNameIs(const hft_backtest::StrategyMetadata& metadata, std::string_view expected) noexcept {
  return metadata.displayNameRu != nullptr && expected == metadata.displayNameRu;
}

bool metadataParamDefaultIs(const hft_backtest::StrategyMetadata& metadata,
                            std::string_view key,
                            std::string_view expected) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key && key == metadata.params[i].key) {
      return metadata.params[i].defaultValue && expected == metadata.params[i].defaultValue;
    }
  }
  return false;
}

bool testStrategyMetadataMirrorsTraderRegistry() {
  const std::size_t descriptorCount = trading_core::strategyDescriptorCount();
  if (!expect(hft_backtest::strategyMetadataCount() == descriptorCount, "metadata count mirrors trader registry")) return false;
  for (std::size_t i = 0; i < descriptorCount; ++i) {
    const trading_core::StrategyDescriptor* descriptor = trading_core::strategyDescriptorAt(i);
    const hft_backtest::StrategyMetadata* metadata = hft_backtest::strategyMetadataAt(i);
    if (!expect(descriptor != nullptr && descriptor->name != nullptr, "registry descriptor has a name")) return false;
    if (!expect(metadata != nullptr && metadata->id != nullptr, "metadata row exists for registry descriptor")) return false;
    if (!expect(std::string_view{metadata->id} == descriptor->name, "metadata id matches registry descriptor")) return false;
    if (!expect(hft_backtest::findStrategyMetadata(descriptor->name) == metadata, "metadata lookup finds registry strategy")) return false;
  }
  return expect(hft_backtest::findStrategyMetadata("removed_strategy") == nullptr, "unknown metadata is hidden");
}

bool testStatArbBandLadderKeepsTradesOptionalForBboOnlyReplay() {
  const trading_core::StrategyDescriptor* descriptor = nullptr;
  const std::size_t descriptorCount = trading_core::strategyDescriptorCount();
  for (std::size_t i = 0; i < descriptorCount; ++i) {
    const trading_core::StrategyDescriptor* candidate = trading_core::strategyDescriptorAt(i);
    if (candidate != nullptr && candidate->name != nullptr &&
        std::string_view{candidate->name} == "stat_arb_band_ladder") {
      descriptor = candidate;
      break;
    }
  }
  if (!expect(descriptor != nullptr, "stat arb band ladder descriptor is exported")) return false;

  return expect(trading_core::strategy::descriptorStreamCount(*descriptor) == 2u,
                "stat arb band ladder keeps bookticker plus optional trades") &&
         expect(trading_core::strategy::descriptorStreamAt(*descriptor, 0u) ==
                    cxet::api::market::PublicMarketDataStream::BookTicker,
                "stat arb band ladder primary descriptor stream is bookticker") &&
         expect(trading_core::strategy::descriptorStreamRequired(*descriptor, 0u),
                "stat arb band ladder requires bookticker") &&
         expect(trading_core::strategy::descriptorStreamAt(*descriptor, 1u) ==
                    cxet::api::market::PublicMarketDataStream::Trades,
                "stat arb band ladder still accepts trades when present") &&
         expect(!trading_core::strategy::descriptorStreamRequired(*descriptor, 1u),
                "stat arb band ladder does not require trades for BBO-only replay");
}

bool testStrategyMetadataExposesOnlySupportedSpreadMakerParams() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("spread_maker1and2");
  return expect(metadata != nullptr, "spread_maker1and2 metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/1and2.ini",
                "spread_maker1and2 metadata discovers strategy profile") &&
         expect(metadataDisplayNameIs(*metadata, "Спред-мейкер 1/2"),
                "spread_maker1and2 display name comes from ini metadata") &&
         expect(metadataHasParam(*metadata, "distance_bps"), "spread metadata contains distance_bps") &&
         expect(metadataHasParam(*metadata, "trigger_bps"), "spread metadata contains trigger_bps") &&
         expect(metadataHasParam(*metadata, "max_quote_age_ms"), "spread metadata contains max_quote_age_ms") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "spread metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "spread metadata contains order_qty") &&
         expect(metadataHasParamGroup(*metadata, 1u, "sizing_mode"), "spread sizing group comes from ini metadata") &&
         expect(metadataParamGroupIs(*metadata, "amount_qty", 1u, true), "spread amount_qty is active sizing default from ini") &&
         expect(metadataParamGroupIs(*metadata, "order_qty", 1u, false), "spread order_qty is inactive sizing option from ini") &&
         expect(metadataParamDefaultIs(*metadata, "max_quote_age_ms", "5000"),
                "spread max quote age default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "amount_qty", "25"), "spread amount_qty default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "0.001"), "spread order_qty default comes from commented ini template") &&
         expect(!metadataHasParam(*metadata, "fixed quote distance in basis points. 1 bps"),
                "spread metadata ignores prose comments with equals") &&
         expect(!metadataHasParam(*metadata, "minimum interval between quote refreshes in milliseconds. 0"),
                "spread metadata ignores prose comments before refresh_ms") &&
         expect(!metadataHasParam(*metadata, "min_equity_pct"),
                "spread metadata does not treat commented risk template as a strategy param") &&
         expect(!metadataHasParam(*metadata, "distance_natr_pct"), "spread metadata does not expose unsupported distance_natr_pct") &&
         expect(!metadataHasParam(*metadata, "trigger_natr_pct"), "spread metadata does not expose unsupported trigger_natr_pct") &&
         expect(!metadataHasParam(*metadata, "natr_ema_period_seconds"), "spread metadata does not expose unsupported natr_ema_period_seconds") &&
         expect(metadata->indicatorCount == 0u, "spread metadata declares no indicator profiles");
}

bool testStatArbBandLadderMetadataRequiresTwoSessions() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("stat_arb_band_ladder");
  return expect(metadata != nullptr, "stat_arb_band_ladder metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/StatArbBandLadder.ini",
                "stat arb band ladder discovers strategy profile") &&
         expect(metadataDisplayNameIs(*metadata, "Стат-арбитраж band ladder"),
                "stat arb band ladder display name comes from ini metadata") &&
         expect(metadata->minSessionCount == 2u, "stat arb band ladder requires at least two sessions") &&
         expect(metadata->maxSessionCount == 2u, "stat arb band ladder allows exactly two sessions") &&
         expect(metadataHasParam(*metadata, "ema_window_ms"), "stat arb band ladder metadata contains ema_window_ms") &&
         expect(metadataHasParam(*metadata, "entry_edge_bps"), "stat arb band ladder metadata contains entry_edge_bps") &&
         expect(metadataHasParam(*metadata, "exit_edge_bps"), "stat arb band ladder metadata contains exit_edge_bps") &&
         expect(!metadataHasParam(*metadata, "pending_timeout_ms"), "stat arb band ladder metadata hides pending_timeout_ms") &&
         expect(metadataHasParam(*metadata, "max_quote_age_ms"), "stat arb band ladder metadata exposes max_quote_age_ms") &&
         expect(!metadataHasParam(*metadata, "max_leg_skew_ms"), "stat arb band ladder metadata hides max_leg_skew_ms") &&
         expect(metadataHasParam(*metadata, "ladder_step_bps"), "stat arb band ladder metadata exposes ladder_step_bps") &&
         expect(metadataHasParam(*metadata, "max_ladder_levels"), "stat arb band ladder metadata exposes max_ladder_levels") &&
         expect(metadataParamDefaultIs(*metadata, "max_ladder_levels", "3"), "ladder levels default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "max_quote_age_ms", "5000"),
                "stat arb band ladder max quote age default comes from ini") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "stat arb band ladder metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "stat arb band ladder metadata contains order_qty") &&
         expect(metadataHasParamGroup(*metadata, 1u, "sizing_mode"), "stat arb band ladder sizing group comes from ini metadata") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "0.001"), "ladder order_qty default comes from commented ini template");
}

bool testStaleProbeProfilesDoNotCreateMetadata() {
  return expect(hft_backtest::findStrategyMetadata("trend_probe") == nullptr, "trend_probe metadata stays hidden without descriptor") &&
         expect(hft_backtest::findStrategyMetadata("volatility_probe") == nullptr,
                "volatility_probe metadata stays hidden without descriptor") &&
         expect(hft_backtest::findStrategyMetadata("toxic_flow_probe") == nullptr,
                "toxic_flow_probe metadata stays hidden without descriptor") &&
         expect(hft_backtest::findStrategyMetadata("toxic_flow_pair_probe") == nullptr,
                "toxic_flow_pair_probe metadata stays hidden without descriptor") &&
         expect(hft_backtest::findStrategyMetadata("ewma_edge_maker") == nullptr,
                "ewma_edge_maker metadata stays hidden without descriptor");
}

bool testStatArbPairwiseTakerMetadataSupportsAllPairs() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("stat_arb_pairwise_taker");
  return expect(metadata != nullptr, "stat_arb_pairwise_taker metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/StatArbPairwiseTaker.ini",
                "stat_arb_pairwise_taker metadata discovers strategy profile") &&
         expect(metadataDisplayNameIs(*metadata, "Стат-арбитраж all-pairs taker"),
                "stat_arb_pairwise_taker display name comes from ini metadata") &&
         expect(metadata->minSessionCount == 2u, "stat_arb_pairwise_taker requires at least two sessions") &&
         expect(metadata->maxSessionCount == 50u, "stat_arb_pairwise_taker allows the GUI multileg cap") &&
         expect(metadataHasParam(*metadata, "ema_window_ms"), "pairwise metadata contains ema_window_ms") &&
         expect(metadataHasParam(*metadata, "entry_edge_bps"), "pairwise metadata contains entry_edge_bps") &&
         expect(metadataHasParam(*metadata, "exit_edge_bps"), "pairwise metadata contains exit_edge_bps") &&
         expect(metadataHasParam(*metadata, "max_quote_age_ms"), "pairwise metadata contains max_quote_age_ms") &&
         expect(!metadataHasParam(*metadata, "max_active_pairs"), "pairwise metadata hides old max_active_pairs") &&
         expect(!metadataHasParam(*metadata, "max_pair_entries_per_cycle"),
                "pairwise metadata hides old max_pair_entries_per_cycle") &&
         expect(!metadataHasParam(*metadata, "full_scan_budget_pairs"),
                "pairwise metadata hides old full_scan_budget_pairs") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "pairwise metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "pairwise metadata contains order_qty") &&
         expect(metadataHasParamGroup(*metadata, 1u, "sizing_mode"), "pairwise sizing group comes from ini metadata") &&
         expect(metadataParamGroupIs(*metadata, "amount_qty", 1u, true),
                "pairwise amount_qty is active sizing default from ini") &&
         expect(metadataParamGroupIs(*metadata, "order_qty", 1u, false),
                "pairwise order_qty is inactive sizing option from ini") &&
         expect(metadataParamDefaultIs(*metadata, "max_quote_age_ms", "5000"),
                "pairwise max quote age default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "amount_qty", "10"), "pairwise amount_qty default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "0.001"),
                "pairwise order_qty default comes from commented ini template");
}

bool testStrategyMetadataExposesInputFreshnessParam() {
  constexpr std::string_view kStrategies[] = {
      "spread_maker1and2",
      "stat_arb_band_ladder",
      "stat_arb_edge_maker",
      "stat_arb_pairwise_edge_maker",
      "stat_arb_pairwise_taker",
      "uptick",
  };
  bool ok = true;
  for (std::string_view strategy : kStrategies) {
    const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata(strategy);
    ok = expect(metadata != nullptr, "strategy metadata is exported for input freshness") && ok;
    if (metadata == nullptr) continue;
    ok = expect(metadataHasParam(*metadata, "max_quote_age_ms"),
                "strategy metadata exposes max_quote_age_ms") &&
         ok;
    ok = expect(metadataParamDefaultIs(*metadata, "max_quote_age_ms", "5000"),
                "strategy metadata max_quote_age_ms default comes from ini") &&
         ok;
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = testBacktestCanonicalizesAsterSessionExchange() && ok;
  ok = testBacktestRateLimitScheduleUsesExchangePreset() && ok;
  ok = testBacktestRateLimitScheduleCanDisablePresets() && ok;
  ok = testWarmupOrderIsServiceHiddenAndMarksHot() && ok;
  ok = testBacktestWarmupGateAllowsRealOrderAfterWarmupAckMarksHot() && ok;
  ok = testRunCycleRejectsReservedSequenceMetadataBeforeApply() && ok;
  ok = testRunCycleAdmitsSimulatedUserEventSemantics() && ok;
  ok = testBacktestFeeScheduleOverridesExchangePreset() && ok;
  ok = testBacktestGlobalFeeOverrideFillsPresetSides() && ok;
  ok = testRateLimitDecisionAllowsAfterReset() && ok;
  ok = testStaleCancelDoesNotCancelReusedSlot() && ok;
  ok = testRateLimitRejectsOrderButAllowsSeparateCancelBucket() && ok;
  ok = testRepeatedRateLimitRejectsShareCooldownWindow() && ok;
  ok = testRateLimitRejectAddsRunWarningWithoutFailingRun() && ok;
  ok = testPendingDrainActivatesLateOrder() && ok;
  ok = testReservedCommandSequenceMetadataFailsClosed() && ok;
  ok = testConservativeReplacementWaitsForCancelTerminal() && ok;
  ok = testCancelAckPublishesCanceledUserEventWithCumulativeFill() && ok;
  ok = testLatencySamplerIsReproducibleAndDeterministicWithoutJitter() && ok;
  ok = testLatencyRuntimeSplitsMarketAndLimitOrderActivation() && ok;
  ok = testLatencyRuntimeTreatsStopMarketAsMarketOrderActivation() && ok;
  ok = testLatencyRuntimeUsesDedicatedCancelChannel() && ok;
  ok = testFillPublishesRuntimeUserEvent() && ok;
  ok = testFillUserEventHasSeparateDeliveryLatency() && ok;
  ok = testPostOnlyCrossingBuyRejectsWithoutTakerFill() && ok;
  ok = testPostOnlyCrossingSellRejectsWithoutTakerFill() && ok;
  ok = testStrictPostOnlyCrossingBuyRejectsWithoutTakerFill() && ok;
  ok = testStrictPostOnlyCrossingSellRejectsWithoutTakerFill() && ok;
  ok = testNoFreeOrderSlotPublishesRejectedUserEvent() && ok;
  ok = testMarketOrderAtSameTimestampWaitsForVenueTopUpdate() && ok;
  ok = testSellMarketOrderAtSameTimestampWaitsForVenueTopUpdate() && ok;
  ok = testStopMarketBuyTriggersOnReconstructedAskForLastSource() && ok;
  ok = testStopMarketBuyRejectsIfAlreadyTriggeredAtActivation() && ok;
  ok = testStopMarketRejectsInvalidTriggerConfig() && ok;
  ok = testStopMarketSellTriggersOnReconstructedBidForLastSource() && ok;
  ok = testStopMarketBuyTriggersOnMarkPrice() && ok;
  ok = testStopMarketSellTriggersOnIndexPrice() && ok;
  ok = testMarketBuyConsumesVisibleAskLevelsWithoutMutatingBook() && ok;
  ok = testMarketBuyFallsBackToBboWhenVisibleDepthCannotFillMarket() && ok;
  ok = testMarketBuyOverlaysBboBeforeWalkingVisibleDepth() && ok;
  ok = testMarketSellOverlaysBboBeforeWalkingVisibleDepth() && ok;
  ok = testLimitBuyBelowBboAskIgnoresStaleDepthAsk() && ok;
  ok = testCrossingLimitBuyOverlaysBboBeforeWalkingVisibleDepth() && ok;
  ok = testLazyExecutionDepthProviderRejectsLegacyDepth() && ok;
  ok = testDeclaredMissingExecutionDepthReportsFallbackStatus() && ok;
  ok = testUndeclaredExecutionDepthUsesPlainBboMode() && ok;
  ok = testLazyExecutionDepthProviderMarketBuyConsumesTapeDepth() && ok;
  ok = testStaleLazyExecutionDepthMarketBuyFallsBackToBbo() && ok;
  ok = testFreshLazyExecutionDepthMarketBuyMayUseSlightlyOlderDepth() && ok;
  ok = testStaleLazyExecutionDepthMarketSellFallsBackToBboBid() && ok;
  ok = testLazyExecutionDepthProviderDoesNotAdvanceWithoutExecutionCandidate() && ok;
  ok = testLazyExecutionDepthProviderInitializesPassiveQueueAtActivation() && ok;
  ok = testPassiveQueueDoesNotFillFromTopQtyShrinkWithoutTrade() && ok;
  ok = testInitialSnapshotBookDoesNotReplaceBboFallbackWithoutExecutionDepth() && ok;
  ok = testCrossingLimitConsumesVisibleDepthThenRestsRemainder() && ok;
  ok = testPostOnlyRestingOrderDoesNotFillOnTopButFillsOnTrade() && ok;
  ok = testMakerFillMinCrossTicksRequiresTradeThrough() && ok;
  ok = testTradeQtyPartialFillKeepsMakerOrderLiveUntilCumulativeFilled() && ok;
  ok = testPassiveQueueAdvancesOnlyFromOppositeTradesAtOrderPrice() && ok;
  ok = testGtcCrossingLimitStillTakerFills() && ok;
  ok = testReduceOnlyMarketBuyDoesNotOpenWhenFlat() && ok;
  ok = testReduceOnlyMarketSellClosesLongAtBid() && ok;
  ok = testReduceOnlyMarketSellCapsToActualLongPosition() && ok;
  ok = testReduceOnlyMarketBuyClosesShortAtAsk() && ok;
  ok = testReduceOnlyInt64MinShortLeavesBoundedResidual() && ok;
  ok = testFeesReduceWalletAndNetPnl() && ok;
  ok = testEquityPointCapturesRealizedOnlyPnl() && ok;
  ok = testRiskSnapshotCapturesCompactMarkToMarketPnl() && ok;
  ok = testRiskSnapshotCadenceLimitsOpenPositionMtmRows() && ok;
  ok = testHardLiquidationClearsStateAndBlocksNewOrders() && ok;
  ok = testTraderRiskStopCancelsOrdersKeepsPositionAndBlocksNewOrders() && ok;
  ok = testTraderRiskStopAfterForceCloseKeepsRiskFieldsAndFlatPosition() && ok;
  ok = testForceCloseOpenLongPositionUsesLazyExecutionDepth() && ok;
  ok = testForceCloseOpenShortPositionSubmitsReduceOnlyMarketBuy() && ok;
  ok = testForceCloseInt64MinPositionFailsClosed() && ok;
  ok = testCancelOpenOrdersAtFinishWritesCancelAcks() && ok;
  ok = testDepthRowBuildsInitialOrderBookSnapshot() && ok;
  ok = testCapturedBboFreshnessUsesReplayClockCoordinates() && ok;
  ok = testTieredCandleHistoryBuildsCounts() && ok;
  ok = testWriterCreatesRunDirectory() && ok;
  ok = testWriterDiagnosesNoSignalBelowThreshold() && ok;
  ok = testWriterParsesSignedDecimalAccountingBounds() && ok;
  ok = testWriterUsesSpreadDiagnosticsWithoutFullTrace() && ok;
  ok = testWriterDiagnosesOrdersWithoutFillsAndRateLimits() && ok;
  ok = testStaleProbeStrategiesAreRejectedByRegistry() && ok;
  ok = testStrategyMetadataMirrorsTraderRegistry() && ok;
  ok = testStatArbBandLadderKeepsTradesOptionalForBboOnlyReplay() && ok;
  ok = testStrategyMetadataExposesOnlySupportedSpreadMakerParams() && ok;
  ok = testStatArbBandLadderMetadataRequiresTwoSessions() && ok;
  ok = testStaleProbeProfilesDoNotCreateMetadata() && ok;
  ok = testStatArbPairwiseTakerMetadataSupportsAllPairs() && ok;
  ok = testStrategyMetadataExposesInputFreshnessParam() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
