#include "BacktestLiquidity.hpp"

#include <limits>
#include <new>
#include <utility>

namespace hft_backtest::detail {

bool ExecutionLiquidity::prepare(std::uint32_t capacityPerSide) noexcept {
  reset();
  if (capacityPerSide == 0u ||
      capacityPerSide > trading_core::kMaxLocalOrderBookLevelsPerSide ||
      static_cast<std::size_t>(capacityPerSide) >
          std::numeric_limits<std::size_t>::max() / (2u * sizeof(Entry))) {
    storage_.reset();
    capacityPerSide_ = 0u;
    return false;
  }
  if (storage_ && capacityPerSide_ == capacityPerSide) return true;
  const std::size_t count = static_cast<std::size_t>(capacityPerSide) * 2u;
  std::unique_ptr<Entry[]> next{new (std::nothrow) Entry[count]{}};
  if (!next) {
    storage_.reset();
    capacityPerSide_ = 0u;
    return false;
  }
  storage_ = std::move(next);
  capacityPerSide_ = capacityPerSide;
  return true;
}

void ExecutionLiquidity::reset() noexcept {
  counts_ = {};
}

std::uint32_t ExecutionLiquidity::lowerBound(std::size_t side,
                                            std::int64_t priceE8) const noexcept {
  const Entry* entries = storage_.get() + side * capacityPerSide_;
  std::uint32_t first = 0u;
  std::uint32_t last = counts_[side];
  while (first < last) {
    const std::uint32_t middle = first + (last - first) / 2u;
    if (entries[middle].priceE8 < priceE8) first = middle + 1u;
    else last = middle;
  }
  return first;
}

void ExecutionLiquidity::invalidate(std::size_t side, std::int64_t priceE8) noexcept {
  const std::uint32_t index = lowerBound(side, priceE8);
  Entry* entries = storage_.get() + side * capacityPerSide_;
  if (index == counts_[side] || entries[index].priceE8 != priceE8) return;
  for (std::uint32_t i = index + 1u; i < counts_[side]; ++i)
    entries[i - 1u] = entries[i];
  --counts_[side];
}

void ExecutionLiquidity::publish(std::span<const PriceLevelRow> levels,
                                  bool snapshot) noexcept {
  if (snapshot) reset();
  if (!storage_) return;
  for (const PriceLevelRow& level : levels) {
    // PriceLevelRow uses 0=bid, 1=ask, including zero-quantity deletions.
    if (level.side != 0 && level.side != 1) continue;
    invalidate(static_cast<std::size_t>(level.side), level.priceE8);
  }
}

std::int64_t ExecutionLiquidity::available(trading_core::BookSide side,
                                          std::int64_t priceE8,
                                          std::int64_t visibleQtyE8) const noexcept {
  const auto sideIndex = static_cast<std::size_t>(side);
  if (!storage_ || sideIndex >= counts_.size() || priceE8 <= 0 || visibleQtyE8 <= 0)
    return 0;
  const std::uint32_t index = lowerBound(sideIndex, priceE8);
  const Entry* entries = storage_.get() + sideIndex * capacityPerSide_;
  if (index == counts_[sideIndex] || entries[index].priceE8 != priceE8)
    return visibleQtyE8;
  const std::int64_t consumed = entries[index].consumedQtyE8;
  return visibleQtyE8 > consumed ? visibleQtyE8 - consumed : 0;
}

bool ExecutionLiquidity::canConsume(trading_core::BookSide side,
                                    std::int64_t priceE8,
                                    std::int64_t qtyE8) const noexcept {
  const auto sideIndex = static_cast<std::size_t>(side);
  if (!storage_ || sideIndex >= counts_.size() || priceE8 <= 0 || qtyE8 <= 0)
    return false;
  const std::uint32_t index = lowerBound(sideIndex, priceE8);
  const Entry* entries = storage_.get() + sideIndex * capacityPerSide_;
  if (index < counts_[sideIndex] && entries[index].priceE8 == priceE8)
    return qtyE8 <= std::numeric_limits<std::int64_t>::max() - entries[index].consumedQtyE8;
  return counts_[sideIndex] < capacityPerSide_;
}

bool ExecutionLiquidity::consume(trading_core::BookSide side,
                                 std::int64_t priceE8,
                                 std::int64_t qtyE8) noexcept {
  if (!canConsume(side, priceE8, qtyE8)) return false;
  const auto sideIndex = static_cast<std::size_t>(side);
  const std::uint32_t index = lowerBound(sideIndex, priceE8);
  Entry* entries = storage_.get() + sideIndex * capacityPerSide_;
  if (index < counts_[sideIndex] && entries[index].priceE8 == priceE8) {
    entries[index].consumedQtyE8 += qtyE8;
    return true;
  }
  for (std::uint32_t i = counts_[sideIndex]; i > index; --i)
    entries[i] = entries[i - 1u];
  entries[index] = Entry{priceE8, qtyE8};
  ++counts_[sideIndex];
  return true;
}

std::size_t ExecutionLiquidity::storageBytes() const noexcept {
  return static_cast<std::size_t>(capacityPerSide_) * 2u * sizeof(Entry);
}

std::uint32_t ExecutionLiquidity::remainingCapacity(trading_core::BookSide side) const noexcept {
  const auto index = static_cast<std::size_t>(side);
  return storage_ && index < counts_.size() ? capacityPerSide_ - counts_[index] : 0u;
}

}  // namespace hft_backtest::detail
