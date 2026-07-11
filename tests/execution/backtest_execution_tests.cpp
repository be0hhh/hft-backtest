#include "engine/backtest_engine.hpp"

#include "accounting/backtest_result_aggregation.hpp"
#include "execution/backtest_execution_depth.hpp"
#include "execution/backtest_latency.hpp"
#include "config/backtest_run_config.hpp"
#include "strategy/backtest_strategy_runtime.hpp"

#include "hft_trader/api/user/UserDataTools.hpp"
#include "hft_trader/core/rate_limit/RateLimit.hpp"
#include "hft_trader/core/rate_limit/RateLimitPresets.hpp"
#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"
#include "hft_trader/runtime/runner/StrategyMarketDataRuntime.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
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

hft_trader::core::OrderIntent orderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Limit;
  out.price = price(px);
  out.qty = amount(kQty1);
  return out;
}

hft_trader::core::OrderIntent postOnlyOrderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  hft_trader::core::OrderIntent out = orderIntent(orderId, side, px);
  out.limitTimeInForcePolicy = hft_trader::core::LimitTimeInForcePolicy::PostOnlyPrefer;
  return out;
}

hft_trader::core::OrderIntent strictPostOnlyOrderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  hft_trader::core::OrderIntent out = orderIntent(orderId, side, px);
  out.limitTimeInForcePolicy = hft_trader::core::LimitTimeInForcePolicy::PostOnlyStrict;
  return out;
}

hft_trader::core::OrderIntent reduceMarketIntent(std::uint64_t orderId, Side side) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Market;
  out.qty = amount(kQty1);
  out.reduceOnly = true;
  return out;
}

hft_trader::core::OrderIntent reduceMarketIntentQty(std::uint64_t orderId, Side side, std::int64_t qtyRaw) noexcept {
  hft_trader::core::OrderIntent out = reduceMarketIntent(orderId, side);
  out.qty = amount(qtyRaw);
  return out;
}

hft_trader::core::OrderIntent marketIntent(std::uint64_t orderId, Side side) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Market;
  out.qty = amount(kQty1);
  return out;
}

hft_trader::core::OrderIntent stopMarketIntent(std::uint64_t orderId,
                                               Side side,
                                               std::int64_t triggerPx,
                                               canon::OrderTriggerSource triggerSource = canon::OrderTriggerSource::Last) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Stop;
  out.triggerPrice = price(triggerPx);
  out.triggerSource = triggerSource;
  out.qty = amount(kQty1);
  return out;
}

hft_trader::core::CancelIntent cancelIntent(std::uint64_t commandId, std::uint64_t targetId) noexcept {
  hft_trader::core::CancelIntent out{};
  out.clientIntentId = id(commandId);
  out.targetClientIntentId = id(targetId);
  out.targetOrderType = canon::OrderType::Limit;
  return out;
}

std::array<hft_trader::core::StrategyCommandIntent, 2> amendCommands(
    hft_trader::core::StrategyCommandSequenceKind kind,
    const hft_trader::core::OrderIntent& replacement,
    const hft_trader::core::CancelIntent& cancel) noexcept {
  std::array<hft_trader::core::StrategyCommandIntent, 2> out{};
  const bool cancelThenNew =
      kind == hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew;
  hft_trader::core::StrategyCommandIntent& first = out[0];
  hft_trader::core::StrategyCommandIntent& second = out[1];
  first.kind = cancelThenNew
      ? hft_trader::core::StrategyCommandIntentKind::Cancel
      : hft_trader::core::StrategyCommandIntentKind::Order;
  second.kind = cancelThenNew
      ? hft_trader::core::StrategyCommandIntentKind::Order
      : hft_trader::core::StrategyCommandIntentKind::Cancel;
  if (cancelThenNew) {
    first.cancel = cancel;
    second.order = replacement;
  } else {
    first.order = replacement;
    second.cancel = cancel;
  }
  first.sequenceKind = kind;
  second.sequenceKind = kind;
  first.sequenceStep = hft_trader::core::StrategyCommandSequenceStep::First;
  second.sequenceStep = hft_trader::core::StrategyCommandSequenceStep::Second;
  first.sequenceId = replacement.clientIntentId;
  second.sequenceId = replacement.clientIntentId;
  return out;
}

void setTop(EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  cxet::composite::BookTickerRuntimeV1 top{};
  top.bid.px.raw = bid;
  top.ask.px.raw = ask;
  hft_trader::core::copyBookTickerSnapshot(engine.snapshot, top);
  hft_trader::core::copyBookTickerSnapshot(engine.venueSnapshot, top);
  engine.lastStrategySourceMarketDataTsNs = 1000u;
  engine.snapshot.lastBookTicker.ts.raw = 1000u;
  engine.venueSnapshot.lastBookTicker.ts.raw = 1000u;
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
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  hft_trader::core::applySnapshot(engine.venueOrderBook, snapshot, engine.symbol, config);
  hft_trader::core::bindOrderBookSnapshot(engine.venueSnapshot, engine.venueOrderBook);
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

const hft_trader::core::RateLimitBucketConfig* findRateLimitBucket(
    const BacktestRateLimitSchedule& schedule,
    hft_trader::core::RateLimitBucketKind kind,
    std::uint64_t intervalNs) noexcept {
  for (const hft_trader::core::RateLimitBucketConfig& bucket : schedule.buckets) {
    if (bucket.enabled && bucket.kind == kind && bucket.intervalNs == intervalNs) return &bucket;
  }
  return nullptr;
}

bool testBacktestRateLimitScheduleUsesExchangePreset() {
  BacktestRunRequest request{};
  const BacktestRateLimitSchedule schedule = resolveRateLimitSchedule(request, "binance", "futures");
  const hft_trader::core::RateLimitBucketConfig* tenSecond = findRateLimitBucket(
      schedule, hft_trader::core::RateLimitBucketKind::Orders, 10000000000ull);
  const hft_trader::core::RateLimitBucketConfig* minute = findRateLimitBucket(
      schedule, hft_trader::core::RateLimitBucketKind::Orders, 60000000000ull);
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

  hft_trader::core::WarmupOrderIntent warmup{};
  warmup.order = strictPostOnlyOrderIntent(500u, Side::Buy(), kPx100 - 200000000ll);
  warmup.order.qty = amount(kQty1 / 10);
  warmup.order.service = true;
  warmup.cancelTimeoutMs = 250u;
  warmup.cancelAfterAck = true;

  hft_trader::core::StrategyCommandIntent command{};
  command.kind = hft_trader::core::StrategyCommandIntentKind::WarmupOrder;
  command.warmup = warmup;
  hft_trader::core::StrategyIo io{};
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(&command, 1u);
  io.commandCount = 1u;

  applyIntents(engine, io, 1000ll, 0u, 0u);

  hft_trader::core::StrategyOrderState visible[4]{};
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

bool warmupGateProbeRunCycle(void* rawState, hft_trader::core::StrategyContext& ctx) noexcept {
  auto* state = static_cast<WarmupGateProbeState*>(rawState);
  if (!state || ctx.io.commandOut.size() == 0u) return false;
  hft_trader::core::StrategyCommandIntent command{};
  command.kind = hft_trader::core::StrategyCommandIntentKind::Order;
  command.order.clientIntentId.raw = state->nextClientId++;
  command.order.side = Side::Buy();
  command.order.orderType = canon::OrderType::Market;
  command.order.qty = amount(kQty1);
  command.order.routeHandle = hft_trader::core::ExecutionRouteHandle{1u};
  ctx.io.commandOut[0] = command;
  ctx.io.commandCount = 1u;
  ++state->cycles;
  return true;
}

void warmupGateProbeCaptureState(const void* rawState, hft_trader::runtime::StrategyStateSnapshot& out) noexcept {
  const auto* state = static_cast<const WarmupGateProbeState*>(rawState);
  out = hft_trader::runtime::StrategyStateSnapshot{};
  if (!state) return;
  out.cycles = state->cycles;
  out.nextClientIntentId.raw = state->nextClientId;
}

void warmupGateProbeRestoreState(void* rawState, const hft_trader::runtime::StrategyStateSnapshot& before) noexcept {
  auto* state = static_cast<WarmupGateProbeState*>(rawState);
  if (!state) return;
  state->cycles = before.cycles;
  state->nextClientId = before.nextClientIntentId.raw;
}

bool testBacktestWarmupGateAllowsRealOrderAfterWarmupAckMarksHot() {
  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "warmup_gate_probe";
  descriptor.streamCount = 0u;
  descriptor.runCycle = warmupGateProbeRunCycle;
  descriptor.captureState = warmupGateProbeCaptureState;
  descriptor.restoreAfterOrderFailure = warmupGateProbeRestoreState;

  std::vector<EngineState> engines(1u);
  EngineState& engine = engines[0];
  engine.exchange = canon::kExchangeIdBinance;
  engine.market = canon::kMarketTypeFutures;
  setTop(engine, kPx100, kPx100 + 100000000ll);
  BacktestLatencyConfig latency{};
  latency.marketOrder = BacktestLatencyProfile{};
  latency.limitOrder = BacktestLatencyProfile{};
  latency.cancelOrder = BacktestLatencyProfile{};
  initLatencyRuntime(engine.latency, latency);

  std::vector<StrategyInstrumentSpec> instruments(1u);
  instruments[0].tickSizeRaw = 100000000ll;
  instruments[0].stepSizeRaw = 100000000ll;
  instruments[0].tickGrid = hft_trader::core::prepareFastStepGrid(instruments[0].tickSizeRaw);
  instruments[0].qtyStepGrid = hft_trader::core::prepareFastStepGrid(instruments[0].stepSizeRaw);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  scratch->reset(1u);
  WarmupGateProbeState state{};
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool warmupOk = hft_backtest::runCycleMulti(
      descriptor, &state, engines, instruments, *scratch, shard, control, 0u, 1000ll, request, nullptr);
  const std::size_t ordersAfterWarmup = engine.orders.size();
  const std::uint64_t hotMonoAfterWarmup = engine.lastOrderPathHotMono.raw;

  const bool realOrderOk = hft_backtest::runCycleMulti(
      descriptor, &state, engines, instruments, *scratch, shard, control, 0u, 1001ll, request, nullptr);

  return expect(warmupOk, "first cycle emits service warmup order") &&
         expect(ordersAfterWarmup == 2u, "warmup order and cancel are recorded") &&
         expect(engine.orders[0].warmup && engine.orders[0].service, "first order is service warmup") &&
         expect(hotMonoAfterWarmup == 1000u, "warmup ack marks route hot") &&
         expect(realOrderOk, "second cycle runs after warmup") &&
         expect(engine.fills.size() == 1u, "route-hot second cycle sends real market order and fills it") &&
         expect(engine.userEvents.size() == 1u, "real fill publishes strategy user event") &&
         expect(engine.orders.size() == 3u, "no second warmup is inserted once route is hot") &&
         expect(!engine.orders.back().service && !engine.orders.back().warmup, "final order is real strategy order");
}

bool amendFilterProbeRunCycle(void* rawState, hft_trader::core::StrategyContext& ctx) noexcept {
  auto* state = static_cast<WarmupGateProbeState*>(rawState);
  if (!state || ctx.io.commandOut.size() < 2u) return false;
  hft_trader::core::OrderIntent replacement =
      postOnlyOrderIntent(state->nextClientId, Side::Buy(), kPx100 - 200000000ll);
  replacement.routeHandle.raw = 2u;
  replacement.quoteSlotId.raw = 7u;
  hft_trader::core::CancelIntent cancel = cancelIntent(state->nextClientId + 1u, 900u);
  cancel.routeHandle = replacement.routeHandle;
  cancel.quoteSlotId = replacement.quoteSlotId;
  const auto commands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      replacement,
      cancel);
  ctx.io.commandOut[0] = commands[0];
  ctx.io.commandOut[1] = commands[1];
  ctx.io.commandCount = commands.size();
  ctx.io.hasCommandSequence = true;
  state->nextClientId += 2u;
  ++state->cycles;
  return true;
}

bool testBacktestTradabilityFilterDropsWholeAmendAndRestoresState() {
  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "amend_filter_probe";
  descriptor.streamCount = 0u;
  descriptor.runCycle = amendFilterProbeRunCycle;
  descriptor.captureState = warmupGateProbeCaptureState;
  descriptor.restoreAfterOrderFailure = warmupGateProbeRestoreState;

  std::vector<EngineState> engines(2u);
  for (std::size_t i = 0u; i < engines.size(); ++i) {
    engines[i].exchange = canon::kExchangeIdBinance;
    engines[i].market = canon::kMarketTypeFutures;
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    setTop(engines[i], kPx100 - 100000000ll, kPx100 + 100000000ll);
  }
  std::vector<StrategyInstrumentSpec> instruments(2u);
  for (StrategyInstrumentSpec& instrument : instruments) {
    instrument.tickSizeRaw = 100000000ll;
    instrument.stepSizeRaw = 100000000ll;
    instrument.tickGrid = hft_trader::core::prepareFastStepGrid(instrument.tickSizeRaw);
    instrument.qtyStepGrid = hft_trader::core::prepareFastStepGrid(instrument.stepSizeRaw);
  }
  auto scratch = std::make_unique<MultiLegCycleScratch>();
  scratch->reset(engines.size());
  WarmupGateProbeState state{};
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};
  request.tradeMode = BacktestTradeMode::PrimaryOnly;
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 0u;

  const bool ok = hft_backtest::runCycleMulti(
      descriptor, &state, engines, instruments, *scratch, shard, control, 0u, 1000ll, request, nullptr);

  return expect(ok, "tradability-filtered amend does not abort replay") &&
         expect(state.cycles == 0u, "tradability-filtered amend restores strategy cycle state") &&
         expect(state.nextClientId == 1u, "tradability-filtered amend restores client id allocation") &&
         expect(engines[0].orders.empty() && engines[1].orders.empty(),
                "tradability filter emits neither half of amend pair");
}

