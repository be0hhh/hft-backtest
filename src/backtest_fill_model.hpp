#pragma once

#include "backtest_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hft_backtest::detail {

struct AggressiveFillChunk {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t avgPriceE8{0};
  std::int64_t bookLevelQtyE8{0};
  std::int64_t bookVisibleExecutableQtyE8{0};
  std::int64_t bookConsumedPctE8{0};
  std::uint32_t chunkIndex{0};
  std::uint32_t chunkCount{0};
  std::int64_t executionQtyE8{0};
  std::int64_t executionAvgPriceE8{0};
  std::int64_t referencePriceE8{0};
  std::int64_t slippageE8{0};
  std::int64_t slippageBpsE8{0};
  std::int64_t executionBookConsumedPctE8{0};
};

struct AggressiveFillPlan {
  std::array<AggressiveFillChunk, rawdata::kMaxOrderbookLevels> chunks{};
  std::size_t count{0u};
  std::int64_t requestedQtyE8{0};
  std::int64_t filledQtyE8{0};
  std::int64_t remainingQtyE8{0};
  std::int64_t bookTsNs{0};
  std::int64_t referencePriceE8{0};
  std::int64_t executionAvgPriceE8{0};
  std::int64_t slippageE8{0};
  std::int64_t slippageBpsE8{0};
  std::int64_t executionBookConsumedPctE8{0};
  bool visibleDepthCapped{false};
};

struct PassiveQueueTradeFill {
  std::int64_t requestedQtyE8{0};
  std::int64_t queueAheadBeforeE8{0};
  std::int64_t queueAheadAfterE8{0};
};

[[nodiscard]] std::int64_t openOrderRemainingQty(const OpenOrder& order) noexcept;
void initializePassiveQueue(OpenOrder& order, const EngineState& engine) noexcept;
void initializePassiveQueue(OpenOrder& order,
                            const EngineState& engine,
                            const hft_trader::core::LocalOrderBookState& book) noexcept;
[[nodiscard]] bool buildAggressiveBookFillPlan(const EngineState& engine,
                                               const OpenOrder& order,
                                               AggressiveFillPlan& out) noexcept;
[[nodiscard]] bool buildAggressiveBookFillPlan(const EngineState& engine,
                                               const OpenOrder& order,
                                               const hft_trader::core::LocalOrderBookState& book,
                                               AggressiveFillPlan& out) noexcept;
[[nodiscard]] PassiveQueueTradeFill consumePassiveQueueForTrade(
    EngineState& engine,
    OpenOrder& order,
    const cxet::composite::TradeRuntimeV1& trade) noexcept;

}  // namespace hft_backtest::detail
