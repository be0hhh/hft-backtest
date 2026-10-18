#pragma once

#include "../../Input/Session/SessionLoader.hpp"
#include "trading_core/Algorithm/Market/LocalOrderBook.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace hft_backtest::detail {

// Execution consumption is separate from the authoritative captured book.
// Storage is allocated only by prepare(); replay operations stay bounded.
class ExecutionLiquidity {
 public:
  [[nodiscard]] bool prepare(std::uint32_t capacityPerSide) noexcept;
  void reset() noexcept;
  void publish(std::span<const PriceLevelRow> levels, bool snapshot) noexcept;

  [[nodiscard]] std::int64_t available(trading_core::BookSide side,
                                      std::int64_t priceE8,
                                      std::int64_t visibleQtyE8) const noexcept;
  [[nodiscard]] bool canConsume(trading_core::BookSide side,
                                std::int64_t priceE8,
                                std::int64_t qtyE8) const noexcept;
  [[nodiscard]] bool consume(trading_core::BookSide side,
                             std::int64_t priceE8,
                             std::int64_t qtyE8) noexcept;
  [[nodiscard]] std::size_t storageBytes() const noexcept;
  [[nodiscard]] std::uint32_t remainingCapacity(trading_core::BookSide side) const noexcept;

 private:
  struct Entry {
    std::int64_t priceE8{0};
    std::int64_t consumedQtyE8{0};
  };

  [[nodiscard]] std::uint32_t lowerBound(std::size_t side,
                                        std::int64_t priceE8) const noexcept;
  void invalidate(std::size_t side, std::int64_t priceE8) noexcept;

  std::unique_ptr<Entry[]> storage_{};
  std::array<std::uint32_t, 2u> counts_{};
  std::uint32_t capacityPerSide_{0u};
};

}  // namespace hft_backtest::detail
