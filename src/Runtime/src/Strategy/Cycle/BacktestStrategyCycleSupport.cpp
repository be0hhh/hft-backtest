#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "../BacktestStrategyRuntime.hpp"

#include "../../Config/BacktestRunConfig.hpp"
#include "../Execution/BacktestStrategyIntentLowering.hpp"
#include "../Diagnostics/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Strategy/Orders/WarmupIntents.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
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

