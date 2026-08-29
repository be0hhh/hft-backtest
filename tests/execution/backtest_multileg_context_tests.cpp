#include "engine/backtest_engine.hpp"
#include "runner/backtest_run_error.hpp"
#include "strategy/backtest_strategy_runtime.hpp"
#include "../support/CapturedSessionFixture.hpp"

#include "trading_core/api/context/StrategyContextView.hpp"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace hft_backtest;
using namespace hft_backtest::detail;

constexpr std::int64_t kPx100 = 10000000000ll;

void writeText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  out << text;
}

std::string bookTickerRow(std::int64_t bid,
                          std::int64_t bidQty,
                          std::int64_t ask,
                          std::int64_t askQty,
                          std::int64_t tsNs,
                          std::uint64_t seq,
                          std::string_view exchange) {
  return hft_backtest::test_support::bookTickerRow(
      bid, bidQty, ask, askQty, tsNs, seq, "BTC_USDT", exchange);
}

std::filesystem::path makePairwiseReplayRoot() {
  return std::filesystem::temp_directory_path() /
         ("hft_backtest_pairwise_full_scan_" + std::to_string(std::rand()));
}

void writePairwiseReplaySession(const std::filesystem::path& dir,
                                std::string_view exchange,
                                std::int64_t priceOffset,
                                std::int64_t secondTsNs,
                                std::int64_t seqBase) {
  writeText(
      dir / "manifest.json",
      hft_backtest::test_support::capturedManifestJson(
          std::string{exchange},
          "futures",
          "BTC_USDT",
          hft_backtest::test_support::CapturedChannelCounts{
              .bookTickers = 2u}));
  writeText(dir / "instrument_metadata.json",
            "{\"tick_size_e8\":1000000,\"lot_size_e8\":100000000,\"contract_base_qty_e8\":100000000}\n");
  const std::int64_t bid = kPx100 + priceOffset;
  const std::int64_t ask = bid + 100000000ll;
  writeText(dir / "jsonl" / "bookticker.jsonl",
            bookTickerRow(bid, 100000000ll, ask, 100000000ll,
                          1000000000ll,
                          static_cast<std::uint64_t>(seqBase), exchange) +
                bookTickerRow(bid + 1000000ll, 100000000ll,
                              ask + 1000000ll, 100000000ll, secondTsNs,
                              static_cast<std::uint64_t>(seqBase + 1ll),
                              exchange));
}

void writePairwiseReplayConfig(const std::filesystem::path& path) {
  writeText(path,
            "[strategy]\n"
            "type=stat_arb_pairwise_taker\n"
            "enabled=1\n"
            "order_qty=1\n"
            "ema_window_ms=5000\n"
            "entry_edge_bps=100000\n"
            "exit_edge_bps=0\n"
            "max_quote_age_ms=5000\n"
            "\n"
            "[venue.binance_futures]\napi_slot=1\nsymbols=BTC_USDT\n"
            "[venue.bybit_futures]\napi_slot=1\nsymbols=BTC_USDT\n"
            "[venue.okx_futures]\napi_slot=1\nsymbols=BTC_USDT\n"
            "[venue.bitget_futures]\napi_slot=1\nsymbols=BTC_USDT\n"
            "[venue.kucoin_futures]\napi_slot=1\nsymbols=BTC_USDT\n");
}

void setTop(EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  cxet::composite::BookTickerRuntimeV1 top{};
  top.bid.px.raw = bid;
  top.ask.px.raw = ask;
  cxet::composite::BboReconstructionView reconstructed{};
  reconstructed.bookTicker = top;
  reconstructed.source =
      cxet::composite::kBboReconstructionSourceBookTicker;
  reconstructed.valid = cxet::composite::kBboReconstructionFlagTrue;
  reconstructed.bidPresent = cxet::composite::kBboReconstructionFlagTrue;
  reconstructed.askPresent = cxet::composite::kBboReconstructionFlagTrue;
  trading_core::copyBookTickerSnapshot(engine.snapshot, top);
  trading_core::copyBookTickerSnapshot(engine.venueSnapshot, top);
  trading_core::copyReconstructedBboSnapshot(
      engine.snapshot, reconstructed, TimeNs{1000u});
  trading_core::copyReconstructedBboSnapshot(
      engine.venueSnapshot, reconstructed, TimeNs{1000u});
  setStrategySourceDeliveryTimestamp(
      engine, cxet::api::market::PublicMarketDataStream::BookTicker,
      1000ll);
}

