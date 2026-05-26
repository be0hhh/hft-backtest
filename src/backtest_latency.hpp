#pragma once

#include "hft_backtest/backtest.hpp"

#include <cstdint>

namespace hft_backtest::detail {

enum class BacktestLatencyChannel : std::uint8_t {
  MarketData = 0,
  MarketOrder = 1,
  LimitOrder = 2,
};

struct BacktestLatencyConfig {
  BacktestLatencyProfile marketData{};
  BacktestLatencyProfile marketOrder{1000u, 0u};
  BacktestLatencyProfile limitOrder{1000u, 0u};
  std::uint64_t seed{0u};
};

struct BacktestLatencyRuntime {
  BacktestLatencyConfig config{};
  std::uint64_t marketDataState{0u};
  std::uint64_t marketOrderState{0u};
  std::uint64_t limitOrderState{0u};
};

[[nodiscard]] BacktestLatencyConfig latencyConfigFromRequest(const BacktestRunRequest& request) noexcept;
void initLatencyRuntime(BacktestLatencyRuntime& runtime, const BacktestLatencyConfig& config) noexcept;
[[nodiscard]] std::uint64_t latencyProfileMaxUs(BacktestLatencyProfile profile) noexcept;
[[nodiscard]] std::uint64_t sampleLatencyUs(BacktestLatencyRuntime& runtime, BacktestLatencyChannel channel) noexcept;

}  // namespace hft_backtest::detail
