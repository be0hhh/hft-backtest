#pragma once

#include "../../Engine/BacktestEngine.hpp"

#include "trading_core/Strategy/Descriptor/StrategyDescriptor.hpp"

#include <cstddef>
#include <cstdint>

namespace hft_backtest {

struct BacktestStrategyInputPolicy {
  cxet::api::market::PublicMarketDataStreamReadyMask requiredStreamReadyMask{0u};
  std::uint64_t maxAgeNs{0u};
};

[[nodiscard]] BacktestStrategyInputPolicy resolveBacktestStrategyInputPolicy(
    const trading_core::StrategyDescriptor& descriptor,
    const trading_core::StrategyParamStorage& params) noexcept;

[[nodiscard]] bool backtestStrategySnapshotReady(
    const BacktestStrategyInputPolicy& inputPolicy,
    const trading_core::StrategyMarketSnapshot& snapshot) noexcept;

[[nodiscard]] std::uint64_t backtestSourceFreshLegMask(
    const BacktestStrategyInputPolicy& inputPolicy,
    const detail::EngineState* engines,
    std::size_t legCount,
    std::int64_t tsNs) noexcept;

void recordBacktestStrategySourceDelivery(
    detail::EngineState& engine,
    const detail::TimelineEvent& event) noexcept;

}  // namespace hft_backtest