BacktestStrategyInputPolicy probeInputPolicy(
    const trading_core::StrategyDescriptor& descriptor) noexcept {
  return resolveBacktestStrategyInputPolicy(
      descriptor, trading_core::StrategyParamStorage{});
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

BacktestRunRequest makeExplicitSessionLimitRequest(std::size_t count) {
  BacktestRunRequest request{};
  request.sessionPath = "/tmp/hft-backtest-leg-0";
  request.configPath = "/tmp/hft-backtest-missing-config.ini";
  for (std::size_t i = 1u; i < count; ++i) {
    BacktestSessionRequest session{};
    session.path = "/tmp/hft-backtest-leg-" + std::to_string(i);
    session.venue = "binance_futures";
    session.symbol = "BTC_USDT";
    request.sessions.push_back(std::move(session));
  }
  return request;
}

bool testBacktestExplicitSessionLimitAllowsRuntimeMaxAndRejectsOneMore() {
  const std::size_t maxLegs = trading_core::kStrategyRuntimeMaxLegs;
  BacktestRunRequest maxRequest = makeExplicitSessionLimitRequest(maxLegs);
  const BacktestRunResult maxResult = runBacktest(maxRequest, nullptr, nullptr);
  const bool maxWasNotRejectedByLegLimit =
      maxResult.error.find("too many backtest sessions") == std::string::npos &&
      maxResult.error.find("max " + std::to_string(maxLegs)) == std::string::npos;

  BacktestRunRequest overRequest = makeExplicitSessionLimitRequest(maxLegs + 1u);
  const BacktestRunResult overResult = runBacktest(overRequest, nullptr, nullptr);

  return expect(maxWasNotRejectedByLegLimit, "runtime max explicit sessions pass the backtest leg-count gate") &&
         expect(overResult.status == Status::InvalidArgument, "runtime max plus one explicit sessions are rejected") &&
         expect(overResult.error.find("too many backtest market legs") != std::string::npos,
                "runtime max plus one explicit sessions report too many market legs") &&
         expect(overResult.error.find("max " + std::to_string(maxLegs)) != std::string::npos,
                "runtime max plus one explicit sessions report the leg cap");
}

struct MultiLegContextProbeState {
  std::size_t marketDataCount{0u};
  std::size_t candleCount{0u};
  std::size_t instrumentCount{0u};
  std::size_t legMarketCount{0u};
  std::size_t orderStateCount{0u};
  std::size_t positionCount{0u};
  std::size_t accountCount{0u};
  std::size_t warmupCount{0u};
  std::uint32_t eventLegIndex{0u};
  std::uint32_t primaryLegIndex{0u};
  std::uint32_t defaultExecutionRouteRaw{0u};
  std::uint64_t tradableLegMask{0u};
  std::uint32_t haltReasonRaw{0u};
  bool hasEventLeg{false};
  bool contextPolicyExposed{false};
  bool inputDegraded{false};
  bool sawLastLegBookTicker{false};
  bool sawLastLegAccount{false};
  bool sawLastLegInstrument{false};
  bool sawLastLegMarket{false};
  bool defaultMarketAliasesMatchConfiguredRoute{false};
  bool defaultAliasesMatchConfiguredRoute{false};
  bool defaultStateAliasesMatchConfiguredRoute{false};
};

struct SingleLegContextProbeState {
  std::size_t legMarketCount{0u};
  std::size_t orderStateCount{0u};
  std::size_t positionCount{0u};
  std::uint32_t primaryLegIndex{0u};
  std::uint32_t defaultExecutionRouteRaw{0u};
  std::uint64_t tradableLegMask{0u};
  bool contextPolicyExposed{false};
  bool inputDegraded{false};
  bool sawRouteOrderState{false};
  bool sawRoutePosition{false};
  bool sawRouteMarket{false};
};

template<typename Context>
void captureContextTradePolicy(const Context& ctx, MultiLegContextProbeState& state) noexcept {
  if constexpr (requires(const Context& candidate) {
                  candidate.primaryLegIndex;
                  candidate.defaultExecutionRoute;
                  candidate.tradableLegMask;
                }) {
    state.contextPolicyExposed = true;
    state.primaryLegIndex = ctx.primaryLegIndex;
    state.defaultExecutionRouteRaw = ctx.defaultExecutionRoute.raw;
    state.tradableLegMask = ctx.tradableLegMask;
  } else {
    state.contextPolicyExposed = false;
  }
}

template<typename Context>
void captureContextTradePolicy(const Context& ctx, SingleLegContextProbeState& state) noexcept {
  if constexpr (requires(const Context& candidate) {
                  candidate.primaryLegIndex;
                  candidate.defaultExecutionRoute;
                  candidate.tradableLegMask;
                }) {
    state.contextPolicyExposed = true;
    state.primaryLegIndex = ctx.primaryLegIndex;
    state.defaultExecutionRouteRaw = ctx.defaultExecutionRoute.raw;
    state.tradableLegMask = ctx.tradableLegMask;
  } else {
    state.contextPolicyExposed = false;
  }
}

bool singleLegContextProbeRunCycle(void* rawState, trading_core::StrategyContext& ctx) noexcept {
  auto& state = *static_cast<SingleLegContextProbeState*>(rawState);
  state.legMarketCount = ctx.legMarkets.count;
  state.orderStateCount = ctx.orderStates.count;
  state.positionCount = ctx.positionStates.count;
  state.inputDegraded =
      ctx.control &&
      ctx.control->inputDegraded.raw == trading_core::kAlgoFlagTrue.raw;
  captureContextTradePolicy(ctx, state);
  state.sawRouteOrderState = ctx.orderStates.get(0u) != nullptr;
  if (const trading_core::StrategyPositionState* position = ctx.positionStates.get(0u)) {
    state.sawRoutePosition = position->valid && position->netQty.raw == 123000000ll;
  }
  if (const canon::MarketType* market = ctx.legMarkets.get(0u)) {
    state.sawRouteMarket = market->raw == canon::kMarketTypeSpot.raw;
  }
  return true;
}

bool multiLegContextProbeRunCycle(void* rawState, trading_core::StrategyContext& ctx) noexcept {
  auto& state = *static_cast<MultiLegContextProbeState*>(rawState);
  state.marketDataCount = ctx.marketData.count;
  state.candleCount = ctx.candles.count;
  state.instrumentCount = ctx.instrumentSpecs.count;
  state.legMarketCount = ctx.legMarkets.count;
  state.orderStateCount = ctx.orderStates.count;
  state.positionCount = ctx.positionStates.count;
  state.accountCount = ctx.accountStates.count;
  state.warmupCount = ctx.orderPathWarmups.count;
  state.eventLegIndex = ctx.eventLegIndex;
  state.hasEventLeg = ctx.hasEventLeg;
  state.inputDegraded = ctx.control && ctx.control->inputDegraded.raw == trading_core::kAlgoFlagTrue.raw;
  state.haltReasonRaw = ctx.control ? ctx.control->haltReason.raw : 0u;
  captureContextTradePolicy(ctx, state);
  if (ctx.marketData.snapshots && ctx.marketData.count > 8u) {
    state.sawLastLegBookTicker =
        ctx.marketData.snapshots[8u].lastBookTicker.bid.px.raw == kPx100 + 8ll * 100000000ll;
  }
  if (const trading_core::StrategyAccountState* account = ctx.accountStates.get(8u)) {
    state.sawLastLegAccount = account->initialBalanceE8 == 900000000ll;
  }
  if (const StrategyInstrumentSpec* instrument = ctx.instrumentSpecs.get(8u)) {
    state.sawLastLegInstrument = instrument->tickSizeRaw == 900000000ll;
  }
  if (const canon::MarketType* market = ctx.legMarkets.get(8u)) {
    state.sawLastLegMarket = market->raw == canon::kMarketTypeSpot.raw;
  }
  const std::size_t defaultLeg =
      trading_core::strategy::executionRouteLegIndex(
          ctx, trading_core::kDefaultExecutionRouteHandle);
  const StrategyInstrumentSpec* defaultInstrument =
      ctx.instrumentSpecs.get(defaultLeg);
  const trading_core::StrategyOrderStateView* defaultOrders =
      ctx.orderStates.get(defaultLeg);
  const trading_core::StrategyPositionState* defaultPosition =
      ctx.positionStates.get(defaultLeg);
  const trading_core::StrategyAccountState* defaultAccount =
      ctx.accountStates.get(defaultLeg);
  const trading_core::StrategyOrderPathWarmupView* defaultWarmup =
      ctx.orderPathWarmups.get(defaultLeg);
  const trading_core::StrategyMarketSnapshot* defaultSnapshot =
      ctx.marketData.snapshots && defaultLeg < ctx.marketData.count
          ? &ctx.marketData.snapshots[defaultLeg]
          : nullptr;
  const trading_core::StrategyCandleSnapshot* defaultCandle =
      ctx.candles.get(defaultLeg);
  const StrategyInstrumentSpec* routeZeroInstrument =
      trading_core::strategy::instrumentSpec(
          ctx, trading_core::kDefaultExecutionRouteHandle);
  const trading_core::StrategyOrderStateView* routeZeroOrders =
      trading_core::strategy::orderState(
          ctx, trading_core::kDefaultExecutionRouteHandle);
  const trading_core::StrategyPositionState* routeZeroPosition =
      trading_core::strategy::positionState(
          ctx, trading_core::kDefaultExecutionRouteHandle);
  const trading_core::StrategyAccountState* routeZeroAccount =
      trading_core::strategy::accountState(
          ctx, trading_core::kDefaultExecutionRouteHandle);
  state.defaultMarketAliasesMatchConfiguredRoute =
      defaultSnapshot && defaultCandle && ctx.snapshot == defaultSnapshot &&
      ctx.candle.ts.raw == defaultCandle->ts.raw &&
      ctx.candle.close.raw == defaultCandle->close.raw &&
      ctx.candle.valid.raw == defaultCandle->valid.raw;
  state.defaultAliasesMatchConfiguredRoute =
      defaultInstrument && defaultOrders && defaultPosition &&
      defaultAccount && defaultWarmup && routeZeroInstrument &&
      routeZeroOrders && routeZeroPosition && routeZeroAccount &&
      ctx.instrumentSpec.tickSizeRaw == defaultInstrument->tickSizeRaw &&
      routeZeroInstrument->tickSizeRaw == defaultInstrument->tickSizeRaw &&
      ctx.orderState.orders == defaultOrders->orders &&
      ctx.orderState.count == defaultOrders->count &&
      routeZeroOrders->orders == defaultOrders->orders &&
      ctx.positionState.netQty.raw == defaultPosition->netQty.raw &&
      routeZeroPosition->netQty.raw == defaultPosition->netQty.raw &&
      ctx.accountState.initialBalanceE8 ==
          defaultAccount->initialBalanceE8 &&
      routeZeroAccount->initialBalanceE8 ==
          defaultAccount->initialBalanceE8 &&
      ctx.orderPathWarmup.lastHotMono.raw ==
          defaultWarmup->lastHotMono.raw;
  state.defaultStateAliasesMatchConfiguredRoute =
      defaultOrders && defaultPosition && defaultAccount && defaultWarmup &&
      ctx.state.orders.primary.orders == defaultOrders->orders &&
      ctx.state.orders.primary.count == defaultOrders->count &&
      ctx.state.orders.primaryWarmup.lastHotMono.raw ==
          defaultWarmup->lastHotMono.raw &&
      ctx.state.positions.primary.netQty.raw == defaultPosition->netQty.raw &&
      ctx.state.account.primary.initialBalanceE8 ==
          defaultAccount->initialBalanceE8;
  return true;
}

struct NonPrimaryRouteOrderState {
  std::uint32_t routeRaw{0u};
};

struct RejectReasonProbeState {
  std::uint8_t reasonRaw{9u};
};

void rejectReasonProbeCaptureState(const void* rawState,
                                   trading_core::StrategyStateSnapshot& out) noexcept {
  out = trading_core::StrategyStateSnapshot{};
  if (!rawState) return;
  out.rejectReasonRaw = static_cast<const RejectReasonProbeState*>(rawState)->reasonRaw;
}

const char* rejectReasonProbeName(std::uint8_t reasonRaw) noexcept {
  return reasonRaw == 9u ? "intent_capacity" : "unknown";
}

bool nonPrimaryRouteOrderRunCycle(void* rawState, trading_core::StrategyContext& ctx) noexcept {
  auto& state = *static_cast<NonPrimaryRouteOrderState*>(rawState);
  if (ctx.io.orderOut.size() == 0u) return false;
  trading_core::OrderIntent& out = ctx.io.orderOut[0];
  out = trading_core::OrderIntent{};
  out.routeHandle = trading_core::ExecutionRouteHandle{state.routeRaw};
  out.clientIntentId.raw = 9001u;
  out.side = Side::Buy();
  out.orderType = canon::OrderType::Market;
  out.qty.raw = 100000000ll;
  ctx.io.orderCount = 1u;
  return true;
}

struct TransactionEmitProbeState {
  bool reportApplied{false};
  bool allAccepted{false};
  bool secondAttempted{false};
  std::uint8_t reportLegCount{0u};
};

template <typename State>
void probeCaptureState(const void*,
                       trading_core::StrategyStateSnapshot& out) noexcept {
  out = trading_core::StrategyStateSnapshot{};
}

template <typename State>
void probeRollbackCapacityCycle(
    void*, const trading_core::StrategyStateSnapshot&) noexcept {}

bool probeApplyEmitReport(
    void*, const trading_core::StrategyEmitReport&) noexcept { return true; }
bool probeReplayTransaction(
    void*, const trading_core::StrategyTransactionIntent&) noexcept { return true; }
std::uint64_t probeFinalizeRecoveryReplay(void*) noexcept { return 0u; }
std::size_t probeRecoveryTargets(
    const void*, std::int64_t*, std::size_t) noexcept { return 0u; }
void probeQuarantineLegs(void*, std::uint64_t) noexcept {}
void probeCompleteRecovery(void*, std::uint64_t, bool) noexcept {}
Id probeAllocateClientId(void*) noexcept {
  Id out{};
  out.raw = 1u;
  return out;
}

template <typename State,
          std::size_t MaxOrders = 0u,
          std::size_t MaxTransactions = 0u,
          std::size_t MaxCancels = 0u,
          std::size_t MaxCommands = 0u>
trading_core::StrategyDescriptor makeProbeDescriptor(
    const char* name,
    trading_core::StrategyRunCycleFn runCycle) noexcept {
  trading_core::StrategyDescriptor descriptor{};
  descriptor.name = name;
  descriptor.streamCount = 1u;
  descriptor.streams[0] =
      cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.footprint = trading_core::strategyRuntimeFootprint<State>(
      MaxOrders, MaxCancels, 0u, MaxCommands, MaxTransactions);
  descriptor.runCycle = runCycle;
  descriptor.captureState = &probeCaptureState<State>;
  if constexpr (MaxOrders != 0u || MaxTransactions != 0u ||
                MaxCancels != 0u || MaxCommands != 0u) {
    descriptor.rollbackCapacityCycle = &probeRollbackCapacityCycle<State>;
  }
  if constexpr (MaxTransactions != 0u) {
    descriptor.applyEmitReport = &probeApplyEmitReport;
    descriptor.replayTransaction = &probeReplayTransaction;
    descriptor.finalizeRecoveryReplay = &probeFinalizeRecoveryReplay;
    descriptor.recoveryTargets = &probeRecoveryTargets;
    descriptor.quarantineLegs = &probeQuarantineLegs;
    descriptor.completeRecovery = &probeCompleteRecovery;
    descriptor.allocateClientId = &probeAllocateClientId;
    descriptor.internalOrderFailureRollback = true;
  }
  return descriptor;
}

std::uint32_t oneMillisecondFreshnessMaxAge(
    const trading_core::StrategyParamStorage&) noexcept {
  return 1u;
}

bool transactionEmitProbeRunCycle(void*, trading_core::StrategyContext& ctx) noexcept {
  if (ctx.io.transactionOut.empty() || ctx.io.orderOut.size() < 2u) return false;

  trading_core::StrategyTransactionIntent transaction{};
  transaction.transactionId.raw = 9100u;
  transaction.touchedLegMask = 0x3u;
  transaction.semantic = trading_core::StrategyOrderSemantic::EntryAdd;
  transaction.legCount = 2u;
  transaction.legs[0].routeHandle = trading_core::ExecutionRouteHandle{1u};
  transaction.legs[0].clientIntentId.raw = 9101u;
  transaction.legs[0].signedDeltaRaw = 100000000ll;
  transaction.legs[0].expectedFillQty.raw = 100000000ll;
  transaction.legs[1].routeHandle = trading_core::ExecutionRouteHandle{2u};
  transaction.legs[1].clientIntentId.raw = 9102u;
  transaction.legs[1].signedDeltaRaw = -100000000ll;
  transaction.legs[1].expectedFillQty.raw = 100000000ll;
  if (!ctx.io.pushTransaction(transaction)) return false;

  trading_core::OrderIntent buy{};
  buy.routeHandle = trading_core::ExecutionRouteHandle{1u};
  buy.transactionId = transaction.transactionId;
  buy.clientIntentId = transaction.legs[0].clientIntentId;
  buy.semantic = transaction.semantic;
  buy.side = Side::Buy();
  buy.orderType = canon::OrderType::Market;
  buy.qty = transaction.legs[0].expectedFillQty;
  if (!ctx.io.pushOrder(buy)) return false;

  trading_core::OrderIntent sell{};
  sell.routeHandle = trading_core::ExecutionRouteHandle{2u};
  sell.transactionId = transaction.transactionId;
  sell.clientIntentId = transaction.legs[1].clientIntentId;
  sell.semantic = transaction.semantic;
  sell.side = Side::Sell();
  sell.orderType = canon::OrderType::Market;
  sell.qty = transaction.legs[1].expectedFillQty;
  return ctx.io.pushOrder(sell);
}

bool transactionEmitProbeApplyReport(void* rawState,
                                     const trading_core::StrategyEmitReport& report) noexcept {
  auto& state = *static_cast<TransactionEmitProbeState*>(rawState);
  state.reportApplied = report.transactionId.raw == 9100u &&
      report.legCount == 2u &&
      report.legs[0].clientIntentId.raw == 9101u &&
      report.legs[1].clientIntentId.raw == 9102u;
  state.allAccepted = report.allAccepted;
  state.secondAttempted = report.legCount > 1u && report.legs[1].attempted;
  state.reportLegCount = report.legCount;
  return state.reportApplied;
}

struct CancelParityProbeState {};

trading_core::OrderIntent parityProbeOrder(
    std::uint64_t clientIntentId) noexcept {
  trading_core::OrderIntent order{};
  order.routeHandle = trading_core::kDefaultExecutionRouteHandle;
  order.clientIntentId.raw = clientIntentId;
  order.side = Side::Buy();
  order.orderType = canon::OrderType::Market;
  order.qty.raw = 100000000ll;
  return order;
}

trading_core::CancelIntent parityProbeCancel(
    std::uint64_t clientIntentId,
    std::uint64_t targetClientIntentId,
    trading_core::ExecutionRouteHandle route) noexcept {
  trading_core::CancelIntent cancel{};
  cancel.routeHandle = route;
  cancel.clientIntentId.raw = clientIntentId;
  cancel.targetClientIntentId.raw = targetClientIntentId;
  cancel.targetOrderType = canon::OrderType::Limit;
  return cancel;
}

bool cancelParityProbeRunCycle(
    void*, trading_core::StrategyContext& ctx) noexcept {
  const trading_core::CancelIntent orderedDefault = parityProbeCancel(
      9201u, 8201u, trading_core::kDefaultExecutionRouteHandle);
  if (!ctx.io.pushOrderedCancel(orderedDefault)) return false;

  const trading_core::OrderIntent order = parityProbeOrder(9204u);
  if (!ctx.io.pushOrder(order)) return false;

  trading_core::WarmupOrderIntent warmup{};
  warmup.order = parityProbeOrder(9205u);
  warmup.order.orderType = canon::OrderType::Limit;
  warmup.order.limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
  warmup.order.price.raw = kPx100;
  warmup.order.service = true;
  if (!ctx.io.pushWarmupOrder(warmup)) return false;

  const trading_core::CancelIntent plainDefault = parityProbeCancel(
      9202u, 8202u, trading_core::kDefaultExecutionRouteHandle);
  if (!ctx.io.pushCancel(plainDefault)) return false;
  const trading_core::CancelIntent explicitNonPrimary = parityProbeCancel(
      9203u, 8203u, trading_core::ExecutionRouteHandle{1u});
  return ctx.io.pushCancel(explicitNonPrimary);
}

struct RouteZeroTransactionProbeState {
  bool reportApplied{false};
  bool accepted{false};
  std::uint64_t clientIntentId{0u};
  std::uint32_t reportRouteRaw{0u};
  std::int64_t requestedQtyRaw{0};
};

struct DefaultWarmupDedupProbeState {};

bool defaultWarmupDedupProbeRunCycle(
    void*, trading_core::StrategyContext& ctx) noexcept {
  trading_core::OrderIntent symbolicDefault = parityProbeOrder(9401u);
  symbolicDefault.orderType = canon::OrderType::Limit;
  symbolicDefault.limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
  symbolicDefault.price.raw = kPx100;
  if (!ctx.io.pushOrder(symbolicDefault)) return false;

  trading_core::OrderIntent explicitDefault = symbolicDefault;
  explicitDefault.clientIntentId.raw = 9402u;
  explicitDefault.routeHandle = trading_core::ExecutionRouteHandle{2u};
  return ctx.io.pushOrder(explicitDefault);
}

bool routeZeroTransactionProbeRunCycle(
    void*, trading_core::StrategyContext& ctx) noexcept {
  trading_core::StrategyTransactionIntent transaction{};
  transaction.transactionId.raw = 9300u;
  transaction.touchedLegMask = 0x2u;
  transaction.semantic = trading_core::StrategyOrderSemantic::EntryAdd;
  transaction.legCount = 1u;
  transaction.legs[0].routeHandle =
      trading_core::ExecutionRouteHandle{2u};
  transaction.legs[0].signedDeltaRaw = 100000000ll;
  if (!ctx.io.pushTransaction(transaction)) return false;

  trading_core::OrderIntent order = parityProbeOrder(9301u);
  order.transactionId = transaction.transactionId;
  order.semantic = transaction.semantic;
  return ctx.io.pushOrder(order);
}

bool routeZeroTransactionProbeApplyReport(
    void* rawState,
    const trading_core::StrategyEmitReport& report) noexcept {
  auto& state = *static_cast<RouteZeroTransactionProbeState*>(rawState);
  state.reportApplied = report.transactionId.raw == 9300u &&
      report.legCount == 1u;
  if (report.legCount == 1u) {
    state.accepted = report.legs[0].accepted;
    state.clientIntentId = report.legs[0].clientIntentId.raw;
    state.reportRouteRaw = report.legs[0].routeHandle.raw;
    state.requestedQtyRaw = report.legs[0].requestedQty.raw;
  }
  return state.reportApplied;
}

bool testRunCycleSinglePassesRouteContext() {
  EngineState engine{};
  StrategyInstrumentSpec instrument{};
  setTop(engine, kPx100, kPx100 + 100000000ll);
  engine.position.valid = true;
  engine.position.netQty.raw = 123000000ll;
  engine.market = canon::kMarketTypeSpot;
  instrument.tickSizeRaw = 100000000ll;
  instrument.stepSizeRaw = 100000000ll;
  instrument.tickGrid = trading_core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid = trading_core::prepareFastStepGrid(instrument.stepSizeRaw);

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<SingleLegContextProbeState>(
          "single_leg_context_probe", singleLegContextProbeRunCycle);

  SingleLegCycleScratch scratch{};
  SingleLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 7u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycle(descriptor, probeInputPolicy(descriptor), &state,
                           engine, instrument, scratch, shard, control,
                           1000ll, request, nullptr);

  return expect(ok, "single-leg cycle runs") &&
         expect(state.orderStateCount == 1u, "single-leg orderStates exposes route 0") &&
         expect(state.legMarketCount == 1u, "single-leg context exposes one market type") &&
         expect(state.positionCount == 1u, "single-leg positionStates exposes route 0") &&
         expect(state.contextPolicyExposed, "single-leg context exposes primary/tradable policy") &&
         expect(state.primaryLegIndex == 0u, "single-leg context clamps primary to leg 0") &&
         expect(state.defaultExecutionRouteRaw == 1u,
                "single-leg context exposes the configured default route") &&
         expect(state.tradableLegMask == 1ull, "single-leg context marks only leg 0 tradable") &&
         expect(state.sawRouteOrderState, "strategy can read single-leg route order state") &&
         expect(state.sawRoutePosition, "strategy can read single-leg route position") &&
         expect(state.sawRouteMarket, "strategy can read single-leg market type");
}

bool testRunCycleMultiPassesNineLegContextWithoutTruncation() {
  constexpr std::size_t kLegs = 9u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100 + static_cast<std::int64_t>(i) * 100000000ll, kPx100 + 1000000000ll);
    engines[i].initialBalanceE8 = static_cast<std::int64_t>(i + 1u) * 100000000ll;
    engines[i].walletBalanceE8 = engines[i].initialBalanceE8;
    engines[i].position.valid = true;
    engines[i].position.netQty.raw =
        static_cast<std::int64_t>(i + 1u) * 100000000ll;
    engines[i].lastOrderPathHotMono.raw =
        static_cast<std::uint64_t>(i + 1u);
    engines[i].candle.ts.raw = static_cast<std::uint64_t>(100u + i);
    engines[i].candle.close.raw =
        kPx100 + static_cast<std::int64_t>(i) * 200000000ll;
    engines[i].candle.valid = trading_core::kAlgoFlagTrue;
    engines[i].market = i == 8u ? canon::kMarketTypeSpot : canon::kMarketTypeFutures;
    instruments[i].tickSizeRaw = static_cast<std::int64_t>(i + 1u) * 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<MultiLegContextProbeState>(
          "multi_leg_context_probe", multiLegContextProbeRunCycle);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 1u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 8u, 1000ll, request,
      nullptr);

  return expect(ok, "9-leg multi cycle runs") &&
         expect(state.marketDataCount == kLegs, "marketData exposes all 9 legs") &&
         expect(state.candleCount == kLegs, "candles exposes all 9 legs") &&
         expect(state.instrumentCount == kLegs, "instrumentSpecs exposes all 9 legs") &&
         expect(state.legMarketCount == kLegs, "legMarkets exposes all 9 legs") &&
         expect(state.orderStateCount == kLegs, "orderStates exposes all 9 legs") &&
         expect(state.positionCount == kLegs, "positionStates exposes all 9 legs") &&
         expect(state.accountCount == kLegs, "accountStates exposes all 9 legs") &&
         expect(state.warmupCount == kLegs, "orderPathWarmups exposes all 9 legs") &&
         expect(state.hasEventLeg && state.eventLegIndex == 8u, "event leg 8 is preserved") &&
         expect(state.contextPolicyExposed, "strategy context exposes primary/tradable policy") &&
         expect(state.primaryLegIndex == 1u, "strategy context carries request primary leg") &&
         expect(state.defaultExecutionRouteRaw == 2u,
                "primary-only context resolves default route to the primary leg") &&
         expect(state.tradableLegMask == 2ull, "strategy context marks only the primary leg tradable") &&
         expect(state.sawLastLegBookTicker, "strategy can read the ninth leg market data") &&
         expect(state.sawLastLegAccount, "strategy can read the ninth leg account") &&
         expect(state.sawLastLegInstrument, "strategy can read the ninth leg instrument") &&
         expect(state.sawLastLegMarket, "strategy can read the ninth leg market type") &&
         expect(state.defaultMarketAliasesMatchConfiguredRoute,
                "snapshot and candle aliases bind configured default leg 1") &&
         expect(state.defaultAliasesMatchConfiguredRoute,
                "default execution aliases bind configured primary leg 1") &&
         expect(state.defaultStateAliasesMatchConfiguredRoute,
                "canonical state aliases bind configured primary leg 1");
}

