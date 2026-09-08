#pragma once

#include "../BacktestEngine.hpp"
#include "../Input/SessionLoader.hpp"

#include "trading_core/Strategy/Descriptor/StrategyDescriptor.hpp"

#include <string>

namespace hft_backtest {

bool seedTradeWarmupFromSession(const trading_core::StrategyDescriptor& descriptor,
                                void* state,
                                const trading_core::StrategyParamStorage& params,
                                const detail::SessionData& session,
                                std::string& error);

}  // namespace hft_backtest
