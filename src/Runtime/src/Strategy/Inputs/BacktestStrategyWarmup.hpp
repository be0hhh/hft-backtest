#pragma once

#include "../../Engine/BacktestEngine.hpp"
#include "../../Input/Session/SessionLoader.hpp"

#include "trading_core/Strategy/Descriptor/StrategyDescriptor.hpp"
#include "trading_core/Strategy/Orders/Intent/WarmupIntents.hpp"

#include <string>

namespace hft_backtest {

bool seedTradeWarmupFromSession(const trading_core::StrategyDescriptor& descriptor,
                                void* state,
                                const trading_core::StrategyParamStorage& params,
                                const detail::SessionData& session,
                                std::string& error);

}  // namespace hft_backtest

namespace hft_backtest::strategy_runtime_detail {
bool orderPathWarmupCandidate(const trading_core::OrderIntent& intent) noexcept;
bool warmupLegSeen(const std::size_t* legs,
                   std::size_t count,
                   std::size_t leg) noexcept;
bool warmupReasonFailOpen(trading_core::strategy::OrderPathWarmupSkipReason reason) noexcept;
trading_core::StrategyOrderPathWarmupView backtestWarmupView(const detail::EngineState& engine) noexcept;
bool backtestOrderPathHot(const detail::EngineState& engine, TimeNs nowMono) noexcept;
Id allocateBacktestServiceClientIntentId(detail::EngineState& engine) noexcept;
void markBacktestWarmupSkipped(detail::EngineState& engine,
                               trading_core::strategy::OrderPathWarmupSkipReason reason) noexcept;
bool applyBacktestWarmupGate(trading_core::StrategyContext& ctx,
                             detail::EngineView engines,
                             std::size_t engineCount,
                             const BacktestRunRequest& request) noexcept;
}