bool testRunCycleMultiAllLegsKeepsSingularAliasesOnDefaultExecutionLeg() {
  constexpr std::size_t kLegs = 2u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i],
           kPx100 + static_cast<std::int64_t>(i) * 100000000ll,
           kPx100 + static_cast<std::int64_t>(i + 1u) * 100000000ll);
    engines[i].initialBalanceE8 =
        static_cast<std::int64_t>(i + 1u) * 100000000ll;
    engines[i].walletBalanceE8 = engines[i].initialBalanceE8;
    engines[i].position.valid = true;
    engines[i].position.netQty.raw =
        static_cast<std::int64_t>(i + 1u) * 100000000ll;
    engines[i].lastOrderPathHotMono.raw =
        static_cast<std::uint64_t>(i + 1u);
    engines[i].candle.ts.raw = static_cast<std::uint64_t>(200u + i);
    engines[i].candle.close.raw =
        kPx100 + static_cast<std::int64_t>(i) * 300000000ll;
    engines[i].candle.valid = trading_core::kAlgoFlagTrue;
    instruments[i].tickSizeRaw =
        static_cast<std::int64_t>(i + 1u) * 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<MultiLegContextProbeState>(
          "all_legs_default_alias_probe", multiLegContextProbeRunCycle);
  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 1u;
  request.tradeMode = BacktestTradeMode::AllLegs;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 1u, 1000ll, request, nullptr);

  return expect(ok, "all-legs alias cycle runs") &&
         expect(state.primaryLegIndex == 1u,
                "all-legs context retains reference primary leg 1") &&
         expect(state.defaultExecutionRouteRaw == 1u,
                "all-legs context resolves the default execution route to leg 0") &&
         expect(state.defaultMarketAliasesMatchConfiguredRoute,
                "snapshot and candle remain on default execution leg 0") &&
         expect(state.defaultAliasesMatchConfiguredRoute,
                "instrument and execution aliases remain on default leg 0") &&
         expect(state.defaultStateAliasesMatchConfiguredRoute,
                "canonical state aliases remain on default leg 0");
}