bool twoAmendFailureProbeRunCycle(void* rawState, hft_trader::core::StrategyContext& ctx) noexcept {
  auto* state = static_cast<WarmupGateProbeState*>(rawState);
  if (!state || ctx.io.commandOut.size() < 4u) return false;
  const auto first = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      postOnlyOrderIntent(state->nextClientId, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(1000u + state->nextClientId, 100u));
  const auto second = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      postOnlyOrderIntent(state->nextClientId + 1u, Side::Sell(), kPx100 + 200000000ll),
      cancelIntent(1001u + state->nextClientId, 101u));
  ctx.io.commandOut[0] = first[0];
  ctx.io.commandOut[1] = first[1];
  ctx.io.commandOut[2] = second[0];
  ctx.io.commandOut[3] = second[1];
  ctx.io.commandCount = 4u;
  ctx.io.hasCommandSequence = true;
  state->nextClientId += 2u;
  ++state->cycles;
  return true;
}

bool testBacktestFailedLaterAmendCleansEarlierAcceptedReplacement() {
  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "two_amend_failure_probe";
  descriptor.streamCount = 0u;
  descriptor.runCycle = twoAmendFailureProbeRunCycle;
  descriptor.captureState = warmupGateProbeCaptureState;
  descriptor.restoreAfterOrderFailure = warmupGateProbeRestoreState;

  EngineState engine{};
  engine.exchange = canon::kExchangeIdBinance;
  engine.market = canon::kMarketTypeFutures;
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 2> initial{
      postOnlyOrderIntent(100u, Side::Buy(), kPx100 - 300000000ll),
      postOnlyOrderIntent(101u, Side::Sell(), kPx100 + 300000000ll)};
  hft_trader::core::StrategyIo seed{};
  seed.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  seed.orderCount = initial.size();
  (void)applyIntents(engine, seed, 1000ll, 0u, 0u);
  engine.lastOrderPathHotMono.raw = 1000u;

  hft_trader::core::RateLimitBucketConfig orders{};
  orders.kind = hft_trader::core::RateLimitBucketKind::Orders;
  orders.limit = 1;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);
  hft_trader::core::RateLimitActionConfig orderAction{};
  orderAction.action = hft_trader::core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1u;
  orderAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = 100000000ll;
  instrument.stepSizeRaw = 100000000ll;
  instrument.tickGrid = hft_trader::core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid = hft_trader::core::prepareFastStepGrid(instrument.stepSizeRaw);
  auto scratch = std::make_unique<SingleLegCycleScratch>();
  WarmupGateProbeState state{};
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool ok = hft_backtest::runCycle(
      descriptor, &state, engine, instrument, *scratch, shard, control, 2000ll, request, nullptr);
  std::array<hft_trader::core::StrategyOrderState, 8> visible{};
  const std::size_t visibleCount = visibleOrders(engine, visible.data(), visible.size());

  return expect(ok, "later amend send failure does not abort replay") &&
         expect(state.cycles == 0u && state.nextClientId == 1u,
                "later amend failure restores full strategy cycle state") &&
         expect(visibleCount == 0u,
                "rollback cleanup removes earlier replacement accepted in the failed cycle") &&
         expect(engine.orders.back().action == kRecordActionCancel,
                "rollback emits cleanup cancel for earlier accepted replacement");
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
  hft_trader::core::RateLimitState limits{};
  hft_trader::core::RateLimitBucketConfig bucket{};
  bucket.kind = hft_trader::core::RateLimitBucketKind::Orders;
  bucket.limit = 1;
  bucket.intervalNs = 100u;
  bucket.enabled = true;
  hft_trader::core::RateLimitActionConfig action{};
  action.action = hft_trader::core::RateLimitActionKind::LimitOrder;
  action.enabled = true;
  action.costCount = 1u;
  action.costs[0].bucket = hft_trader::core::RateLimitBucketKind::Orders;
  action.costs[0].cost = 1;
  TimeNs first{};
  first.raw = 1u;
  TimeNs blockedTs{};
  blockedTs.raw = 2u;

  const bool configured = limits.configureBucket(bucket) && limits.configureAction(action);
  const hft_trader::core::RateLimitDecision firstDecision =
      limits.consumeDecision(hft_trader::core::RateLimitActionKind::LimitOrder, first);
  const hft_trader::core::RateLimitDecision blockedDecision =
      limits.consumeDecision(hft_trader::core::RateLimitActionKind::LimitOrder, blockedTs);
  const hft_trader::core::RateLimitDecision resetDecision =
      limits.consumeDecision(hft_trader::core::RateLimitActionKind::LimitOrder, blockedDecision.resetMono);

  return expect(configured, "rate-limit test state is configured") &&
         expect(firstDecision.allowed, "first action consumes available quota") &&
         expect(!blockedDecision.allowed, "second action is blocked before reset") &&
         expect(blockedDecision.blockedBucket == hft_trader::core::RateLimitBucketKind::Orders,
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

Symbol btcUsdtSymbol() {
  Symbol symbol{};
  symbol.copyFrom("BTC_USDT");
  return symbol;
}

bool testStaleCancelDoesNotCancelReusedSlot() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 2> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll), orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(10, 1)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), 1);
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0);
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  applyIntents(engine, io, 10, 0, 100);
  (void)fillOrder(engine, engine.open[0], kPx100, 20, false);
  io.clear();
  io.orderOut = Span<hft_trader::core::OrderIntent>(&orders[1], 1);
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

  hft_trader::core::RateLimitBucketConfig orders{};
  orders.kind = hft_trader::core::RateLimitBucketKind::Orders;
  orders.limit = 1;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);

  hft_trader::core::RateLimitBucketConfig cancels{};
  cancels.kind = hft_trader::core::RateLimitBucketKind::CancelOrders;
  cancels.limit = 1;
  cancels.intervalNs = 1000000000ll;
  cancels.enabled = true;
  engine.rateLimits.configureBucket(cancels);

  hft_trader::core::RateLimitActionConfig orderAction{};
  orderAction.action = hft_trader::core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1;
  orderAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  hft_trader::core::RateLimitActionConfig cancelAction{};
  cancelAction.action = hft_trader::core::RateLimitActionKind::CancelOrder;
  cancelAction.costCount = 1;
  cancelAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::CancelOrders;
  cancelAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(cancelAction);

  std::array<hft_trader::core::OrderIntent, 1> first{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(first.data(), first.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<hft_trader::core::CancelIntent, 1> cancel{cancelIntent(10, 1)};
  std::array<hft_trader::core::OrderIntent, 1> second{orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancel.data(), cancel.size());
  io.cancelCount = 1;
  io.orderOut = Span<hft_trader::core::OrderIntent>(second.data(), second.size());
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

  hft_trader::core::RateLimitBucketConfig orders{};
  orders.kind = hft_trader::core::RateLimitBucketKind::Orders;
  orders.limit = 1;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);

  hft_trader::core::RateLimitActionConfig orderAction{};
  orderAction.action = hft_trader::core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1;
  orderAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  std::array<hft_trader::core::OrderIntent, 1> first{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(first.data(), first.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<hft_trader::core::OrderIntent, 2> blocked{
      orderIntent(2, Side::Sell(), kPx100 + 200000000ll),
      orderIntent(3, Side::Sell(), kPx100 + 300000000ll)};
  io.clear();
  io.orderOut = Span<hft_trader::core::OrderIntent>(blocked.data(), blocked.size());
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

  hft_trader::core::RateLimitBucketConfig orders{};
  orders.kind = hft_trader::core::RateLimitBucketKind::Orders;
  orders.limit = 0;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);

  hft_trader::core::RateLimitActionConfig orderAction{};
  orderAction.action = hft_trader::core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1;
  orderAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  std::array<hft_trader::core::OrderIntent, 1> order{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(order.data(), order.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

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
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(3, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 100, 0);
  return expect(engine.orders[0].status == kRecordStatusPending, "late order initially pending") &&
         expect(engine.open[0].state.state == hft_trader::core::StrategyOrderLocalState::PendingNew,
                "late order is strategy-visible but not exchange-live before latency drain") &&
         (applyDueActions(engine, 101000), true) &&
         expect(engine.orders[0].status == kRecordStatusActive, "final drain activates due order") &&
         expect(engine.open[0].state.state == hft_trader::core::StrategyOrderLocalState::LiveLocal,
                "late order becomes exchange-live after latency drain");
}

bool testCancelThenReplacementLimitMaterializesReplace() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(1, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(2, 1)};
  std::array<hft_trader::core::OrderIntent, 1> replacements{postOnlyOrderIntent(2, Side::Buy(), kPx100 - 200000000ll)};
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  io.orderOut = Span<hft_trader::core::OrderIntent>(replacements.data(), replacements.size());
  io.orderCount = 1;
  applyIntents(engine, io, 2000, 0, 0);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  const std::size_t visibleCount = visibleOrders(engine, visible.data(), visible.size());

  return expect(engine.orders.size() == 3u, "replace writes cancel and replacement order records") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "replace cancels original order row") &&
         expect(engine.orders[1].action == kRecordActionCancel, "replace writes cancel command row") &&
         expect(engine.orders[1].targetId == 1u, "replace cancel targets original order") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "replace cancel command is acked") &&
         expect(engine.orders[2].action == kRecordActionOrder, "replace writes replacement order row") &&
         expect(engine.orders[2].id == 2u, "replacement order uses replacement client intent id") &&
         expect(engine.orders[2].type == kRecordTypeLimit, "replacement remains a limit order") &&
         expect(engine.orders[2].priceE8 == kPx100 - 200000000ll, "replacement keeps replacement price") &&
         expect(engine.orders[2].postOnly, "replacement keeps post-only policy") &&
         expect(visibleCount == 1u, "replacement order is visible to strategy") &&
         expect(visible[0].clientIntentId.raw == 2u, "visible order is replacement id") &&
         expect(visible[0].desiredPrice.raw == kPx100 - 200000000ll, "visible order has replacement desired price");
}

