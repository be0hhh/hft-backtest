#pragma once

#include "cxet/Primitives/Raw/Bool/Side.hpp"

#include <cstdint>
#include <limits>

namespace hft_backtest::detail {
// The shared accounting primitive intentionally saturates. A replay fill must
// preserve its exact signed quantity before either venue or observed commit.
[[nodiscard]] inline bool fillPositionQuantityOverflows(std::int64_t before,
    Side side, std::int64_t quantity) noexcept {
  if (quantity <= 0 || (side.raw != Side::Buy().raw && side.raw != Side::Sell().raw)) return false;
  const auto after = static_cast<__int128>(before) +
      (side.raw == Side::Buy().raw ? static_cast<__int128>(quantity) : -static_cast<__int128>(quantity));
  return after < std::numeric_limits<std::int64_t>::min() ||
      after > std::numeric_limits<std::int64_t>::max();
}
} // namespace hft_backtest::detail
