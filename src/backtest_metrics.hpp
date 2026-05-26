#pragma once

#include "backtest_engine.hpp"

#include <cstdint>

namespace hft_backtest::detail {

struct BacktestExcursionMetrics {
  std::int64_t maxDrawdownE8{0};
  std::int64_t mfeE8{0};
  std::int64_t maeE8{0};
};

BacktestExcursionMetrics calculateExcursionMetrics(const EngineState& engine) noexcept;

}  // namespace hft_backtest::detail
