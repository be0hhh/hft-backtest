#pragma once

#include "engine/backtest_engine.hpp"
#include "input/session_loader.hpp"

#include "trading_core/api/descriptor/StrategyDescriptor.hpp"

#include <string>

namespace hft_backtest {

bool seedTradeWarmupFromSession(const trading_core::StrategyDescriptor& descriptor,
                                void* state,
                                const trading_core::StrategyParamStorage& params,
                                const detail::SessionData& session,
                                std::string& error);

}  // namespace hft_backtest