bool testCancelAckPublishesCanceledUserEventWithCumulativeFill() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 77u;
  config.cancelOrder.baseUs = 120u;
  config.userData.baseUs = 500u;
  initLatencyRuntime(engine.latency, config);

  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(61, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  FillExecutionMetadata metadata{};
  metadata.fillReason = kFillReasonPassiveQueue;
  if (!fillOrderQty(engine, engine.open[0], kPx100, 25000000, 2000, false, metadata)) {
    return expect(false, "partial fill before cancel succeeds");
  }

  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(62, 61)};
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = cancels.size();
  applyIntents(engine, io, 3000, engine.latency);
  applyDueActions(engine, 123000);

  hft_trader::strategy::RuntimeClientOrderId parsed{};
  return expect(engine.userEvents.size() == 2u, "partial fill plus cancel publish two user events") &&
         expect(engine.userEvents[1].kind == cxet::composite::UserDataEventKind::Order, "cancel user event is order kind") &&
         expect(engine.userEvents[1].orderStatus == canon::OrderStatus::Canceled, "cancel ack publishes canceled status") &&
         expect(engine.userEvents[1].lastFilledQty.raw == 0, "cancel user event has zero last fill") &&
         expect(engine.userEvents[1].filledQty.raw == 25000000, "cancel user event carries cumulative filled qty") &&
         expect(hft_trader::strategy::parseRuntimeClientOrderId(engine.userEvents[1].clientOrderId, parsed),
                "cancel client order id parses") &&
         expect(parsed.clientIntentId.raw == 61u, "cancel user event targets original order id") &&
         expect(engine.userEventDeliveryTsNs.size() == 2u, "cancel user event stores delivery timestamp") &&
         expect(engine.userEventDeliveryTsNs[1] == 623000, "cancel user event delivery applies user-data latency") &&
         expect(!engine.open[0].state.valid, "cancel ack clears open order");
}

bool testOrderedCommandNewBeforeCancelKeepsReplacementLive() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> initial{postOnlyOrderIntent(1, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  io.orderCount = initial.size();
  applyIntents(engine, io, 1000, 0, 0);

  std::array<hft_trader::core::StrategyCommandIntent, 2> commands{};
  commands[0].kind = hft_trader::core::StrategyCommandIntentKind::Order;
  commands[0].order = postOnlyOrderIntent(2, Side::Buy(), kPx100 - 200000000ll);
  commands[1].kind = hft_trader::core::StrategyCommandIntentKind::Cancel;
  commands[1].cancel = cancelIntent(3, 1);
  io.clear();
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  io.commandCount = commands.size();
  applyIntents(engine, io, 2000, 0, 0);

  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  const std::size_t visibleCount = visibleOrders(engine, visible.data(), visible.size());

  return expect(engine.orders.size() == 3u, "ordered replace writes initial, replacement, cancel records") &&
         expect(engine.orders[1].action == kRecordActionOrder, "ordered command emits replacement order first") &&
         expect(engine.orders[1].id == 2u, "ordered replacement uses replacement id") &&
         expect(engine.orders[1].status == kRecordStatusActive, "ordered replacement remains active") &&
         expect(engine.orders[2].action == kRecordActionCancel, "ordered command emits cancel second") &&
         expect(engine.orders[2].targetId == 1u, "ordered cancel targets original order") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "ordered cancel removes original order") &&
         expect(visibleCount == 1u, "only replacement remains visible") &&
         expect(visible[0].clientIntentId.raw == 2u, "visible order is ordered replacement");
}

bool testTaggedAmendEqualLatencyPreservesCommandOrder() {
  EngineState cancelThenNew{};
  setTop(cancelThenNew, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> initial{
      postOnlyOrderIntent(1u, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  io.orderCount = initial.size();
  (void)applyIntents(cancelThenNew, io, 1000ll, 0u, 0u);

  auto cancelFirstCommands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 1u));
  io.clear();
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(
      cancelFirstCommands.data(), cancelFirstCommands.size());
  io.commandCount = cancelFirstCommands.size();
  io.hasCommandSequence = true;
  const IntentApplyResult cancelFirstResult = applyIntents(cancelThenNew, io, 2000ll, 100u, 100u);

  EngineState newThenCancel{};
  setTop(newThenCancel, kPx100 - 100000000ll, kPx100 + 100000000ll);
  io.clear();
  io.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  io.orderCount = initial.size();
  (void)applyIntents(newThenCancel, io, 1000ll, 0u, 0u);
  auto newFirstCommands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendNewThenCancel,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 1u));
  io.clear();
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(
      newFirstCommands.data(), newFirstCommands.size());
  io.commandCount = newFirstCommands.size();
  io.hasCommandSequence = true;
  const IntentApplyResult newFirstResult = applyIntents(newThenCancel, io, 2000ll, 100u, 100u);

  return expect(cancelFirstResult.disposition == IntentApplyDisposition::Applied,
                "cancel-then-new equal-latency amend is accepted") &&
         expect(cancelThenNew.pending.size() == 2u,
                "cancel-then-new schedules both effects without waiting for ack") &&
         expect(cancelThenNew.pending.front().kind == PendingActionKind::CancelAck,
                "cancel-then-new equal-latency tie exposes cancel first") &&
         expect(newFirstResult.disposition == IntentApplyDisposition::Applied,
                "new-then-cancel equal-latency amend is accepted") &&
         expect(newThenCancel.pending.size() == 2u,
                "new-then-cancel schedules both effects without waiting for ack") &&
         expect(newThenCancel.pending.front().kind == PendingActionKind::OrderAck,
                "new-then-cancel equal-latency tie exposes new first");
}

