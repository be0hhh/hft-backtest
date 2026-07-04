#include "engine/backtest_engine.hpp"
#include "runner/backtest_run_error.hpp"
#include "strategy/backtest_strategy_runtime.hpp"

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
                          std::int64_t seq) {
  return "[" + std::to_string(bid) + "," + std::to_string(bidQty) + "," +
         std::to_string(ask) + "," + std::to_string(askQty) + "," +
         std::to_string(tsNs) + ",1," + std::to_string(seq) + "]\n";
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
  std::string manifest =
      "{\n"
      "  \"identity\": {\"exchange\": \"";
  manifest.append(exchange);
  manifest +=
      "\", \"market\": \"futures\", \"symbols\": [\"BTC_USDT\"]},\n"
      "  \"channels\": {\n"
      "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"}\n"
      "  },\n"
      "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
      "}\n";
  writeText(dir / "manifest.json", manifest);
  writeText(dir / "instrument_metadata.json",
            "{\"tick_size_e8\":1000000,\"lot_size_e8\":100000000,\"contract_base_qty_e8\":100000000}\n");
  const std::int64_t bid = kPx100 + priceOffset;
  const std::int64_t ask = bid + 100000000ll;
  writeText(dir / "jsonl" / "bookticker.jsonl",
            bookTickerRow(bid, 100000000ll, ask, 100000000ll, 1000000000ll, seqBase) +
                bookTickerRow(bid + 1000000ll, 100000000ll, ask + 1000000ll, 100000000ll, secondTsNs, seqBase + 1ll));
}

