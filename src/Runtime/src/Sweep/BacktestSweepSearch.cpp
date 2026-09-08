#include "BacktestSweepInternal.hpp"

#include <limits>
#include <utility>

namespace hft_backtest::detail {
namespace {

std::uint64_t valueCount(const BacktestSweepParamRange& range) noexcept {
  if (range.stepRaw <= 0 || range.maxRaw < range.minRaw) return 0;
  const std::uint64_t span = static_cast<std::uint64_t>(range.maxRaw - range.minRaw);
  return (span / static_cast<std::uint64_t>(range.stepRaw)) + 1u;
}

std::int64_t valueAt(const BacktestSweepParamRange& range, std::uint64_t index) noexcept {
  return range.minRaw + static_cast<std::int64_t>(index) * range.stepRaw;
}

}  // namespace

std::vector<BacktestSweepPoint> buildSweepSearchPlan(const BacktestSweepRequest& request) {
  std::vector<BacktestSweepPoint> out{};
  if (request.ranges.empty() || request.runBudget == 0u) return out;
  std::vector<std::uint64_t> counts{};
  counts.reserve(request.ranges.size());
  std::uint64_t fullGrid = 1u;
  for (std::size_t dim = 0u; dim < request.ranges.size(); ++dim) {
    const BacktestSweepParamRange& range = request.ranges[dim];
    const std::uint64_t count = valueCount(range);
    if (count == 0u) return {};
    counts.push_back(count);
    if (fullGrid != std::numeric_limits<std::uint64_t>::max()) {
      const std::uint64_t cap = std::numeric_limits<std::uint64_t>::max();
      fullGrid = count > cap / fullGrid ? cap : fullGrid * count;
    }
  }

  const std::uint64_t target = std::min(request.runBudget, fullGrid);
  out.reserve(static_cast<std::size_t>(target));
  for (std::uint64_t ordinal = 0u; ordinal < target; ++ordinal) {
    const std::uint64_t gridOrdinal = target == fullGrid ? ordinal : static_cast<std::uint64_t>((static_cast<unsigned __int128>(ordinal) * fullGrid) / target);
    std::uint64_t cursor = gridOrdinal;
    BacktestSweepPoint point{};
    point.pointId = ordinal;
    point.values.resize(request.ranges.size());
    for (std::size_t dim = 0u; dim < request.ranges.size(); ++dim) {
      const std::uint64_t count = counts[dim];
      const std::uint64_t index = cursor % count;
      cursor /= count;
      point.values[dim] = valueAt(request.ranges[dim], index);
    }
    out.push_back(std::move(point));
  }
  return out;
}

}  // namespace hft_backtest::detail