bool testTaggedCancelThenNewStopsWhenCancelSubmissionFails() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  auto commands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 999u));
  hft_trader::core::StrategyIo io{};
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  io.commandCount = commands.size();
  io.hasCommandSequence = true;

  const IntentApplyResult result = applyIntents(engine, io, 2000ll, 0u, 0u);

  return expect(result.disposition == IntentApplyDisposition::RestoreStrategyState,
                "failed cancel-then-new asks runtime to restore strategy state") &&
         expect(engine.orders.size() == 1u, "failed cancel records only cancel attempt") &&
         expect(engine.orders[0].action == kRecordActionCancel, "failed first amend step is cancel") &&
         expect(engine.orders[0].status == kRecordStatusMissing, "missing cancel target is explicit") &&
         expect(engine.pending.empty(), "failed cancel does not schedule replacement") &&
         expect(!engine.open[0].state.valid, "failed cancel does not create replacement order");
}

bool testTaggedNewThenCancelStopsWhenNewSubmissionFails() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> initial{
      postOnlyOrderIntent(1u, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  io.orderCount = initial.size();
  (void)applyIntents(engine, io, 1000ll, 0u, 0u);

  hft_trader::core::RateLimitBucketConfig orders{};
  orders.kind = hft_trader::core::RateLimitBucketKind::Orders;
  orders.limit = 0;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);
  hft_trader::core::RateLimitActionConfig orderAction{};
  orderAction.action = hft_trader::core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1u;
  orderAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  auto commands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendNewThenCancel,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 1u));
  io.clear();
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  io.commandCount = commands.size();
  io.hasCommandSequence = true;
  const IntentApplyResult result = applyIntents(engine, io, 2000ll, 0u, 0u);

  return expect(result.disposition == IntentApplyDisposition::RestoreStrategyState,
                "failed new-then-cancel asks runtime to restore strategy state") &&
         expect(engine.orders.size() == 2u, "failed new records no cancel attempt") &&
         expect(engine.orders[1].action == kRecordActionOrder, "failed first amend step is new") &&
         expect(engine.orders[1].status == kRecordStatusRejected, "rate-limited new is rejected") &&
         expect(engine.orders[0].status == kRecordStatusActive, "failed new leaves old order live");
}

bool testTaggedCancelThenNewRestoresAfterAcceptedCancelAndFailedNew() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> initial{
      postOnlyOrderIntent(1u, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  io.orderCount = initial.size();
  (void)applyIntents(engine, io, 1000ll, 0u, 0u);

  hft_trader::core::RateLimitBucketConfig orders{};
  orders.kind = hft_trader::core::RateLimitBucketKind::Orders;
  orders.limit = 0;
  orders.intervalNs = 1000000000ll;
  orders.enabled = true;
  engine.rateLimits.configureBucket(orders);
  hft_trader::core::RateLimitActionConfig orderAction{};
  orderAction.action = hft_trader::core::RateLimitActionKind::LimitOrder;
  orderAction.costCount = 1u;
  orderAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::Orders;
  orderAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(orderAction);

  auto commands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 1u));
  io.clear();
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  io.commandCount = commands.size();
  io.hasCommandSequence = true;
  const IntentApplyResult result = applyIntents(engine, io, 2000ll, 0u, 0u);

  return expect(result.disposition == IntentApplyDisposition::RestoreStrategyState,
                "accepted cancel plus failed new requests strategy rollback") &&
         expect(engine.orders.size() == 3u, "partial cancel-then-new records both attempts") &&
         expect(engine.orders[0].status == kRecordStatusCancelled,
                "already accepted cancel still reaches exchange") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "cancel command remains accepted") &&
         expect(engine.orders[2].status == kRecordStatusRejected, "replacement new is rejected") &&
         expect(!engine.open[0].state.valid, "failed replacement does not resurrect canceled order");
}

bool testTaggedNewThenCancelPreservesAcceptedNewWhenCancelFails() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> initial{
      postOnlyOrderIntent(1u, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  io.orderCount = initial.size();
  (void)applyIntents(engine, io, 1000ll, 0u, 0u);

  hft_trader::core::RateLimitBucketConfig cancels{};
  cancels.kind = hft_trader::core::RateLimitBucketKind::CancelOrders;
  cancels.limit = 0;
  cancels.intervalNs = 1000000000ll;
  cancels.enabled = true;
  engine.rateLimits.configureBucket(cancels);
  hft_trader::core::RateLimitActionConfig cancelAction{};
  cancelAction.action = hft_trader::core::RateLimitActionKind::CancelOrder;
  cancelAction.costCount = 1u;
  cancelAction.costs[0].bucket = hft_trader::core::RateLimitBucketKind::CancelOrders;
  cancelAction.costs[0].cost = 1;
  engine.rateLimits.configureAction(cancelAction);

  auto commands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendNewThenCancel,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 1u));
  io.clear();
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  io.commandCount = commands.size();
  io.hasCommandSequence = true;
  const IntentApplyResult result = applyIntents(engine, io, 2000ll, 0u, 0u);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  const std::size_t visibleCount = visibleOrders(engine, visible.data(), visible.size());

  return expect(result.disposition == IntentApplyDisposition::PreserveAcceptedSequence,
                "failed second step preserves accepted new-then-cancel sequence") &&
         expect(engine.orders.size() == 3u, "partial new-then-cancel records new and failed cancel") &&
         expect(engine.orders[1].status == kRecordStatusActive, "accepted replacement stays live") &&
         expect(engine.orders[2].status == kRecordStatusRejected, "second-step cancel is rejected") &&
         expect(visibleCount == 2u, "old and accepted replacement remain visible after cancel failure");
}

bool testTaggedCancelThenNewAllowsLatencyOverlap() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> initial{
      postOnlyOrderIntent(1u, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(initial.data(), initial.size());
  io.orderCount = initial.size();
  (void)applyIntents(engine, io, 1000ll, 0u, 0u);

  auto commands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 1u));
  io.clear();
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  io.commandCount = commands.size();
  io.hasCommandSequence = true;
  const IntentApplyResult result = applyIntents(engine, io, 2000ll, 50u, 200u);
  applyDueActions(engine, 52000ll);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  const std::size_t visibleCount = visibleOrders(engine, visible.data(), visible.size());

  return expect(result.disposition == IntentApplyDisposition::Applied,
                "overlapping cancel-then-new amend is accepted") &&
         expect(engine.orders[2].status == kRecordStatusActive,
                "replacement activates before slower cancel effect") &&
         expect(engine.orders[0].status == kRecordStatusActive,
                "old order remains exchange-live until cancel effect") &&
         expect(engine.open[0].state.cancelPending,
                "old order remains cancel-pending during overlap") &&
         expect(visibleCount == 2u, "strategy order book exposes both orders during overlap");
}

bool testMalformedTaggedAmendFailsClosed() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  auto commands = amendCommands(
      hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew,
      postOnlyOrderIntent(2u, Side::Buy(), kPx100 - 200000000ll),
      cancelIntent(3u, 1u));
  commands[1].sequenceStep = hft_trader::core::StrategyCommandSequenceStep::First;
  hft_trader::core::StrategyIo io{};
  io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  io.commandCount = commands.size();
  io.hasCommandSequence = true;

  const IntentApplyResult result = applyIntents(engine, io, 2000ll, 0u, 0u);

  return expect(result.disposition == IntentApplyDisposition::RestoreStrategyState,
                "malformed amend requests strategy rollback") &&
         expect(engine.orders.empty(), "malformed amend emits no partial command") &&
         expect(engine.pending.empty(), "malformed amend schedules no exchange effect");
}

bool testLatencySamplerIsReproducibleAndDeterministicWithoutJitter() {
  BacktestLatencyConfig config{};
  config.seed = 77u;
  config.marketData.baseUs = 2500u;
  config.marketData.jitterUs = 1000u;
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
    const std::uint64_t a = sampleLatencyUs(first, BacktestLatencyChannel::MarketData);
    const std::uint64_t b = sampleLatencyUs(second, BacktestLatencyChannel::MarketData);
    same = same && a == b;
    if (i != 0u && a != previous) varied = true;
    previous = a;
  }

  return expect(same, "same latency seed produces same sequence") &&
         expect(varied, "jittered latency sequence varies") &&
         expect(sampleLatencyUs(first, BacktestLatencyChannel::MarketOrder) == 3100u,
                "zero jitter market order latency returns base") &&
         expect(latencyProfileMaxUs(config.marketData) == 10000u, "market data latency cap uses built-in spike bound");
}

