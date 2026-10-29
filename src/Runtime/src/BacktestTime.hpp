#pragma once

#include "cxet/Primitives/Raw/Counts/TimeCount.hpp"

#include <cstdint>

namespace hft_backtest {

[[nodiscard]] inline TimeNs timeNsFromI64(std::int64_t value) noexcept {
  TimeNs out{};
  if (value > 0) out.raw = static_cast<std::uint64_t>(value);
  return out;
}

}  // namespace hft_backtest
