#pragma once

#include "hft_backtest/BacktestSweep.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hft_backtest::detail {

struct BacktestSweepPoint {
  std::uint64_t pointId{0};
  std::vector<std::int64_t> values{};
};

struct BacktestSweepPointResult {
  BacktestSweepPoint point{};
  BacktestRunResult run{};
};

std::vector<BacktestSweepPoint> buildSweepSearchPlan(const BacktestSweepRequest& request);
std::filesystem::path sweepResultPathFor(const BacktestSweepRequest& request, std::string_view sweepId);
Status writeSweepManifest(const BacktestSweepRequest& request,
                          const BacktestSweepResult& result,
                          const std::vector<BacktestSweepPointResult>& points) noexcept;
Status writeSweepRows(const BacktestSweepRequest& request,
                      const BacktestSweepResult& result,
                      const std::vector<BacktestSweepPointResult>& points) noexcept;
Status writeSweepCurves(const BacktestSweepRequest& request,
                        const BacktestSweepResult& result,
                        const std::vector<BacktestSweepPointResult>& points) noexcept;

}  // namespace hft_backtest::detail
