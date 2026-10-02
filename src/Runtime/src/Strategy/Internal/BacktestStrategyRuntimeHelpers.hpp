#pragma once

#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "../BacktestStrategyRuntime.hpp"

#include "../../Config/BacktestRunConfig.hpp"
#include "../Execution/BacktestStrategyIntentLowering.hpp"
#include "../Diagnostics/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Execution/Order/OrderPathWarmup.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "../Inputs/BacktestStrategyWarmup.hpp"

namespace hft_backtest::strategy_runtime_detail {
using strategy_intent_lowering::applyToLegs;
using strategy_intent_lowering::applyToSingleLeg;
using strategy_intent_lowering::applyTransactionCycle;
using strategy_intent_lowering::bindTransactionLegs;
using strategy_intent_lowering::configuredDefaultExecutionRoute;
using strategy_intent_lowering::configuredPrimaryLegIndex;
using strategy_intent_lowering::footprintSupported;
using strategy_intent_lowering::validateOutput;
using strategy_intent_lowering::routeLegIndex;
using strategy_intent_lowering::routeTradable;
using strategy_intent_lowering::transactionAdmission;




std::int64_t nextPendingTs(const detail::EngineState& engine) noexcept;
std::int64_t userEventFallbackTs(const cxet::composite::UserDataEvent& event) noexcept;
[[nodiscard]] cxet::composite::UserDataSemanticMask
backtestSimulatedUserEventSemanticMask(
    const cxet::composite::UserDataEvent* event) noexcept;
std::int64_t nextUserEventDeliveryTs(const detail::EngineState& engine) noexcept;
std::int64_t nextWakeTs(const trading_core::StrategyDescriptor& descriptor, const void* state) noexcept;
std::int64_t nextRateLimitCooldownTs(const detail::EngineState& engine) noexcept;
std::int64_t nextRateLimitCooldownTs(const std::vector<detail::EngineState>& engines) noexcept;
void clearRateLimitCooldown(detail::EngineState& engine, std::int64_t tsNs) noexcept;
void clearRateLimitCooldown(std::vector<detail::EngineState>& engines, std::int64_t tsNs) noexcept;
bool dueWithinBoundary(std::int64_t tsNs, std::int64_t untilTs, RunUntilBoundary boundary) noexcept;
std::int64_t nextPendingTs(const std::vector<detail::EngineState>& engines) noexcept;
std::int64_t nextUserEventDeliveryTs(const std::vector<detail::EngineState>& engines) noexcept;
bool drainUserEvents(const trading_core::StrategyDescriptor& descriptor,
                     const BacktestStrategyInputPolicy& inputPolicy,
                     void* state,
                     detail::EngineState& engine,
                     const StrategyInstrumentSpec& instrument,
                     SingleLegCycleScratch& scratch,
                     const trading_core::SymbolShardConfig& shard,
                     const trading_core::ShardControlState& control,
                     const BacktestRunRequest& request,
                     std::int64_t untilTs,
                     RunUntilBoundary boundary = RunUntilBoundary::Inclusive);
bool drainUserEventsMulti(const trading_core::StrategyDescriptor& descriptor,
                          const BacktestStrategyInputPolicy& inputPolicy,
                          void* state,
                          std::vector<detail::EngineState>& engines,
                          const std::vector<StrategyInstrumentSpec>& legInstruments,
                          MultiLegCycleScratch& scratch,
                          const trading_core::SymbolShardConfig& shard,
                          const trading_core::ShardControlState& control,
                          const BacktestRunRequest& request,
                          std::int64_t untilTs,
                          RunUntilBoundary boundary = RunUntilBoundary::Inclusive);
bool hasUndeliveredUserEvents(const std::vector<detail::EngineState>& engines) noexcept;
bool portfolioHasOpenState(const std::vector<detail::EngineState>& engines) noexcept;
std::int64_t toInt64Time(TimeNs value) noexcept;
[[nodiscard]] bool hasExactExchangeTimestamp(
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTimestampNs) noexcept;
[[nodiscard]] trading_core::BboReconstructionTimestampOrigin
reconstructionTimestampOrigin(
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTimestampNs) noexcept;
void overlayBacktestReconstructedBbo(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::LocalOrderBookState& orderBook,
    const trading_core::BboReconstructionView& view) noexcept;
void publishBacktestReconstructedBbo(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::LocalOrderBookState& orderBook,
    const trading_core::BboReconstructionState& bbo,
    std::uint64_t receiveMonotonicNs) noexcept;
void refreshCapturedBookTicker(
    trading_core::StrategyMarketSnapshot& snapshot,
    const detail::TimelineEvent& event) noexcept;
[[nodiscard]] bool applyCapturedBookTicker(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::BboReconstructionState& bbo,
    const detail::BookTickerRow& row,
    const detail::TimelineEvent& event,
    std::uint64_t replayReceiveMonotonicNs,
    const cxet::composite::BookTicker& ticker) noexcept;
std::uint64_t backtestTradableLegMask(const BacktestRunRequest& request,
                                      std::size_t legCount) noexcept;
trading_core::ShardControlState backtestControlWithInputFreshness(
    const trading_core::ShardControlState& control,
    const trading_core::strategy::StrategyInputSourceMaskState& freshness) noexcept;
void retainCancelOutputsWhenNewOrdersBlocked(
    trading_core::StrategyIo& io) noexcept;
void bindBacktestStrategyStateView(
    trading_core::StrategyContext& ctx) noexcept;
const BacktestStrategyVenueConfig* venueByName(
    const BacktestStrategyConfig& cfg,
    const std::string& name) noexcept;
bool venueMatchesSession(const BacktestStrategyVenueConfig& venue,
                         const detail::SessionData& session) noexcept;
const BacktestStrategyVenueConfig* venueForSession(
    const BacktestStrategyConfig& cfg,
    const detail::SessionData& session) noexcept;
void cancelOpenOrdersForTraderRiskClose(detail::EngineState& engine, std::int64_t tsNs) noexcept;

template<typename Context>
void setBacktestTradePolicy(Context& ctx,
                            const BacktestRunRequest& request,
                            std::size_t legCount,
                            std::uint64_t effectiveTradableLegMask) noexcept {
  const std::uint32_t primary = configuredPrimaryLegIndex(request, legCount);
  const std::uint8_t modeRaw = static_cast<std::uint8_t>(request.tradeMode);
  if constexpr (requires(Context& candidate, bool value) { candidate.hasPrimaryLegIndex = value; }) {
    ctx.hasPrimaryLegIndex = request.hasPrimaryLegIndex;
  }
  if constexpr (requires(Context& candidate, std::uint32_t value) { candidate.primaryLegIndex = value; }) {
    ctx.primaryLegIndex = primary;
  } else if constexpr (requires(Context& candidate) { candidate.primaryLegIndex; }) {
    ctx.primaryLegIndex = static_cast<decltype(ctx.primaryLegIndex)>(primary);
  }
  if constexpr (requires(Context& candidate, std::uint64_t value) { candidate.tradableLegMask = value; }) {
    ctx.tradableLegMask = effectiveTradableLegMask;
  } else if constexpr (requires(Context& candidate) { candidate.tradableLegMask; }) {
    ctx.tradableLegMask = static_cast<decltype(ctx.tradableLegMask)>(effectiveTradableLegMask);
  }
  if constexpr (requires(Context& candidate,
                         trading_core::ExecutionRouteHandle value) {
                  candidate.defaultExecutionRoute = value;
                }) {
    ctx.defaultExecutionRoute =
        configuredDefaultExecutionRoute(request, legCount);
  }
  if constexpr (requires(Context& candidate, std::uint8_t value) { candidate.tradeMode = value; }) {
    ctx.tradeMode = modeRaw;
  } else if constexpr (requires(Context& candidate) { candidate.tradeMode; }) {
    ctx.tradeMode = static_cast<decltype(ctx.tradeMode)>(modeRaw);
  }
}


}  // namespace hft_backtest::strategy_runtime_detail