void writePairwiseReplayConfig(const std::filesystem::path& path) {
  writeText(path,
            "[strategy]\n"
            "type=stat_arb_pairwise_taker\n"
            "enabled=1\n"
            "order_qty=1\n"
            "ema_window_ms=5000\n"
            "entry_edge_bps=100000\n"
            "ladder_step_bps=1\n"
            "exit_edge_bps=0\n"
            "max_ladder_levels=3\n"
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
  hft_trader::core::copyBookTickerSnapshot(engine.snapshot, top);
  hft_trader::core::copyBookTickerSnapshot(engine.venueSnapshot, top);
  engine.lastStrategySourceMarketDataTsNs = 1000ll;
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
  const std::size_t maxLegs = hft_trader::core::kStrategyRuntimeMaxLegs;
  BacktestRunRequest maxRequest = makeExplicitSessionLimitRequest(maxLegs);
  const BacktestRunResult maxResult = runBacktest(maxRequest, nullptr, nullptr);
  const bool maxWasNotRejectedByLegLimit =
      maxResult.error.find("too many backtest sessions") == std::string::npos &&
      maxResult.error.find("max " + std::to_string(maxLegs)) == std::string::npos;

  BacktestRunRequest overRequest = makeExplicitSessionLimitRequest(maxLegs + 1u);
  const BacktestRunResult overResult = runBacktest(overRequest, nullptr, nullptr);

  return expect(maxWasNotRejectedByLegLimit, "runtime max explicit sessions pass the backtest leg-count gate") &&
         expect(overResult.status == Status::InvalidArgument, "runtime max plus one explicit sessions are rejected") &&
         expect(overResult.error.find("too many backtest sessions") != std::string::npos,
                "runtime max plus one explicit sessions report too many sessions") &&
         expect(overResult.error.find("max " + std::to_string(maxLegs)) != std::string::npos,
                "runtime max plus one explicit sessions report the leg cap");
}

struct MultiLegContextProbeState {
  std::size_t marketDataCount{0u};
  std::size_t candleCount{0u};
  std::size_t instrumentCount{0u};
  std::size_t orderStateCount{0u};
  std::size_t positionCount{0u};
  std::size_t accountCount{0u};
  std::size_t warmupCount{0u};
  std::uint32_t eventLegIndex{0u};
  std::uint32_t primaryLegIndex{0u};
  std::uint64_t tradableLegMask{0u};
  std::uint32_t haltReasonRaw{0u};
  bool hasEventLeg{false};
  bool contextPolicyExposed{false};
  bool inputDegraded{false};
  bool sawLastLegBookTicker{false};
  bool sawLastLegAccount{false};
  bool sawLastLegInstrument{false};
};

struct SingleLegContextProbeState {
  std::size_t orderStateCount{0u};
  std::size_t positionCount{0u};
  std::uint32_t primaryLegIndex{0u};
  std::uint64_t tradableLegMask{0u};
  bool contextPolicyExposed{false};
  bool sawRouteOrderState{false};
  bool sawRoutePosition{false};
};

template<typename Context>
void captureContextTradePolicy(const Context& ctx, MultiLegContextProbeState& state) noexcept {
  if constexpr (requires(const Context& candidate) {
                  candidate.primaryLegIndex;
                  candidate.tradableLegMask;
                }) {
    state.contextPolicyExposed = true;
    state.primaryLegIndex = ctx.primaryLegIndex;
    state.tradableLegMask = ctx.tradableLegMask;
  } else {
    state.contextPolicyExposed = false;
  }
}

template<typename Context>
void captureContextTradePolicy(const Context& ctx, SingleLegContextProbeState& state) noexcept {
  if constexpr (requires(const Context& candidate) {
                  candidate.primaryLegIndex;
                  candidate.tradableLegMask;
                }) {
    state.contextPolicyExposed = true;
    state.primaryLegIndex = ctx.primaryLegIndex;
    state.tradableLegMask = ctx.tradableLegMask;
  } else {
    state.contextPolicyExposed = false;
  }
}

bool singleLegContextProbeRunCycle(void* rawState, hft_trader::core::StrategyContext& ctx) noexcept {
  auto& state = *static_cast<SingleLegContextProbeState*>(rawState);
  state.orderStateCount = ctx.orderStates.count;
  state.positionCount = ctx.positionStates.count;
  captureContextTradePolicy(ctx, state);
  state.sawRouteOrderState = ctx.orderStates.get(0u) != nullptr;
  if (const hft_trader::core::StrategyPositionState* position = ctx.positionStates.get(0u)) {
    state.sawRoutePosition = position->valid && position->netQty.raw == 123000000ll;
  }
  return true;
}

bool multiLegContextProbeRunCycle(void* rawState, hft_trader::core::StrategyContext& ctx) noexcept {
  auto& state = *static_cast<MultiLegContextProbeState*>(rawState);
  state.marketDataCount = ctx.marketData.count;
  state.candleCount = ctx.candles.count;
  state.instrumentCount = ctx.instrumentSpecs.count;
  state.orderStateCount = ctx.orderStates.count;
  state.positionCount = ctx.positionStates.count;
  state.accountCount = ctx.accountStates.count;
  state.warmupCount = ctx.orderPathWarmups.count;
  state.eventLegIndex = ctx.eventLegIndex;
  state.hasEventLeg = ctx.hasEventLeg;
  state.inputDegraded = ctx.control && ctx.control->inputDegraded.raw == hft_trader::core::kAlgoFlagTrue.raw;
  state.haltReasonRaw = ctx.control ? ctx.control->haltReason.raw : 0u;
  captureContextTradePolicy(ctx, state);
  if (ctx.marketData.snapshots && ctx.marketData.count > 8u) {
    state.sawLastLegBookTicker =
        ctx.marketData.snapshots[8u].lastBookTicker.bid.px.raw == kPx100 + 8ll * 100000000ll;
  }
  if (const hft_trader::core::StrategyAccountState* account = ctx.accountStates.get(8u)) {
    state.sawLastLegAccount = account->initialBalanceE8 == 900000000ll;
  }
  if (const StrategyInstrumentSpec* instrument = ctx.instrumentSpecs.get(8u)) {
    state.sawLastLegInstrument = instrument->tickSizeRaw == 900000000ll;
  }
  return true;
}

struct NonPrimaryRouteOrderState {
  std::uint32_t routeRaw{0u};
};

struct RejectReasonProbeState {
  std::uint8_t reasonRaw{9u};
};

void rejectReasonProbeCaptureState(const void* rawState,
                                   hft_trader::runtime::StrategyStateSnapshot& out) noexcept {
  out = hft_trader::runtime::StrategyStateSnapshot{};
  if (!rawState) return;
  out.rejectReasonRaw = static_cast<const RejectReasonProbeState*>(rawState)->reasonRaw;
}

const char* rejectReasonProbeName(std::uint8_t reasonRaw) noexcept {
  return reasonRaw == 9u ? "intent_capacity" : "unknown";
}

bool nonPrimaryRouteOrderRunCycle(void* rawState, hft_trader::core::StrategyContext& ctx) noexcept {
  auto& state = *static_cast<NonPrimaryRouteOrderState*>(rawState);
  if (ctx.io.orderOut.size() == 0u) return false;
  hft_trader::core::OrderIntent& out = ctx.io.orderOut[0];
  out = hft_trader::core::OrderIntent{};
  out.routeHandle = hft_trader::core::ExecutionRouteHandle{state.routeRaw};
  out.clientIntentId.raw = 9001u;
  out.side = Side::Buy();
  out.orderType = canon::OrderType::Market;
  out.qty.raw = 100000000ll;
  ctx.io.orderCount = 1u;
  return true;
}

bool testRunCycleSinglePassesRouteContext() {
  EngineState engine{};
  StrategyInstrumentSpec instrument{};
  setTop(engine, kPx100, kPx100 + 100000000ll);
  engine.position.valid = true;
  engine.position.netQty.raw = 123000000ll;
  instrument.tickSizeRaw = 100000000ll;
  instrument.stepSizeRaw = 100000000ll;
  instrument.tickGrid = hft_trader::core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid = hft_trader::core::prepareFastStepGrid(instrument.stepSizeRaw);

  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "single_leg_context_probe";
  descriptor.streamCount = 1u;
  descriptor.streams[0] = cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.runCycle = singleLegContextProbeRunCycle;

  SingleLegCycleScratch scratch{};
  SingleLegContextProbeState state{};
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 7u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycle(descriptor, &state, engine, instrument, scratch, shard, control, 1000ll, request, nullptr);

  return expect(ok, "single-leg cycle runs") &&
         expect(state.orderStateCount == 1u, "single-leg orderStates exposes route 0") &&
         expect(state.positionCount == 1u, "single-leg positionStates exposes route 0") &&
         expect(state.contextPolicyExposed, "single-leg context exposes primary/tradable policy") &&
         expect(state.primaryLegIndex == 0u, "single-leg context clamps primary to leg 0") &&
         expect(state.tradableLegMask == 1ull, "single-leg context marks only leg 0 tradable") &&
         expect(state.sawRouteOrderState, "strategy can read single-leg route order state") &&
         expect(state.sawRoutePosition, "strategy can read single-leg route position");
}

bool testRunCycleMultiPassesNineLegContextWithoutTruncation() {
  constexpr std::size_t kLegs = 9u;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100 + static_cast<std::int64_t>(i) * 100000000ll, kPx100 + 1000000000ll);
    engines[i].initialBalanceE8 = static_cast<std::int64_t>(i + 1u) * 100000000ll;
    engines[i].walletBalanceE8 = engines[i].initialBalanceE8;
    instruments[i].tickSizeRaw = static_cast<std::int64_t>(i + 1u) * 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = hft_trader::core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = hft_trader::core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "multi_leg_context_probe";
  descriptor.streamCount = 1u;
  descriptor.streams[0] = cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.runCycle = multiLegContextProbeRunCycle;

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 1u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, &state, engines, instruments, *scratch, shard, control, 8u, 1000ll, request, nullptr);

  return expect(ok, "9-leg multi cycle runs") &&
         expect(state.marketDataCount == kLegs, "marketData exposes all 9 legs") &&
         expect(state.candleCount == kLegs, "candles exposes all 9 legs") &&
         expect(state.instrumentCount == kLegs, "instrumentSpecs exposes all 9 legs") &&
         expect(state.orderStateCount == kLegs, "orderStates exposes all 9 legs") &&
         expect(state.positionCount == kLegs, "positionStates exposes all 9 legs") &&
         expect(state.accountCount == kLegs, "accountStates exposes all 9 legs") &&
         expect(state.warmupCount == kLegs, "orderPathWarmups exposes all 9 legs") &&
         expect(state.hasEventLeg && state.eventLegIndex == 8u, "event leg 8 is preserved") &&
         expect(state.contextPolicyExposed, "strategy context exposes primary/tradable policy") &&
         expect(state.primaryLegIndex == 1u, "strategy context carries request primary leg") &&
         expect(state.tradableLegMask == 2ull, "strategy context marks only the primary leg tradable") &&
         expect(state.sawLastLegBookTicker, "strategy can read the ninth leg market data") &&
         expect(state.sawLastLegAccount, "strategy can read the ninth leg account") &&
         expect(state.sawLastLegInstrument, "strategy can read the ninth leg instrument");
}

