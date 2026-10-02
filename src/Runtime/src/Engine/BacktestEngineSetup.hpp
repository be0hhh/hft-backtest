#pragma once

#include "hft_backtest/Backtest.hpp"

#include "BacktestEngine.hpp"

#include <string>

namespace hft_backtest {

bool applyRateLimitSchedule(detail::EngineState& engine,
                            const BacktestRateLimitSchedule& schedule,
                            std::string& error);

}  // namespace hft_backtest
