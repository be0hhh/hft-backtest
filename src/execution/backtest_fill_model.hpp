#pragma once

#include "engine/backtest_engine.hpp"

#include <cstddef>
#include <cstdint>

namespace hft_backtest::detail {

struct PassiveQueueTradeFill {
  std::int64_t requestedQtyE8{0};
  std::int64_t queueAheadBeforeE8{0};
  std::int64_t queueAheadAfterE8{0};
};

struct PassiveBboTouchFill {
  std::int64_t requestedQtyE8{0};
  std::int64_t queueAheadBeforeE8{0};
  std::int64_t queueAheadAfterE8{0};
};

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
[[nodiscard]] PassiveQueueTradeFill consumePassiveQueueForTrade(
    EngineState& engine,
    OpenOrder& order,
    const cxet::composite::TradeRuntimeV1& trade) noexcept;
[[nodiscard]] PassiveBboTouchFill consumePassiveQueueForBboTouch(
    EngineState& engine,
    OpenOrder& order,
    const cxet::composite::BookTickerRuntimeV1& top,
    std::int64_t tsNs) noexcept;

}  // namespace hft_backtest::detail