bool testRunCycleMultiMasksStaleSourceLegWithoutGlobalHalt() {
  constexpr std::size_t kLegs = 3u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100 + static_cast<std::int64_t>(i) * 100000000ll, kPx100 + 1000000000ll);
    engines[i].lastStrategySourceMarketDataTsNs = i < 2u ? kNowNs : 1ll;
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = hft_trader::core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = hft_trader::core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "multi_leg_source_freshness_probe";
  descriptor.streamCount = 1u;
  descriptor.streams[0] = cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.runCycle = multiLegContextProbeRunCycle;

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool ok = runCycleMulti(
      descriptor, &state, engines, instruments, *scratch, shard, control, 2u, kNowNs, request, nullptr);

  return expect(ok, "3-leg partial stale cycle runs") &&
         expect(state.contextPolicyExposed, "partial stale context exposes trade policy") &&
         expect(!state.inputDegraded, "3-leg context does not globally halt while two sources are fresh") &&
         expect(state.tradableLegMask == 3ull, "3-leg context masks only the stale source leg");
}

bool testRunCycleMultiGlobalHaltsWhenFewerThanTwoSourcesRemain() {
  constexpr std::size_t kLegs = 3u;
  constexpr std::int64_t kNowNs = 6000000000ll;
  std::vector<EngineState> engines(kLegs);
  std::vector<StrategyInstrumentSpec> instruments(kLegs);
  for (std::size_t i = 0u; i < kLegs; ++i) {
    setTop(engines[i], kPx100 + static_cast<std::int64_t>(i) * 100000000ll, kPx100 + 1000000000ll);
    engines[i].lastStrategySourceMarketDataTsNs = i == 0u ? kNowNs : 1ll;
    instruments[i].tickSizeRaw = 100000000ll;
    instruments[i].stepSizeRaw = 100000000ll;
    instruments[i].tickGrid = hft_trader::core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = hft_trader::core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "multi_leg_source_halt_probe";
  descriptor.streamCount = 1u;
  descriptor.streams[0] = cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.runCycle = multiLegContextProbeRunCycle;

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  MultiLegContextProbeState state{};
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};

  const bool ok = runCycleMulti(
      descriptor, &state, engines, instruments, *scratch, shard, control, 0u, kNowNs, request, nullptr);

  return expect(ok, "3-leg global stale cycle runs") &&
         expect(state.inputDegraded, "3-leg context globally halts when fewer than two sources are fresh") &&
         expect(state.haltReasonRaw == hft_trader::core::kHaltReasonStaleInput.raw,
                "3-leg global stale context reports stale input halt") &&
         expect(state.tradableLegMask == 0ull, "3-leg global stale context disables tradable legs");
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
    instruments[i].tickGrid = hft_trader::core::prepareFastStepGrid(instruments[i].tickSizeRaw);
    instruments[i].qtyStepGrid = hft_trader::core::prepareFastStepGrid(instruments[i].stepSizeRaw);
  }

  hft_trader::runtime::StrategyDescriptor descriptor{};
  descriptor.name = "primary_only_filter_probe";
  descriptor.streamCount = 1u;
  descriptor.streams[0] = cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.runCycle = nonPrimaryRouteOrderRunCycle;

  auto scratch = std::make_unique<MultiLegCycleScratch>();
  NonPrimaryRouteOrderState state{};
  state.routeRaw = 2u;
  hft_trader::core::SymbolShardConfig shard{};
  hft_trader::core::ShardControlState control{};
  BacktestRunRequest request{};
  request.hasPrimaryLegIndex = true;
  request.primaryLegIndex = 0u;
  request.tradeMode = BacktestTradeMode::PrimaryOnly;

  const bool ok = runCycleMulti(
      descriptor, &state, engines, instruments, *scratch, shard, control, 0u, 1000ll, request, nullptr);

  return expect(ok, "primary-only filter cycle runs") &&
         expect(engines[0].orders.empty(), "primary leg receives no rerouted non-primary order") &&
         expect(engines[1].orders.empty(), "non-primary route order is not executed");
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
  hft_trader::runtime::StrategyDescriptor descriptor{};
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
  request.marketDataLatency.baseUs = 0;
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
  const std::uint64_t expectedReadyCycles = kLegs + 1u;
  return expect(result.bookTickers == kLegs * 2u, "pairwise replay counts all bookticker rows") &&
         expect(result.inputCoverage.strategyCycles == expectedReadyCycles,
                "pairwise replay invokes the first complete portfolio cycle and every later delivered bookticker") &&
         expect(result.inputCoverage.cyclesWithBookTicker == expectedReadyCycles,
                "pairwise replay cycles all see bookticker context after portfolio readiness");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testBacktestExplicitSessionLimitAllowsRuntimeMaxAndRejectsOneMore() && ok;
  ok = testRunCycleSinglePassesRouteContext() && ok;
  ok = testRunCycleMultiPassesNineLegContextWithoutTruncation() && ok;
  ok = testRunCycleMultiMasksStaleSourceLegWithoutGlobalHalt() && ok;
  ok = testRunCycleMultiGlobalHaltsWhenFewerThanTwoSourcesRemain() && ok;
  ok = testRunCycleMultiPrimaryOnlyDropsNonPrimaryRouteOrder() && ok;
  ok = testBacktestRejectsExplicitInvalidPrimaryBeforeLoad() && ok;
  ok = testStrategyRunCycleErrorIncludesRejectReasonContext() && ok;
  ok = testPairwiseTakerMultiLegReplayRunsCycleForEveryDeliveredBookTicker() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