bool testLatencyRuntimeSplitsMarketAndLimitOrderActivation() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 5u;
  config.marketOrder.baseUs = 100u;
  config.limitOrder.baseUs = 700u;
  initLatencyRuntime(engine.latency, config);

  std::array<hft_trader::core::OrderIntent, 2> orders{reduceMarketIntent(30, Side::Buy()), orderIntent(31, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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

  std::array<hft_trader::core::OrderIntent, 2> orders{
      stopMarketIntent(32, Side::Buy(), kPx100 + 300000000ll),
      orderIntent(33, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(70, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(71, Side::Sell())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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

  std::array<hft_trader::core::OrderIntent, 1> orders{stopMarketIntent(74, Side::Buy(), 110000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  const bool resting = engine.fills.empty() && engine.open[0].state.valid && engine.open[0].active;
  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 2000;
  trade.price.raw = 115000000ll;
  trade.qty.raw = kQty1;
  tryFillOnTrade(engine, trade);
  const bool tradeDidNotTrigger = engine.fills.empty() && engine.open[0].state.valid;
  setTop(engine, 109000000ll, 110000000ll);
  tryFillOnTop(engine, 3000);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{stopMarketIntent(75, Side::Buy(), 110000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::array<hft_trader::core::OrderIntent, 1> zeroPrice{stopMarketIntent(790, Side::Buy(), 0)};
  hft_trader::core::StrategyIo zeroIo{};
  zeroIo.orderOut = Span<hft_trader::core::OrderIntent>(zeroPrice.data(), zeroPrice.size());
  zeroIo.orderCount = zeroPrice.size();
  applyIntents(badPrice, zeroIo, 1000, 0, 0);

  EngineState badSource{};
  setTop(badSource, 114000000ll, 115000000ll);
  std::array<hft_trader::core::OrderIntent, 1> unknownSource{
      stopMarketIntent(791, Side::Buy(), 110000000ll, canon::OrderTriggerSource::Unknown)};
  hft_trader::core::StrategyIo sourceIo{};
  sourceIo.orderOut = Span<hft_trader::core::OrderIntent>(unknownSource.data(), unknownSource.size());
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

  std::array<hft_trader::core::OrderIntent, 1> orders{stopMarketIntent(76, Side::Sell(), 110000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  const bool resting = engine.fills.empty() && engine.open[0].state.valid && engine.open[0].active;
  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 2000;
  trade.price.raw = 109000000ll;
  trade.qty.raw = kQty1;
  tryFillOnTrade(engine, trade);
  const bool tradeDidNotTrigger = engine.fills.empty() && engine.open[0].state.valid;
  setTop(engine, 110000000ll, 111000000ll);
  tryFillOnTop(engine, 3000);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{
      stopMarketIntent(77, Side::Buy(), 110000000ll, canon::OrderTriggerSource::Mark)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  setTop(engine, 114000000ll, 115000000ll);
  cxet::composite::MarkPriceRuntimeV1 mark{};
  mark.ts.raw = 2500;
  mark.markPrice.raw = 110000000ll;
  tryTriggerStopMarketsOnMark(engine, mark);

  return expect(engine.fills.size() == 1u, "mark stop-market fills after mark reaches trigger") &&
         expect(engine.fills[0].priceE8 == 115000000ll, "mark stop-market buy still executes at ask") &&
         expect(engine.orders[0].triggerSource == static_cast<std::uint8_t>(canon::OrderTriggerSource::Mark), "mark stop-market records trigger source") &&
         expect(engine.orders[0].triggerSourcePriceE8 == 110000000ll, "mark stop-market records mark trigger price") &&
         expect(engine.orders[0].triggerTsNs == 2500, "mark stop-market records trigger timestamp");
}

bool testStopMarketSellTriggersOnIndexPrice() {
  EngineState engine{};
  setTop(engine, 109000000ll, 110000000ll);

  std::array<hft_trader::core::OrderIntent, 1> orders{
      stopMarketIntent(78, Side::Sell(), 110000000ll, canon::OrderTriggerSource::Index)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  cxet::composite::IndexPriceRuntimeV1 index{};
  index.ts.raw = 2600;
  index.indexPrice.raw = 110000000ll;
  tryTriggerStopMarketsOnIndex(engine, index);

  return expect(engine.fills.size() == 1u, "index stop-market fills after index reaches trigger") &&
         expect(engine.fills[0].priceE8 == 109000000ll, "index stop-market sell still executes at bid") &&
         expect(engine.orders[0].triggerSource == static_cast<std::uint8_t>(canon::OrderTriggerSource::Index), "index stop-market records trigger source") &&
         expect(engine.orders[0].triggerSourcePriceE8 == 110000000ll, "index stop-market records index trigger price") &&
         expect(engine.orders[0].triggerTsNs == 2600, "index stop-market records trigger timestamp");
}

bool testMarketBuyConsumesVisibleAskLevelsWithoutMutatingBook() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, 500000000ll}},
                {{10100000000ll, 40000000ll}, {10200000000ll, 60000000ll}});

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(72, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  tryFillOnTop(engine, 2000);

  const hft_trader::core::LocalOrderBookView book = hft_trader::core::orderBook(engine.venueOrderBook);
  const hft_trader::core::LocalOrderBookSideState* asks = book.side(hft_trader::core::BookSide::Ask);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(73, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  tryFillOnTop(engine, 2000);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(178, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  const hft_trader::core::LocalOrderBookView book = hft_trader::core::orderBook(engine.venueOrderBook);
  const hft_trader::core::LocalOrderBookSideState* asks = book.side(hft_trader::core::BookSide::Ask);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(179, Side::Sell())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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

  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(180, Side::Buy(), 14000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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

  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(181, Side::Buy(), 15120000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.fills.size() == 1u, "crossing limit buy overlays BBO before consuming ask depth") &&
         expect(engine.fills[0].priceE8 == 15120000ll, "crossing limit buy fills at reconstructed BBO ask") &&
         expect(engine.fills[0].qtyE8 == kQty1, "crossing limit buy fills requested quantity at valid top") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "crossing limit buy remains depth-aware taker fill") &&
         expect(!engine.open[0].state.valid, "crossing limit buy clears when overlayed depth covers full quantity");
}

bool testLazyExecutionDepthProviderMarketBuyConsumesLegacyDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_legacy_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,100000000,0],[10100000000,40000000,1],[10200000000,60000000,1],1000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9900000000ll, 10100000000ll);
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(172, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  return expect(opened, "lazy execution depth provider opens legacy depth channel") &&
         expect(provider.rowsRead() == 1u, "lazy execution depth provider reads only needed depth row") &&
         expect(engine.fills.size() == 2u, "lazy execution depth market buy writes one fill per depth level") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "lazy execution depth uses best ask instead of fallback BBO") &&
         expect(engine.fills[1].priceE8 == 10200000000ll, "lazy execution depth walks second ask level") &&
         expect(engine.fills[0].chunkCount == 2u, "lazy execution depth carries chunk count") &&
         expect(engine.fills[0].executionAvgPriceE8 == 10160000000ll, "lazy execution depth carries execution avg");
}

bool testDeclaredMissingExecutionDepthReportsFallbackStatus() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_missing_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  hft_trader::core::LocalOrderBookConfig config{};
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
  writeText(dir / "manifest.json", "{\"channels\":{}}\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  hft_trader::core::LocalOrderBookConfig config{};
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
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776808,9900000000,100000000,10100000000,40000000,10200000000,60000000]\n"
            "[9223372036854776809,9900000000,100000000,10300000000,100000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[9223372036854776808,0,1,1,2]\n"
            "[9223372036854776809,0,1,1,1]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9800000000ll, 10100000000ll);
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(174, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,100000000,0],[10100000000,100000000,1],1000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTopAt(engine, 9900000000ll, 10400000000ll, 501002000u);
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(175, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 501002000, 0, 0);

  return expect(opened, "stale lazy depth provider opens legacy fixture") &&
         expect(provider.rowsRead() == 1u, "stale lazy depth provider advances to active timestamp") &&
         expect(engine.fills.size() == 1u, "stale lazy depth market buy falls back to one BBO fill") &&
         expect(engine.fills[0].priceE8 == 10400000000ll, "stale lazy depth market buy fills at BBO ask") &&
         expect(engine.fills[0].qtyE8 == kQty1, "stale lazy depth market buy fills full order quantity") &&
         expect(engine.fills[0].fillReason == kFillReasonTop, "stale lazy depth market buy tags top fill reason");
}

bool testFreshLazyExecutionDepthMarketBuyMayUseSlightlyOlderDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_fresh_buy_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,100000000,0],[10100000000,40000000,1],[10200000000,60000000,1],1000000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTopAt(engine, 9900000000ll, 10100000000ll, 26000000u);
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(176, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 26000000, 0, 0);

  return expect(opened, "fresh lazy depth provider opens legacy fixture") &&
         expect(provider.rowsRead() == 1u, "fresh lazy depth provider advances to active timestamp") &&
         expect(engine.fills.size() == 2u, "fresh lazy depth market buy still consumes depth chunks") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "fresh lazy depth market buy uses best depth ask") &&
         expect(engine.fills[1].priceE8 == 10200000000ll, "fresh lazy depth market buy walks second depth ask") &&
         expect(engine.fills[0].fillReason == kFillReasonAggressiveBook, "fresh lazy depth market buy stays depth-aware");
}

bool testStaleLazyExecutionDepthMarketSellFallsBackToBboBid() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_stale_sell_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,100000000,0],[10100000000,100000000,1],1000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTopAt(engine, 9700000000ll, 10400000000ll, 501002000u);
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(177, Side::Sell())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 501002000, 0, 0);

  return expect(opened, "stale lazy sell depth provider opens legacy fixture") &&
         expect(provider.rowsRead() == 1u, "stale lazy sell depth provider advances to active timestamp") &&
         expect(engine.fills.size() == 1u, "stale lazy depth market sell falls back to one BBO fill") &&
         expect(engine.fills[0].priceE8 == 9700000000ll, "stale lazy depth market sell fills at BBO bid") &&
         expect(engine.fills[0].qtyE8 == kQty1, "stale lazy depth market sell fills full order quantity") &&
         expect(engine.fills[0].fillReason == kFillReasonTop, "stale lazy depth market sell tags top fill reason");
}

bool testLazyExecutionDepthProviderDoesNotAdvanceWithoutExecutionCandidate() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_idle_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,100000000,0],[10100000000,100000000,1],1000]\n"
            "[[9900000000,100000000,0],[10200000000,100000000,1],2000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9900000000ll, 10100000000ll);
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  tryFillOnTop(engine, 2000);

  return expect(opened, "lazy execution depth provider opens idle fixture") &&
         expect(provider.rowsRead() == 0u, "lazy execution depth does not advance without executable orders") &&
         expect(engine.fills.empty(), "idle lazy execution does not create fills");
}

