#pragma once

#include "../BacktestEngine.hpp"

#include <cstdint>
#include <vector>

namespace hft_backtest::detail {

struct BacktestExcursionMetrics {
  std::int64_t maxDrawdownE8{0};
  std::int64_t mfeE8{0};
  std::int64_t maeE8{0};
};

BacktestExcursionMetrics calculateExcursionMetrics(const EngineState& engine) noexcept;
BacktestExcursionMetrics calculatePortfolioExcursionMetrics(const std::vector<EngineState>& engines);
std::vector<std::int64_t> buildPortfolioTotalPnlCurveE8(const std::vector<EngineState>& engines);

}  // namespace hft_backtest::detail