bool testRunCycleMultiMasksStaleSourceWithoutNewGlobalPolicy() {
  constexpr std::size_t kLegs = 3u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100 + static_cast<std::int64_t>(i) * 100000000ll, kPx100 + 1000000000ll);
    setStrategySourceDeliveryTimestamp(
        engines[i], cxet::api::market::PublicMarketDataStream::BookTicker,
        i < 2u ? kNowNs : 1ll);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<MultiLegContextProbeState>(
          "multi_leg_source_freshness_probe", multiLegContextProbeRunCycle);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 2u, kNowNs, request,
      nullptr);

  return expect(ok, "3-leg partial stale cycle runs") &&
         expect(state.contextPolicyExposed, "partial stale context exposes trade policy") &&
         expect(!state.inputDegraded,
                "3-leg context keeps its existing two-source global threshold") &&
         expect(state.haltReasonRaw == trading_core::kHaltReasonNone.raw,
                "3-leg partial stale context does not invent a global halt") &&
         expect(state.tradableLegMask == 3ull,
                "3-leg partial stale context masks only the stale leg");
}

bool testRunCycleMultiFailsClosedOnFutureSourceTimestamp() {
  constexpr std::size_t kLegs = 3u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100 + static_cast<std::int64_t>(i) * 100000000ll,
           kPx100 + 1000000000ll);
    setStrategySourceDeliveryTimestamp(
        engines[i], cxet::api::market::PublicMarketDataStream::BookTicker,
        i == 2u ? kNowNs + 1ll : kNowNs);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<MultiLegContextProbeState>(
          "multi_leg_source_future_probe", multiLegContextProbeRunCycle);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 2u, kNowNs, request,
      nullptr);

  return expect(ok, "3-leg future timestamp cycle runs for fail-closed handling") &&
         expect(!state.inputDegraded,
                "one future leg does not invent a new global DG-2 policy") &&
         expect(state.haltReasonRaw == trading_core::kHaltReasonNone.raw,
                "one future leg does not invent a global halt") &&
         expect(state.tradableLegMask == 3ull,
                "future source timestamp removes only its leg from tradable routes");
}

