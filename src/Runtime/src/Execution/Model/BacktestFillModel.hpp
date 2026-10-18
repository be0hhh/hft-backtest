#pragma once

#include "../../Engine/BacktestEngine.hpp"

#include <cstddef>
#include <cstdint>

namespace hft_backtest::detail {

[[nodiscard]] std::int64_t openOrderRemainingQty(const OpenOrder& order) noexcept;
[[nodiscard]] bool triggeredStopLimitOrder(const OpenOrder& order) noexcept;
[[nodiscard]] bool triggeredStopFokLimitOrder(const OpenOrder& order) noexcept;
void initializePassiveQueue(OpenOrder& order, const EngineState& engine) noexcept;
void initializePassiveQueue(OpenOrder& order,
                            const EngineState& engine,
                            const trading_core::LocalOrderBookState& book) noexcept;
[[nodiscard]] bool buildAggressiveBookFillPlan(const EngineState& engine,
                                               const OpenOrder& order,
                                               AggressiveFillPlan& out) noexcept;
[[nodiscard]] bool buildAggressiveBookFillPlan(const EngineState& engine,
                                               const OpenOrder& order,
                                               const trading_core::LocalOrderBookState& book,
                                               AggressiveFillPlan& out) noexcept;
[[nodiscard]] ExecutionLiquidity& depthLiquidity(EngineState& engine) noexcept;
[[nodiscard]] const ExecutionLiquidity& depthLiquidity(const EngineState& engine) noexcept;

}  // namespace hft_backtest::detail
