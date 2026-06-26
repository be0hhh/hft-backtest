#pragma once

#include "engine/backtest_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hft_backtest::detail {

struct ExecutionRepriceStats {
  std::uint64_t fillsVisited{0};
  std::uint64_t fillsRepriced{0};
  std::uint64_t fillsMissingDepth{0};
  std::uint64_t depthRowsRead{0};
  std::uint64_t fillsBboRepriced{0};
  std::uint64_t fillsDepthVwapRepriced{0};
  std::uint64_t fillsDepthRejectedUnseeded{0};
  std::uint64_t fillsDepthRejectedCrossed{0};
  std::uint64_t fillsDepthRejectedStale{0};
  std::uint64_t fillsMissingBbo{0};
};

[[nodiscard]] bool repriceFillFromBook(
    JsonFill& fill,
    const hft_trader::core::LocalOrderBookState& book) noexcept;

[[nodiscard]] bool repriceFillsWithDepth(const std::filesystem::path& sessionPath,
                                         const Symbol& symbol,
                                         std::int64_t tickSizeE8,
                                         std::vector<JsonFill>& fills,
                                         ExecutionRepriceStats& stats) noexcept;

void rebuildAccountingFromFills(EngineState& engine,
                                std::int64_t startTsNs,
                                std::int64_t finalTsNs);

}  // namespace hft_backtest::detail