bool testLazyExecutionDepthProviderInitializesPassiveQueueAtActivation() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_execution_depth_queue_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,55000000,0],[10100000000,100000000,1],1000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  setTop(engine, 9900000000ll, 10100000000ll);
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(146, Side::Buy(), 9900000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 55000000ll;

  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 2000;
  trade.side = Side::Sell();
  trade.price.raw = 9900000000ll;
  trade.qty.raw = 70000000ll;
  tryFillOnTrade(engine, trade);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(190, Side::Buy(), 9900000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 55000000ll;

  setTopWithQty(engine, 9900000000ll, 5000000ll, 10100000000ll, 100000000ll);
  tryFillOnTop(engine, 2000);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{marketIntent(173, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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

  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(74, Side::Buy(), 10100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);
  tryFillOnTop(engine, 2000);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(61, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(62, 61)};
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = cancels.size();
  applyIntents(engine, io, 2000, engine.latency);

  return expect(engine.orders.size() == 2u, "cancel channel test records order and cancel") &&
         expect(engine.orders[1].action == kRecordActionCancel, "second record is cancel") &&
         expect(engine.orders[1].activeTsNs == 122000, "cancel uses dedicated cancel latency, not limit latency");
}

bool testFillPublishesRuntimeUserEvent() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(4, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0);
  const bool filled = fillOrder(engine, engine.open[0], kPx100, 20, false);
  hft_trader::strategy::RuntimeClientOrderId parsed{};
  return expect(filled, "manual fill succeeds") &&
         expect(engine.userEvents.size() == 1u, "fill creates one user event") &&
         expect(engine.userEvents[0].kind == cxet::composite::UserDataEventKind::Order, "user event is order kind") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Closed, "user event is final fill") &&
         expect(engine.userEvents[0].lastFilledQty.raw == kQty1, "user event has last fill qty") &&
         expect(hft_trader::strategy::parseRuntimeClientOrderId(engine.userEvents[0].clientOrderId, parsed), "client order id parses") &&
         expect(parsed.clientIntentId.raw == 4, "client order id carries intent id");
}

bool testFillUserEventHasSeparateDeliveryLatency() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 7u;
  config.userData.baseUs = 500u;
  initLatencyRuntime(engine.latency, config);

  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(64, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(40, Side::Buy(), kPx100 + 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders.size() == 1u, "post-only crossing buy is recorded") &&
         expect(engine.orders[0].postOnly, "post-only crossing buy record keeps post-only flag") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "post-only crossing buy is rejected") &&
         expect(engine.fills.empty(), "post-only crossing buy does not taker-fill") &&
         expect(!engine.open[0].state.valid, "post-only crossing buy leaves no open order") &&
         expect(engine.userEvents.size() == 1u, "post-only crossing buy publishes rejected user event") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "post-only crossing buy event is rejected");
}

bool testPostOnlyCrossingSellRejectsWithoutTakerFill() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(41, Side::Sell(), kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::array<hft_trader::core::OrderIntent, 1> orders{strictPostOnlyOrderIntent(140, Side::Buy(), kPx100 + 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::array<hft_trader::core::OrderIntent, 1> orders{strictPostOnlyOrderIntent(141, Side::Sell(), kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::array<hft_trader::core::OrderIntent, kMaxOrderStates + 1u> orders{};
  for (std::size_t i = 0u; i < orders.size(); ++i) {
    orders[i] = orderIntent(1000u + i, Side::Buy(), kPx100 - 200000000ll);
  }
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();

  applyIntents(engine, io, 1000, 100, 0);

  hft_trader::strategy::RuntimeClientOrderId parsed{};
  return expect(engine.orders.size() == kMaxOrderStates + 1u, "overflow order is recorded") &&
         expect(engine.orders[kMaxOrderStates].status == kRecordStatusRejected, "overflow order is rejected") &&
         expect(engine.userEvents.size() == 1u, "overflow order publishes rejected user event") &&
         expect(engine.userEventDeliveryTsNs.size() == 1u, "overflow rejection stores delivery timestamp") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "overflow user event is rejected") &&
         expect(hft_trader::strategy::parseRuntimeClientOrderId(engine.userEvents[0].clientOrderId, parsed),
                "overflow rejected client order id parses") &&
         expect(parsed.clientIntentId.raw == 1000u + kMaxOrderStates, "overflow rejection carries intent id");
}

bool testPostOnlyRestingOrderDoesNotFillOnTopButFillsOnTrade() {
  EngineState engine{};
  setTop(engine, kPx100 - 200000000ll, kPx100 + 200000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(42, Side::Buy(), kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);
  const bool policyVisible = engine.open[0].state.limitTimeInForcePolicy == hft_trader::core::LimitTimeInForcePolicy::PostOnlyPrefer;

  setTop(engine, kPx100 - 200000000ll, kPx100);
  tryFillOnTop(engine, 2000);

  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 3000;
  trade.side = Side::Sell();
  trade.price.raw = kPx100 - 100000000ll;
  trade.qty.raw = kQty1;
  tryFillOnTrade(engine, trade);

  return expect(policyVisible, "post-only resting order state keeps post-only policy") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "post-only resting order fills from trade") &&
         expect(engine.fills.size() == 1u, "post-only resting order has one maker fill") &&
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
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(44, Side::Buy(), orderPrice)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  cxet::composite::TradeRuntimeV1 touch{};
  touch.ts.raw = 2000;
  touch.side = Side::Sell();
  touch.price.raw = orderPrice;
  touch.qty.raw = kQty1;
  tryFillOnTrade(engine, touch);

  const bool noTouchFill = engine.fills.empty() && engine.open[0].state.valid;

  cxet::composite::TradeRuntimeV1 through{};
  through.ts.raw = 3000;
  through.side = Side::Sell();
  through.price.raw = orderPrice - engine.tickSizeE8;
  through.qty.raw = kQty1;
  tryFillOnTrade(engine, through);

  return expect(noTouchFill, "maker min-cross ignores touch-only trade") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "maker min-cross fills after trade-through") &&
         expect(engine.fills.size() == 1u, "maker min-cross writes one fill") &&
         expect(engine.fills[0].priceE8 == orderPrice, "maker min-cross fills at order price");
}

bool testTradeQtyPartialFillKeepsMakerOrderLiveUntilCumulativeFilled() {
  EngineState engine{};
  setTop(engine, kPx100 - 200000000ll, kPx100 + 200000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(43, Side::Buy(), kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  cxet::composite::TradeRuntimeV1 first{};
  first.ts.raw = 2000;
  first.side = Side::Sell();
  first.price.raw = kPx100 - 100000000ll;
  first.qty.raw = 25000000;
  tryFillOnTrade(engine, first);

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
  tryFillOnTrade(engine, second);

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

  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(46, Side::Buy(), 9900000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 55000000ll;

  cxet::composite::TradeRuntimeV1 first{};
  first.ts.raw = 2000;
  first.side = Side::Sell();
  first.price.raw = 9900000000ll;
  first.qty.raw = 50000000ll;
  tryFillOnTrade(engine, first);
  const bool stillWaiting = engine.fills.empty() && engine.open[0].queueAheadQtyRaw == 5000000ll;

  cxet::composite::TradeRuntimeV1 second{};
  second.ts.raw = 3000;
  second.side = Side::Sell();
  second.price.raw = 9900000000ll;
  second.qty.raw = 70000000ll;
  tryFillOnTrade(engine, second);

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
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(45, Side::Buy(), kPx100 + 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntent(7, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntent(5, Side::Sell())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, 40000000, 100, 0, netRealized);
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntentQty(50, Side::Sell(), kQty1)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Sell(), kPx100, kQty1, 100, 0, netRealized);
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntent(6, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0);
  return expect(engine.netQtyRaw == 0, "reduce-only buy flattens short") &&
         expect(engine.fills.size() == 1u, "short close writes fill") &&
         expect(engine.fills[0].priceE8 == kPx100 - 200000000ll, "short close uses ask") &&
         expect(engine.netRealizedPnlE8 == 200000000ll, "short close realizes entry-ask pnl");
}

bool testFeesReduceWalletAndNetPnl() {
  EngineState engine{};
  engine.initialBalanceE8 = 100000000000ll;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.takerFeeBpsE8 = 100000000ll;
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, engine.takerFeeBpsE8, netRealized);
  updatePosition(engine, Side::Sell(), kPx100 + 100000000ll, kQty1, 200, engine.takerFeeBpsE8, netRealized);
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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);

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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);

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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);
  appendRiskSnapshot(engine, 100, kRiskSnapshotReasonFill);

  setTop(engine, kPx100 + 100000000ll, kPx100 + 200000000ll);
  const bool early = appendRiskSnapshotIfDue(engine, 100 + 500000000ll);
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  const bool due = appendRiskSnapshotIfDue(engine, 100 + 1000000000ll);
  const bool unchanged = appendRiskSnapshotIfDue(engine, 100 + 2000000000ll);

  return expect(!early, "risk cadence skips sub-second MTM changes") &&
         expect(due, "risk cadence writes after one second") &&
         expect(!unchanged, "risk cadence skips unchanged MTM value") &&
         expect(engine.riskSnapshots.size() == 2u, "risk snapshots stay compact") &&
         expect(engine.riskSnapshots[1].reason == kRiskSnapshotReasonInterval, "cadence row is marked interval");
}

bool testHardLiquidationClearsStateAndBlocksNewOrders() {
  hft_backtest::BacktestRunRequest request{};
  request.initialBalanceE8 = 5000000000ll;

  EngineState engine{};
  initializeAccounting(engine, request);
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(40, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 100, 0);

  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0, netRealized);
  setTop(engine, kPx100 - 6000000000ll, kPx100 - 5900000000ll);
  const std::int64_t breachedEquity = equityE8(engine);

  hardLiquidate(engine, 2000, breachedEquity);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  io.clear();
  orders[0] = orderIntent(41, Side::Sell(), kPx100 + 200000000ll);
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(50, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0, netRealized);
  setTop(engine, kPx100 - 3000000000ll, kPx100 - 2900000000ll);
  const std::int64_t breachedEquity = equityE8(engine);
  const std::int64_t thresholdEquity = 3000000000ll;

  stopForTraderRisk(engine, 2000, breachedEquity, thresholdEquity);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  io.clear();
  orders[0] = orderIntent(51, Side::Sell(), kPx100 + 200000000ll);
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0, netRealized);
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
  writeText(dir / "manifest.json",
            "{\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,40000000,0],[9800000000,60000000,0],[10100000000,100000000,1],1000]\n");

  EngineState engine{};
  engine.symbol.copyFrom("BTC_USDT");
  engine.takerFeeBpsE8 = 0;
  setTopWithQty(engine, 9900000000ll, 40000000ll, 10100000000ll, 100000000ll);
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);

  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  ExecutionDepthProvider provider{};
  const bool opened = provider.open(dir, engine.symbol, config, engine.venueOrderBook) == ExecutionDepthOpenStatus::Ready;
  engine.executionDepthProvider = &provider;

  const bool closed = forceClosePosition(engine, 2000, 0);

  return expect(opened, "force-close lazy depth provider opens legacy fixture") &&
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
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Sell(), kPx100, kQty1, 100, 0, netRealized);

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