bool testRunCycleMultiGlobalHaltsWhenFewerThanTwoSourcesRemain() {
  constexpr std::size_t kLegs = 3u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100 + static_cast<std::int64_t>(i) * 100000000ll, kPx100 + 1000000000ll);
    setStrategySourceDeliveryTimestamp(
        engines[i], cxet::api::market::PublicMarketDataStream::BookTicker,
        i == 0u ? kNowNs : 1ll);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<MultiLegContextProbeState>(
          "multi_leg_source_halt_probe", multiLegContextProbeRunCycle);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 0u, kNowNs, request,
      nullptr);

  return expect(ok, "3-leg global stale cycle runs") &&
         expect(state.inputDegraded, "3-leg context globally halts when fewer than two sources are fresh") &&
         expect(state.haltReasonRaw == trading_core::kHaltReasonStaleInput.raw,
                "3-leg global stale context reports stale input halt") &&
         expect(state.tradableLegMask == 0ull, "3-leg global stale context disables tradable legs");
}

bool testRunCycleMultiRequiresEveryDescriptorStreamPerLeg() {
  constexpr std::size_t kLegs = 3u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    cxet::composite::TradeRuntimeV1 trade{};
    trading_core::copyTradeSnapshot(engines[i].snapshot, trade);
    setStrategySourceDeliveryTimestamp(
        engines[i], cxet::api::market::PublicMarketDataStream::BookTicker,
        kNowNs);
    setStrategySourceDeliveryTimestamp(
        engines[i], cxet::api::market::PublicMarketDataStream::Trades,
        i < 2u ? kNowNs : 1ll);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<MultiLegContextProbeState>(
          "multi_stream_source_freshness_probe",
          multiLegContextProbeRunCycle);
  descriptor.streamCount = 2u;
  descriptor.streams[0] =
      cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.streams[1] =
      cxet::api::market::PublicMarketDataStream::Trades;
  descriptor.requireFirstTradeForReadiness = true;
  const BacktestStrategyInputPolicy inputPolicy =
      probeInputPolicy(descriptor);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  const bool ok = runCycleMulti(
      descriptor, inputPolicy, &state, engines, instruments, *scratch,
      shard, control, 2u, kNowNs, request, nullptr);

  descriptor.optionalStreamMask = 1u << 1u;
  const BacktestStrategyInputPolicy optionalTrades =
      probeInputPolicy(descriptor);
  descriptor.optionalStreamMask = 0u;
  descriptor.requireFirstTradeForReadiness = false;
  const BacktestStrategyInputPolicy noFirstTradeGate =
      probeInputPolicy(descriptor);
  const auto bookTickerBit =
      cxet::api::market::publicMarketDataStreamReadyBit(
          cxet::api::market::PublicMarketDataStream::BookTicker);

  return expect(ok, "multi-stream freshness cycle runs") &&
         expect(!state.inputDegraded,
                "two fully fresh legs preserve the existing 3-leg threshold") &&
         expect(state.tradableLegMask == 3ull,
                "fresh BBO cannot hide stale required Trades on leg 2") &&
         expect(optionalTrades.requiredStreamReadyMask == bookTickerBit,
                "optional Trades is excluded from the freshness contract") &&
         expect(noFirstTradeGate.requiredStreamReadyMask == bookTickerBit,
                "Trades without first-trade readiness is excluded like live");
}

