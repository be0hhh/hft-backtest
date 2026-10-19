#include "trading_core/Contracts/Numeric/Integer/Saturating.hpp"
#include "BacktestMetrics.hpp"

#include <algorithm>

#include "trading_core/Contracts/Numeric/PriceQuantity/NotionalQty.hpp"

namespace hft_backtest::detail {
namespace {

BacktestExcursionMetrics calculateFromPnlSeries(const std::vector<std::int64_t>& series) noexcept {
  BacktestExcursionMetrics out{};
  // PnL starts at zero before the first recorded observation.
  std::int64_t peak = 0;
  for (const std::int64_t pnl : series) {
    if (pnl > peak) peak = pnl;
    const std::int64_t drawdown =
        peak > pnl ? trading_core::saturatingSubI64(peak, pnl) : 0;
    if (drawdown > out.maxDrawdownE8) out.maxDrawdownE8 = drawdown;
    if (pnl > out.mfeE8) out.mfeE8 = pnl;
    if (pnl < out.maeE8) out.maeE8 = pnl;
  }
  return out;
}

template <typename RowFn>
std::vector<std::int64_t> buildPortfolioPnlSeries(const std::vector<EngineState>& engines, RowFn rowFn) {
  std::vector<std::int64_t> timestamps{};
  for (const EngineState& engine : engines) {
    const auto& rows = rowFn(engine);
    timestamps.reserve(timestamps.size() + rows.size());
    for (const auto& row : rows) timestamps.push_back(row.tsNs);
  }
  std::sort(timestamps.begin(), timestamps.end());
  timestamps.erase(std::unique(timestamps.begin(), timestamps.end()), timestamps.end());

  std::vector<std::size_t> indexes(engines.size(), 0u);
  std::vector<std::int64_t> current(engines.size(), 0);
  std::vector<std::int64_t> out{};
  out.reserve(timestamps.size());
  for (const std::int64_t tsNs : timestamps) {
    std::int64_t total = 0;
    for (std::size_t leg = 0u; leg < engines.size(); ++leg) {
      const auto& rows = rowFn(engines[leg]);
      while (indexes[leg] < rows.size() && rows[indexes[leg]].tsNs <= tsNs) {
        current[leg] = rows[indexes[leg]].netTotalPnlE8;
        ++indexes[leg];
      }
      total = trading_core::saturatingAddI64(total, current[leg]);
    }
    out.push_back(total);
  }
  return out;
}

}  // namespace

BacktestExcursionMetrics calculateExcursionMetrics(const EngineState& engine) noexcept {
  BacktestExcursionMetrics out{};
  // Include the initial zero-PnL account baseline even without an explicit row.
  std::int64_t peak = 0;
  auto observe = [&](std::int64_t pnl) noexcept {
    if (pnl > peak) peak = pnl;
    const std::int64_t drawdown =
        peak > pnl ? trading_core::saturatingSubI64(peak, pnl) : 0;
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

BacktestExcursionMetrics calculatePortfolioExcursionMetrics(const std::vector<EngineState>& engines) {
  bool allHaveRiskSnapshots = !engines.empty();
  for (const EngineState& engine : engines) {
    if (engine.riskSnapshots.empty()) allHaveRiskSnapshots = false;
  }
  if (allHaveRiskSnapshots) {
    return calculateFromPnlSeries(buildPortfolioPnlSeries(engines, [](const EngineState& engine) -> const std::vector<JsonRiskSnapshot>& {
      return engine.riskSnapshots;
    }));
  }
  return calculateFromPnlSeries(buildPortfolioTotalPnlCurveE8(engines));
}

std::vector<std::int64_t> buildPortfolioTotalPnlCurveE8(const std::vector<EngineState>& engines) {
  return buildPortfolioPnlSeries(engines, [](const EngineState& engine) -> const std::vector<JsonEquityPoint>& {
    return engine.equityPoints;
  });
}

}  // namespace hft_backtest::detail