bool testCancelOpenOrdersAtFinishWritesCancelAcks() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
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
bool testLoadSessionUsesManifestChannelPathsAndMetadata() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_loader_manifest_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTC_USDT\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
            "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"},\n"
            "    \"price_limit\": {\"path\": \"jsonl/price_limit.jsonl\"},\n"
            "    \"depth\": {\"path\": \"jsonl/depth.jsonl\"},\n"
            "    \"candles\": {\"path\": \"jsonl/candles.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"expiry_utc_ns\":1781913600000000000}\n");
  writeText(dir / "jsonl" / "trades.jsonl", "[10000000000,100000000,1,1000]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,1001]\n");
  writeText(dir / "jsonl" / "price_limit.jsonl", "[1001,9500000000,10500000000,1]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],1002]\n");
  writeText(dir / "jsonl" / "candles.jsonl", "[1,900,10200000000,9800000000,500000000]\n");

  SessionLoadPlan plan{};
  plan.loadTrades = true;
  plan.loadBookTickers = true;
  plan.loadDepths = true;
  plan.loadCandles = true;
  plan.requireTrades = true;
  plan.requireBookTickers = true;
  plan.requireDepths = true;
  plan.requireCandles = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts current manifest channel path schema") &&
         expect(data.exchange == "binance", "loader reads exchange identity") &&
         expect(data.market == "futures", "loader reads market identity") &&
         expect(data.symbol == "BTC_USDT", "loader reads first symbol identity") &&
         expect(data.trades.size() == 1u, "loader reads trades from channel path") &&
         expect(data.bookTickers.size() == 1u, "loader reads bookticker from channel path") &&
         expect(data.priceLimits.size() == 1u, "loader reads price_limit from channel path") &&
         expect(data.depths.size() == 1u, "loader reads depth from channel path") &&
         expect(data.candles.size() == 1u, "loader reads candles from channel path") &&
         expect(data.instrument.tickSizeE8 == 1000000, "loader reads tick size metadata") &&
         expect(data.instrument.lotSizeE8 == 1000, "loader reads lot size metadata") &&
         expect(data.instrument.expiryUtcNs == 1781913600000000000ll, "loader reads expiry metadata");
}

bool testLoadSessionFailsMissingRequiredDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_loader_missing_depth_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTC_USDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData, "loader rejects missing required depth") &&
         expect(error.find("depth") != std::string::npos, "missing required depth error names channel");
}

bool testLoadSessionPrefersDepthTapeSidecar() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTC_USDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl", "[9223372036854776810,0,1,1,1]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9700000000,100000000,0],[10300000000,100000000,1],1002]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts depth tape sidecar package") &&
         expect(data.depths.size() == 1u, "depth tape sidecar yields one depth row") &&
         expect(data.depths[0].tsNs == 1002, "depth tape sidecar decodes tagged timestamp") &&
         expect(data.depths[0].levels.size() == 2u, "depth tape sidecar decodes levels") &&
         expect(data.depths[0].levels[0].priceE8 == 9900000000ll, "depth tape sidecar wins over legacy depth") &&
         expect(data.depths[0].levels[0].side == 0, "depth tape sidecar applies bid side") &&
         expect(data.depths[0].levels[1].side == 1, "depth tape sidecar applies ask side");
}

bool testLoadSessionKeepsDepthTapeSidecarPrefixWhenTapeTailIsUnpaired() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_partial_tail_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTC_USDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n"
            "[9223372036854776811,9910000000,100000000,10110000000,200000000]\n"
            "[9223372036854776812,9920000000,100000000,10120000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[9223372036854776810,0,1,1,1]\n"
            "[9223372036854776811,0,1,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts paired depth prefix with unpaired tape tail") &&
         expect(error.empty(), "partial depth tail does not become hard loader error") &&
         expect(data.depths.size() == 2u, "partial depth tail keeps valid paired prefix") &&
         expect(data.warnings.size() == 1u, "partial depth tail emits one warning") &&
         expect(data.warnings[0].find("dropped 1 unpaired depth_tape rows") != std::string::npos,
                "partial depth tail warning names dropped tape rows");
}

bool testLoadSessionResyncsDepthTapeSidecarAfterMissingMiddleSidecarRow() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_resync_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTC_USDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n"
            "[9223372036854776811,9910000000,100000000,10110000000,200000000]\n"
            "[9223372036854776812,9920000000,100000000,10120000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[9223372036854776810,0,1,1,1]\n"
            "[9223372036854776812,0,1,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader resyncs after a missing middle sidecar row") &&
         expect(error.empty(), "resynced depth corruption does not become hard loader error") &&
         expect(data.depths.size() == 2u, "resynced depth load keeps rows before and after bad row") &&
         expect(data.depths[0].tsNs == 1002, "resynced depth keeps first row") &&
         expect(data.depths[1].tsNs == 1004, "resynced depth resumes at next matching timestamp") &&
         expect(!data.warnings.empty(), "resynced depth emits warning") &&
         expect(data.warnings[0].find("dropped 1 unpaired depth_tape rows") != std::string::npos,
                "resync warning names skipped tape row");
}

bool testLoadSessionSkipsCorruptMiddleDepthTapeSidecarPair() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_corrupt_pair_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTC_USDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n"
            "[9223372036854776811,9910000000,100000000,10110000000,200000000]\n"
            "[9223372036854776812,9920000000,100000000,10120000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[9223372036854776810,0,1,1,1]\n"
            "[9223372036854776811,0,3]\n"
            "[9223372036854776812,0,1,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader skips a corrupt middle depth pair") &&
         expect(error.empty(), "skipped corrupt depth pair does not become hard loader error") &&
         expect(data.depths.size() == 2u, "skipped corrupt depth pair keeps surrounding rows") &&
         expect(data.depths[0].tsNs == 1002, "skipped corrupt depth keeps first row") &&
         expect(data.depths[1].tsNs == 1004, "skipped corrupt depth resumes on later valid row") &&
         expect(!data.warnings.empty(), "skipped corrupt depth pair emits warning") &&
         expect(data.warnings[0].find("dropped corrupt depth_tape/depth_sidecar row") != std::string::npos,
                "corrupt pair warning names skipped row");
}