bool testBacktestResolvesDescriptorFreshnessMaxAge() {
  constexpr std::int64_t kNowNs = 2000000ll;
  EngineState engine{};
  StrategyInstrumentSpec instrument{};
  setTop(engine, kPx100, kPx100 + 100000000ll);
  instrument.tickSizeRaw = 100000000ll;
  instrument.stepSizeRaw = 100000000ll;
  instrument.tickGrid =
      trading_core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid =
      trading_core::prepareFastStepGrid(instrument.stepSizeRaw);

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<SingleLegContextProbeState>(
          "configured_freshness_age_probe", singleLegContextProbeRunCycle);
  descriptor.inputFreshnessMaxAgeMs = &oneMillisecondFreshnessMaxAge;
  const BacktestStrategyInputPolicy inputPolicy =
      probeInputPolicy(descriptor);
  SingleLegCycleScratch scratch{};
  SingleLegContextProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  const bool ok = runCycle(
      descriptor, inputPolicy, &state, engine, instrument, scratch, shard,
      control, kNowNs, request, nullptr);

  return expect(inputPolicy.maxAgeNs == 1000000u,
                "descriptor max_quote_age_ms is resolved once into ns") &&
         expect(ok, "configured freshness cycle runs") &&
         expect(state.inputDegraded,
                "configured 1ms age rejects input that default 5000ms accepts") &&
         expect(state.tradableLegMask == 0u,
                "configured stale input disables the single tradable leg");
}

bool testBacktestTimelineKindsOwnIndependentFreshnessSlots() {
  using Stream = cxet::api::market::PublicMarketDataStream;
  struct Mapping {
    std::uint8_t kind;
    Stream stream;
  };
  constexpr std::array<Mapping,
                       cxet::api::market::kPublicMarketDataStreamCount>
      mappings{{
          {kTimelineKindDepth, Stream::Orderbook},
          {kTimelineKindTrade, Stream::Trades},
          {kTimelineKindBookTicker, Stream::BookTicker},
          {kTimelineKindPriceLimit, Stream::PriceLimit},
          {kTimelineKindLiquidation, Stream::Liquidations},
          {kTimelineKindMarkPrice, Stream::MarkPrice},
          {kTimelineKindIndexPrice, Stream::IndexPrice},
          {kTimelineKindFunding, Stream::Funding},
          {kTimelineKindOpenInterest, Stream::OpenInterest},
      }};
  EngineState engine{};
  std::int64_t timestampNs = 100u;
  for (const Mapping mapping : mappings) {
    TimelineEvent event{};
    event.kind = mapping.kind;
    event.deliveryTsNs = timestampNs++;
    event.applicationOnly = mapping.kind == kTimelineKindBookTicker;
    recordBacktestStrategySourceDelivery(engine, event);
    if (!expect(strategySourceDeliveryTimestamp(engine, mapping.stream) ==
                    event.deliveryTsNs,
                "timeline kind updates only its native freshness slot")) {
      return false;
    }
  }
  const auto beforeCandle = engine.strategySourceDeliveryTsNs;
  TimelineEvent candle{};
  candle.kind = kTimelineKindCandle;
  candle.deliveryTsNs = 999u;
  recordBacktestStrategySourceDelivery(engine, candle);
  return expect(engine.strategySourceDeliveryTsNs == beforeCandle,
                "candle delivery cannot refresh a public market stream");
}

bool testRunCycleMultiPrimaryOnlyDropsNonPrimaryRouteOrder() {
  constexpr std::size_t kLegs = 2u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<NonPrimaryRouteOrderState, 1u>(
          "primary_only_filter_probe", nonPrimaryRouteOrderRunCycle);

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  NonPrimaryRouteOrderState state{};
  state.routeRaw = 2u;
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 0u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 0u, 1000ll, request,
      nullptr);

  return expect(ok, "primary-only filter cycle runs") &&
         expect(engines[0].orders.empty(), "primary leg receives no rerouted non-primary order") &&
         expect(engines[1].orders.empty(), "non-primary route order is not executed");
}

