#pragma once

#include "hft_backtest/Backtest.hpp"

#include <memory>

namespace hft_backtest::detail {

// One replay leg's bounded execution authority. Storage is allocated during
// configuration and remains at a stable address when EngineState is moved.
class BacktestQuota final {
 public:
  BacktestQuota() noexcept;
  ~BacktestQuota();
  BacktestQuota(BacktestQuota&&) noexcept;
  BacktestQuota& operator=(BacktestQuota&&) noexcept;
  BacktestQuota(const BacktestQuota&) = delete;
  BacktestQuota& operator=(const BacktestQuota&) = delete;

  [[nodiscard]] bool configure(const BacktestRateLimitSchedule&, std::string& error);
  [[nodiscard]] trading_core::execution::PermitDecision admit(
      trading_core::RateLimitActionKind, TimeNs now) noexcept;
  [[nodiscard]] trading_core::StrategyRateLimitBook observationBook(
      trading_core::ExecutionRouteHandle, ExchangeId, canon::MarketType,
      std::uint8_t apiSlot, TimeNs now) noexcept;
  [[nodiscard]] bool configured() const noexcept { return storage_ != nullptr; }

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
  bool disabled_{false};
};

}  // namespace hft_backtest::detail
