#include "backtest_metrics.hpp"

namespace hft_backtest::detail {

BacktestExcursionMetrics calculateExcursionMetrics(const EngineState& engine) noexcept {
  BacktestExcursionMetrics out{};
  bool hasPoint = false;
  std::int64_t peak = 0;
  auto observe = [&](std::int64_t pnl) noexcept {
    if (!hasPoint) {
      hasPoint = true;
      peak = pnl;
      out.mfeE8 = pnl > 0 ? pnl : 0;
      out.maeE8 = pnl < 0 ? pnl : 0;
      return;
    }
    if (pnl > peak) peak = pnl;
    const std::int64_t drawdown = peak > pnl ? peak - pnl : 0;
    if (drawdown > out.maxDrawdownE8) out.maxDrawdownE8 = drawdown;
    if (pnl > out.mfeE8) out.mfeE8 = pnl;
    if (pnl < out.maeE8) out.maeE8 = pnl;
  };
  if (!engine.riskSnapshots.empty()) {
    for (const JsonRiskSnapshot& row : engine.riskSnapshots) observe(row.netTotalPnlE8);
    return out;
  }
  for (const JsonEquityPoint& row : engine.equityPoints) observe(row.netTotalPnlE8);
  return out;
}

}  // namespace hft_backtest::detail
