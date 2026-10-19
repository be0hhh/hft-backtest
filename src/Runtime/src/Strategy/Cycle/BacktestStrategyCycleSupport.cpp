#include "trading_core/Contracts/Numeric/Integer/Saturating.hpp"
#include "../BacktestStrategyRuntime.hpp"

#include "../../Config/BacktestRunConfig.hpp"
#include "../Execution/BacktestStrategyIntentLowering.hpp"
#include "../Diagnostics/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Strategy/Orders/Intent/WarmupIntents.hpp"
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
#include <iterator>
#include <string>

#include "../Internal/BacktestStrategyRuntimeHelpers.hpp"

namespace hft_backtest::strategy_runtime_detail {

std::uint64_t backtestTradableLegMask(const BacktestRunRequest& request,
                                      std::size_t legCount) noexcept {
  if (request.tradeMode == BacktestTradeMode::PrimaryOnly) {
    const std::uint32_t primary = configuredPrimaryLegIndex(request, legCount);
    return primary < 64u ? (std::uint64_t{1} << primary) : 0u;
  }
  if (legCount >= 64u) return trading_core::kAllStrategyLegsTradableMask;
  return legCount == 0u ? 0u : ((std::uint64_t{1} << legCount) - 1u);
}

trading_core::ShardControlState backtestControlWithInputFreshness(
    const trading_core::ShardControlState& control,
    const trading_core::strategy::StrategyInputSourceMaskState& freshness) noexcept {
  trading_core::ShardControlState out = control;
  if (!freshness.globallyDegraded) return out;
  out.inputDegraded = trading_core::kAlgoFlagTrue;
  if (out.haltReason.raw == trading_core::kHaltReasonNone.raw ||
      out.haltReason.raw == trading_core::kHaltReasonStaleInput.raw) {
    out.haltReason = trading_core::kHaltReasonStaleInput;
  }
  return out;
}

void retainCancelOutputsWhenNewOrdersBlocked(
    trading_core::StrategyIo& io) noexcept {
  io.orderCount = 0u;
  const std::size_t readableCommands =
      io.commandCount < io.commandOut.size()
          ? io.commandCount
          : io.commandOut.size();
  std::size_t writeIndex = 0u;
  for (std::size_t readIndex = 0u;
       readIndex < readableCommands;
       ++readIndex) {
    if (io.commandOut[readIndex].kind !=
        trading_core::StrategyCommandIntentKind::Cancel) {
      continue;
    }
    if (writeIndex != readIndex) {
      io.commandOut[writeIndex] = io.commandOut[readIndex];
    }
    ++writeIndex;
  }
  io.commandCount = writeIndex;
}

void bindBacktestStrategyStateView(
    trading_core::StrategyContext& ctx) noexcept {
  ctx.state.orders.primary = ctx.orderState;
  ctx.state.orders.legs = ctx.orderStates;
  ctx.state.orders.primaryWarmup = ctx.orderPathWarmup;
  ctx.state.orders.warmups = ctx.orderPathWarmups;
  ctx.state.positions.primary = ctx.positionState;
  ctx.state.positions.legs = ctx.positionStates;
  ctx.state.account.primary = ctx.accountState;
  ctx.state.account.legs = ctx.accountStates;
  ctx.state.account.rateLimits = ctx.rateLimits;
  trading_core::bindStrategyContextStateAliases(ctx);
}

}  // namespace hft_backtest::strategy_runtime_detail


namespace hft_backtest {
void bindBacktestUserEventIdentity(trading_core::StrategyContext& context,
    const detail::EngineState& engine) noexcept {
  context.userEventClientIntentId = {};
  context.userEventGeneration = 0u;
  if (!context.hasUserEvent || !context.userEvent || engine.deliveredUserEvents == 0u) return;
  const auto index = engine.lastDeliveredUserEvent < engine.userEvents.size()
      ? engine.lastDeliveredUserEvent : engine.deliveredUserEvents - 1u;
  if (index >= engine.userEvents.size() || index >= engine.userEventClientIntentIds.size() ||
      index >= engine.userEventGenerations.size() || index >= engine.userEventCommandIds.size() ||
      index >= engine.userEventContextIndices.size() || index >= engine.userEventConsumed.size() ||
      !engine.userEventConsumed[index] ||
      (index < engine.userEventServiceFlags.size() && engine.userEventServiceFlags[index]) ||
      engine.userEventContextIndices[index] != engine.activeContextIndex ||
      engine.userEvents[index].eventId.raw != context.userEvent->eventId.raw ||
      engine.userEvents[index].exchangeId.raw != context.userEvent->exchangeId.raw ||
      engine.userEvents[index].orderStatus != context.userEvent->orderStatus ||
      engine.userEvents[index].updateTime.raw != context.userEvent->updateTime.raw ||
      engine.userEvents[index].filledQty.raw != context.userEvent->filledQty.raw ||
      !std::equal(std::begin(engine.userEvents[index].symbol.data), std::end(engine.userEvents[index].symbol.data),
          std::begin(context.userEvent->symbol.data)) ||
      !std::equal(std::begin(engine.userEvents[index].clientOrderId.data), std::end(engine.userEvents[index].clientOrderId.data),
          std::begin(context.userEvent->clientOrderId.data))) return;
  const auto view = detail::simulationOrderEvidenceView(engine);
  const auto* evidence = trading_core::strategy::canonicalOrderEvidenceIdentity(
      view, engine.userEventClientIntentIds[index], engine.userEventGenerations[index]);
  if (!evidence || !engine.userEventGenerations[index] ||
      evidence->commandId != engine.userEventCommandIds[index]) return;
  context.userEventClientIntentId = evidence->clientIntentId;
  context.userEventGeneration = evidence->generation;
}
}  // namespace hft_backtest