bool testRunCycleMultiDefaultRouteTargetsConfiguredPrimary() {
  constexpr std::size_t kLegs = 2u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    engines[i].lastOrderPathHotMono.raw = 1000u;
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<NonPrimaryRouteOrderState, 1u>(
          "default_route_primary_probe", nonPrimaryRouteOrderRunCycle);
  auto scratch = std::make_unique<MultiLegCycleScratch>();
  NonPrimaryRouteOrderState state{};
  state.routeRaw = trading_core::kDefaultExecutionRouteHandle.raw;
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 1u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 1u, 1000ll, request,
      nullptr);

  return expect(ok, "default-route primary-only cycle runs") &&
         expect(engines[0].orders.empty(),
                "default route does not fall back to leg 0") &&
         expect(engines[1].orders.size() == 1u,
                "default route targets configured primary leg 1");
}

bool testRunCycleMultiDefaultRouteRejectsStaleConfiguredLeg() {
  constexpr std::size_t kLegs = 3u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    setStrategySourceDeliveryTimestamp(
        engines[i], cxet::api::market::PublicMarketDataStream::BookTicker,
        i == 0u ? 1ll : kNowNs);
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    engines[i].lastOrderPathHotMono.raw =
        static_cast<std::uint64_t>(kNowNs);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<NonPrimaryRouteOrderState, 1u>(
          "default_route_stale_probe", nonPrimaryRouteOrderRunCycle);
  auto scratch = std::make_unique<MultiLegCycleScratch>();
  NonPrimaryRouteOrderState state{};
  state.routeRaw = trading_core::kDefaultExecutionRouteHandle.raw;
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 2u, kNowNs, request,
      nullptr);

  return expect(ok, "default-route partial-stale cycle runs") &&
         expect(engines[0].orders.empty(),
                "stale configured default leg rejects order-like output") &&
         expect(engines[1].orders.empty() && engines[2].orders.empty(),
                "stale default order is not rerouted to a fresh leg");
}

bool testRunCycleMultiRetainsCancelsWhenDefaultPrimaryIsStale() {
  constexpr std::size_t kLegs = 2u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    setStrategySourceDeliveryTimestamp(
        engines[i], cxet::api::market::PublicMarketDataStream::BookTicker,
        i == 0u ? kNowNs : 1ll);
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<CancelParityProbeState, 1u, 0u, 2u, 2u>(
          "stale_default_cancel_parity_probe", cancelParityProbeRunCycle);
  auto scratch = std::make_unique<MultiLegCycleScratch>();
  CancelParityProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 1u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 0u, kNowNs, request,
      nullptr);

  const auto hasRecord = [](const EngineState& engine,
                            std::uint64_t id,
                            std::uint8_t action) noexcept {
    for (const JsonOrder& record : engine.orders) {
      if (record.id == id && record.action == action) return true;
    }
    return false;
  };
  return expect(ok, "stale default-route cancel cycle runs") &&
         expect(hasRecord(engines[1], 9201u, kRecordActionCancel),
                "ordered default-route cancel survives stale primary") &&
         expect(hasRecord(engines[1], 9202u, kRecordActionCancel),
                "plain default-route cancel survives stale primary") &&
         expect(hasRecord(engines[0], 9203u, kRecordActionCancel),
                "explicit non-primary cancel survives PrimaryOnly") &&
         expect(!hasRecord(engines[1], 9204u, kRecordActionOrder),
                "default-route order is stripped while emission is blocked") &&
         expect(!hasRecord(engines[1], 9205u, kRecordActionOrder),
                "default-route warmup is stripped while emission is blocked") &&
         expect(engines[0].orders.size() == 1u &&
                    engines[1].orders.size() == 2u,
                "blocked cycle emits only the three requested cancels");
}

bool testRunCycleMultiBridgesTransactionOutputToEmitReport() {
  constexpr std::size_t kLegs = 2u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<TransactionEmitProbeState, 2u, 1u>(
          "transaction_emit_probe", transactionEmitProbeRunCycle);
  descriptor.applyEmitReport = transactionEmitProbeApplyReport;

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  TransactionEmitProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  std::vector<EngineState> blockedEngines = engines;
  std::vector<EngineState> filteredEngines = engines;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 0u, 1000ll, request,
      nullptr);
  control.commandDegraded = trading_core::kAlgoFlagTrue;
  TransactionEmitProbeState blockedState{};
  const bool blockedOk = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &blockedState, blockedEngines,
      instruments, *scratch, shard, control, 0u, 1001ll, request, nullptr);
  control = trading_core::ShardControlState{};
  request.hasPrimaryLegIndex = true;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;
  TransactionEmitProbeState filteredState{};
  const bool filteredOk = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &filteredState, filteredEngines,
      instruments, *scratch, shard, control, 0u, 1002ll, request, nullptr);

  return expect(ok, "multi-leg transaction bridge cycle runs") &&
         expect(state.reportApplied, "multi-leg transaction bridge applies accepted emit report") &&
         expect(state.allAccepted, "multi-leg transaction bridge marks both legs accepted") &&
         expect(state.reportLegCount == 2u, "multi-leg transaction bridge preserves both transaction legs") &&
         expect(engines[0].orders.size() == 1u, "multi-leg transaction bridge submits first leg order") &&
         expect(engines[1].orders.size() == 1u, "multi-leg transaction bridge submits second leg order") &&
         expect(!blockedOk && blockedState.reportApplied && !blockedState.allAccepted,
                "blocked transaction reports local fault then fails closed") &&
         expect(blockedEngines[0].orders.empty() && blockedEngines[1].orders.empty(),
                "blocked transaction never reaches simulated order effects") &&
         expect(!filteredOk && filteredState.reportApplied &&
                    !filteredState.allAccepted &&
                    !filteredState.secondAttempted,
                "filtered transaction reports unattempted leg then fails closed") &&
         expect(filteredEngines[0].orders.empty() &&
                    filteredEngines[1].orders.empty(),
                "filtered transaction never reaches simulated order effects");
}

bool testRunCycleMultiBindsRouteZeroOrderToExplicitTransactionLeg() {
  constexpr std::size_t kLegs = 2u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    engines[i].lastOrderPathHotMono.raw = 1000u;
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<RouteZeroTransactionProbeState, 1u, 1u>(
          "route_zero_transaction_binding_probe",
          routeZeroTransactionProbeRunCycle);
  descriptor.applyEmitReport = routeZeroTransactionProbeApplyReport;
  auto scratch = std::make_unique<MultiLegCycleScratch>();
  RouteZeroTransactionProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 1u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 1u, 1000ll, request,
      nullptr);

  return expect(ok, "route-zero transaction binding cycle runs") &&
         expect(state.reportApplied,
                "route-zero transaction binding applies an emit report") &&
         expect(state.accepted,
                "route-zero order resolves to and accepts explicit leg 2") &&
         expect(state.clientIntentId == 9301u,
                "transaction leg receives route-zero order client id") &&
         expect(state.reportRouteRaw == 2u,
                "emit report records the resolved explicit transaction route") &&
         expect(state.requestedQtyRaw == 100000000ll,
                "transaction leg receives route-zero order quantity") &&
         expect(engines[0].orders.empty(),
                "route-zero transaction order does not fall back to leg 0") &&
         expect(engines[1].orders.size() == 1u &&
                    engines[1].orders[0].id == 9301u &&
                    engines[1].orders[0].action == kRecordActionOrder,
                "route-zero transaction order executes on configured leg 1");
}

