#pragma once

#include "engine/backtest_engine.hpp"
#include "input/session_loader.hpp"

#include "hft_trader/runtime/runner/StrategyDescriptor.hpp"

#include <string>

namespace hft_backtest {

bool seedTradeWarmupFromSession(const hft_trader::runtime::StrategyDescriptor& descriptor,
                                void* state,
                                const hft_trader::runtime::StrategyParamStorage& params,
                                const detail::SessionData& session,
                                std::string& error);

}  // namespace hft_backtest