bool testLoadSessionRejectsDepthTapeWithoutTaggedTimestamp() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_bad_tag_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTC_USDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[1002,9900000000,100000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl", "[1002,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData,
                "loader rejects depth tape without tagged timestamp") &&
         expect(error.find("depth_tape") != std::string::npos,
                "bad depth tape error names depth_tape");
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

  hft_trader::core::LocalOrderBookState book{};
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  config.tickSizeRaw = 1000000;
  Symbol symbol{};
  symbol.copyFrom("BTC_USDT");
  hft_trader::core::applySnapshot(book, snapshot, symbol, config);

  const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(book);
  return expect(snapshot.levelCount.raw == 2u, "depth row snapshot carries levels") &&
         expect(sides.sideCount.raw == 2u, "depth row tape carries side count") &&
         expect(sides.sides[0] == Side::Buy().raw, "depth row tape maps side zero to bid") &&
         expect(sides.sides[1] == Side::Sell().raw, "depth row tape maps side one to ask") &&
         expect(book.initialized.raw == hft_trader::core::kAlgoFlagTrue.raw, "depth row snapshot initializes local orderbook") &&
         expect(hft_trader::core::bestBid(view).raw == 9900000000ll, "depth row snapshot sets best bid") &&
         expect(hft_trader::core::bestAsk(view).raw == 10100000000ll, "depth row snapshot sets best ask");
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

bool testWriterCreatesV2RunDirectory() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_v2_writer_" + std::to_string(std::rand()));
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
  leg.marketDataLatency = hft_backtest::BacktestLatencyProfile{250u, 100u};
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
         expect(manifest.find("\"type\": \"run.result.v2\"") != std::string::npos, "manifest declares v2 result") &&
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
         expect(manifest.find("\"market_data_latency\": {\"base_us\": 250, \"jitter_us\": 100}") != std::string::npos,
                "manifest leg summary carries per-leg market data latency") &&
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
  const std::size_t descriptorCount = hft_trader::runtime::strategyDescriptorCount();
  if (!expect(hft_backtest::strategyMetadataCount() == descriptorCount, "metadata count mirrors trader registry")) return false;
  for (std::size_t i = 0; i < descriptorCount; ++i) {
    const hft_trader::runtime::StrategyDescriptor* descriptor = hft_trader::runtime::strategyDescriptorAt(i);
    const hft_backtest::StrategyMetadata* metadata = hft_backtest::strategyMetadataAt(i);
    if (!expect(descriptor != nullptr && descriptor->name != nullptr, "registry descriptor has a name")) return false;
    if (!expect(metadata != nullptr && metadata->id != nullptr, "metadata row exists for registry descriptor")) return false;
    if (!expect(std::string_view{metadata->id} == descriptor->name, "metadata id matches registry descriptor")) return false;
    if (!expect(hft_backtest::findStrategyMetadata(descriptor->name) == metadata, "metadata lookup finds registry strategy")) return false;
  }
  return expect(hft_backtest::findStrategyMetadata("removed_strategy") == nullptr, "unknown metadata is hidden");
}

bool testStatArbBandLadderKeepsTradesOptionalForBboOnlyReplay() {
  const hft_trader::runtime::StrategyDescriptor* descriptor = nullptr;
  const std::size_t descriptorCount = hft_trader::runtime::strategyDescriptorCount();
  for (std::size_t i = 0; i < descriptorCount; ++i) {
    const hft_trader::runtime::StrategyDescriptor* candidate = hft_trader::runtime::strategyDescriptorAt(i);
    if (candidate != nullptr && candidate->name != nullptr &&
        std::string_view{candidate->name} == "stat_arb_band_ladder") {
      descriptor = candidate;
      break;
    }
  }
  if (!expect(descriptor != nullptr, "stat arb band ladder descriptor is exported")) return false;

  return expect(hft_trader::runtime::descriptorStreamCount(*descriptor) == 2u,
                "stat arb band ladder keeps bookticker plus optional trades") &&
         expect(hft_trader::runtime::descriptorStreamAt(*descriptor, 0u) ==
                    cxet::api::market::PublicMarketDataStream::BookTicker,
                "stat arb band ladder primary descriptor stream is bookticker") &&
         expect(hft_trader::runtime::descriptorStreamRequired(*descriptor, 0u),
                "stat arb band ladder requires bookticker") &&
         expect(hft_trader::runtime::descriptorStreamAt(*descriptor, 1u) ==
                    cxet::api::market::PublicMarketDataStream::Trades,
                "stat arb band ladder still accepts trades when present") &&
         expect(!hft_trader::runtime::descriptorStreamRequired(*descriptor, 1u),
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
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/stat_arb_band_ladder.ini",
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
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/stat_arb_pairwise_taker.ini",
                "stat_arb_pairwise_taker metadata discovers strategy profile") &&
         expect(metadataDisplayNameIs(*metadata, "Стат-арбитраж all-pairs taker"),
                "stat_arb_pairwise_taker display name comes from ini metadata") &&
         expect(metadata->minSessionCount == 2u, "stat_arb_pairwise_taker requires at least two sessions") &&
         expect(metadata->maxSessionCount == 50u, "stat_arb_pairwise_taker allows the GUI multileg cap") &&
         expect(metadataHasParam(*metadata, "ema_window_ms"), "pairwise metadata contains ema_window_ms") &&
         expect(metadataHasParam(*metadata, "entry_edge_bps"), "pairwise metadata contains entry_edge_bps") &&
         expect(metadataHasParam(*metadata, "ladder_step_bps"), "pairwise metadata contains ladder_step_bps") &&
         expect(metadataHasParam(*metadata, "exit_edge_bps"), "pairwise metadata contains exit_edge_bps") &&
         expect(metadataHasParam(*metadata, "max_ladder_levels"), "pairwise metadata contains max_ladder_levels") &&
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
         expect(metadataParamDefaultIs(*metadata, "ladder_step_bps", "2"),
                "pairwise ladder step default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "max_ladder_levels", "3"),
                "pairwise ladder levels default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "max_quote_age_ms", "5000"),
                "pairwise max quote age default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "amount_qty", "10"), "pairwise amount_qty default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "0.001"),
                "pairwise order_qty default comes from commented ini template");
}

bool testStrategyMetadataExposesInputFreshnessParam() {
  constexpr std::string_view kStrategies[] = {
      "basis_convergence_probe",
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
  ok = testBacktestTradabilityFilterDropsWholeAmendAndRestoresState() && ok;
  ok = testBacktestFailedLaterAmendCleansEarlierAcceptedReplacement() && ok;
  ok = testBacktestFeeScheduleOverridesExchangePreset() && ok;
  ok = testBacktestGlobalFeeOverrideFillsPresetSides() && ok;
  ok = testRateLimitDecisionAllowsAfterReset() && ok;
  ok = testStaleCancelDoesNotCancelReusedSlot() && ok;
  ok = testRateLimitRejectsOrderButAllowsSeparateCancelBucket() && ok;
  ok = testRepeatedRateLimitRejectsShareCooldownWindow() && ok;
  ok = testRateLimitRejectAddsRunWarningWithoutFailingRun() && ok;
  ok = testPendingDrainActivatesLateOrder() && ok;
  ok = testCancelThenReplacementLimitMaterializesReplace() && ok;
  ok = testCancelAckPublishesCanceledUserEventWithCumulativeFill() && ok;
  ok = testOrderedCommandNewBeforeCancelKeepsReplacementLive() && ok;
  ok = testTaggedAmendEqualLatencyPreservesCommandOrder() && ok;
  ok = testTaggedCancelThenNewStopsWhenCancelSubmissionFails() && ok;
  ok = testTaggedNewThenCancelStopsWhenNewSubmissionFails() && ok;
  ok = testTaggedCancelThenNewRestoresAfterAcceptedCancelAndFailedNew() && ok;
  ok = testTaggedNewThenCancelPreservesAcceptedNewWhenCancelFails() && ok;
  ok = testTaggedCancelThenNewAllowsLatencyOverlap() && ok;
  ok = testMalformedTaggedAmendFailsClosed() && ok;
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
  ok = testLazyExecutionDepthProviderMarketBuyConsumesLegacyDepth() && ok;
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
  ok = testFeesReduceWalletAndNetPnl() && ok;
  ok = testEquityPointCapturesRealizedOnlyPnl() && ok;
  ok = testRiskSnapshotCapturesCompactMarkToMarketPnl() && ok;
  ok = testRiskSnapshotCadenceLimitsOpenPositionMtmRows() && ok;
  ok = testHardLiquidationClearsStateAndBlocksNewOrders() && ok;
  ok = testTraderRiskStopCancelsOrdersKeepsPositionAndBlocksNewOrders() && ok;
  ok = testTraderRiskStopAfterForceCloseKeepsRiskFieldsAndFlatPosition() && ok;
  ok = testForceCloseOpenLongPositionUsesLazyExecutionDepth() && ok;
  ok = testForceCloseOpenShortPositionSubmitsReduceOnlyMarketBuy() && ok;
  ok = testCancelOpenOrdersAtFinishWritesCancelAcks() && ok;
  ok = testLoadSessionUsesManifestChannelPathsAndMetadata() && ok;
  ok = testLoadSessionFailsMissingRequiredDepth() && ok;
  ok = testLoadSessionPrefersDepthTapeSidecar() && ok;
  ok = testLoadSessionKeepsDepthTapeSidecarPrefixWhenTapeTailIsUnpaired() && ok;
  ok = testLoadSessionResyncsDepthTapeSidecarAfterMissingMiddleSidecarRow() && ok;
  ok = testLoadSessionSkipsCorruptMiddleDepthTapeSidecarPair() && ok;
  ok = testLoadSessionRejectsDepthTapeWithoutTaggedTimestamp() && ok;
  ok = testDepthRowBuildsInitialOrderBookSnapshot() && ok;
  ok = testTieredCandleHistoryBuildsCounts() && ok;
  ok = testWriterCreatesV2RunDirectory() && ok;
  ok = testWriterDiagnosesNoSignalBelowThreshold() && ok;
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