bool testRunCycleMultiDeduplicatesWarmupByResolvedDefaultLeg() {
  constexpr std::size_t kLegs = 2u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100, kPx100 + 100000000ll);
    engines[i].legIndex = static_cast<std::uint32_t>(i);
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid =
        trading_core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid =
        trading_core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  trading_core::StrategyDescriptor descriptor =
      makeProbeDescriptor<DefaultWarmupDedupProbeState, 2u, 0u, 0u, 2u>(
          "resolved_default_warmup_dedup_probe",
          defaultWarmupDedupProbeRunCycle);
  descriptor.restoreAfterOrderFailure =
      &probeRollbackCapacityCycle<DefaultWarmupDedupProbeState>;

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  DefaultWarmupDedupProbeState state{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 1u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, probeInputPolicy(descriptor), &state, engines,
      instruments, *scratch, shard, control, 0u, 1000ll, request,
      nullptr);

  return expect(ok, "resolved-default warmup cycle runs") &&
         expect(engines[0].orders.empty(),
                "default-route warmup is not misrouted to leg 0") &&
         expect(engines[1].orders.size() == 1u,
                "symbolic and explicit default aliases emit one warmup");
}

bool testBacktestRejectsExplicitInvalidPrimaryBeforeLoad() {
  BacktestRunRequest request = makeExplicitSessionLimitRequest(2u);
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 2u;

  const BacktestRunResult result = runBacktest(request, nullptr, nullptr);

  return expect(result.status == Status::InvalidArgument, "invalid explicit primary leg is rejected") &&
         expect(result.error.find("primary leg index") != std::string::npos,
                "invalid explicit primary leg reports primary leg index") &&
         expect(result.error.find("config") == std::string::npos,
                "invalid explicit primary leg is rejected before config load");
}

bool testStrategyRunCycleErrorIncludesRejectReasonContext() {
  trading_core::StrategyDescriptor descriptor{};
  descriptor.name = "reject_reason_probe";
  descriptor.captureState = rejectReasonProbeCaptureState;
  descriptor.rejectReasonName = rejectReasonProbeName;
  RejectReasonProbeState state{};
  state.reasonRaw = 9u;

  const std::string error = formatStrategyRunCycleError(descriptor, &state, "delivered", 123456789ll, 7u);

  return expect(error.find("strategy runCycle failed on delivered market event") != std::string::npos,
                "runCycle error keeps phase message") &&
         expect(error.find("strategy=reject_reason_probe") != std::string::npos,
                "runCycle error includes strategy name") &&
         expect(error.find("reject_reason=intent_capacity") != std::string::npos,
                "runCycle error includes reject reason name") &&
         expect(error.find("reject_reason_raw=9") != std::string::npos,
                "runCycle error includes raw reject reason") &&
         expect(error.find("ts_ns=123456789") != std::string::npos,
                "runCycle error includes timestamp") &&
         expect(error.find("event_leg_index=7") != std::string::npos,
                "runCycle error includes event leg index");
}

bool testPairwiseTakerMultiLegReplayRunsCycleForEveryDeliveredBookTicker() {
  constexpr std::size_t kLegs = 5u;
  const char* exchanges[kLegs] = {"binance", "bybit", "okx", "bitget", "kucoin"};
  const char* venues[kLegs] = {"binance_futures", "bybit_futures", "okx_futures", "bitget_futures", "kucoin_futures"};
  const std::filesystem::path root = makePairwiseReplayRoot();
  for (std::size_t i = 0u; i < kLegs; ++i) {
    writePairwiseReplaySession(root / ("leg" + std::to_string(i)),
                               exchanges[i],
                               static_cast<std::int64_t>(i) * 100000000ll,
                               2000000000ll + static_cast<std::int64_t>(i) * 1000000ll,
                               static_cast<std::int64_t>(i) * 10ll + 1ll);
  }
  writePairwiseReplayConfig(root / "pairwise.ini");

  BacktestRunRequest request{};
  request.sessionPath = root / "leg0";
  for (std::size_t i = 1u; i < kLegs; ++i) {
    request.sessions.push_back(BacktestSessionRequest{root / ("leg" + std::to_string(i)), venues[i], "BTC_USDT"});
  }
  request.configPath = root / "pairwise.ini";
  request.outputPath = root / "backtests" / "pairwise-full-scan";
  request.strategy = "stat_arb_pairwise_taker";
  request.runId = "pairwise-full-scan";
  request.initialBalanceE8 = 1000000000000ll;
  request.hasMakerFeeBps = true;
  request.hasTakerFeeBps = true;
  request.makerFeeBpsE8 = 0;
  request.takerFeeBpsE8 = 0;
  request.marketOrderLatency.baseUs = 0;
  request.limitOrderLatency.baseUs = 0;
  request.cancelOrderLatency.baseUs = 0;
  request.userDataLatency.baseUs = 0;
  request.orderLatencyUs = 0;
  request.cancelLatencyUs = 0;

  const BacktestRunResult result = runBacktest(request, nullptr, nullptr);
  if (!expect(result.status == Status::Ok, "pairwise replay completes")) {
    std::cerr << "status=" << statusToString(result.status)
              << " error=" << result.error << "\n";
    return false;
  }
  const std::uint64_t expectedDeliveredCycles = kLegs * 2u;
  return expect(result.bookTickers == kLegs * 2u, "pairwise replay counts all bookticker rows") &&
         expect(result.inputCoverage.strategyCycles == expectedDeliveredCycles,
                "pairwise replay invokes every delivered bookticker cycle so degraded input is observable") &&
         expect(result.inputCoverage.cyclesWithBookTicker == expectedDeliveredCycles,
                "pairwise replay cycles retain the available bookticker context");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testBacktestExplicitSessionLimitAllowsRuntimeMaxAndRejectsOneMore() && ok;
  ok = testRunCycleSinglePassesRouteContext() && ok;
  ok = testRunCycleMultiPassesNineLegContextWithoutTruncation() && ok;
  ok = testRunCycleMultiAllLegsKeepsSingularAliasesOnDefaultExecutionLeg() && ok;
  ok = testRunCycleMultiMasksStaleSourceWithoutNewGlobalPolicy() && ok;
  ok = testRunCycleMultiFailsClosedOnFutureSourceTimestamp() && ok;
  ok = testRunCycleMultiGlobalHaltsWhenFewerThanTwoSourcesRemain() && ok;
  ok = testRunCycleMultiRequiresEveryDescriptorStreamPerLeg() && ok;
  ok = testBacktestResolvesDescriptorFreshnessMaxAge() && ok;
  ok = testBacktestTimelineKindsOwnIndependentFreshnessSlots() && ok;
  ok = testRunCycleMultiPrimaryOnlyDropsNonPrimaryRouteOrder() && ok;
  ok = testRunCycleMultiDefaultRouteTargetsConfiguredPrimary() && ok;
  ok = testRunCycleMultiDefaultRouteRejectsStaleConfiguredLeg() && ok;
  ok = testRunCycleMultiRetainsCancelsWhenDefaultPrimaryIsStale() && ok;
  ok = testRunCycleMultiBridgesTransactionOutputToEmitReport() && ok;
  ok = testRunCycleMultiBindsRouteZeroOrderToExplicitTransactionLeg() && ok;
  ok = testRunCycleMultiDeduplicatesWarmupByResolvedDefaultLeg() && ok;
  ok = testBacktestRejectsExplicitInvalidPrimaryBeforeLoad() && ok;
  ok = testStrategyRunCycleErrorIncludesRejectReasonContext() && ok;
  ok = testPairwiseTakerMultiLegReplayRunsCycleForEveryDeliveredBookTicker() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
