#include "engine/backtest_engine.hpp"
#include "strategy/backtest_strategy_runtime.hpp"

#include <cstdlib>
#include <cstdint>
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

void setTop(EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  cxet::composite::BookTickerRuntimeV1 top{};
  top.bid.px.raw = bid;
  top.ask.px.raw = ask;
  hft_trader::core::copyBookTickerSnapshot(engine.snapshot, top);
  hft_trader::core::copyBookTickerSnapshot(engine.venueSnapshot, top);
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
    session.symbol = "BTCUSDT";
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
  bool hasEventLeg{false};
  bool sawLastLegBookTicker{false};
  bool sawLastLegAccount{false};
  bool sawLastLegInstrument{false};
};

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
         expect(state.sawLastLegBookTicker, "strategy can read the ninth leg market data") &&
         expect(state.sawLastLegAccount, "strategy can read the ninth leg account") &&
         expect(state.sawLastLegInstrument, "strategy can read the ninth leg instrument");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testBacktestExplicitSessionLimitAllowsRuntimeMaxAndRejectsOneMore() && ok;
  ok = testRunCycleMultiPassesNineLegContextWithoutTruncation() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
